#pragma once

#include "pixelstatus/appearance.hpp"
#include "pixelstatus/color.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
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

struct AppConfig {
    int schema_version{1};
    DisplayConfig display;
    std::unordered_map<std::string, TimelineAppearance> statuses;
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
