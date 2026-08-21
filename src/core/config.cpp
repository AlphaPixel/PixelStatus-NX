#include "pixelstatus/config.hpp"
#include "pixelstatus/http_url.hpp"
#include "pixelstatus/validation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
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
constexpr std::size_t maximum_layout_widgets = 1'024U;
constexpr std::size_t maximum_layout_nodes = 2'048U;
constexpr std::size_t maximum_layout_depth = 16U;
constexpr std::size_t maximum_layout_weight = 1'000'000U;
constexpr std::size_t maximum_cards = 32U;
constexpr std::size_t maximum_bitmap_palette_entries = 32U;
constexpr std::size_t maximum_statuses = 256U;
constexpr std::size_t maximum_monitors = 256U;
constexpr Duration maximum_appearance_duration = std::chrono::hours(24);
constexpr Duration minimum_monitor_interval = std::chrono::seconds(1);
constexpr Duration maximum_monitor_interval = std::chrono::hours(24);
constexpr Duration maximum_monitor_ttl = std::chrono::hours(24 * 7);
constexpr Duration maximum_monitor_timeout = std::chrono::seconds(30);
constexpr Duration minimum_card_hold = std::chrono::seconds(1);
constexpr Duration maximum_card_hold = std::chrono::hours(24);
constexpr Duration maximum_card_transition = std::chrono::seconds(10);
constexpr std::size_t maximum_monitor_rules = 32U;
constexpr std::size_t maximum_monitor_response_bytes = 64U * 1024U;
constexpr std::size_t maximum_url_bytes = 2U * 1024U;
constexpr std::size_t maximum_json_pointer_bytes = 512U;
constexpr std::size_t maximum_host_bytes = 253U;
constexpr std::size_t maximum_http_request_body_bytes = 16U * 1024U;
constexpr std::size_t maximum_http_header_count = 32U;
constexpr std::size_t maximum_http_header_name_bytes = 128U;
constexpr std::size_t maximum_http_header_value_bytes = 2U * 1024U;
constexpr std::size_t maximum_http_headers_bytes = 8U * 1024U;
constexpr std::size_t maximum_secret_name_bytes = 128U;
constexpr std::size_t maximum_tcp_send_bytes = 4U * 1024U;
constexpr std::size_t maximum_tcp_delimiter_bytes = 256U;

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

[[nodiscard]] std::optional<std::string> required_identifier(
    const Json& object,
    std::string_view key,
    std::string_view path,
    ConfigLoadResult& result) {
    auto value = required_string(object, key, path, result);
    if (value && !is_valid_identifier(*value)) {
        add_error(
            result,
            std::string(path) + "." + std::string(key)
                + " must contain 1-64 letters, digits, dots, underscores, or hyphens");
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::string> optional_identifier(
    const Json& object,
    std::string_view key,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return std::nullopt;
    }
    if (!found->is_string() || !is_valid_identifier(found->get_ref<const std::string&>())) {
        add_error(
            result,
            std::string(path) + "." + std::string(key)
                + " must contain 1-64 letters, digits, dots, underscores, or hyphens");
        return std::nullopt;
    }
    return found->get<std::string>();
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

[[nodiscard]] std::optional<Duration> duration_value(
    const Json& value,
    std::string_view path,
    ConfigLoadResult& result) {
    if (!value.is_string()) {
        add_error(result, std::string(path) + " must be a duration string");
        return std::nullopt;
    }
    const auto duration = parse_duration(value.get_ref<const std::string&>());
    if (!duration || *duration <= Duration::zero()
        || *duration > maximum_appearance_duration) {
        add_error(result, std::string(path) + " must be greater than zero and no more than 24h");
        return std::nullopt;
    }
    return duration;
}

[[nodiscard]] std::optional<Duration> bounded_duration_value(
    const Json& value,
    std::string_view path,
    Duration minimum,
    Duration maximum,
    ConfigLoadResult& result) {
    if (!value.is_string()) {
        add_error(result, std::string(path) + " must be a duration string");
        return std::nullopt;
    }
    const auto duration = parse_duration(value.get_ref<const std::string&>());
    if (!duration || *duration < minimum || *duration > maximum) {
        add_error(
            result,
            std::string(path) + " must be between " + std::to_string(minimum.count())
                + "ms and " + std::to_string(maximum.count()) + "ms");
        return std::nullopt;
    }
    return duration;
}

[[nodiscard]] std::optional<double> percentage_value(
    const Json& object,
    std::string_view key,
    double default_value,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return default_value;
    }

    std::optional<std::int64_t> percent;
    if (found->is_number_integer()) {
        if (found->is_number_unsigned()) {
            const auto value = found->get<std::uint64_t>();
            if (value <= 100U) {
                percent = static_cast<std::int64_t>(value);
            }
        } else {
            percent = found->get<std::int64_t>();
        }
    } else if (found->is_string()) {
        const auto& text = found->get_ref<const std::string&>();
        if (text.size() > 1U && text.back() == '%') {
            std::int64_t value{};
            const std::string_view digits(text.data(), text.size() - 1U);
            const auto parsed = std::from_chars(
                digits.data(), digits.data() + digits.size(), value, 10);
            if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size()) {
                percent = value;
            }
        }
    }

    if (!percent || *percent < 0 || *percent > 100) {
        add_error(
            result,
            std::string(path) + "." + std::string(key)
                + " must be a percentage from 0 to 100");
        return std::nullopt;
    }
    return static_cast<double>(*percent) / 100.0;
}

struct ParsedSteps {
    std::vector<ColorKeyframe> keyframes;
    Duration cycle{};
};

[[nodiscard]] std::optional<ParsedSteps> parse_steps(
    const Json& value,
    std::string_view path,
    std::size_t minimum_steps,
    Transition transition,
    ConfigLoadResult& result) {
    if (!value.is_array() || value.size() < minimum_steps || value.size() > 64U) {
        add_error(
            result,
            std::string(path) + " must contain between " + std::to_string(minimum_steps)
                + " and 64 steps");
        return std::nullopt;
    }

    ParsedSteps parsed;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto& step = value[index];
        const auto step_path = std::string(path) + "[" + std::to_string(index) + "]";
        if (!step.is_object()) {
            add_error(result, step_path + " must be an object");
            return std::nullopt;
        }
        reject_unknown_fields(step, {"color", "duration"}, step_path, result);
        const auto color_field = step.find("color");
        const auto duration_field = step.find("duration");
        if (color_field == step.end() || duration_field == step.end()) {
            add_error(result, step_path + " requires color and duration");
            return std::nullopt;
        }
        const auto color = color_value(*color_field, step_path + ".color", result);
        const auto duration = duration_value(*duration_field, step_path + ".duration", result);
        if (!color || !duration) {
            return std::nullopt;
        }
        if (*duration > maximum_appearance_duration - parsed.cycle) {
            add_error(result, std::string(path) + " total duration must not exceed 24h");
            return std::nullopt;
        }
        parsed.keyframes.push_back({parsed.cycle, *color, transition});
        parsed.cycle += *duration;
    }
    return parsed;
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
    reject_unknown_fields(
        *appearance,
        {"solid", "blink", "toggle", "fade", "pulse", "sequence", "cycle"},
        std::string(path) + ".appearance",
        result);
    if (appearance->size() != 1U) {
        add_error(result, std::string(path) + ".appearance must define exactly one appearance type");
        return std::nullopt;
    }

    if (const auto solid = appearance->find("solid"); solid != appearance->end()) {
        const auto color = color_value(*solid, std::string(path) + ".appearance.solid", result);
        if (color) {
            return TimelineAppearance::solid(*color);
        }
        return std::nullopt;
    }

    if (const auto blink = appearance->find("blink"); blink != appearance->end()) {
        if (!blink->is_object()) {
            add_error(result, std::string(path) + ".appearance.blink must be an object");
            return std::nullopt;
        }
        reject_unknown_fields(
            *blink,
            {"color", "on", "off"},
            std::string(path) + ".appearance.blink",
            result);
        const auto color_field = blink->find("color");
        const auto on_field = blink->find("on");
        const auto off_field = blink->find("off");
        if (color_field == blink->end() || on_field == blink->end() || off_field == blink->end()) {
            add_error(result, std::string(path) + ".appearance.blink requires color, on, and off");
            return std::nullopt;
        }
        const auto color = color_value(
            *color_field, std::string(path) + ".appearance.blink.color", result);
        const auto on = duration_value(*on_field, std::string(path) + ".appearance.blink.on", result);
        const auto off = duration_value(*off_field, std::string(path) + ".appearance.blink.off", result);
        if (color && on && off && *on <= maximum_appearance_duration - *off) {
            return TimelineAppearance::blink(*color, *on, *off);
        }
        if (on && off && *on > maximum_appearance_duration - *off) {
            add_error(result, std::string(path) + ".appearance.blink total duration must not exceed 24h");
        }
        return std::nullopt;
    }

    if (const auto toggle = appearance->find("toggle"); toggle != appearance->end()) {
        const auto toggle_path = std::string(path) + ".appearance.toggle";
        if (!toggle->is_object()) {
            add_error(result, toggle_path + " must be an object");
            return std::nullopt;
        }
        reject_unknown_fields(*toggle, {"colors", "period"}, toggle_path, result);
        const auto colors = toggle->find("colors");
        const auto period_field = toggle->find("period");
        if (colors == toggle->end() || !colors->is_array()
            || colors->size() < 2U || colors->size() > 16U) {
            add_error(result, toggle_path + ".colors must contain between 2 and 16 colors");
            return std::nullopt;
        }
        if (period_field == toggle->end()) {
            add_error(result, toggle_path + ".period is required");
            return std::nullopt;
        }
        const auto period = duration_value(*period_field, toggle_path + ".period", result);
        if (!period || period->count() > maximum_appearance_duration.count()
                / static_cast<Duration::rep>(colors->size())) {
            if (period) {
                add_error(result, toggle_path + " total duration must not exceed 24h");
            }
            return std::nullopt;
        }

        std::vector<ColorKeyframe> keyframes;
        keyframes.reserve(colors->size());
        for (std::size_t index = 0; index < colors->size(); ++index) {
            const auto color = color_value(
                (*colors)[index], toggle_path + ".colors[" + std::to_string(index) + "]", result);
            if (!color) {
                return std::nullopt;
            }
            keyframes.push_back({*period * static_cast<Duration::rep>(index), *color, Transition::step});
        }
        return TimelineAppearance(
            std::move(keyframes),
            *period * static_cast<Duration::rep>(colors->size()),
            true);
    }

    if (const auto fade = appearance->find("fade"); fade != appearance->end()) {
        const auto fade_path = std::string(path) + ".appearance.fade";
        if (!fade->is_object()) {
            add_error(result, fade_path + " must be an object");
            return std::nullopt;
        }
        reject_unknown_fields(*fade, {"from", "to", "period"}, fade_path, result);
        const auto from_field = fade->find("from");
        const auto to_field = fade->find("to");
        const auto period_field = fade->find("period");
        if (from_field == fade->end() || to_field == fade->end() || period_field == fade->end()) {
            add_error(result, fade_path + " requires from, to, and period");
            return std::nullopt;
        }
        const auto from = color_value(*from_field, fade_path + ".from", result);
        const auto to = color_value(*to_field, fade_path + ".to", result);
        const auto period = duration_value(*period_field, fade_path + ".period", result);
        if (!from || !to || !period || period->count() < 2) {
            if (period && period->count() < 2) {
                add_error(result, fade_path + ".period must be at least 2ms");
            }
            return std::nullopt;
        }
        return TimelineAppearance(
            {
                {Duration::zero(), *from, Transition::linear},
                {Duration{period->count() / 2}, *to, Transition::linear},
            },
            *period,
            true);
    }

    if (const auto pulse = appearance->find("pulse"); pulse != appearance->end()) {
        const auto pulse_path = std::string(path) + ".appearance.pulse";
        if (!pulse->is_object()) {
            add_error(result, pulse_path + " must be an object");
            return std::nullopt;
        }
        reject_unknown_fields(*pulse, {"color", "period", "minimum", "maximum"}, pulse_path, result);
        const auto color_field = pulse->find("color");
        const auto period_field = pulse->find("period");
        if (color_field == pulse->end() || period_field == pulse->end()) {
            add_error(result, pulse_path + " requires color and period");
            return std::nullopt;
        }
        const auto color = color_value(*color_field, pulse_path + ".color", result);
        const auto period = duration_value(*period_field, pulse_path + ".period", result);
        const auto minimum = percentage_value(*pulse, "minimum", 0.1, pulse_path, result);
        const auto maximum = percentage_value(*pulse, "maximum", 1.0, pulse_path, result);
        if (!color || !period || !minimum || !maximum || period->count() < 2) {
            if (period && period->count() < 2) {
                add_error(result, pulse_path + ".period must be at least 2ms");
            }
            return std::nullopt;
        }
        if (*minimum > *maximum) {
            add_error(result, pulse_path + ".minimum must not exceed maximum");
            return std::nullopt;
        }
        return TimelineAppearance(
            {
                {Duration::zero(), scale_brightness(*color, *minimum), Transition::linear},
                {Duration{period->count() / 2}, scale_brightness(*color, *maximum), Transition::linear},
            },
            *period,
            true);
    }

    if (const auto sequence = appearance->find("sequence"); sequence != appearance->end()) {
        const auto sequence_path = std::string(path) + ".appearance.sequence";
        if (!sequence->is_object()) {
            add_error(result, sequence_path + " must be an object");
            return std::nullopt;
        }
        reject_unknown_fields(*sequence, {"repeat", "steps"}, sequence_path, result);
        bool repeat = true;
        if (const auto repeat_field = sequence->find("repeat"); repeat_field != sequence->end()) {
            if (!repeat_field->is_boolean()) {
                add_error(result, sequence_path + ".repeat must be boolean");
                return std::nullopt;
            }
            repeat = repeat_field->get<bool>();
        }
        const auto steps = sequence->find("steps");
        if (steps == sequence->end()) {
            add_error(result, sequence_path + ".steps is required");
            return std::nullopt;
        }
        auto parsed = parse_steps(*steps, sequence_path + ".steps", 1, Transition::step, result);
        if (!parsed) {
            return std::nullopt;
        }
        return TimelineAppearance(std::move(parsed->keyframes), parsed->cycle, repeat);
    }

    if (const auto cycle = appearance->find("cycle"); cycle != appearance->end()) {
        const auto cycle_path = std::string(path) + ".appearance.cycle";
        if (!cycle->is_object()) {
            add_error(result, cycle_path + " must be an object");
            return std::nullopt;
        }
        reject_unknown_fields(*cycle, {"transition", "steps"}, cycle_path, result);
        auto transition = Transition::linear;
        if (const auto field = cycle->find("transition"); field != cycle->end()) {
            if (!field->is_string()
                || (field->get_ref<const std::string&>() != "linear"
                    && field->get_ref<const std::string&>() != "step")) {
                add_error(result, cycle_path + ".transition must be linear or step");
                return std::nullopt;
            }
            transition = field->get_ref<const std::string&>() == "linear"
                ? Transition::linear
                : Transition::step;
        }
        const auto steps = cycle->find("steps");
        if (steps == cycle->end()) {
            add_error(result, cycle_path + ".steps is required");
            return std::nullopt;
        }
        auto parsed = parse_steps(*steps, cycle_path + ".steps", 2, transition, result);
        if (!parsed) {
            return std::nullopt;
        }
        return TimelineAppearance(std::move(parsed->keyframes), parsed->cycle, true);
    }

    add_error(
        result,
        std::string(path)
            + ".appearance must define solid, blink, toggle, fade, pulse, sequence, or cycle");
    return std::nullopt;
}

[[nodiscard]] std::optional<StateValue> scalar_state_value(
    const Json& value,
    std::string_view path,
    ConfigLoadResult& result) {
    if (value.is_null()) {
        return StateValue{};
    }
    if (value.is_boolean()) {
        return StateValue{value.get<bool>()};
    }
    if (value.is_number_integer()) {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                add_error(result, std::string(path) + " integer is out of range");
                return std::nullopt;
            }
            return StateValue{static_cast<std::int64_t>(number)};
        }
        return StateValue{value.get<std::int64_t>()};
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number)) {
            add_error(result, std::string(path) + " number must be finite");
            return std::nullopt;
        }
        return StateValue{number};
    }
    if (value.is_string()) {
        return StateValue{value.get<std::string>()};
    }
    add_error(result, std::string(path) + " must be a scalar JSON value");
    return std::nullopt;
}

[[nodiscard]] std::optional<ComparisonOperation> comparison_operation(
    std::string_view name) {
    if (name == "exists") {
        return ComparisonOperation::exists;
    }
    if (name == "not_exists") {
        return ComparisonOperation::not_exists;
    }
    if (name == "equals") {
        return ComparisonOperation::equals;
    }
    if (name == "not_equals") {
        return ComparisonOperation::not_equals;
    }
    if (name == "contains") {
        return ComparisonOperation::contains;
    }
    if (name == "not_contains") {
        return ComparisonOperation::not_contains;
    }
    if (name == "greater_than") {
        return ComparisonOperation::greater_than;
    }
    if (name == "greater_or_equal") {
        return ComparisonOperation::greater_or_equal;
    }
    if (name == "less_than") {
        return ComparisonOperation::less_than;
    }
    if (name == "less_or_equal") {
        return ComparisonOperation::less_or_equal;
    }
    if (name == "between") {
        return ComparisonOperation::between;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<EvaluationCondition> parse_evaluation_condition(
    const Json& when,
    std::string_view path,
    ConfigLoadResult& result) {
    if (!when.is_object() || when.size() != 1U || !when.contains("value")
        || !when["value"].is_object() || when["value"].size() != 1U) {
        add_error(
            result,
            std::string(path) + " must contain exactly one value comparison");
        return std::nullopt;
    }

    const auto& comparison = when["value"];
    const auto field = comparison.begin();
    const auto operation = comparison_operation(field.key());
    if (!operation) {
        add_error(result, std::string(path) + ".value contains an unknown comparison");
        return std::nullopt;
    }

    EvaluationCondition condition;
    condition.operation = *operation;
    const auto operand_path = std::string(path) + ".value." + field.key();
    if (*operation == ComparisonOperation::exists
        || *operation == ComparisonOperation::not_exists) {
        if (!field.value().is_boolean() || !field.value().get<bool>()) {
            add_error(result, operand_path + " must be true");
            return std::nullopt;
        }
        return condition;
    }
    if (*operation == ComparisonOperation::between) {
        if (!field.value().is_array() || field.value().size() != 2U) {
            add_error(result, operand_path + " must contain two numeric bounds");
            return std::nullopt;
        }
        const auto lower = scalar_state_value(field.value()[0], operand_path + "[0]", result);
        const auto upper = scalar_state_value(field.value()[1], operand_path + "[1]", result);
        if (!lower || !upper) {
            return std::nullopt;
        }
        condition.expected = *lower;
        condition.upper_bound = *upper;
        return condition;
    }

    const auto operand = scalar_state_value(field.value(), operand_path, result);
    if (!operand) {
        return std::nullopt;
    }
    condition.expected = *operand;
    return condition;
}

[[nodiscard]] bool status_is_defined(
    std::string_view status,
    const std::unordered_map<std::string, TimelineAppearance>& statuses) {
    return statuses.find(std::string(status)) != statuses.end();
}

[[nodiscard]] std::optional<EvaluationPolicy> parse_evaluation_policy(
    const Json& monitor,
    std::string_view path,
    const std::unordered_map<std::string, TimelineAppearance>& statuses,
    ConfigLoadResult& result) {
    EvaluationPolicy policy;
    if (const auto status = optional_identifier(
            monitor, "transport_failure_status", path, result)) {
        policy.transport_failure_status = *status;
    }
    if (const auto status = optional_identifier(monitor, "no_match_status", path, result)) {
        policy.no_match_status = *status;
    }

    const auto evaluate = monitor.find("evaluate");
    if (evaluate == monitor.end() || !evaluate->is_array() || evaluate->empty()
        || evaluate->size() > maximum_monitor_rules) {
        add_error(
            result,
            std::string(path) + ".evaluate must contain between 1 and 32 rules");
        return std::nullopt;
    }

    for (std::size_t index = 0; index < evaluate->size(); ++index) {
        const auto& rule = (*evaluate)[index];
        const auto rule_path = std::string(path) + ".evaluate[" + std::to_string(index) + "]";
        if (!rule.is_object()) {
            add_error(result, rule_path + " must be an object");
            continue;
        }

        if (rule.contains("when")) {
            reject_unknown_fields(rule, {"when", "status"}, rule_path, result);
            const auto status = required_identifier(rule, "status", rule_path, result);
            const auto condition = parse_evaluation_condition(rule["when"], rule_path + ".when", result);
            if (status && condition) {
                policy.rules.push_back({*condition, *status});
            }
            continue;
        }
        if (rule.contains("otherwise")) {
            reject_unknown_fields(rule, {"otherwise"}, rule_path, result);
            const auto& otherwise = rule["otherwise"];
            if (!otherwise.is_object()) {
                add_error(result, rule_path + ".otherwise must be an object");
                continue;
            }
            reject_unknown_fields(otherwise, {"status"}, rule_path + ".otherwise", result);
            const auto status = required_identifier(
                otherwise, "status", rule_path + ".otherwise", result);
            if (status) {
                policy.rules.push_back({std::nullopt, *status});
            }
            continue;
        }
        add_error(result, rule_path + " must define when or otherwise");
    }

    const Evaluator evaluator;
    if (const auto error = evaluator.validate(policy)) {
        add_error(result, std::string(path) + ".evaluate: " + *error);
        return std::nullopt;
    }
    if (!status_is_defined(policy.transport_failure_status, statuses)) {
        add_error(
            result,
            std::string(path) + ".transport_failure_status is not defined in statuses");
    }
    if (!status_is_defined(policy.no_match_status, statuses)) {
        add_error(result, std::string(path) + ".no_match_status is not defined in statuses");
    }
    for (std::size_t index = 0; index < policy.rules.size(); ++index) {
        if (!status_is_defined(policy.rules[index].status, statuses)) {
            add_error(
                result,
                std::string(path) + ".evaluate[" + std::to_string(index)
                    + "] refers to an undefined status");
        }
    }
    return policy;
}

[[nodiscard]] bool valid_http_url(std::string_view url) {
    return url.size() <= maximum_url_bytes && parse_http_url(url).has_value();
}

[[nodiscard]] std::string ascii_lower(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const auto character : value) {
        lowered.push_back(character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A'))
            : character);
    }
    return lowered;
}

[[nodiscard]] bool valid_http_header_name(std::string_view name) {
    if (name.empty() || name.size() > maximum_http_header_name_bytes) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        const auto alpha_numeric = (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9');
        return alpha_numeric || character == '!' || character == '#'
            || character == '$' || character == '%' || character == '&'
            || character == static_cast<char>(0x27) || character == '*'
            || character == '+' || character == '-' || character == '.'
            || character == '^' || character == '_'
            || character == static_cast<char>(0x60) || character == '|'
            || character == '~';
    });
}

[[nodiscard]] bool valid_http_header_value(std::string_view value) {
    if (value.size() > maximum_http_header_value_bytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte != 0x7FU;
    });
}

[[nodiscard]] bool transport_owns_http_header(std::string_view lowered_name) {
    return lowered_name == "host" || lowered_name == "content-length"
        || lowered_name == "transfer-encoding" || lowered_name == "connection";
}

[[nodiscard]] bool valid_secret_name(std::string_view name) {
    return !name.empty() && name.size() <= maximum_secret_name_bytes
        && std::all_of(name.begin(), name.end(), [](char character) {
            return (character >= 'A' && character <= 'Z')
                || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '.' || character == '_' || character == '-';
        });
}

[[nodiscard]] bool valid_secret_template(std::string_view value) {
    constexpr std::string_view prefix = "${secret:";
    std::size_t offset{};
    while (true) {
        const auto begin = value.find(prefix, offset);
        if (begin == std::string_view::npos) {
            return true;
        }
        const auto name_begin = begin + prefix.size();
        const auto end = value.find('}', name_begin);
        if (end == std::string_view::npos
            || !valid_secret_name(value.substr(name_begin, end - name_begin))) {
            return false;
        }
        offset = end + 1U;
    }
}

[[nodiscard]] std::optional<HttpMethod> parse_http_method_name(std::string_view method) {
    if (method == "GET") {
        return HttpMethod::get;
    }
    if (method == "HEAD") {
        return HttpMethod::head;
    }
    if (method == "POST") {
        return HttpMethod::post;
    }
    if (method == "PUT") {
        return HttpMethod::put;
    }
    if (method == "PATCH") {
        return HttpMethod::patch;
    }
    if (method == "DELETE") {
        return HttpMethod::delete_;
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_json_pointer(std::string_view pointer) {
    if (pointer.size() > maximum_json_pointer_bytes
        || (!pointer.empty() && pointer.front() != '/')) {
        return false;
    }
    for (std::size_t index = 0; index < pointer.size(); ++index) {
        if (pointer[index] == '~') {
            if (++index >= pointer.size()
                || (pointer[index] != '0' && pointer[index] != '1')) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool valid_network_host(std::string_view host) {
    if (host.empty() || host.size() > maximum_host_bytes) {
        return false;
    }
    return std::none_of(host.begin(), host.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte <= 0x20U || byte == 0x7FU
            || character == '/' || character == '?' || character == '#'
            || character == '[' || character == ']';
    });
}

[[nodiscard]] std::optional<HttpMonitorConfig> parse_http_monitor(
    const Json& monitor,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto url = required_string(monitor, "url", path, result);
    const auto url_valid = url && valid_http_url(*url);
    if (url && !url_valid) {
        add_error(
            result,
            std::string(path)
                + ".url must be an http:// or https:// URL without credentials or a fragment");
    }

    HttpMonitorConfig config;
    if (url) {
        config.url = *url;
    }
    bool request_valid = true;
    if (const auto field = monitor.find("method"); field != monitor.end()) {
        if (!field->is_string()) {
            add_error(result, std::string(path) + ".method must be a string");
            request_valid = false;
        } else if (const auto method = parse_http_method_name(
                       field->get_ref<const std::string&>())) {
            config.method = *method;
        } else {
            add_error(
                result,
                std::string(path)
                    + ".method must be GET, HEAD, POST, PUT, PATCH, or DELETE");
            request_valid = false;
        }
    }
    if (const auto field = monitor.find("headers"); field != monitor.end()) {
        if (!field->is_object() || field->size() > maximum_http_header_count) {
            add_error(result, std::string(path) + ".headers must be an object with at most 32 fields");
            request_valid = false;
        } else {
            std::unordered_set<std::string> names;
            std::size_t total_bytes{};
            for (auto header = field->begin(); header != field->end(); ++header) {
                const auto lowered_name = ascii_lower(header.key());
                if (!valid_http_header_name(header.key())) {
                    add_error(result, std::string(path) + ".headers contains an invalid field name");
                    request_valid = false;
                    continue;
                }
                if (!names.emplace(lowered_name).second) {
                    add_error(
                        result,
                        std::string(path)
                            + ".headers contains duplicate case-insensitive field names");
                    request_valid = false;
                    continue;
                }
                if (transport_owns_http_header(lowered_name)) {
                    add_error(
                        result,
                        std::string(path) + ".headers cannot override " + header.key());
                    request_valid = false;
                    continue;
                }
                if (!header.value().is_string()
                    || !valid_http_header_value(header.value().get_ref<const std::string&>())) {
                    add_error(
                        result,
                        std::string(path) + ".headers." + header.key()
                            + " must be a string of at most 2048 bytes without control characters");
                    request_valid = false;
                    continue;
                }
                const auto value = header.value().get<std::string>();
                if (!valid_secret_template(value)) {
                    add_error(
                        result,
                        std::string(path) + ".headers." + header.key()
                            + " contains an invalid secret reference");
                    request_valid = false;
                    continue;
                }
                total_bytes += header.key().size() + value.size();
                config.headers.push_back({header.key(), value});
            }
            if (total_bytes > maximum_http_headers_bytes) {
                add_error(result, std::string(path) + ".headers exceed the 8192-byte aggregate limit");
                request_valid = false;
            }
        }
    }
    if (const auto field = monitor.find("body"); field != monitor.end()) {
        if (!field->is_string()
            || field->get_ref<const std::string&>().size() > maximum_http_request_body_bytes) {
            add_error(result, std::string(path) + ".body must be a string of at most 16384 bytes");
            request_valid = false;
        } else {
            config.body = field->get<std::string>();
        }
    }
    if (!config.body.empty()
        && (config.method == HttpMethod::get || config.method == HttpMethod::head)) {
        add_error(result, std::string(path) + ".body is not allowed with GET or HEAD");
        request_valid = false;
    }
    if (const auto field = monitor.find("timeout"); field != monitor.end()) {
        const auto timeout = bounded_duration_value(
            *field, std::string(path) + ".timeout", Duration{1}, maximum_monitor_timeout, result);
        if (timeout) {
            config.timeout = *timeout;
        }
    }
    if (monitor.contains("maximum_response_bytes")) {
        const auto maximum = bounded_size(
            monitor,
            "maximum_response_bytes",
            path,
            1U,
            maximum_monitor_response_bytes,
            result);
        if (maximum) {
            config.maximum_response_bytes = *maximum;
        }
    }

    const auto observe = monitor.find("observe");
    if (observe == monitor.end() || !observe->is_object() || observe->size() != 1U) {
        add_error(result, std::string(path) + ".observe must define exactly one observation");
        return std::nullopt;
    }
    if (observe->contains("status_code")) {
        if (!(*observe)["status_code"].is_boolean()
            || !(*observe)["status_code"].get<bool>()) {
            add_error(result, std::string(path) + ".observe.status_code must be true");
            return std::nullopt;
        }
        config.observation = HttpObservation::status_code;
    } else if (observe->contains("body")) {
        if (!(*observe)["body"].is_boolean() || !(*observe)["body"].get<bool>()) {
            add_error(result, std::string(path) + ".observe.body must be true");
            return std::nullopt;
        }
        config.observation = HttpObservation::body;
    } else if (observe->contains("json_pointer")) {
        const auto& pointer = (*observe)["json_pointer"];
        if (!pointer.is_string()
            || !valid_json_pointer(pointer.get_ref<const std::string&>())) {
            add_error(result, std::string(path) + ".observe.json_pointer is invalid");
            return std::nullopt;
        }
        config.observation = HttpObservation::json_pointer;
        config.json_pointer = pointer.get<std::string>();
    } else {
        add_error(result, std::string(path) + ".observe contains an unknown observation");
        return std::nullopt;
    }
    if (!url_valid || !request_valid) {
        return std::nullopt;
    }
    return config;
}

[[nodiscard]] std::optional<TcpConnectMonitorConfig> parse_tcp_connect_monitor(
    const Json& monitor,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto host = required_string(monitor, "host", path, result);
    const auto host_valid = host && valid_network_host(*host);
    if (host && !host_valid) {
        add_error(
            result,
            std::string(path)
                + ".host must be a hostname or unbracketed IP address of at most 253 bytes");
    }
    const auto port = bounded_size(monitor, "port", path, 1U, 65'535U, result);

    TcpConnectMonitorConfig config;
    if (host) {
        config.host = *host;
    }
    if (port) {
        config.port = static_cast<std::uint16_t>(*port);
    }
    if (const auto field = monitor.find("timeout"); field != monitor.end()) {
        const auto timeout = bounded_duration_value(
            *field,
            std::string(path) + ".timeout",
            Duration{1},
            maximum_monitor_timeout,
            result);
        if (timeout) {
            config.timeout = *timeout;
        } else {
            return std::nullopt;
        }
    }
    if (!host_valid || !port) {
        return std::nullopt;
    }
    return config;
}

[[nodiscard]] std::optional<IcmpPingMonitorConfig> parse_icmp_ping_monitor(
    const Json& monitor,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto host = required_string(monitor, "host", path, result);
    const auto host_valid = host && valid_network_host(*host);
    if (host && !host_valid) {
        add_error(
            result,
            std::string(path)
                + ".host must be a hostname or unbracketed IP address of at most 253 bytes");
    }

    IcmpPingMonitorConfig config;
    if (host) {
        config.host = *host;
    }
    if (const auto field = monitor.find("timeout"); field != monitor.end()) {
        const auto timeout = bounded_duration_value(
            *field,
            std::string(path) + ".timeout",
            Duration{1},
            maximum_monitor_timeout,
            result);
        if (timeout) {
            config.timeout = *timeout;
        } else {
            return std::nullopt;
        }
    }
    if (!host_valid) {
        return std::nullopt;
    }
    return config;
}

[[nodiscard]] std::optional<TcpExchangeMonitorConfig> parse_tcp_exchange_monitor(
    const Json& monitor,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto host = required_string(monitor, "host", path, result);
    const auto host_valid = host && valid_network_host(*host);
    if (host && !host_valid) {
        add_error(
            result,
            std::string(path)
                + ".host must be a hostname or unbracketed IP address of at most 253 bytes");
    }
    const auto port = bounded_size(monitor, "port", path, 1U, 65'535U, result);

    TcpExchangeMonitorConfig config;
    if (host) {
        config.host = *host;
    }
    if (port) {
        config.port = static_cast<std::uint16_t>(*port);
    }

    bool timeout_valid = true;
    if (const auto field = monitor.find("timeout"); field != monitor.end()) {
        const auto timeout = bounded_duration_value(
            *field,
            std::string(path) + ".timeout",
            Duration{1},
            maximum_monitor_timeout,
            result);
        if (timeout) {
            config.timeout = *timeout;
        } else {
            timeout_valid = false;
        }
    }

    bool send_valid = true;
    if (const auto field = monitor.find("send"); field != monitor.end()) {
        if (!field->is_string()
            || field->get_ref<const std::string&>().size() > maximum_tcp_send_bytes) {
            add_error(result, std::string(path) + ".send must be a string of at most 4096 bytes");
            send_valid = false;
        } else {
            config.send = field->get<std::string>();
        }
    }

    bool delimiter_valid = true;
    const auto delimiter = monitor.find("read_until");
    if (delimiter == monitor.end() || !delimiter->is_string()
        || delimiter->get_ref<const std::string&>().empty()
        || delimiter->get_ref<const std::string&>().size() > maximum_tcp_delimiter_bytes) {
        add_error(result, std::string(path) + ".read_until must contain 1-256 bytes");
        delimiter_valid = false;
    } else {
        config.read_until = delimiter->get<std::string>();
    }

    bool response_limit_valid = true;
    if (monitor.contains("maximum_response_bytes")) {
        const auto maximum = bounded_size(
            monitor,
            "maximum_response_bytes",
            path,
            1U,
            maximum_monitor_response_bytes,
            result);
        if (maximum) {
            config.maximum_response_bytes = *maximum;
        } else {
            response_limit_valid = false;
        }
    }
    if (delimiter_valid && response_limit_valid
        && config.maximum_response_bytes < config.read_until.size()) {
        add_error(
            result,
            std::string(path)
                + ".maximum_response_bytes must be at least the read_until byte length");
        response_limit_valid = false;
    }

    bool observation_valid = true;
    const auto observe = monitor.find("observe");
    if (observe == monitor.end() || !observe->is_object() || observe->size() != 1U) {
        add_error(result, std::string(path) + ".observe must define exactly one observation");
        observation_valid = false;
    } else {
        const auto observation = observe->begin();
        if (!observation.value().is_boolean() || !observation.value().get<bool>()) {
            add_error(result, std::string(path) + ".observe values must be true");
            observation_valid = false;
        } else if (observation.key() == "body") {
            config.observation = TcpExchangeObservation::body;
        } else if (observation.key() == "latency_ms") {
            config.observation = TcpExchangeObservation::latency_ms;
        } else {
            add_error(result, std::string(path) + ".observe contains an unknown observation");
            observation_valid = false;
        }
    }

    if (!host_valid || !port || !timeout_valid || !send_valid || !delimiter_valid
        || !response_limit_valid || !observation_valid) {
        return std::nullopt;
    }
    return config;
}

[[nodiscard]] std::optional<DnsMonitorConfig> parse_dns_monitor(
    const Json& monitor,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto host = required_string(monitor, "host", path, result);
    const auto host_valid = host && valid_network_host(*host);
    if (host && !host_valid) {
        add_error(
            result,
            std::string(path)
                + ".host must be a hostname or unbracketed IP address of at most 253 bytes");
    }

    DnsMonitorConfig config;
    if (host) {
        config.host = *host;
    }
    bool family_valid = true;
    if (const auto field = monitor.find("family"); field != monitor.end()) {
        if (!field->is_string()) {
            add_error(result, std::string(path) + ".family must be any, ipv4, or ipv6");
            family_valid = false;
        } else {
            const auto family = field->get_ref<const std::string&>();
            if (family == "any") {
                config.family = DnsAddressFamily::any;
            } else if (family == "ipv4") {
                config.family = DnsAddressFamily::ipv4;
            } else if (family == "ipv6") {
                config.family = DnsAddressFamily::ipv6;
            } else {
                add_error(result, std::string(path) + ".family must be any, ipv4, or ipv6");
                family_valid = false;
            }
        }
    }
    bool timeout_valid = true;
    if (const auto field = monitor.find("timeout"); field != monitor.end()) {
        const auto timeout = bounded_duration_value(
            *field,
            std::string(path) + ".timeout",
            Duration{1},
            maximum_monitor_timeout,
            result);
        if (timeout) {
            config.timeout = *timeout;
        } else {
            timeout_valid = false;
        }
    }

    const auto observe = monitor.find("observe");
    bool observation_valid = true;
    if (observe == monitor.end() || !observe->is_object() || observe->size() != 1U) {
        add_error(result, std::string(path) + ".observe must define exactly one observation");
        observation_valid = false;
    } else {
        const auto observation = observe->begin();
        if (!observation.value().is_boolean() || !observation.value().get<bool>()) {
            add_error(result, std::string(path) + ".observe values must be true");
            observation_valid = false;
        } else if (observation.key() == "addresses") {
            config.observation = DnsObservation::addresses;
        } else if (observation.key() == "address_count") {
            config.observation = DnsObservation::address_count;
        } else if (observation.key() == "latency_ms") {
            config.observation = DnsObservation::latency_ms;
        } else {
            add_error(result, std::string(path) + ".observe contains an unknown observation");
            observation_valid = false;
        }
    }
    if (!host_valid || !family_valid || !timeout_valid || !observation_valid) {
        return std::nullopt;
    }
    return config;
}

[[nodiscard]] std::optional<PullMonitorConfig> parse_pull_monitor(
    const Json& monitor,
    std::string_view path,
    const std::unordered_map<std::string, TimelineAppearance>& statuses,
    ConfigLoadResult& result) {
    if (!monitor.is_object()) {
        add_error(result, std::string(path) + " must be an object");
        return std::nullopt;
    }
    const auto id = required_identifier(monitor, "id", path, result);
    const auto type = required_string(monitor, "type", path, result);
    if (type && *type == "http") {
        reject_unknown_fields(
            monitor,
            {
                "id", "type", "url", "method", "headers", "body",
                "interval", "ttl", "timeout",
                "maximum_response_bytes", "observe", "evaluate",
                "transport_failure_status", "no_match_status",
            },
            path,
            result);
    } else if (type && *type == "tcp_connect") {
        reject_unknown_fields(
            monitor,
            {
                "id", "type", "host", "port", "interval", "ttl", "timeout",
                "evaluate", "transport_failure_status", "no_match_status",
            },
            path,
            result);
    } else if (type && *type == "icmp_ping") {
        reject_unknown_fields(
            monitor,
            {
                "id", "type", "host", "interval", "ttl", "timeout",
                "evaluate", "transport_failure_status", "no_match_status",
            },
            path,
            result);
    } else if (type && *type == "tcp_exchange") {
        reject_unknown_fields(
            monitor,
            {
                "id", "type", "host", "port", "interval", "ttl", "timeout",
                "send", "read_until", "maximum_response_bytes", "observe", "evaluate",
                "transport_failure_status", "no_match_status",
            },
            path,
            result);
    } else if (type && *type == "dns") {
        reject_unknown_fields(
            monitor,
            {
                "id", "type", "host", "family", "interval", "ttl", "timeout",
                "observe", "evaluate", "transport_failure_status", "no_match_status",
            },
            path,
            result);
    } else {
        reject_unknown_fields(
            monitor,
            {
                "id", "type", "url", "method", "headers", "body", "host", "port",
                "family", "interval", "ttl", "timeout", "send", "read_until",
                "maximum_response_bytes", "observe", "evaluate",
                "transport_failure_status", "no_match_status",
            },
            path,
            result);
        if (type) {
            add_error(
                result,
                std::string(path)
                    + ".type must be http, tcp_connect, icmp_ping, tcp_exchange, or dns");
        }
    }
    const auto interval_field = monitor.find("interval");
    if (interval_field == monitor.end()) {
        add_error(result, std::string(path) + ".interval is required");
    }
    const auto interval = interval_field == monitor.end()
        ? std::optional<Duration>{}
        : bounded_duration_value(
            *interval_field,
            std::string(path) + ".interval",
            minimum_monitor_interval,
            maximum_monitor_interval,
            result);

    std::optional<Duration> ttl;
    bool ttl_valid = true;
    if (const auto field = monitor.find("ttl"); field != monitor.end()) {
        ttl = bounded_duration_value(
            *field, std::string(path) + ".ttl", Duration{1}, maximum_monitor_ttl, result);
        ttl_valid = ttl.has_value();
    }
    const auto evaluation = parse_evaluation_policy(monitor, path, statuses, result);
    std::optional<MonitorSourceConfig> source;
    if (type && *type == "http") {
        if (const auto http = parse_http_monitor(monitor, path, result)) {
            source = *http;
        }
    } else if (type && *type == "tcp_connect") {
        if (const auto tcp = parse_tcp_connect_monitor(monitor, path, result)) {
            source = *tcp;
        }
    } else if (type && *type == "icmp_ping") {
        if (const auto ping = parse_icmp_ping_monitor(monitor, path, result)) {
            source = *ping;
        }
    } else if (type && *type == "tcp_exchange") {
        if (const auto exchange = parse_tcp_exchange_monitor(monitor, path, result)) {
            source = *exchange;
        }
    } else if (type && *type == "dns") {
        if (const auto dns = parse_dns_monitor(monitor, path, result)) {
            source = *dns;
        }
    }
    if (!id || !type || !interval || !ttl_valid || !evaluation || !source) {
        return std::nullopt;
    }

    PullMonitorConfig config;
    config.id = *id;
    config.interval = *interval;
    config.ttl = ttl;
    config.evaluation = *evaluation;
    config.source = std::move(*source);
    return config;
}

[[nodiscard]] std::optional<IndicatorConfig> parse_indicator_config(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result) {
    if (!item.is_object()) {
        add_error(result, std::string(path) + " must be an object");
        return std::nullopt;
    }
    reject_unknown_fields(
        item,
        {"id", "source", "x", "y", "width", "height"},
        path,
        result);

    const auto id = required_identifier(item, "id", path, result);
    const auto source = required_identifier(item, "source", path, result);
    const auto x = bounded_size(item, "x", path, 0, maximum_dimension, result);
    const auto y = bounded_size(item, "y", path, 0, maximum_dimension, result);
    const auto width = bounded_size(item, "width", path, 1, maximum_dimension, result);
    const auto height = bounded_size(item, "height", path, 1, maximum_dimension, result);
    if (!id || !source || !x || !y || !width || !height) {
        return std::nullopt;
    }
    if (*x >= display.width || *width > display.width - *x
        || *y >= display.height || *height > display.height - *y) {
        add_error(result, std::string(path) + " extends outside the configured display");
        return std::nullopt;
    }
    return IndicatorConfig{*id, *source, *x, *y, *width, *height};
}

struct LayoutBounds {
    std::size_t x{};
    std::size_t y{};
    std::size_t width{};
    std::size_t height{};
};

[[nodiscard]] std::optional<LayoutBounds> parse_layout_bounds(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result,
    const std::optional<LayoutBounds>& assigned) {
    if (assigned) {
        return assigned;
    }
    const auto x = bounded_size(item, "x", path, 0, maximum_dimension, result);
    const auto y = bounded_size(item, "y", path, 0, maximum_dimension, result);
    const auto width = bounded_size(item, "width", path, 1, maximum_dimension, result);
    const auto height = bounded_size(item, "height", path, 1, maximum_dimension, result);
    if (!x || !y || !width || !height) {
        return std::nullopt;
    }
    if (*x >= display.width || *width > display.width - *x
        || *y >= display.height || *height > display.height - *y) {
        add_error(result, std::string(path) + " extends outside the configured display");
        return std::nullopt;
    }
    return LayoutBounds{*x, *y, *width, *height};
}

[[nodiscard]] std::optional<double> optional_finite_number(
    const Json& item,
    std::string_view key,
    double default_value,
    std::string_view path,
    ConfigLoadResult& result) {
    const auto field = item.find(key);
    if (field == item.end()) {
        return default_value;
    }
    if (!field->is_number()) {
        add_error(result, std::string(path) + "." + std::string(key) + " must be a number");
        return std::nullopt;
    }
    const auto value = field->get<double>();
    if (!std::isfinite(value)) {
        add_error(result, std::string(path) + "." + std::string(key) + " must be finite");
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool layout_bounds_are_valid(
    const LayoutBounds& bounds,
    std::string_view path,
    ConfigLoadResult& result) {
    if (bounds.width == 0U || bounds.height == 0U) {
        add_error(result, std::string(path) + " resolved to a zero-sized rectangle");
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<LayoutClockConfig> parse_layout_clock_config(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result,
    const std::optional<LayoutBounds>& assigned = std::nullopt) {
    if (assigned) {
        reject_unknown_fields(
            item,
            {"id", "type", "timezone", "color", "size", "weight"},
            path,
            result);
    } else {
        reject_unknown_fields(
            item,
            {"id", "type", "x", "y", "width", "height", "timezone", "color"},
            path,
            result);
    }
    const auto id = required_identifier(item, "id", path, result);
    const auto bounds = parse_layout_bounds(item, path, display, result, assigned);
    const auto timezone = required_string(item, "timezone", path, result);
    if (!id || !bounds || !timezone || !layout_bounds_are_valid(*bounds, path, result)) {
        return std::nullopt;
    }
    if (bounds->width < 15U || bounds->height < 7U) {
        add_error(result, std::string(path) + " requires bounds of at least 15x7 pixels");
        return std::nullopt;
    }

    LayoutClockConfig clock;
    clock.id = *id;
    clock.x = bounds->x;
    clock.y = bounds->y;
    clock.width = bounds->width;
    clock.height = bounds->height;
    if (*timezone == "local") {
        clock.timezone = ClockTimeZone::local;
        clock.color = {0x00, 0xB0, 0xFF};
    } else if (*timezone == "utc") {
        clock.timezone = ClockTimeZone::utc;
        clock.color = {0xFF, 0xD6, 0x00};
    } else {
        add_error(result, std::string(path) + ".timezone must be local or utc");
        return std::nullopt;
    }
    if (const auto field = item.find("color"); field != item.end()) {
        const auto parsed = color_value(*field, std::string(path) + ".color", result);
        if (!parsed) {
            return std::nullopt;
        }
        clock.color = *parsed;
    }
    return clock;
}

[[nodiscard]] std::optional<IndicatorConfig> parse_layout_indicator_config(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result,
    const std::optional<LayoutBounds>& assigned) {
    if (assigned) {
        reject_unknown_fields(
            item,
            {"id", "type", "source", "size", "weight"},
            path,
            result);
    } else {
        reject_unknown_fields(
            item,
            {"id", "type", "source", "x", "y", "width", "height"},
            path,
            result);
    }
    const auto id = required_identifier(item, "id", path, result);
    const auto source = required_identifier(item, "source", path, result);
    const auto bounds = parse_layout_bounds(item, path, display, result, assigned);
    if (!id || !source || !bounds || !layout_bounds_are_valid(*bounds, path, result)) {
        return std::nullopt;
    }
    return IndicatorConfig{
        *id, *source, bounds->x, bounds->y, bounds->width, bounds->height};
}

[[nodiscard]] std::optional<LayoutBarConfig> parse_layout_bar_config(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result,
    const std::optional<LayoutBounds>& assigned) {
    if (assigned) {
        reject_unknown_fields(
            item,
            {"id", "type", "source", "direction", "minimum", "maximum",
             "track_color", "size", "weight"},
            path,
            result);
    } else {
        reject_unknown_fields(
            item,
            {"id", "type", "source", "x", "y", "width", "height", "direction",
             "minimum", "maximum", "track_color"},
            path,
            result);
    }
    const auto id = required_identifier(item, "id", path, result);
    const auto source = required_identifier(item, "source", path, result);
    const auto bounds = parse_layout_bounds(item, path, display, result, assigned);
    const auto minimum = optional_finite_number(item, "minimum", 0.0, path, result);
    const auto maximum = optional_finite_number(item, "maximum", 100.0, path, result);
    if (!id || !source || !bounds || !minimum || !maximum
        || !layout_bounds_are_valid(*bounds, path, result)) {
        return std::nullopt;
    }
    if (*maximum <= *minimum) {
        add_error(result, std::string(path) + ".maximum must be greater than .minimum");
        return std::nullopt;
    }

    LayoutBarConfig bar;
    bar.id = *id;
    bar.source = *source;
    bar.x = bounds->x;
    bar.y = bounds->y;
    bar.width = bounds->width;
    bar.height = bounds->height;
    bar.minimum = *minimum;
    bar.maximum = *maximum;
    bar.track_color = display.background;
    if (const auto field = item.find("direction"); field != item.end()) {
        if (!field->is_string()) {
            add_error(result, std::string(path) + ".direction must be a string");
            return std::nullopt;
        }
        const auto& direction = field->get_ref<const std::string&>();
        if (direction == "right") {
            bar.direction = BarDirection::right;
        } else if (direction == "left") {
            bar.direction = BarDirection::left;
        } else if (direction == "up") {
            bar.direction = BarDirection::up;
        } else if (direction == "down") {
            bar.direction = BarDirection::down;
        } else {
            add_error(result, std::string(path) + ".direction must be right, left, up, or down");
            return std::nullopt;
        }
    }
    if (const auto field = item.find("track_color"); field != item.end()) {
        const auto color = color_value(*field, std::string(path) + ".track_color", result);
        if (!color) {
            return std::nullopt;
        }
        bar.track_color = *color;
    }
    return bar;
}

[[nodiscard]] std::optional<LayoutStatusGridConfig> parse_layout_status_grid_config(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result,
    const std::optional<LayoutBounds>& assigned) {
    if (assigned) {
        reject_unknown_fields(
            item,
            {"id", "type", "sources", "columns", "gap", "size", "weight"},
            path,
            result);
    } else {
        reject_unknown_fields(
            item,
            {"id", "type", "sources", "columns", "gap", "x", "y", "width", "height"},
            path,
            result);
    }
    const auto id = required_identifier(item, "id", path, result);
    const auto bounds = parse_layout_bounds(item, path, display, result, assigned);
    const auto sources_field = item.find("sources");
    if (sources_field == item.end() || !sources_field->is_array()
        || sources_field->empty() || sources_field->size() > maximum_layout_widgets) {
        add_error(result, std::string(path) + ".sources must contain between 1 and 1024 identifiers");
        return std::nullopt;
    }
    const auto columns = bounded_size(
        item, "columns", path, 1, sources_field->size(), result);
    std::size_t gap{};
    if (item.contains("gap")) {
        const auto parsed = bounded_size(item, "gap", path, 0, maximum_dimension, result);
        if (!parsed) {
            return std::nullopt;
        }
        gap = *parsed;
    }
    if (!id || !bounds || !columns || !layout_bounds_are_valid(*bounds, path, result)) {
        return std::nullopt;
    }

    LayoutStatusGridConfig grid;
    grid.id = *id;
    grid.x = bounds->x;
    grid.y = bounds->y;
    grid.width = bounds->width;
    grid.height = bounds->height;
    grid.columns = *columns;
    grid.gap = gap;
    std::unordered_set<std::string> sources;
    bool valid = true;
    for (std::size_t index = 0; index < sources_field->size(); ++index) {
        const auto source_path = std::string(path) + ".sources[" + std::to_string(index) + "]";
        if (!(*sources_field)[index].is_string()
            || !is_valid_identifier((*sources_field)[index].get_ref<const std::string&>())) {
            add_error(result, source_path + " must be a valid identifier");
            valid = false;
            continue;
        }
        auto source = (*sources_field)[index].get<std::string>();
        if (!sources.insert(source).second) {
            add_error(result, source_path + " duplicates an earlier grid source");
            valid = false;
            continue;
        }
        grid.sources.push_back(std::move(source));
    }
    const auto rows = (sources_field->size() + *columns - 1U) / *columns;
    const auto horizontal_gaps = gap * (*columns - 1U);
    const auto vertical_gaps = gap * (rows - 1U);
    if (horizontal_gaps > bounds->width || bounds->width - horizontal_gaps < *columns
        || vertical_gaps > bounds->height || bounds->height - vertical_gaps < rows) {
        add_error(result, std::string(path) + " bounds cannot fit its grid cells and gaps");
        valid = false;
    }
    if (!valid) {
        return std::nullopt;
    }
    return grid;
}

[[nodiscard]] std::optional<LayoutBitmapConfig> parse_layout_bitmap_config(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result,
    const std::optional<LayoutBounds>& assigned) {
    if (assigned) {
        reject_unknown_fields(
            item,
            {"id", "type", "palette", "pixels", "size", "weight"},
            path,
            result);
    } else {
        reject_unknown_fields(
            item,
            {"id", "type", "palette", "pixels", "x", "y", "width", "height"},
            path,
            result);
    }
    const auto id = required_identifier(item, "id", path, result);
    const auto bounds = parse_layout_bounds(item, path, display, result, assigned);
    const auto palette = item.find("palette");
    const auto pixels = item.find("pixels");
    if (!id || !bounds || !layout_bounds_are_valid(*bounds, path, result)) {
        return std::nullopt;
    }
    if (palette == item.end() || !palette->is_object() || palette->empty()
        || palette->size() > maximum_bitmap_palette_entries) {
        add_error(result, std::string(path) + ".palette must contain between 1 and 32 entries");
        return std::nullopt;
    }
    LayoutBitmapConfig bitmap;
    bitmap.id = *id;
    bitmap.x = bounds->x;
    bitmap.y = bounds->y;
    bitmap.width = bounds->width;
    bitmap.height = bounds->height;
    bool valid = true;
    for (auto entry = palette->begin(); entry != palette->end(); ++entry) {
        if (entry.key().size() != 1U
            || static_cast<unsigned char>(entry.key().front()) < 0x20U
            || static_cast<unsigned char>(entry.key().front()) > 0x7EU) {
            add_error(result, std::string(path) + ".palette keys must be one printable ASCII character");
            valid = false;
            continue;
        }
        const auto color = color_value(
            entry.value(), std::string(path) + ".palette." + entry.key(), result);
        if (color) {
            bitmap.palette.emplace(entry.key().front(), *color);
        } else {
            valid = false;
        }
    }
    if (pixels == item.end() || !pixels->is_array() || pixels->size() != bounds->height) {
        add_error(result, std::string(path) + ".pixels must match the resolved widget height");
        return std::nullopt;
    }
    for (std::size_t row = 0; row < pixels->size(); ++row) {
        const auto row_path = std::string(path) + ".pixels[" + std::to_string(row) + "]";
        if (!(*pixels)[row].is_string()) {
            add_error(result, row_path + " must be a string");
            valid = false;
            continue;
        }
        const auto value = (*pixels)[row].get<std::string>();
        if (value.size() != bounds->width) {
            add_error(result, row_path + " must match the resolved widget width");
            valid = false;
        } else if (std::any_of(value.begin(), value.end(), [&bitmap](char character) {
                       return !bitmap.palette.contains(character);
                   })) {
            add_error(result, row_path + " uses a character missing from palette");
            valid = false;
        }
        bitmap.pixels.push_back(value);
    }
    if (!valid) {
        return std::nullopt;
    }
    return bitmap;
}

[[nodiscard]] std::optional<LayoutWidgetConfig> parse_layout_widget(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result,
    const std::optional<LayoutBounds>& assigned = std::nullopt) {
    if (!item.is_object()) {
        add_error(result, std::string(path) + " must be an object");
        return std::nullopt;
    }
    const auto type = required_string(item, "type", path, result);
    if (!type) {
        return std::nullopt;
    }
    if (*type == "indicator") {
        if (auto indicator = parse_layout_indicator_config(
                item, path, display, result, assigned)) {
            return LayoutWidgetConfig{std::move(*indicator)};
        }
        return std::nullopt;
    }
    if (*type == "clock") {
        if (auto clock = parse_layout_clock_config(item, path, display, result, assigned)) {
            return LayoutWidgetConfig{std::move(*clock)};
        }
        return std::nullopt;
    }
    if (*type == "bar") {
        if (auto bar = parse_layout_bar_config(item, path, display, result, assigned)) {
            return LayoutWidgetConfig{std::move(*bar)};
        }
        return std::nullopt;
    }
    if (*type == "status_grid") {
        if (auto grid = parse_layout_status_grid_config(
                item, path, display, result, assigned)) {
            return LayoutWidgetConfig{std::move(*grid)};
        }
        return std::nullopt;
    }
    if (*type == "bitmap") {
        if (auto bitmap = parse_layout_bitmap_config(item, path, display, result, assigned)) {
            return LayoutWidgetConfig{std::move(*bitmap)};
        }
        return std::nullopt;
    }
    reject_unknown_fields(
        item,
        {"id", "type", "source", "sources", "x", "y", "width", "height",
         "timezone", "color", "direction", "minimum", "maximum", "track_color",
         "columns", "gap", "palette", "pixels", "size", "weight"},
        path,
        result);
    add_error(
        result,
        std::string(path)
            + ".type must be indicator, clock, bar, status_grid, or bitmap");
    return std::nullopt;
}

struct LayoutParseState {
    std::size_t nodes{};
    std::unordered_set<std::string> ids;
};

[[nodiscard]] bool parse_layout_node(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    const LayoutBounds& bounds,
    std::size_t depth,
    LayoutParseState& state,
    LayoutCardConfig& content,
    ConfigLoadResult& result) {
    if (!item.is_object()) {
        add_error(result, std::string(path) + " must be an object");
        return false;
    }
    ++state.nodes;
    if (state.nodes > maximum_layout_nodes) {
        add_error(result, std::string(path) + " exceeds the layout limit of 2048 nodes");
        return false;
    }
    if (depth > maximum_layout_depth) {
        add_error(result, std::string(path) + " exceeds the layout nesting limit of 16");
        return false;
    }
    const auto type = required_string(item, "type", path, result);
    if (!type) {
        return false;
    }
    if (*type != "row" && *type != "column") {
        auto widget = parse_layout_widget(item, path, display, result, bounds);
        if (!widget) {
            return false;
        }
        const auto& widget_id = std::visit(
            [](const auto& value) -> const std::string& { return value.id; }, *widget);
        if (!state.ids.insert(widget_id).second) {
            add_error(result, std::string(path) + ".id duplicates an earlier widget on this card");
            return false;
        }
        content.widgets.push_back(std::move(*widget));
        return true;
    }

    reject_unknown_fields(
        item, {"type", "gap", "children", "size", "weight"}, path, result);
    const auto children = item.find("children");
    if (children == item.end() || !children->is_array() || children->empty()
        || children->size() > maximum_layout_widgets) {
        add_error(result, std::string(path) + ".children must contain between 1 and 1024 nodes");
        return false;
    }
    std::size_t gap{};
    if (item.contains("gap")) {
        const auto parsed = bounded_size(item, "gap", path, 0, maximum_dimension, result);
        if (!parsed) {
            return false;
        }
        gap = *parsed;
    }
    const auto extent = *type == "row" ? bounds.width : bounds.height;
    const auto total_gaps = gap * (children->size() - 1U);
    if (total_gaps > extent) {
        add_error(result, std::string(path) + " gaps exceed the available split extent");
        return false;
    }

    struct Allocation {
        bool fixed{};
        std::size_t value{};
        std::size_t resolved{};
    };
    std::vector<Allocation> allocations;
    allocations.reserve(children->size());
    std::size_t fixed_total{};
    std::size_t weighted_count{};
    std::uint64_t total_weight{};
    bool valid = true;
    for (std::size_t index = 0; index < children->size(); ++index) {
        const auto& child = (*children)[index];
        const auto child_path = std::string(path) + ".children[" + std::to_string(index) + "]";
        if (!child.is_object()) {
            add_error(result, child_path + " must be an object");
            allocations.push_back({});
            valid = false;
            continue;
        }
        const bool has_size = child.contains("size");
        const bool has_weight = child.contains("weight");
        if (has_size && has_weight) {
            add_error(result, child_path + " cannot specify both size and weight");
            allocations.push_back({});
            valid = false;
            continue;
        }
        if (has_size) {
            const auto value = bounded_size(
                child, "size", child_path, 1, maximum_dimension, result);
            if (!value) {
                allocations.push_back({});
                valid = false;
                continue;
            }
            fixed_total += *value;
            allocations.push_back({true, *value, *value});
        } else {
            std::size_t weight = 1U;
            if (has_weight) {
                const auto value = bounded_size(
                    child, "weight", child_path, 1, maximum_layout_weight, result);
                if (!value) {
                    allocations.push_back({});
                    valid = false;
                    continue;
                }
                weight = *value;
            }
            ++weighted_count;
            total_weight += weight;
            allocations.push_back({false, weight, 0U});
        }
    }
    if (!valid) {
        return false;
    }
    if (fixed_total + total_gaps > extent) {
        add_error(result, std::string(path) + " fixed sizes and gaps exceed the available split extent");
        return false;
    }
    const auto weighted_extent = extent - fixed_total - total_gaps;
    if (weighted_count == 0U && weighted_extent != 0U) {
        add_error(result, std::string(path) + " fixed children and gaps must consume the split extent");
        return false;
    }
    if (weighted_count > 0U && weighted_extent < weighted_count) {
        add_error(result, std::string(path) + " cannot allocate at least one pixel to each weighted child");
        return false;
    }
    std::uint64_t cumulative_weight{};
    std::size_t previous_extent{};
    for (auto& allocation : allocations) {
        if (allocation.fixed) {
            continue;
        }
        cumulative_weight += allocation.value;
        const auto next_extent = static_cast<std::size_t>(
            static_cast<std::uint64_t>(weighted_extent) * cumulative_weight
            / total_weight);
        allocation.resolved = next_extent - previous_extent;
        previous_extent = next_extent;
        if (allocation.resolved == 0U) {
            add_error(result, std::string(path) + " weight rounding produced a zero-sized child");
            return false;
        }
    }

    std::size_t cursor = *type == "row" ? bounds.x : bounds.y;
    for (std::size_t index = 0; index < children->size(); ++index) {
        auto child_bounds = bounds;
        if (*type == "row") {
            child_bounds.x = cursor;
            child_bounds.width = allocations[index].resolved;
        } else {
            child_bounds.y = cursor;
            child_bounds.height = allocations[index].resolved;
        }
        const auto child_path = std::string(path) + ".children[" + std::to_string(index) + "]";
        if (!parse_layout_node(
                (*children)[index],
                child_path,
                display,
                child_bounds,
                depth + 1U,
                state,
                content,
                result)) {
            valid = false;
        }
        cursor += allocations[index].resolved + gap;
    }
    return valid;
}

[[nodiscard]] std::optional<CardTransitionConfig> parse_card_transition(
    const Json& card,
    std::string_view path,
    ConfigLoadResult& result) {
    CardTransitionConfig transition;
    const auto field = card.find("transition");
    if (field == card.end()) {
        return transition;
    }
    const auto transition_path = std::string(path) + ".transition";
    if (!field->is_object()) {
        add_error(result, transition_path + " must be an object");
        return std::nullopt;
    }
    reject_unknown_fields(*field, {"type", "duration"}, transition_path, result);
    const auto type = required_string(*field, "type", transition_path, result);
    if (!type) {
        return std::nullopt;
    }
    if (*type == "instant") {
        if (field->contains("duration")) {
            add_error(result, transition_path + ".duration is not allowed for instant");
            return std::nullopt;
        }
        return transition;
    }
    if (*type == "fade") {
        transition.type = CardTransition::fade;
    } else if (*type == "slide_left") {
        transition.type = CardTransition::slide_left;
    } else if (*type == "slide_right") {
        transition.type = CardTransition::slide_right;
    } else if (*type == "slide_up") {
        transition.type = CardTransition::slide_up;
    } else if (*type == "slide_down") {
        transition.type = CardTransition::slide_down;
    } else {
        add_error(
            result,
            transition_path
                + ".type must be instant, fade, slide_left, slide_right, slide_up, or slide_down");
        return std::nullopt;
    }
    const auto duration = field->find("duration");
    if (duration == field->end()) {
        add_error(result, transition_path + ".duration is required");
        return std::nullopt;
    }
    const auto parsed = bounded_duration_value(
        *duration,
        transition_path + ".duration",
        Duration{1},
        maximum_card_transition,
        result);
    if (!parsed) {
        return std::nullopt;
    }
    transition.duration = *parsed;
    return transition;
}

[[nodiscard]] std::optional<CardConfig> parse_card(
    const Json& item,
    std::string_view path,
    const DisplayConfig& display,
    ConfigLoadResult& result) {
    if (!item.is_object()) {
        add_error(result, std::string(path) + " must be an object");
        return std::nullopt;
    }
    const auto id = required_identifier(item, "id", path, result);
    const auto type = required_string(item, "type", path, result);
    const auto hold_field = item.find("hold");
    if (hold_field == item.end()) {
        add_error(result, std::string(path) + ".hold is required");
    }
    const auto hold = hold_field == item.end()
        ? std::optional<Duration>{}
        : bounded_duration_value(
            *hold_field,
            std::string(path) + ".hold",
            minimum_card_hold,
            maximum_card_hold,
            result);
    const auto transition = parse_card_transition(item, path, result);
    if (!id || !type || !hold || !transition) {
        return std::nullopt;
    }

    CardConfig card;
    card.id = *id;
    card.hold = *hold;
    card.transition = *transition;
    if (*type == "bitmap") {
        reject_unknown_fields(
            item,
            {"id", "type", "hold", "transition", "palette", "pixels"},
            path,
            result);
        const auto palette = item.find("palette");
        const auto pixels = item.find("pixels");
        if (palette == item.end() || !palette->is_object() || palette->empty()
            || palette->size() > maximum_bitmap_palette_entries) {
            add_error(
                result,
                std::string(path) + ".palette must contain between 1 and 32 entries");
            return std::nullopt;
        }
        BitmapCardConfig bitmap;
        bool valid = true;
        for (auto entry = palette->begin(); entry != palette->end(); ++entry) {
            if (entry.key().size() != 1U
                || static_cast<unsigned char>(entry.key().front()) < 0x20U
                || static_cast<unsigned char>(entry.key().front()) > 0x7EU) {
                add_error(
                    result,
                    std::string(path)
                        + ".palette keys must be one printable ASCII character");
                valid = false;
                continue;
            }
            const auto color = color_value(
                entry.value(),
                std::string(path) + ".palette." + entry.key(),
                result);
            if (color) {
                bitmap.palette.emplace(entry.key().front(), *color);
            } else {
                valid = false;
            }
        }
        if (pixels == item.end() || !pixels->is_array()
            || pixels->size() != display.height) {
            add_error(
                result,
                std::string(path) + ".pixels must contain exactly "
                    + std::to_string(display.height) + " rows");
            return std::nullopt;
        }
        for (std::size_t row = 0; row < pixels->size(); ++row) {
            const auto row_path =
                std::string(path) + ".pixels[" + std::to_string(row) + "]";
            if (!(*pixels)[row].is_string()) {
                add_error(result, row_path + " must be a string");
                valid = false;
                continue;
            }
            const auto value = (*pixels)[row].get<std::string>();
            if (value.size() != display.width) {
                add_error(
                    result,
                    row_path + " must contain exactly "
                        + std::to_string(display.width) + " palette characters");
                valid = false;
                continue;
            }
            if (std::any_of(value.begin(), value.end(), [&bitmap](char character) {
                    return !bitmap.palette.contains(character);
                })) {
                add_error(result, row_path + " uses a character missing from palette");
                valid = false;
            }
            bitmap.pixels.push_back(value);
        }
        if (!valid) {
            return std::nullopt;
        }
        card.content = std::move(bitmap);
        return card;
    }

    if (*type == "clock") {
        reject_unknown_fields(
            item,
            {"id", "type", "hold", "transition", "local_color", "utc_color"},
            path,
            result);
        if (display.width < 15U || display.height < 15U) {
            add_error(
                result,
                std::string(path) + " requires a display of at least 15x15 pixels");
            return std::nullopt;
        }
        ClockCardConfig clock;
        bool valid = true;
        if (const auto field = item.find("local_color"); field != item.end()) {
            if (const auto color = color_value(
                    *field, std::string(path) + ".local_color", result)) {
                clock.local_color = *color;
            } else {
                valid = false;
            }
        }
        if (const auto field = item.find("utc_color"); field != item.end()) {
            if (const auto color = color_value(
                    *field, std::string(path) + ".utc_color", result)) {
                clock.utc_color = *color;
            } else {
                valid = false;
            }
        }
        if (!valid) {
            return std::nullopt;
        }
        card.content = clock;
        return card;
    }

    if (*type == "indicators") {
        reject_unknown_fields(
            item,
            {"id", "type", "hold", "transition", "indicators"},
            path,
            result);
        const auto indicators = item.find("indicators");
        if (indicators == item.end() || !indicators->is_array()
            || indicators->empty() || indicators->size() > maximum_indicators) {
            add_error(
                result,
                std::string(path)
                    + ".indicators must contain between 1 and 1024 entries");
            return std::nullopt;
        }
        IndicatorCardConfig content;
        std::unordered_set<std::string> ids;
        for (std::size_t index = 0; index < indicators->size(); ++index) {
            const auto indicator_path = std::string(path) + ".indicators["
                + std::to_string(index) + "]";
            auto indicator = parse_indicator_config(
                (*indicators)[index], indicator_path, display, result);
            if (!indicator) {
                continue;
            }
            if (!ids.insert(indicator->id).second) {
                add_error(
                    result,
                    indicator_path + ".id duplicates an earlier indicator on this card");
                continue;
            }
            content.indicators.push_back(std::move(*indicator));
        }
        if (content.indicators.size() != indicators->size()) {
            return std::nullopt;
        }
        card.content = std::move(content);
        return card;
    }

    if (*type == "layout") {
        reject_unknown_fields(
            item,
            {"id", "type", "hold", "transition", "widgets", "root"},
            path,
            result);
        const auto widgets = item.find("widgets");
        const auto root = item.find("root");
        if ((widgets == item.end()) == (root == item.end())) {
            add_error(
                result,
                std::string(path) + " must contain exactly one of widgets or root");
            return std::nullopt;
        }
        LayoutCardConfig content;
        if (widgets != item.end()) {
            if (!widgets->is_array() || widgets->empty()
                || widgets->size() > maximum_layout_widgets) {
                add_error(
                    result,
                    std::string(path)
                        + ".widgets must contain between 1 and 1024 entries");
                return std::nullopt;
            }
            std::unordered_set<std::string> ids;
            for (std::size_t index = 0; index < widgets->size(); ++index) {
                const auto widget_path = std::string(path) + ".widgets["
                    + std::to_string(index) + "]";
                auto widget = parse_layout_widget(
                    (*widgets)[index], widget_path, display, result);
                if (!widget) {
                    continue;
                }
                const auto& widget_id = std::visit(
                    [](const auto& value) -> const std::string& { return value.id; },
                    *widget);
                if (!ids.insert(widget_id).second) {
                    add_error(
                        result,
                        widget_path + ".id duplicates an earlier widget on this card");
                    continue;
                }
                content.widgets.push_back(std::move(*widget));
            }
            if (content.widgets.size() != widgets->size()) {
                return std::nullopt;
            }
        } else {
            if (!root->is_object()) {
                add_error(result, std::string(path) + ".root must be an object");
                return std::nullopt;
            }
            if (root->contains("size") || root->contains("weight")) {
                add_error(
                    result,
                    std::string(path) + ".root cannot specify size or weight");
                return std::nullopt;
            }
            LayoutParseState state;
            if (!parse_layout_node(
                    *root,
                    std::string(path) + ".root",
                    display,
                    LayoutBounds{0U, 0U, display.width, display.height},
                    0U,
                    state,
                    content,
                    result)) {
                return std::nullopt;
            }
            if (content.widgets.empty() || content.widgets.size() > maximum_layout_widgets) {
                add_error(
                    result,
                    std::string(path) + ".root must resolve to between 1 and 1024 widgets");
                return std::nullopt;
            }
        }
        card.content = std::move(content);
        return card;
    }

    reject_unknown_fields(
        item,
        {
            "id", "type", "hold", "transition", "palette", "pixels",
            "local_color", "utc_color", "indicators", "widgets", "root",
        },
        path,
        result);
    add_error(
        result,
        std::string(path) + ".type must be bitmap, clock, indicators, or layout");
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
            {"schema_version", "display", "statuses", "monitors", "indicators", "cards"},
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
                if (!is_valid_identifier(status.key())) {
                    add_error(
                        result,
                        path_text
                            + " name must contain 1-64 letters, digits, dots, underscores, or hyphens");
                    continue;
                }
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

        if (const auto monitors = root.find("monitors"); monitors != root.end()) {
            if (!monitors->is_array() || monitors->size() > maximum_monitors) {
                add_error(result, "monitors must be an array with no more than 256 entries");
            } else {
                std::unordered_set<std::string> ids;
                for (std::size_t index = 0; index < monitors->size(); ++index) {
                    const auto path_text = std::string("monitors[")
                        + std::to_string(index) + "]";
                    auto monitor = parse_pull_monitor(
                        (*monitors)[index], path_text, config.statuses, result);
                    if (!monitor) {
                        continue;
                    }
                    if (!ids.insert(monitor->id).second) {
                        add_error(result, path_text + ".id duplicates an earlier monitor");
                        continue;
                    }
                    config.monitors.push_back(std::move(*monitor));
                }
            }
        }

        const auto indicators = root.find("indicators");
        const auto cards = root.find("cards");
        if ((indicators == root.end()) == (cards == root.end())) {
            add_error(result, "configuration must define exactly one of indicators or cards");
        } else if (indicators != root.end()
            && (!indicators->is_array() || indicators->empty())) {
            add_error(result, "indicators must be a non-empty array");
        } else if (indicators != root.end()
            && indicators->size() > maximum_indicators) {
            add_error(result, "indicators exceeds the limit of 1024 entries");
        } else if (indicators != root.end()) {
            std::unordered_set<std::string> ids;
            for (std::size_t index = 0; index < indicators->size(); ++index) {
                const auto path_text = std::string("indicators[") + std::to_string(index) + "]";
                auto indicator = parse_indicator_config(
                    (*indicators)[index], path_text, config.display, result);
                if (!indicator) {
                    continue;
                }
                if (!ids.insert(indicator->id).second) {
                    add_error(result, path_text + ".id duplicates an earlier indicator");
                    continue;
                }
                config.indicators.push_back(std::move(*indicator));
            }
        } else if (!cards->is_array() || cards->empty()
            || cards->size() > maximum_cards) {
            add_error(result, "cards must be an array with between 1 and 32 entries");
        } else {
            std::unordered_set<std::string> ids;
            std::size_t total_widgets{};
            for (std::size_t index = 0; index < cards->size(); ++index) {
                const auto path_text =
                    std::string("cards[") + std::to_string(index) + "]";
                auto card = parse_card(
                    (*cards)[index], path_text, config.display, result);
                if (!card) {
                    continue;
                }
                if (!ids.insert(card->id).second) {
                    add_error(result, path_text + ".id duplicates an earlier card");
                    continue;
                }
                if (const auto* content =
                        std::get_if<IndicatorCardConfig>(&card->content)) {
                    total_widgets += content->indicators.size();
                } else if (const auto* layout_content =
                               std::get_if<LayoutCardConfig>(&card->content)) {
                    total_widgets += layout_content->widgets.size();
                }
                if (total_widgets > maximum_layout_widgets) {
                    add_error(
                        result,
                        "cards contain more than 1024 widgets in total");
                    continue;
                }
                config.cards.push_back(std::move(*card));
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
