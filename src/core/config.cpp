#include "pixelstatus/config.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace pixelstatus {
namespace {

using Json = nlohmann::json;
constexpr std::size_t maximum_config_bytes = 1024U * 1024U;
constexpr std::size_t maximum_dimension = 256U;
constexpr std::size_t maximum_pixels = 65'536U;
constexpr std::size_t maximum_indicators = 1'024U;
constexpr std::size_t maximum_statuses = 256U;

void add_error(ConfigLoadResult& result, std::string message) {
    result.errors.push_back(std::move(message));
}

void reject_unknown_fields(
    const Json& object,
    std::initializer_list<std::string_view> allowed,
    std::string_view path,
    ConfigLoadResult& result) {
    for (auto field = object.begin(); field != object.end(); ++field) {
        bool known{};
        for (const auto candidate : allowed) {
            if (field.key() == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            add_error(result, std::string(path) + " contains unknown field: " + field.key());
        }
    }
}

[[nodiscard]] std::optional<std::string> required_string(
    const Json& object,
    std::string_view key,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        add_error(result, std::string(path) + "." + std::string(key) + " must be a string");
        return std::nullopt;
    }
    const auto value = found->get<std::string>();
    if (value.empty()) {
        add_error(result, std::string(path) + "." + std::string(key) + " must not be empty");
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::size_t> bounded_size(
    const Json& object,
    std::string_view key,
    std::string_view path,
    std::size_t minimum,
    std::size_t maximum,
    ConfigLoadResult& result) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) {
        add_error(result, std::string(path) + "." + std::string(key) + " must be an integer");
        return std::nullopt;
    }

    std::uint64_t value{};
    if (found->is_number_unsigned()) {
        value = found->get<std::uint64_t>();
    } else {
        const auto signed_value = found->get<std::int64_t>();
        if (signed_value < 0) {
            add_error(result, std::string(path) + "." + std::string(key) + " must not be negative");
            return std::nullopt;
        }
        value = static_cast<std::uint64_t>(signed_value);
    }

    if (value < minimum || value > maximum) {
        add_error(
            result,
            std::string(path) + "." + std::string(key) + " must be between "
                + std::to_string(minimum) + " and " + std::to_string(maximum));
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::optional<Rgb> color_value(
    const Json& value,
    std::string_view path,
    ConfigLoadResult& result) {
    if (!value.is_string()) {
        add_error(result, std::string(path) + " must be an RGB string such as #00FF00");
        return std::nullopt;
    }
    const auto color = parse_rgb_hex(value.get_ref<const std::string&>());
    if (!color) {
        add_error(result, std::string(path) + " must use #RRGGBB format");
    }
    return color;
}

[[nodiscard]] std::optional<TimelineAppearance> parse_appearance(
    const Json& status,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto appearance = status.find("appearance");
    if (appearance == status.end() || !appearance->is_object()) {
        add_error(result, std::string(path) + ".appearance must be an object");
        return std::nullopt;
    }
    reject_unknown_fields(status, {"appearance"}, path, result);

    if (const auto solid = appearance->find("solid"); solid != appearance->end()) {
        reject_unknown_fields(*appearance, {"solid"}, std::string(path) + ".appearance", result);
        const auto color = color_value(*solid, std::string(path) + ".appearance.solid", result);
        if (appearance->size() != 1U) {
            add_error(result, std::string(path) + ".appearance must define exactly one appearance type");
        }
        if (color) {
            return TimelineAppearance::solid(*color);
        }
        return std::nullopt;
    }

    if (const auto blink = appearance->find("blink"); blink != appearance->end()) {
        reject_unknown_fields(*appearance, {"blink"}, std::string(path) + ".appearance", result);
        if (!blink->is_object()) {
            add_error(result, std::string(path) + ".appearance.blink must be an object");
            return std::nullopt;
        }
        reject_unknown_fields(
            *blink,
            {"color", "on", "off"},
            std::string(path) + ".appearance.blink",
            result);
        if (appearance->size() != 1U) {
            add_error(result, std::string(path) + ".appearance must define exactly one appearance type");
        }

        const auto color_field = blink->find("color");
        const auto on_field = blink->find("on");
        const auto off_field = blink->find("off");
        if (color_field == blink->end()) {
            add_error(result, std::string(path) + ".appearance.blink.color is required");
        }
        if (on_field == blink->end() || !on_field->is_string()) {
            add_error(result, std::string(path) + ".appearance.blink.on must be a duration string");
        }
        if (off_field == blink->end() || !off_field->is_string()) {
            add_error(result, std::string(path) + ".appearance.blink.off must be a duration string");
        }

        const auto color = color_field == blink->end()
            ? std::optional<Rgb>{}
            : color_value(*color_field, std::string(path) + ".appearance.blink.color", result);
        const auto on = on_field != blink->end() && on_field->is_string()
            ? parse_duration(on_field->get_ref<const std::string&>())
            : std::optional<Duration>{};
        const auto off = off_field != blink->end() && off_field->is_string()
            ? parse_duration(off_field->get_ref<const std::string&>())
            : std::optional<Duration>{};

        if (on_field != blink->end() && on_field->is_string() && (!on || *on <= Duration::zero())) {
            add_error(result, std::string(path) + ".appearance.blink.on must be greater than zero");
        }
        if (off_field != blink->end() && off_field->is_string() && (!off || *off <= Duration::zero())) {
            add_error(result, std::string(path) + ".appearance.blink.off must be greater than zero");
        }
        if (color && on && off && *on > Duration::zero() && *off > Duration::zero()) {
            return TimelineAppearance::blink(*color, *on, *off);
        }
        return std::nullopt;
    }

    add_error(result, std::string(path) + ".appearance must define solid or blink");
    return std::nullopt;
}

}  // namespace

std::optional<Duration> parse_duration(std::string_view value) {
    std::int64_t multiplier{};
    std::string_view digits;
    if (value.ends_with("ms")) {
        multiplier = 1;
        digits = value.substr(0, value.size() - 2U);
    } else if (value.ends_with('s')) {
        multiplier = 1'000;
        digits = value.substr(0, value.size() - 1U);
    } else if (value.ends_with('m')) {
        multiplier = 60'000;
        digits = value.substr(0, value.size() - 1U);
    } else if (value.ends_with('h')) {
        multiplier = 3'600'000;
        digits = value.substr(0, value.size() - 1U);
    } else {
        return std::nullopt;
    }

    if (digits.empty()) {
        return std::nullopt;
    }

    std::int64_t amount{};
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), amount, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || amount < 0) {
        return std::nullopt;
    }
    if (amount > std::numeric_limits<std::int64_t>::max() / multiplier) {
        return std::nullopt;
    }
    return Duration{amount * multiplier};
}

ConfigLoadResult load_config_file(const std::filesystem::path& path) {
    ConfigLoadResult result;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        add_error(result, "Unable to open configuration file: " + path.string());
        return result;
    }

    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > maximum_config_bytes) {
        add_error(result, "Configuration file exceeds the 1 MiB host limit");
        return result;
    }
    input.seekg(0);
    std::string contents(static_cast<std::size_t>(length), '\0');
    input.read(contents.data(), length);
    if (!input && length != 0) {
        add_error(result, "Unable to read configuration file: " + path.string());
        return result;
    }

    const auto root = Json::parse(contents, nullptr, false, true);
    if (root.is_discarded() || !root.is_object()) {
        add_error(result, "Configuration root must be a valid JSON object");
        return result;
    }

    AppConfig config;
    try {
        reject_unknown_fields(
            root,
            {"schema_version", "display", "statuses", "indicators"},
            "configuration",
            result);
        const auto schema = root.find("schema_version");
        if (schema == root.end() || !schema->is_number_integer()) {
            add_error(result, "schema_version must be the integer 1");
        } else {
            config.schema_version = schema->get<int>();
            if (config.schema_version != 1) {
                add_error(result, "Only schema_version 1 is supported");
            }
        }

        const auto display = root.find("display");
        if (display == root.end() || !display->is_object()) {
            add_error(result, "display must be an object");
        } else {
            reject_unknown_fields(*display, {"width", "height", "background"}, "display", result);
            const auto width = bounded_size(*display, "width", "display", 1, maximum_dimension, result);
            const auto height = bounded_size(*display, "height", "display", 1, maximum_dimension, result);
            if (width) {
                config.display.width = *width;
            }
            if (height) {
                config.display.height = *height;
            }
            if (width && height && *width > maximum_pixels / *height) {
                add_error(result, "display contains too many pixels");
            }
            if (const auto background = display->find("background"); background != display->end()) {
                if (const auto color = color_value(*background, "display.background", result)) {
                    config.display.background = *color;
                }
            }
        }

        const auto statuses = root.find("statuses");
        if (statuses == root.end() || !statuses->is_object() || statuses->empty()) {
            add_error(result, "statuses must be a non-empty object");
        } else if (statuses->size() > maximum_statuses) {
            add_error(result, "statuses exceeds the limit of 256 entries");
        } else {
            for (auto status = statuses->begin(); status != statuses->end(); ++status) {
                const auto path_text = std::string("statuses.") + status.key();
                if (!status.value().is_object()) {
                    add_error(result, path_text + " must be an object");
                    continue;
                }
                if (auto appearance = parse_appearance(status.value(), path_text, result)) {
                    config.statuses.emplace(status.key(), std::move(*appearance));
                }
            }
            if (!statuses->contains("unknown")) {
                add_error(result, "statuses.unknown is required");
            }
            if (!statuses->contains("stale")) {
                add_error(result, "statuses.stale is required");
            }
        }

        const auto indicators = root.find("indicators");
        if (indicators == root.end() || !indicators->is_array() || indicators->empty()) {
            add_error(result, "indicators must be a non-empty array");
        } else if (indicators->size() > maximum_indicators) {
            add_error(result, "indicators exceeds the limit of 1024 entries");
        } else {
            std::unordered_set<std::string> ids;
            for (std::size_t index = 0; index < indicators->size(); ++index) {
                const auto& item = (*indicators)[index];
                const auto path_text = std::string("indicators[") + std::to_string(index) + "]";
                if (!item.is_object()) {
                    add_error(result, path_text + " must be an object");
                    continue;
                }
                reject_unknown_fields(
                    item,
                    {"id", "source", "x", "y", "width", "height"},
                    path_text,
                    result);

                IndicatorConfig indicator;
                const auto id = required_string(item, "id", path_text, result);
                const auto source = required_string(item, "source", path_text, result);
                const auto x = bounded_size(item, "x", path_text, 0, maximum_dimension, result);
                const auto y = bounded_size(item, "y", path_text, 0, maximum_dimension, result);
                const auto width = bounded_size(item, "width", path_text, 1, maximum_dimension, result);
                const auto height = bounded_size(item, "height", path_text, 1, maximum_dimension, result);
                if (!id || !source || !x || !y || !width || !height) {
                    continue;
                }
                if (!ids.insert(*id).second) {
                    add_error(result, path_text + ".id duplicates an earlier indicator");
                    continue;
                }
                if (*x >= config.display.width || *width > config.display.width - *x
                    || *y >= config.display.height || *height > config.display.height - *y) {
                    add_error(result, path_text + " extends outside the configured display");
                    continue;
                }

                indicator.id = *id;
                indicator.source = *source;
                indicator.x = *x;
                indicator.y = *y;
                indicator.width = *width;
                indicator.height = *height;
                config.indicators.push_back(std::move(indicator));
            }
        }
    } catch (const std::exception& error) {
        add_error(result, std::string("Configuration conversion failed: ") + error.what());
    }

    if (result.errors.empty()) {
        result.config = std::move(config);
    }
    return result;
}

}  // namespace pixelstatus
