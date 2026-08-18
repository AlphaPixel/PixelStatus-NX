#include "pixelstatus/appearance.hpp"
#include "pixelstatus/config.hpp"
#include "pixelstatus/frame.hpp"
#include "pixelstatus/mi_protocol.hpp"
#include "pixelstatus/renderer.hpp"
#include "pixelstatus/state.hpp"
#include "pixelstatus/status_api.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace std::chrono_literals;

int failures{};

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

void test_color_and_duration_parsing() {
    CHECK((pixelstatus::parse_rgb_hex("#12ABef") == pixelstatus::Rgb{0x12, 0xAB, 0xEF}));
    CHECK(!pixelstatus::parse_rgb_hex("12ABEF"));
    CHECK(pixelstatus::to_rgb_hex({0x01, 0xA2, 0x0B}) == "#01A20B");
    CHECK(pixelstatus::parse_duration("250ms") == 250ms);
    CHECK(pixelstatus::parse_duration("2s") == 2s);
    CHECK(pixelstatus::parse_duration("3m") == 3min);
    CHECK(!pixelstatus::parse_duration("1.5s"));
}

void test_frame_bounds() {
    pixelstatus::Frame frame(4, 4);
    CHECK(frame.fill_rect(1, 1, 2, 2, {1, 2, 3}));
    CHECK((*frame.pixel(1, 1) == pixelstatus::Rgb{1, 2, 3}));
    CHECK((*frame.pixel(2, 2) == pixelstatus::Rgb{1, 2, 3}));
    CHECK(*frame.pixel(0, 0) == pixelstatus::Rgb{});
    CHECK(!frame.fill_rect(3, 3, 2, 2, {9, 9, 9}));
    CHECK(frame.pixel(4, 0) == nullptr);
}

void test_appearance_sampling() {
    const auto red = pixelstatus::Rgb{255, 0, 0};
    const auto blink = pixelstatus::TimelineAppearance::blink(red, 500ms, 500ms);
    CHECK(blink.sample(0ms) == red);
    CHECK(blink.sample(499ms) == red);
    CHECK(blink.sample(500ms) == pixelstatus::Rgb{});
    CHECK(blink.sample(999ms) == pixelstatus::Rgb{});
    CHECK(blink.sample(1000ms) == red);

    const pixelstatus::TimelineAppearance fade(
        {
            {0ms, {0, 0, 0}, pixelstatus::Transition::linear},
            {1000ms, {100, 200, 50}, pixelstatus::Transition::step},
        },
        1000ms,
        false);
    CHECK((fade.sample(500ms) == pixelstatus::Rgb{50, 100, 25}));

    const pixelstatus::TimelineAppearance repeating_fade(
        {
            {0ms, {0, 0, 0}, pixelstatus::Transition::linear},
            {1000ms, {100, 200, 50}, pixelstatus::Transition::linear},
        },
        2000ms,
        true);
    CHECK((repeating_fade.sample(1500ms) == pixelstatus::Rgb{50, 100, 25}));
    CHECK(repeating_fade.sample(2000ms) == pixelstatus::Rgb{});
}

void test_state_freshness_and_epoch() {
    const auto origin = pixelstatus::TimePoint{};
    pixelstatus::StateStore states;

    pixelstatus::MonitorState state;
    state.id = "service";
    state.status = "ok";
    state.observed_at = origin;
    state.updated_at = origin;
    state.ttl = 1s;
    CHECK(states.upsert(state));

    const auto fresh = states.resolve("service", origin + 999ms);
    CHECK(fresh && fresh->effective_status == "ok");
    CHECK(fresh && !fresh->freshness_expired);

    const auto stale = states.resolve("service", origin + 1s);
    CHECK(stale && stale->effective_status == "stale");
    CHECK(stale && stale->freshness_expired);
    CHECK(stale && stale->status_entered_at == origin + 1s);

    state.updated_at = origin + 2s;
    CHECK(states.upsert(state));
    const auto refreshed = states.resolve("service", origin + 2s);
    CHECK(refreshed && refreshed->effective_status == "ok");
    CHECK(refreshed && refreshed->status_entered_at == origin + 2s);

    state.status = "fail";
    state.updated_at = origin + 3s;
    CHECK(states.upsert(state));
    const auto failed = states.resolve("service", origin + 3s);
    CHECK(failed && failed->status_entered_at == origin + 3s);
}

void test_renderer() {
    const auto origin = pixelstatus::TimePoint{};
    pixelstatus::AppConfig config;
    config.display = {4, 2, {1, 1, 1}};
    config.statuses.emplace("ok", pixelstatus::TimelineAppearance::solid({0, 255, 0}));
    config.statuses.emplace("stale", pixelstatus::TimelineAppearance::solid({255, 128, 0}));
    config.statuses.emplace("unknown", pixelstatus::TimelineAppearance::solid({255, 0, 255}));
    config.indicators.push_back({"left", "known", 0, 0, 2, 2});
    config.indicators.push_back({"right", "missing", 2, 0, 2, 2});

    pixelstatus::StateStore states;
    pixelstatus::MonitorState state;
    state.id = "known";
    state.status = "ok";
    state.observed_at = origin;
    state.updated_at = origin;
    CHECK(states.upsert(std::move(state)));

    pixelstatus::Frame frame(4, 2);
    const pixelstatus::Renderer renderer(origin);
    const auto report = renderer.render(states, config, origin, frame);
    CHECK(report.success);
    CHECK(report.rendered_indicators == 2);
    CHECK(report.missing_sources == 1);
    CHECK((*frame.pixel(0, 0) == pixelstatus::Rgb{0, 255, 0}));
    CHECK((*frame.pixel(3, 1) == pixelstatus::Rgb{255, 0, 255}));
}

void test_mi_protocol_vectors() {
    const auto pixel_zero = pixelstatus::mi::build_pixel_packet(0, {255, 0, 0});
    const pixelstatus::mi::PixelPacket expected_zero{
        0xBC, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x55};
    CHECK(pixel_zero == expected_zero);

    const auto pixel_255 = pixelstatus::mi::build_pixel_packet(255, {1, 2, 3});
    CHECK(pixel_255[4] == 0xFF);
    CHECK(pixel_255[8] == 0x00);
    CHECK(pixelstatus::mi::xy_to_index(15, 15) == 255);
    CHECK(!pixelstatus::mi::xy_to_index(16, 0));

    std::array<pixelstatus::Rgb, pixelstatus::mi::pixels_per_block> colors{};
    colors.front() = {1, 2, 3};
    colors.back() = {4, 5, 6};
    const auto block = pixelstatus::mi::build_block_packet(7, colors);
    CHECK(block.has_value());
    CHECK(block && (*block)[0] == 0xBC);
    CHECK(block && (*block)[1] == 0x0F);
    CHECK(block && (*block)[2] == 0x08);
    CHECK(block && (*block)[3] == 1 && (*block)[4] == 2 && (*block)[5] == 3);
    CHECK(block && (*block)[96] == 4 && (*block)[97] == 5 && (*block)[98] == 6);
    CHECK(block && (*block)[99] == 0x55);
    CHECK(!pixelstatus::mi::build_block_packet(8, colors));
}

void test_sample_configuration() {
    const auto path = std::filesystem::path(PIXELSTATUS_TEST_DATA_DIR)
        / "pixelstatus.sample.json";
    const auto loaded = pixelstatus::load_config_file(path);
    if (!loaded) {
        for (const auto& error : loaded.errors) {
            std::cerr << "config error: " << error << '\n';
        }
    }
    CHECK(loaded);
    CHECK(loaded.config && loaded.config->display.width == 16);
    CHECK(loaded.config && loaded.config->display.height == 16);
    CHECK(loaded.config && loaded.config->indicators.size() == 4);
    CHECK(loaded.config && loaded.config->statuses.contains("stale"));
    if (loaded.config) {
        const auto& statuses = loaded.config->statuses;
        CHECK((statuses.at("info").sample(0ms) == pixelstatus::Rgb{0x00, 0x50, 0xC8}));
        CHECK((statuses.at("unknown").sample(500ms) == pixelstatus::Rgb{0x62, 0x00, 0xEA}));
        CHECK((statuses.at("stale").sample(1000ms) == pixelstatus::Rgb{0xFF, 0x91, 0x00}));
        CHECK((statuses.at("communication_failure").sample(300ms)
            == pixelstatus::Rgb{}));
        CHECK((statuses.at("fade_demo").sample(1000ms)
            == pixelstatus::Rgb{0x00, 0xA0, 0xFF}));
    }
}

pixelstatus::ApiRequest api_request(
    std::string method,
    std::string target,
    std::string body = {}) {
    pixelstatus::ApiRequest request;
    request.method = std::move(method);
    request.target = std::move(target);
    request.headers.emplace("Authorization", "Bearer test-token");
    if (!body.empty()) {
        request.headers.emplace("Content-Type", "application/json; charset=utf-8");
    }
    request.body = std::move(body);
    return request;
}

void test_status_api() {
    const auto origin = pixelstatus::TimePoint{};
    pixelstatus::StateStore states;
    pixelstatus::StatusApi api(states, "test-token");

    pixelstatus::ApiRequest unauthorized;
    unauthorized.method = "GET";
    unauthorized.target = "/api/v1/status";
    CHECK(api.handle(unauthorized, origin).status == 401);

    auto created = api.handle(
        api_request(
            "POST",
            "/api/v1/status",
            R"({"id":"build","status":"ok","value":42,"message":"host test","ttl":1})"),
        origin);
    CHECK(created.status == 201);
    CHECK(created.body.find("\"id\":\"build\"") != std::string::npos);
    CHECK(created.body.find("\"stale\":false") != std::string::npos);
    CHECK(states.size() == 1U);

    const auto stored = states.find("build");
    CHECK(stored && stored->status == "ok");
    CHECK(stored && std::get<std::int64_t>(stored->value) == 42);
    CHECK(stored && stored->message == "host test");
    CHECK(stored && stored->ttl == 1s);

    const auto listed = api.handle(api_request("GET", "/api/v1/status"), origin + 500ms);
    CHECK(listed.status == 200);
    CHECK(listed.body.find("\"statuses\"") != std::string::npos);

    const auto stale = api.handle(
        api_request("GET", "/api/v1/status/build"), origin + 1s);
    CHECK(stale.status == 200);
    CHECK(stale.body.find("\"status\":\"stale\"") != std::string::npos);
    CHECK(stale.body.find("\"reported_status\":\"ok\"") != std::string::npos);
    CHECK(stale.body.find("\"stale\":true") != std::string::npos);

    const auto updated = api.handle(
        api_request("POST", "/api/v1/status/build", R"({"status":"fail"})"),
        origin + 2s);
    CHECK(updated.status == 200);
    CHECK(updated.body.find("\"status\":\"fail\"") != std::string::npos);

    pixelstatus::AppConfig render_config;
    render_config.display = {1, 1, {}};
    render_config.statuses.emplace("fail", pixelstatus::TimelineAppearance::solid({255, 0, 0}));
    render_config.statuses.emplace("stale", pixelstatus::TimelineAppearance::solid({255, 128, 0}));
    render_config.statuses.emplace("unknown", pixelstatus::TimelineAppearance::solid({255, 0, 255}));
    render_config.indicators.push_back({"build-indicator", "build", 0, 0, 1, 1});
    pixelstatus::Frame rendered_frame(1, 1);
    const pixelstatus::Renderer renderer(origin);
    CHECK(renderer.render(states, render_config, origin + 2s, rendered_frame).success);
    CHECK((*rendered_frame.pixel(0, 0) == pixelstatus::Rgb{255, 0, 0}));

    CHECK(api.handle(
        api_request(
            "POST",
            "/api/v1/status/build",
            R"({"id":"other","status":"ok"})"),
        origin).status == 400);
    CHECK(api.handle(
        api_request("POST", "/api/v1/status", R"({"id":"new","status":"ok","extra":1})"),
        origin).status == 400);
    CHECK(api.handle(
        api_request("POST", "/api/v1/status", R"({"id":"new","status":"ok","ttl":0})"),
        origin).status == 400);
    CHECK(api.handle(
        api_request("POST", "/api/v1/status", R"({"id":"bad/id","status":"ok"})"),
        origin).status == 400);
    CHECK(api.handle(api_request("PUT", "/api/v1/status/build"), origin).status == 405);
    CHECK(api.handle(api_request("GET", "/api/v1/status/missing"), origin).status == 404);

    auto wrong_content_type = api_request(
        "POST", "/api/v1/status", R"({"id":"new","status":"ok"})");
    wrong_content_type.headers["Content-Type"] = "text/plain";
    CHECK(api.handle(wrong_content_type, origin).status == 415);
    wrong_content_type.headers["Content-Type"] = "application/jsonbad";
    CHECK(api.handle(wrong_content_type, origin).status == 415);

    auto oversized = api_request("POST", "/api/v1/status", std::string(4097U, 'x'));
    CHECK(api.handle(oversized, origin).status == 413);

    pixelstatus::StateStore limited_states;
    pixelstatus::StatusApiLimits limits;
    limits.maximum_states = 1U;
    pixelstatus::StatusApi limited_api(limited_states, "test-token", limits);
    CHECK(limited_api.handle(
        api_request("POST", "/api/v1/status/a", R"({"status":"ok"})"),
        origin).status == 201);
    CHECK(limited_api.handle(
        api_request("POST", "/api/v1/status/b", R"({"status":"ok"})"),
        origin).status == 507);
}

void test_configuration_rejects_invalid_identifiers() {
    const auto path = std::filesystem::temp_directory_path()
        / "pixelstatus-nx-invalid-identifier-test.json";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << R"({
          "schema_version": 1,
          "display": {"width": 1, "height": 1},
          "statuses": {
            "unknown": {"appearance": {"solid": "#000000"}},
            "stale": {"appearance": {"solid": "#FF8000"}}
          },
          "indicators": [
            {"id": "one", "source": "bad/source", "x": 0, "y": 0, "width": 1, "height": 1}
          ]
        })";
    }

    const auto loaded = pixelstatus::load_config_file(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    CHECK(!loaded);
    CHECK(!loaded.errors.empty());
}

void test_configuration_rejects_unknown_fields() {
    const auto path = std::filesystem::temp_directory_path()
        / "pixelstatus-nx-invalid-config-test.json";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << R"({
          "schema_version": 1,
          "display": {"width": 1, "height": 1},
          "statuses": {
            "unknown": {"appearance": {"solid": "#000000"}},
            "stale": {"appearance": {"solid": "#FF8000"}}
          },
          "indicators": [
            {"id": "one", "source": "one", "x": 0, "y": 0, "width": 1, "height": 1}
          ],
          "unexpected": true
        })";
    }

    const auto loaded = pixelstatus::load_config_file(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    CHECK(!loaded);
    CHECK(!loaded.errors.empty());
}

}  // namespace

int main() {
    test_color_and_duration_parsing();
    test_frame_bounds();
    test_appearance_sampling();
    test_state_freshness_and_epoch();
    test_renderer();
    test_mi_protocol_vectors();
    test_sample_configuration();
    test_configuration_rejects_unknown_fields();
    test_configuration_rejects_invalid_identifiers();
    test_status_api();

    if (failures != 0) {
        std::cerr << failures << " test check(s) failed\n";
        return 1;
    }
    std::cout << "All PixelStatus NX host tests passed\n";
    return 0;
}
