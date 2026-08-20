#include "pixelstatus/appearance.hpp"
#include "pixelstatus/config.hpp"
#include "pixelstatus/frame.hpp"
#include "pixelstatus/http_url.hpp"
#include "pixelstatus/mi_protocol.hpp"
#include "pixelstatus/monitor_engine.hpp"
#include "pixelstatus/renderer.hpp"
#include "pixelstatus/state.hpp"
#include "pixelstatus/status_api.hpp"

#ifdef PIXELSTATUS_TEST_HOST_HTTP
#include "pixelstatus/host/http_display_driver.hpp"
#include "pixelstatus/host/http_monitor_runner.hpp"
#include "pixelstatus/host/monitor_executor.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

void test_http_url_parsing() {
    auto parsed = pixelstatus::parse_http_url("http://example.test:8080/health?full=1");
    CHECK(parsed.has_value());
    CHECK(parsed && parsed->scheme == pixelstatus::HttpScheme::http);
    CHECK(parsed && parsed->base == "http://example.test:8080");
    CHECK(parsed && parsed->target == "/health?full=1");

    parsed = pixelstatus::parse_http_url("https://example.test?health=1");
    CHECK(parsed && parsed->scheme == pixelstatus::HttpScheme::https);
    CHECK(parsed && parsed->target == "/?health=1");
    CHECK(pixelstatus::parse_http_url("http://[::1]:8080/health").has_value());

    CHECK(!pixelstatus::parse_http_url("ftp://example.test/health"));
    CHECK(!pixelstatus::parse_http_url("http://user@example.test/health"));
    CHECK(!pixelstatus::parse_http_url("http://example.test:99999/health"));
    CHECK(!pixelstatus::parse_http_url("http://example.test/health#fragment"));
    CHECK(!pixelstatus::parse_http_url("http://example.test/bad path"));
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

void test_http_monitor_configuration() {
    const auto path = std::filesystem::path(PIXELSTATUS_TEST_DATA_DIR)
        / "http-monitor.example.json";
    const auto loaded = pixelstatus::load_config_file(path);
    if (!loaded) {
        for (const auto& error : loaded.errors) {
            std::cerr << "monitor config error: " << error << '\n';
        }
    }
    CHECK(loaded);
    CHECK(loaded.config && loaded.config->monitors.size() == 1U);
    if (!loaded.config || loaded.config->monitors.empty()) {
        return;
    }

    const auto& monitor = loaded.config->monitors.front();
    CHECK(monitor.id == "replication-lag");
    CHECK(monitor.interval == 10s);
    CHECK(monitor.ttl == 30s);
    CHECK(monitor.evaluation.rules.size() == 3U);
    CHECK(monitor.evaluation.rules.front().status == "critical");
    CHECK(monitor.evaluation.rules.front().when.has_value());
    CHECK(monitor.evaluation.rules.front().when
        && monitor.evaluation.rules.front().when->operation
            == pixelstatus::ComparisonOperation::greater_or_equal);
    CHECK(std::holds_alternative<pixelstatus::HttpMonitorConfig>(monitor.source));
    const auto& http = std::get<pixelstatus::HttpMonitorConfig>(monitor.source);
    CHECK(http.url == "http://127.0.0.1:18080/health");
    CHECK(http.timeout == 2s);
    CHECK(http.maximum_response_bytes == 4096U);
    CHECK(http.observation == pixelstatus::HttpObservation::json_pointer);
    CHECK(http.json_pointer == "/database/replication_lag");
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

pixelstatus::EvaluationOutcome evaluate_condition(
    pixelstatus::EvaluationCondition condition,
    pixelstatus::StateValue actual) {
    pixelstatus::EvaluationPolicy policy;
    policy.rules.push_back({std::move(condition), "matched"});
    pixelstatus::MonitorResult result;
    result.transport_success = true;
    result.value = std::move(actual);
    return pixelstatus::Evaluator{}.evaluate(result, policy);
}

void test_evaluator_comparisons() {
    using pixelstatus::ComparisonOperation;
    using pixelstatus::EvaluationCondition;
    using pixelstatus::StateValue;

    CHECK(evaluate_condition(
        {ComparisonOperation::exists, {}, std::nullopt},
        StateValue{true}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::not_exists, {}, std::nullopt},
        StateValue{}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::equals, StateValue{42.0}, std::nullopt},
        StateValue{std::int64_t{42}}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::not_equals, StateValue{"down"}, std::nullopt},
        StateValue{"up"}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::contains, StateValue{"healthy"}, std::nullopt},
        StateValue{"database healthy"}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::not_contains, StateValue{"error"}, std::nullopt},
        StateValue{"healthy"}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::greater_than, StateValue{std::int64_t{9}}, std::nullopt},
        StateValue{10.0}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::greater_or_equal, StateValue{10.0}, std::nullopt},
        StateValue{std::int64_t{10}}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::less_than, StateValue{10.0}, std::nullopt},
        StateValue{std::int64_t{9}}).status == "matched");
    CHECK(evaluate_condition(
        {ComparisonOperation::less_or_equal, StateValue{std::int64_t{10}}, std::nullopt},
        StateValue{10.0}).status == "matched");
    CHECK(evaluate_condition(
        {
            ComparisonOperation::between,
            StateValue{std::int64_t{10}},
            StateValue{20.0},
        },
        StateValue{std::int64_t{20}}).status == "matched");

    pixelstatus::EvaluationPolicy thresholds;
    thresholds.rules = {
        {EvaluationCondition{
             ComparisonOperation::greater_or_equal,
             StateValue{95.0},
             std::nullopt},
         "critical"},
        {EvaluationCondition{
             ComparisonOperation::greater_or_equal,
             StateValue{85.0},
             std::nullopt},
         "warn"},
        {std::nullopt, "ok"},
    };
    const pixelstatus::Evaluator evaluator;
    CHECK(!evaluator.validate(thresholds));

    pixelstatus::MonitorResult result;
    result.transport_success = true;
    result.value = std::int64_t{97};
    auto outcome = evaluator.evaluate(result, thresholds);
    CHECK(outcome.status == "critical");
    CHECK(outcome.matched_rule == 0U);

    result.value = 90.0;
    outcome = evaluator.evaluate(result, thresholds);
    CHECK(outcome.status == "warn");
    CHECK(outcome.matched_rule == 1U);

    result.value = std::int64_t{1};
    outcome = evaluator.evaluate(result, thresholds);
    CHECK(outcome.status == "ok");
    CHECK(outcome.matched_rule == 2U);

    result.transport_success = false;
    result.error = pixelstatus::MonitorError::timeout;
    outcome = evaluator.evaluate(result, thresholds);
    CHECK(outcome.status == "communication_failure");
    CHECK(outcome.transport_failure);
    CHECK(!outcome.matched_rule);

    auto invalid = thresholds;
    invalid.rules.insert(invalid.rules.begin(), {std::nullopt, "ok"});
    CHECK(evaluator.validate(invalid).has_value());
    invalid = thresholds;
    invalid.rules.front().when = EvaluationCondition{
        ComparisonOperation::between,
        StateValue{20.0},
        StateValue{10.0},
    };
    CHECK(evaluator.validate(invalid).has_value());
    invalid = thresholds;
    invalid.rules.front().when = EvaluationCondition{
        ComparisonOperation::greater_than,
        StateValue{std::numeric_limits<double>::quiet_NaN()},
        std::nullopt,
    };
    CHECK(evaluator.validate(invalid).has_value());
}

struct ScriptedRunnerProbe {
    std::deque<pixelstatus::MonitorResult> results;
    std::size_t calls{};
};

class ScriptedMonitorRunner final : public pixelstatus::MonitorRunner {
public:
    explicit ScriptedMonitorRunner(std::shared_ptr<ScriptedRunnerProbe> probe)
        : probe_(std::move(probe)) {}

    pixelstatus::MonitorResult run(pixelstatus::TimePoint) override {
        ++probe_->calls;
        if (probe_->results.empty()) {
            throw std::runtime_error("script exhausted");
        }
        auto result = std::move(probe_->results.front());
        probe_->results.pop_front();
        return result;
    }

private:
    std::shared_ptr<ScriptedRunnerProbe> probe_;
};

pixelstatus::MonitorResult successful_monitor_result(pixelstatus::StateValue value) {
    pixelstatus::MonitorResult result;
    result.transport_success = true;
    result.value = std::move(value);
    result.latency = 25ms;
    return result;
}

void test_monitor_engine() {
    const auto origin = pixelstatus::TimePoint{};
    pixelstatus::StateStore states;
    pixelstatus::MonitorEngine engine(states);

    pixelstatus::EvaluationPolicy thresholds;
    thresholds.rules = {
        {pixelstatus::EvaluationCondition{
             pixelstatus::ComparisonOperation::greater_or_equal,
             std::int64_t{95},
             std::nullopt},
         "critical"},
        {pixelstatus::EvaluationCondition{
             pixelstatus::ComparisonOperation::greater_or_equal,
             std::int64_t{85},
             std::nullopt},
         "warn"},
        {std::nullopt, "ok"},
    };

    auto probe = std::make_shared<ScriptedRunnerProbe>();
    probe->results.push_back(successful_monitor_result(std::int64_t{97}));
    probe->results.push_back(successful_monitor_result(88.0));
    pixelstatus::MonitorResult timeout;
    timeout.transport_success = false;
    timeout.error = pixelstatus::MonitorError::timeout;
    timeout.detail = "TCP connection timed out";
    probe->results.push_back(std::move(timeout));

    pixelstatus::MonitorDefinition definition;
    definition.id = "temperature";
    definition.interval = 10s;
    definition.ttl = 15s;
    definition.evaluation = thresholds;
    const auto registered = engine.add(
        definition,
        std::make_unique<ScriptedMonitorRunner>(probe),
        origin);
    CHECK(registered);
    CHECK(engine.size() == 1U);

    const auto duplicate = engine.add(
        definition,
        std::make_unique<ScriptedMonitorRunner>(std::make_shared<ScriptedRunnerProbe>()),
        origin);
    CHECK(!duplicate);

    auto invalid_definition = definition;
    invalid_definition.id = "invalid";
    invalid_definition.interval = 0ms;
    CHECK(!engine.add(
        invalid_definition,
        std::make_unique<ScriptedMonitorRunner>(std::make_shared<ScriptedRunnerProbe>()),
        origin));

    auto report = engine.run_due(origin, 0U);
    CHECK(report.due == 1U);
    CHECK(report.executed == 0U);
    CHECK(!states.find("temperature"));

    report = engine.run_due(origin);
    CHECK(report.executed == 1U);
    CHECK(report.state_updates == 1U);
    CHECK(probe->calls == 1U);
    CHECK(engine.next_due("temperature") == origin + 10s);
    auto state = states.resolve("temperature", origin);
    CHECK(state && state->effective_status == "critical");
    CHECK(state && std::get<std::int64_t>(state->state.value) == 97);

    pixelstatus::AppConfig render_config;
    render_config.display = {1, 1, {}};
    render_config.statuses.emplace("critical", pixelstatus::TimelineAppearance::solid({255, 0, 0}));
    render_config.statuses.emplace("stale", pixelstatus::TimelineAppearance::solid({255, 128, 0}));
    render_config.statuses.emplace("unknown", pixelstatus::TimelineAppearance::solid({255, 0, 255}));
    render_config.indicators.push_back({"temperature-indicator", "temperature", 0, 0, 1, 1});
    pixelstatus::Frame frame(1, 1);
    CHECK(pixelstatus::Renderer(origin).render(states, render_config, origin, frame).success);
    CHECK((*frame.pixel(0, 0) == pixelstatus::Rgb{255, 0, 0}));

    CHECK(engine.run_due(origin + 9s).executed == 0U);
    report = engine.run_due(origin + 10s);
    CHECK(report.executed == 1U);
    state = states.resolve("temperature", origin + 10s);
    CHECK(state && state->effective_status == "warn");

    report = engine.run_due(origin + 20s);
    CHECK(report.transport_failures == 1U);
    CHECK(report.runner_exceptions == 0U);
    state = states.resolve("temperature", origin + 20s);
    CHECK(state && state->effective_status == "communication_failure");
    CHECK(state && state->state.message == "TCP connection timed out");

    report = engine.run_due(origin + 100s);
    CHECK(report.executed == 1U);
    CHECK(report.transport_failures == 1U);
    CHECK(report.runner_exceptions == 1U);
    CHECK(engine.next_due("temperature") == origin + 110s);
    state = states.resolve("temperature", origin + 100s);
    CHECK(state && state->effective_status == "communication_failure");
    CHECK(state && state->state.message.find("script exhausted") != std::string::npos);
    state = states.resolve("temperature", origin + 115s);
    CHECK(state && state->effective_status == "stale");

    pixelstatus::StateStore bounded_states;
    pixelstatus::MonitorEngine bounded_engine(bounded_states);
    pixelstatus::EvaluationPolicy always_ok;
    always_ok.rules.push_back({std::nullopt, "ok"});
    for (const auto* id : {"b", "a"}) {
        auto runner_probe = std::make_shared<ScriptedRunnerProbe>();
        runner_probe->results.push_back(successful_monitor_result(true));
        pixelstatus::MonitorDefinition monitor;
        monitor.id = id;
        monitor.interval = 1s;
        monitor.evaluation = always_ok;
        CHECK(bounded_engine.add(
            std::move(monitor),
            std::make_unique<ScriptedMonitorRunner>(std::move(runner_probe)),
            origin));
    }
    report = bounded_engine.run_due(origin, 1U);
    CHECK(report.due == 2U);
    CHECK(report.executed == 1U);
    CHECK(bounded_states.find("a").has_value());
    CHECK(!bounded_states.find("b"));
    report = bounded_engine.run_due(origin, 1U);
    CHECK(report.due == 1U);
    CHECK(bounded_states.find("b").has_value());
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

void test_configuration_rejects_invalid_monitor() {
    const auto path = std::filesystem::temp_directory_path()
        / "pixelstatus-nx-invalid-monitor-test.json";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << R"({
          "schema_version": 1,
          "display": {"width": 1, "height": 1},
          "statuses": {
            "ok": {"appearance": {"solid": "#00FF00"}},
            "communication_failure": {"appearance": {"solid": "#FF00FF"}},
            "unknown": {"appearance": {"solid": "#000000"}},
            "stale": {"appearance": {"solid": "#FF8000"}}
          },
          "monitors": [
            {
              "id": "invalid-monitor",
              "type": "http",
              "url": "http://127.0.0.1:99999/health",
              "interval": "100ms",
              "observe": {"json_pointer": "not-a-pointer"},
              "evaluate": [
                {"otherwise": {"status": "undefined_status"}}
              ]
            }
          ],
          "indicators": [
            {"id": "one", "source": "invalid-monitor", "x": 0, "y": 0, "width": 1, "height": 1}
          ]
        })";
    }

    const auto loaded = pixelstatus::load_config_file(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    CHECK(!loaded);
    const auto errors_contain = [&loaded](std::string_view text) {
        return std::any_of(loaded.errors.begin(), loaded.errors.end(), [text](const auto& error) {
            return error.find(text) != std::string::npos;
        });
    };
    CHECK(errors_contain(".url must be"));
    CHECK(errors_contain(".interval must be between"));
    CHECK(errors_contain(".observe.json_pointer is invalid"));
    CHECK(errors_contain("undefined status"));
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

#ifdef PIXELSTATUS_TEST_HOST_HTTP

struct BlockingRunnerProbe {
    std::mutex mutex;
    std::condition_variable condition;
    bool started{};
    bool release{};
    std::atomic_size_t calls{};
    std::atomic_size_t active{};
    std::atomic_size_t maximum_active{};
};

class BlockingMonitorRunner final : public pixelstatus::MonitorRunner {
public:
    explicit BlockingMonitorRunner(std::shared_ptr<BlockingRunnerProbe> probe)
        : probe_(std::move(probe)) {}

    pixelstatus::MonitorResult run(pixelstatus::TimePoint) override {
        probe_->calls.fetch_add(1U, std::memory_order_relaxed);
        const auto active = probe_->active.fetch_add(1U, std::memory_order_relaxed) + 1U;
        auto maximum = probe_->maximum_active.load(std::memory_order_relaxed);
        while (maximum < active
               && !probe_->maximum_active.compare_exchange_weak(
                   maximum, active, std::memory_order_relaxed)) {
        }

        {
            std::unique_lock lock(probe_->mutex);
            probe_->started = true;
            probe_->condition.notify_all();
            probe_->condition.wait(lock, [this] { return probe_->release; });
        }
        probe_->active.fetch_sub(1U, std::memory_order_relaxed);
        return successful_monitor_result(std::int64_t{1});
    }

private:
    std::shared_ptr<BlockingRunnerProbe> probe_;
};

class ImmediateMonitorRunner final : public pixelstatus::MonitorRunner {
public:
    explicit ImmediateMonitorRunner(std::shared_ptr<std::atomic_size_t> calls)
        : calls_(std::move(calls)) {}

    pixelstatus::MonitorResult run(pixelstatus::TimePoint) override {
        calls_->fetch_add(1U, std::memory_order_relaxed);
        return successful_monitor_result(std::int64_t{2});
    }

private:
    std::shared_ptr<std::atomic_size_t> calls_;
};

void test_monitor_executor_concurrency() {
    pixelstatus::StateStore states;
    pixelstatus::MonitorEngine engine(states);
    pixelstatus::EvaluationPolicy always_ok;
    always_ok.rules.push_back({std::nullopt, "ok"});
    const auto first_due = std::chrono::steady_clock::now();

    auto blocking_probe = std::make_shared<BlockingRunnerProbe>();
    pixelstatus::MonitorDefinition slow;
    slow.id = "a-slow";
    slow.interval = 1ms;
    slow.evaluation = always_ok;
    CHECK(engine.add(
        std::move(slow),
        std::make_unique<BlockingMonitorRunner>(blocking_probe),
        first_due));

    auto fast_calls = std::make_shared<std::atomic_size_t>(0U);
    pixelstatus::MonitorDefinition fast;
    fast.id = "b-fast";
    fast.interval = 10s;
    fast.evaluation = always_ok;
    CHECK(engine.add(
        std::move(fast),
        std::make_unique<ImmediateMonitorRunner>(fast_calls),
        first_due));

    pixelstatus::host::MonitorExecutorOptions options;
    options.worker_count = 2U;
    options.idle_poll_interval = 1ms;
    pixelstatus::host::MonitorExecutor executor(engine, options);
    CHECK(executor.start());
    CHECK(executor.running());
    CHECK(executor.worker_count() == 2U);

    {
        std::unique_lock lock(blocking_probe->mutex);
        CHECK(blocking_probe->condition.wait_for(
            lock, 1s, [&blocking_probe] { return blocking_probe->started; }));
    }

    const auto fast_deadline = std::chrono::steady_clock::now() + 1s;
    while (!states.find("b-fast") && std::chrono::steady_clock::now() < fast_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(states.find("b-fast").has_value());
    CHECK(fast_calls->load(std::memory_order_relaxed) == 1U);

    std::this_thread::sleep_for(20ms);
    CHECK(blocking_probe->calls.load(std::memory_order_relaxed) == 1U);
    CHECK(blocking_probe->maximum_active.load(std::memory_order_relaxed) == 1U);

    {
        std::scoped_lock lock(blocking_probe->mutex);
        blocking_probe->release = true;
    }
    blocking_probe->condition.notify_all();

    const auto slow_deadline = std::chrono::steady_clock::now() + 1s;
    while (!states.find("a-slow") && std::chrono::steady_clock::now() < slow_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(states.find("a-slow").has_value());
    executor.stop();
    CHECK(!executor.running());
    CHECK(executor.worker_count() == 0U);
    const auto stats = executor.stats();
    CHECK(stats.jobs_executed >= 2U);
    CHECK(stats.state_updates >= 2U);

    options.worker_count = 0U;
    pixelstatus::host::MonitorExecutor invalid(engine, options);
    CHECK(!invalid.start());
    CHECK(!invalid.error().empty());
}

class MockHttpMonitorServer {
public:
    MockHttpMonitorServer() {
        server_.Get("/health", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(
                R"({"database":{"replication_lag":12},"healthy":true})",
                "application/json");
        });
        server_.Get("/missing", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(R"({"database":{}})", "application/json");
        });
        server_.Get("/invalid", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("not-json", "application/json");
        });
        server_.Get("/body", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("service healthy", "text/plain");
        });
        server_.Get("/large", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(std::string(2048U, 'x'), "text/plain");
        });
        server_.Get("/unavailable", [](const httplib::Request&, httplib::Response& response) {
            response.status = 503;
            response.set_content(R"({"status":"unavailable"})", "application/json");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) {
            throw std::runtime_error("Unable to bind mock HTTP monitor server");
        }
        worker_ = std::thread([this] {
            static_cast<void>(server_.listen_after_bind());
        });
        for (int attempt = 0; attempt < 100 && !server_.is_running(); ++attempt) {
            std::this_thread::sleep_for(1ms);
        }
        if (!server_.is_running()) {
            server_.stop();
            if (worker_.joinable()) {
                worker_.join();
            }
            throw std::runtime_error("Mock HTTP monitor server did not start");
        }
    }

    ~MockHttpMonitorServer() {
        server_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    MockHttpMonitorServer(const MockHttpMonitorServer&) = delete;
    MockHttpMonitorServer& operator=(const MockHttpMonitorServer&) = delete;

    [[nodiscard]] std::string url(std::string_view target) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(target);
    }

private:
    httplib::Server server_;
    int port_{};
    std::thread worker_;
};

void test_host_http_monitor_runner() {
    MockHttpMonitorServer server;

    pixelstatus::HttpMonitorConfig config;
    config.url = server.url("/health");
    config.timeout = 1s;
    config.maximum_response_bytes = 4096U;
    config.observation = pixelstatus::HttpObservation::json_pointer;
    config.json_pointer = "/database/replication_lag";
    auto created = pixelstatus::host::create_http_monitor_runner(config);
    CHECK(created);
    auto result = created.runner->run(std::chrono::steady_clock::now());
    CHECK(result.transport_success);
    CHECK(result.error == pixelstatus::MonitorError::none);
    CHECK(std::get<std::int64_t>(result.value) == 12);
    CHECK(result.detail == "HTTP 200");

    config.url = server.url("/unavailable");
    config.observation = pixelstatus::HttpObservation::status_code;
    created = pixelstatus::host::create_http_monitor_runner(config);
    CHECK(created);
    result = created.runner->run(std::chrono::steady_clock::now());
    CHECK(result.transport_success);
    CHECK(std::get<std::int64_t>(result.value) == 503);

    config.url = server.url("/body");
    config.observation = pixelstatus::HttpObservation::body;
    created = pixelstatus::host::create_http_monitor_runner(config);
    CHECK(created);
    result = created.runner->run(std::chrono::steady_clock::now());
    CHECK(result.transport_success);
    CHECK(std::get<std::string>(result.value) == "service healthy");

    config.url = server.url("/missing");
    config.observation = pixelstatus::HttpObservation::json_pointer;
    config.json_pointer = "/database/replication_lag";
    created = pixelstatus::host::create_http_monitor_runner(config);
    CHECK(created);
    result = created.runner->run(std::chrono::steady_clock::now());
    CHECK(result.transport_success);
    CHECK(std::holds_alternative<std::monostate>(result.value));

    config.url = server.url("/invalid");
    created = pixelstatus::host::create_http_monitor_runner(config);
    CHECK(created);
    result = created.runner->run(std::chrono::steady_clock::now());
    CHECK(!result.transport_success);
    CHECK(result.error == pixelstatus::MonitorError::invalid_response);

    config.url = server.url("/large");
    config.observation = pixelstatus::HttpObservation::body;
    config.maximum_response_bytes = 32U;
    created = pixelstatus::host::create_http_monitor_runner(config);
    CHECK(created);
    result = created.runner->run(std::chrono::steady_clock::now());
    CHECK(!result.transport_success);
    CHECK(result.error == pixelstatus::MonitorError::response_too_large);

    config.url = "https://example.test/health";
    CHECK(!pixelstatus::host::create_http_monitor_runner(config));
    config.url = "http://127.0.0.1:99999/health";
    CHECK(!pixelstatus::host::create_http_monitor_runner(config));

    const auto example_path = std::filesystem::path(PIXELSTATUS_TEST_DATA_DIR)
        / "http-monitor.example.json";
    const auto loaded = pixelstatus::load_config_file(example_path);
    CHECK(loaded);
    if (!loaded.config || loaded.config->monitors.empty()) {
        return;
    }
    auto pull = loaded.config->monitors.front();
    auto http = std::get<pixelstatus::HttpMonitorConfig>(pull.source);
    http.url = server.url("/health");
    created = pixelstatus::host::create_http_monitor_runner(std::move(http));
    CHECK(created);

    pixelstatus::StateStore states;
    pixelstatus::MonitorEngine engine(states);
    pixelstatus::MonitorDefinition definition;
    definition.id = pull.id;
    definition.interval = pull.interval;
    definition.ttl = pull.ttl;
    definition.evaluation = pull.evaluation;
    const auto now = std::chrono::steady_clock::now();
    CHECK(engine.add(std::move(definition), std::move(created.runner), now));
    const auto report = engine.run_due(now);
    CHECK(report.executed == 1U);
    CHECK(report.state_updates == 1U);
    const auto state = states.resolve("replication-lag", std::chrono::steady_clock::now());
    CHECK(state && state->effective_status == "ok");
    CHECK(state && std::get<std::int64_t>(state->state.value) == 12);

    pixelstatus::Frame frame(1, 1);
    const auto render_report = pixelstatus::Renderer(now).render(
        states,
        *loaded.config,
        std::chrono::steady_clock::now(),
        frame);
    CHECK(render_report.success);
    CHECK((*frame.pixel(0, 0) == pixelstatus::Rgb{0x00, 0xC8, 0x53}));
}

void test_http_display_driver() {
    pixelstatus::host::HttpDisplayOptions options;
    options.port = 0;
    options.default_refresh_interval = 33ms;
    pixelstatus::host::HttpDisplayDriver display(2U, 1U, options);
    CHECK(display.begin());
    CHECK(display.running());
    CHECK(display.port() != 0U);
    CHECK(display.url().starts_with("http://127.0.0.1:"));
    if (!display.running() || display.port() == 0U) {
        return;
    }

    httplib::Client client("127.0.0.1", display.port());
    client.set_connection_timeout(1s);
    client.set_read_timeout(1s);

    std::this_thread::sleep_for(5ms);
    const auto page = client.Get("/");
    CHECK(page);
    if (!page) {
        display.stop();
        return;
    }
    CHECK(page->status == 200);
    CHECK(page->get_header_value("Content-Type").starts_with("text/html"));
    CHECK(page->body.find("pixelstatus-display") != std::string::npos);
    CHECK(page->body.find("refresh-rate") != std::string::npos);
    CHECK(page->body.find("Open minimal display window") != std::string::npos);

    const auto initial = client.Get("/api/v1/display");
    CHECK(initial);
    if (!initial) {
        display.stop();
        return;
    }
    CHECK(initial->status == 200);
    const auto initial_json = nlohmann::json::parse(initial->body);
    CHECK(initial_json.at("schema_version") == 1);
    CHECK(initial_json.at("width") == 2U);
    CHECK(initial_json.at("height") == 1U);
    CHECK(initial_json.at("sequence") == 0U);
    CHECK(initial_json.at("format") == "rgb888");
    CHECK(initial_json.at("default_refresh_ms") == 33);
    CHECK(initial_json.at("pixels") == nlohmann::json::array({0U, 0U}));

    const auto initial_etag = initial->get_header_value("ETag");
    CHECK(!initial_etag.empty());
    const auto unchanged = client.Get(
        "/api/v1/display",
        httplib::Headers{{"If-None-Match", initial_etag}});
    CHECK(unchanged);
    CHECK(unchanged && unchanged->status == 304);

    pixelstatus::Frame frame(2U, 1U);
    CHECK(frame.set_pixel(0U, 0U, {255U, 0U, 0U}));
    CHECK(frame.set_pixel(1U, 0U, {0U, 255U, 0U}));
    CHECK(display.submit_frame(frame) == pixelstatus::FrameSubmitResult::accepted);
    CHECK(display.submit_frame(frame) == pixelstatus::FrameSubmitResult::coalesced);
    CHECK(display.submit_frame(pixelstatus::Frame(1U, 1U))
          == pixelstatus::FrameSubmitResult::unavailable);

    const auto updated = client.Get("/api/v1/display");
    CHECK(updated);
    if (updated) {
        CHECK(updated->status == 200);
        const auto updated_json = nlohmann::json::parse(updated->body);
        CHECK(updated_json.at("sequence") == 2U);
        CHECK(updated_json.at("pixels")
              == nlohmann::json::array({0xFF0000U, 0x00FF00U}));
    }

    const auto manifest = client.Get("/manifest.webmanifest");
    CHECK(manifest);
    CHECK(manifest && manifest->status == 200);
    CHECK(manifest && manifest->body.find("\"standalone\"") != std::string::npos);

    const auto state = display.state();
    CHECK(state.connection == pixelstatus::DriverConnectionState::ready);
    CHECK(state.submitted_frames == 2U);
    CHECK(state.coalesced_frames == 1U);
    display.stop();
    CHECK(!display.running());
    CHECK(display.state().connection == pixelstatus::DriverConnectionState::stopped);

    options.default_refresh_interval = 15ms;
    pixelstatus::host::HttpDisplayDriver invalid(1U, 1U, options);
    CHECK(!invalid.begin());
    CHECK(invalid.state().connection == pixelstatus::DriverConnectionState::failed);
}

#endif

}  // namespace

int main() {
    test_color_and_duration_parsing();
    test_http_url_parsing();
    test_frame_bounds();
    test_appearance_sampling();
    test_state_freshness_and_epoch();
    test_renderer();
    test_mi_protocol_vectors();
    test_sample_configuration();
    test_http_monitor_configuration();
    test_configuration_rejects_unknown_fields();
    test_configuration_rejects_invalid_identifiers();
    test_configuration_rejects_invalid_monitor();
    test_status_api();
    test_evaluator_comparisons();
    test_monitor_engine();
#ifdef PIXELSTATUS_TEST_HOST_HTTP
    test_monitor_executor_concurrency();
    test_host_http_monitor_runner();
    test_http_display_driver();
#endif

    if (failures != 0) {
        std::cerr << failures << " test check(s) failed\n";
        return 1;
    }
    std::cout << "All PixelStatus NX host tests passed\n";
    return 0;
}
