#include "pixelstatus/evaluator.hpp"

#include "pixelstatus/validation.hpp"

#include <cmath>
#include <optional>
#include <variant>

namespace pixelstatus {
namespace {

std::optional<long double> number_value(const StateValue& value) {
    if (const auto integer = std::get_if<std::int64_t>(&value)) {
        return static_cast<long double>(*integer);
    }
    if (const auto floating = std::get_if<double>(&value)) {
        if (std::isfinite(*floating)) {
            return static_cast<long double>(*floating);
        }
    }
    return std::nullopt;
}

bool is_number(const StateValue& value) {
    return number_value(value).has_value();
}

bool values_equal(const StateValue& actual, const StateValue& expected) {
    if (is_number(actual) && is_number(expected)) {
        return *number_value(actual) == *number_value(expected);
    }
    return actual == expected;
}

bool condition_matches(const StateValue& actual, const EvaluationCondition& condition) {
    const auto actual_number = number_value(actual);
    const auto expected_number = number_value(condition.expected);

    switch (condition.operation) {
        case ComparisonOperation::exists:
            return !std::holds_alternative<std::monostate>(actual);
        case ComparisonOperation::not_exists:
            return std::holds_alternative<std::monostate>(actual);
        case ComparisonOperation::equals:
            return values_equal(actual, condition.expected);
        case ComparisonOperation::not_equals:
            return !values_equal(actual, condition.expected);
        case ComparisonOperation::contains: {
            const auto text = std::get_if<std::string>(&actual);
            const auto expected = std::get_if<std::string>(&condition.expected);
            return text && expected && text->find(*expected) != std::string::npos;
        }
        case ComparisonOperation::not_contains: {
            const auto text = std::get_if<std::string>(&actual);
            const auto expected = std::get_if<std::string>(&condition.expected);
            return text && expected && text->find(*expected) == std::string::npos;
        }
        case ComparisonOperation::greater_than:
            return actual_number && expected_number && *actual_number > *expected_number;
        case ComparisonOperation::greater_or_equal:
            return actual_number && expected_number && *actual_number >= *expected_number;
        case ComparisonOperation::less_than:
            return actual_number && expected_number && *actual_number < *expected_number;
        case ComparisonOperation::less_or_equal:
            return actual_number && expected_number && *actual_number <= *expected_number;
        case ComparisonOperation::between: {
            if (!actual_number || !expected_number || !condition.upper_bound) {
                return false;
            }
            const auto upper = number_value(*condition.upper_bound);
            return upper && *actual_number >= *expected_number && *actual_number <= *upper;
        }
    }
    return false;
}

std::optional<std::string> validate_condition(const EvaluationCondition& condition) {
    switch (condition.operation) {
        case ComparisonOperation::exists:
        case ComparisonOperation::not_exists:
            if (!std::holds_alternative<std::monostate>(condition.expected)
                || condition.upper_bound) {
                return "exists comparisons do not accept operands";
            }
            return std::nullopt;
        case ComparisonOperation::equals:
        case ComparisonOperation::not_equals:
            if (condition.upper_bound) {
                return "equality comparisons accept one operand";
            }
            return std::nullopt;
        case ComparisonOperation::contains:
        case ComparisonOperation::not_contains:
            if (!std::holds_alternative<std::string>(condition.expected)
                || condition.upper_bound) {
                return "contains comparisons require one string operand";
            }
            return std::nullopt;
        case ComparisonOperation::greater_than:
        case ComparisonOperation::greater_or_equal:
        case ComparisonOperation::less_than:
        case ComparisonOperation::less_or_equal:
            if (!is_number(condition.expected) || condition.upper_bound) {
                return "ordered comparisons require one numeric operand";
            }
            return std::nullopt;
        case ComparisonOperation::between:
            if (!is_number(condition.expected) || !condition.upper_bound
                || !is_number(*condition.upper_bound)) {
                return "between requires numeric lower and upper bounds";
            }
            if (*number_value(condition.expected) > *number_value(*condition.upper_bound)) {
                return "between lower bound must not exceed its upper bound";
            }
            return std::nullopt;
    }
    return "unknown comparison operation";
}

}  // namespace

std::optional<std::string> Evaluator::validate(const EvaluationPolicy& policy) const {
    if (!is_valid_identifier(policy.transport_failure_status)) {
        return "transport failure status is invalid";
    }
    if (!is_valid_identifier(policy.no_match_status)) {
        return "no-match status is invalid";
    }

    bool found_otherwise{};
    for (std::size_t index = 0; index < policy.rules.size(); ++index) {
        const auto& rule = policy.rules[index];
        if (!is_valid_identifier(rule.status)) {
            return "evaluation rule " + std::to_string(index) + " has an invalid status";
        }
        if (!rule.when) {
            if (found_otherwise || index + 1U != policy.rules.size()) {
                return "otherwise must be the final and only unconditional rule";
            }
            found_otherwise = true;
            continue;
        }
        if (const auto error = validate_condition(*rule.when)) {
            return "evaluation rule " + std::to_string(index) + ": " + *error;
        }
    }
    return std::nullopt;
}

EvaluationOutcome Evaluator::evaluate(
    const MonitorResult& result,
    const EvaluationPolicy& policy) const {
    if (!result.transport_success) {
        return EvaluationOutcome{policy.transport_failure_status, true, std::nullopt};
    }

    for (std::size_t index = 0; index < policy.rules.size(); ++index) {
        const auto& rule = policy.rules[index];
        if (!rule.when || condition_matches(result.value, *rule.when)) {
            return EvaluationOutcome{rule.status, false, index};
        }
    }
    return EvaluationOutcome{policy.no_match_status, false, std::nullopt};
}

}  // namespace pixelstatus
