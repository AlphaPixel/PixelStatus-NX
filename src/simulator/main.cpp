#include "win32_output_driver.hpp"

#include "pixelstatus/config.hpp"
#include "pixelstatus/frame.hpp"
#include "pixelstatus/renderer.hpp"
#include "pixelstatus/state.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::filesystem::path default_config_path(const char* executable) {
    const auto executable_path = std::filesystem::absolute(executable);
    return executable_path.parent_path() / "pixelstatus.sample.json";
}

std::vector<std::string> unique_sources(const pixelstatus::AppConfig& config) {
    std::vector<std::string> sources;
    std::unordered_set<std::string> seen;
    for (const auto& indicator : config.indicators) {
        if (seen.insert(indicator.source).second) {
            sources.push_back(indicator.source);
        }
    }
    return sources;
}

std::string available_status(
    const pixelstatus::AppConfig& config,
    std::string preferred,
    std::string fallback = "unknown") {
    return config.statuses.contains(preferred) ? preferred : fallback;
}

struct CommandLine {
    std::filesystem::path config_path;
    std::optional<std::chrono::milliseconds> run_for;
};

std::optional<CommandLine> parse_command_line(int argc, char** argv) {
    CommandLine options{default_config_path(argv[0]), std::nullopt};
    bool config_was_set{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--run-for-ms") {
            if (++index >= argc) {
                return std::nullopt;
            }
            std::int64_t milliseconds{};
            const std::string_view value(argv[index]);
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), milliseconds, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || milliseconds <= 0) {
                return std::nullopt;
            }
            options.run_for = std::chrono::milliseconds(milliseconds);
            continue;
        }
        if (config_was_set || argument.starts_with('-')) {
            return std::nullopt;
        }
        options.config_path = argument;
        config_was_set = true;
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = parse_command_line(argc, argv);
    if (!options) {
        std::cerr << "Usage: pixelstatus_simulator [config.json] [--run-for-ms N]\n";
        return 2;
    }
    const auto loaded = pixelstatus::load_config_file(options->config_path);
    if (!loaded) {
        std::cerr << "Unable to load " << options->config_path << '\n';
        for (const auto& error : loaded.errors) {
            std::cerr << "  - " << error << '\n';
        }
        return 1;
    }
    const auto& config = *loaded.config;

    pixelstatus::simulator::Win32OutputDriver display(
        config.display.width,
        config.display.height,
        L"PixelStatus NX - Win32 Display Simulator");
    if (!display.begin()) {
        std::cerr << "Unable to start the Win32 simulator: " << display.state().detail << '\n';
        return 1;
    }

    const auto started_at = std::chrono::steady_clock::now();
    pixelstatus::StateStore states;
    const auto sources = unique_sources(config);
    const std::vector<std::string> preferred_statuses{"ok", "fail", "info", "ok"};
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto state = pixelstatus::MonitorState{};
        state.id = sources[index];
        state.status = available_status(config, preferred_statuses[index % preferred_statuses.size()]);
        state.message = "Simulator-generated state";
        state.observed_at = started_at;
        state.updated_at = started_at;
        if (index + 1U == sources.size()) {
            state.ttl = 6s;
        }
        if (!states.upsert(std::move(state))) {
            std::cerr << "Unable to initialize simulator state\n";
            return 1;
        }
    }

    pixelstatus::Frame frame(config.display.width, config.display.height);
    const pixelstatus::Renderer renderer(started_at);
    auto next_frame = started_at;
    auto next_transition = started_at + 4s;
    bool first_source_failed = false;

    std::cout << "Loaded " << options->config_path << '\n'
              << "The first region changes status every four seconds.\n"
              << "The final region becomes stale after six seconds.\n"
              << "Close the simulator window to exit.\n";

    while (display.process_events()) {
        const auto now = std::chrono::steady_clock::now();
        if (options->run_for && now - started_at >= *options->run_for) {
            break;
        }
        if (!sources.empty() && now >= next_transition) {
            first_source_failed = !first_source_failed;
            pixelstatus::MonitorState state;
            state.id = sources.front();
            state.status = available_status(config, first_source_failed ? "fail" : "ok");
            state.message = "Periodic simulator transition";
            state.observed_at = now;
            state.updated_at = now;
            static_cast<void>(states.upsert(std::move(state)));
            next_transition = now + 4s;
        }

        if (now >= next_frame) {
            const auto report = renderer.render(states, config, now, frame);
            if (!report.success) {
                std::cerr << "Rendering failed: " << report.error << '\n';
                return 1;
            }
            static_cast<void>(display.submit_frame(frame));
            next_frame = now + 33ms;
        }
        std::this_thread::sleep_for(2ms);
    }

    const auto final_state = display.state();
    std::cout << "Frames submitted: " << final_state.submitted_frames
              << ", coalesced: " << final_state.coalesced_frames << '\n';
    return 0;
}
