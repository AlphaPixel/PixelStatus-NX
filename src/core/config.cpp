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
constexpr std::size_t maximum_statuses = 256U;
constexpr std::size_t maximum_monitors = 256U;
constexpr Duration maximum_appearance_duration = std::chrono::hours(24);
constexpr Duration minimum_monitor_interval = std::chrono::seconds(1);
constexpr Duration maximum_monitor_interval = std::chrono::hours(24);
constexpr Duration maximum_monitor_ttl = std::chrono::hours(24 * 7);
constexpr Duration maximum_monitor_timeout = std::chrono::seconds(30);
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
                std::string(path) + ".type must be http, tcp_connect, tcp_exchange, or dns");
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
            {"schema_version", "display", "statuses", "monitors", "indicators"},
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
                const auto id = required_identifier(item, "id", path_text, result);
                const auto source = required_identifier(item, "source", path_text, result);
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
