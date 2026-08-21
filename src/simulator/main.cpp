#include "win32_output_driver.hpp"
#include "http_status_server.hpp"

#include "pixelstatus/config.hpp"
#include "pixelstatus/frame.hpp"
#include "pixelstatus/host/http_display_driver.hpp"
#include "pixelstatus/host/monitor_executor.hpp"
#include "pixelstatus/host/monitor_runner_factory.hpp"
#include "pixelstatus/monitor_engine.hpp"
#include "pixelstatus/renderer.hpp"
#include "pixelstatus/state.hpp"
#include "pixelstatus/status_api.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <variant>
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
    for (const auto& card : config.cards) {
        const auto* content =
            std::get_if<pixelstatus::IndicatorCardConfig>(&card.content);
        if (content == nullptr) {
            continue;
        }
        for (const auto& indicator : content->indicators) {
            if (seen.insert(indicator.source).second) {
                sources.push_back(indicator.source);
            }
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
    std::uint16_t api_port{8787};
    std::optional<std::string> api_token;
    bool api_enabled{true};
    std::string web_display_bind{"127.0.0.1"};
    std::uint16_t web_display_port{8788};
    std::chrono::milliseconds web_refresh_interval{100};
    std::size_t monitor_worker_count{2U};
    bool web_display_enabled{true};
    bool window_enabled{true};
};

std::optional<CommandLine> parse_command_line(int argc, char** argv) {
    CommandLine options{default_config_path(argv[0])};
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
        if (argument == "--api-port") {
            if (++index >= argc) {
                return std::nullopt;
            }
            std::uint32_t port{};
            const std::string_view value(argv[index]);
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), port, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || port == 0U || port > 65'535U) {
                return std::nullopt;
            }
            options.api_port = static_cast<std::uint16_t>(port);
            continue;
        }
        if (argument == "--api-token") {
            if (++index >= argc || std::string_view(argv[index]).empty()) {
                return std::nullopt;
            }
            options.api_token = argv[index];
            continue;
        }
        if (argument == "--no-api") {
            options.api_enabled = false;
            continue;
        }
        if (argument == "--web-display-port") {
            if (++index >= argc) {
                return std::nullopt;
            }
            std::uint32_t port{};
            const std::string_view value(argv[index]);
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), port, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || port == 0U || port > 65'535U) {
                return std::nullopt;
            }
            options.web_display_port = static_cast<std::uint16_t>(port);
            continue;
        }
        if (argument == "--web-display-bind") {
            if (++index >= argc || std::string_view(argv[index]).empty()) {
                return std::nullopt;
            }
            options.web_display_bind = argv[index];
            continue;
        }
        if (argument == "--web-refresh-ms") {
            if (++index >= argc) {
                return std::nullopt;
            }
            std::int64_t milliseconds{};
            const std::string_view value(argv[index]);
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), milliseconds, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || milliseconds < 16 || milliseconds > 5 * 60 * 1000) {
                return std::nullopt;
            }
            options.web_refresh_interval = std::chrono::milliseconds(milliseconds);
            continue;
        }
        if (argument == "--no-web-display") {
            options.web_display_enabled = false;
            continue;
        }
        if (argument == "--no-window") {
            options.window_enabled = false;
            continue;
        }
        if (argument == "--monitor-workers") {
            if (++index >= argc) {
                return std::nullopt;
            }
            std::uint32_t worker_count{};
            const std::string_view value(argv[index]);
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), worker_count, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || worker_count == 0U
                || worker_count > pixelstatus::host::maximum_monitor_worker_count) {
                return std::nullopt;
            }
            options.monitor_worker_count = worker_count;
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

std::string generate_bearer_token() {
    std::random_device source;
    std::array<std::uint32_t, 8> words{};
    for (auto& word : words) {
        word = source();
    }

    std::ostringstream token;
    token << std::hex << std::setfill('0');
    for (const auto word : words) {
        token << std::setw(8) << word;
    }
    return token.str();
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = parse_command_line(argc, argv);
    if (!options) {
        std::cerr
            << "Usage: pixelstatus_simulator [config.json] [--run-for-ms N] "
               "[--api-port N] [--api-token TOKEN] [--no-api] "
               "[--web-display-port N] [--web-display-bind ADDRESS] "
               "[--web-refresh-ms N] [--no-web-display] [--no-window] "
               "[--monitor-workers N]\n";
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

    if (!options->window_enabled && !options->web_display_enabled) {
        std::cerr << "At least one display backend must be enabled\n";
        return 1;
    }

    std::unique_ptr<pixelstatus::simulator::Win32OutputDriver> display;
    if (options->window_enabled) {
        display = std::make_unique<pixelstatus::simulator::Win32OutputDriver>(
            config.display.width,
            config.display.height,
            L"PixelStatus NX - Win32 Display Simulator");
        if (!display->begin()) {
            std::cerr << "Unable to start the Win32 simulator: "
                      << display->state().detail << '\n';
            return 1;
        }
    }

    std::unique_ptr<pixelstatus::host::HttpDisplayDriver> web_display;
    if (options->web_display_enabled) {
        pixelstatus::host::HttpDisplayOptions web_options;
        web_options.bind_address = options->web_display_bind;
        web_options.port = options->web_display_port;
        web_options.default_refresh_interval = options->web_refresh_interval;
        web_display = std::make_unique<pixelstatus::host::HttpDisplayDriver>(
            config.display.width,
            config.display.height,
            std::move(web_options));
        if (!web_display->begin()) {
            std::cerr << "Unable to start the HTTP display: "
                      << web_display->state().detail << '\n';
            return 1;
        }
        std::cout << "Browser display: " << web_display->url() << '\n';
    }

    const auto started_at = std::chrono::steady_clock::now();
    pixelstatus::StateStore states;
    const auto sources = unique_sources(config);
    const std::vector<std::string> preferred_statuses{"ok", "fail", "info", "ok"};
    const auto demo_states = config.monitors.empty();
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto state = pixelstatus::MonitorState{};
        state.id = sources[index];
        state.status = demo_states
            ? available_status(config, preferred_statuses[index % preferred_statuses.size()])
            : "unknown";
        state.message = demo_states
            ? "Simulator-generated state"
            : "Awaiting monitor or push update";
        state.observed_at = started_at;
        state.updated_at = started_at;
        if (demo_states && index + 1U == sources.size()) {
            state.ttl = 6s;
        }
        if (!states.upsert(std::move(state))) {
            std::cerr << "Unable to initialize simulator state\n";
            return 1;
        }
    }

    pixelstatus::MonitorEngine monitor_engine(states);
    for (const auto& configured : config.monitors) {
        auto created = pixelstatus::host::create_monitor_runner(configured.source);
        if (!created) {
            std::cerr << "Unable to create monitor " << configured.id
                      << ": " << created.error << '\n';
            return 1;
        }

        pixelstatus::MonitorDefinition definition;
        definition.id = configured.id;
        definition.interval = configured.interval;
        definition.ttl = configured.ttl;
        definition.evaluation = configured.evaluation;
        const auto registered = monitor_engine.add(
            std::move(definition), std::move(created.runner), started_at);
        if (!registered) {
            std::cerr << "Unable to register monitor " << configured.id
                      << ": " << registered.error << '\n';
            return 1;
        }
    }
    std::unique_ptr<pixelstatus::host::MonitorExecutor> monitor_executor;
    if (monitor_engine.size() != 0U) {
        pixelstatus::host::MonitorExecutorOptions executor_options;
        executor_options.worker_count = std::min(
            options->monitor_worker_count,
            monitor_engine.size());
        monitor_executor = std::make_unique<pixelstatus::host::MonitorExecutor>(
            monitor_engine,
            executor_options);
        if (!monitor_executor->start()) {
            std::cerr << "Unable to start monitor executor: "
                      << monitor_executor->error() << '\n';
            return 1;
        }
    }

    std::unique_ptr<pixelstatus::StatusApi> status_api;
    std::unique_ptr<pixelstatus::simulator::HttpStatusServer> http_server;
    if (options->api_enabled) {
        const auto generated_token = !options->api_token.has_value();
        auto token = options->api_token.value_or(generate_bearer_token());
        status_api = std::make_unique<pixelstatus::StatusApi>(states, token);
        http_server = std::make_unique<pixelstatus::simulator::HttpStatusServer>(*status_api);
        if (!http_server->start("127.0.0.1", options->api_port)) {
            std::cerr << http_server->error() << '\n';
            return 1;
        }
        std::cout << "Status API: http://127.0.0.1:" << options->api_port
                  << "/api/v1/status\n";
        if (generated_token) {
            std::cout << "Generated bearer token: " << token << '\n';
        } else {
            std::cout << "Using the bearer token supplied on the command line.\n";
        }
    }

    pixelstatus::Frame frame(config.display.width, config.display.height);
    const pixelstatus::Renderer renderer(started_at);
    auto next_frame = started_at;
    auto next_transition = started_at + 4s;
    bool first_source_failed = false;

    std::cout << "Loaded " << options->config_path << '\n';
    if (config.cards.empty()) {
        std::cout << "The final region becomes stale after six seconds.\n";
    } else {
        std::cout << "Configured display cards: " << config.cards.size() << '\n';
    }
    if (display) {
        std::cout << "Close the simulator window to exit.\n";
    } else {
        std::cout << "Running headless; press Ctrl+C to exit.\n";
    }
    if (config.monitors.empty()) {
        std::cout << "The first region changes status every four seconds.\n";
    } else {
        std::cout << "Configured pull monitors: " << config.monitors.size() << '\n';
    }

    while (!display || display->process_events()) {
        const auto now = std::chrono::steady_clock::now();
        if (options->run_for && now - started_at >= *options->run_for) {
            break;
        }
        if (http_server && !http_server->running()) {
            std::cerr << "Status API failure: " << http_server->error() << '\n';
            return 1;
        }
        if (web_display && !web_display->running()) {
            std::cerr << "HTTP display failure: " << web_display->state().detail << '\n';
            return 1;
        }
        if (monitor_executor && !monitor_executor->running()) {
            std::cerr << monitor_executor->error() << '\n';
            return 1;
        }
        if (config.monitors.empty() && !sources.empty() && now >= next_transition) {
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
            if (display) {
                static_cast<void>(display->submit_frame(frame));
            }
            if (web_display) {
                static_cast<void>(web_display->submit_frame(frame));
            }
            next_frame = now + 33ms;
        }
        std::this_thread::sleep_for(2ms);
    }

    if (display) {
        const auto final_state = display->state();
        std::cout << "Win32 frames submitted: " << final_state.submitted_frames
                  << ", coalesced: " << final_state.coalesced_frames << '\n';
    }
    if (web_display) {
        const auto final_state = web_display->state();
        std::cout << "HTTP frames submitted: " << final_state.submitted_frames
                  << ", coalesced: " << final_state.coalesced_frames << '\n';
    }
    if (monitor_executor) {
        const auto final_worker_count = monitor_executor->worker_count();
        monitor_executor->stop();
        const auto final_stats = monitor_executor->stats();
        std::cout << "Monitor workers: " << final_worker_count
                  << ", checks executed: " << final_stats.jobs_executed
                  << ", transport failures: " << final_stats.transport_failures << '\n';
    }
    return 0;
}
