#pragma once

#include "pixelstatus/appearance.hpp"
#include "pixelstatus/color.hpp"
#include "pixelstatus/evaluator.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pixelstatus {

struct DisplayConfig {
    std::size_t width{16};
    std::size_t height{16};
    Rgb background{};
};

struct IndicatorConfig {
    std::string id;
    std::string source;
    std::size_t x{};
    std::size_t y{};
    std::size_t width{1};
    std::size_t height{1};
};

enum class HttpObservation {
    status_code,
    body,
    json_pointer,
};

struct HttpMonitorConfig {
    std::string url;
    Duration timeout{std::chrono::seconds(2)};
    std::size_t maximum_response_bytes{4U * 1024U};
    HttpObservation observation{HttpObservation::status_code};
    std::string json_pointer;
};

using MonitorSourceConfig = std::variant<HttpMonitorConfig>;

struct PullMonitorConfig {
    std::string id;
    Duration interval{};
    std::optional<Duration> ttl;
    EvaluationPolicy evaluation;
    MonitorSourceConfig source;
};

struct AppConfig {
    int schema_version{1};
    DisplayConfig display;
    std::unordered_map<std::string, TimelineAppearance> statuses;
    std::vector<PullMonitorConfig> monitors;
    std::vector<IndicatorConfig> indicators;
};

struct ConfigLoadResult {
    std::optional<AppConfig> config;
    std::vector<std::string> errors;

    [[nodiscard]] explicit operator bool() const noexcept {
        return config.has_value() && errors.empty();
    }
};

[[nodiscard]] ConfigLoadResult load_config_file(const std::filesystem::path& path);
[[nodiscard]] std::optional<Duration> parse_duration(std::string_view value);

}  // namespace pixelstatus
