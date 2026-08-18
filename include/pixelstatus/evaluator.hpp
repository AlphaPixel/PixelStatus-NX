#pragma once

#include "pixelstatus/monitor.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pixelstatus {

enum class ComparisonOperation {
    exists,
    not_exists,
    equals,
    not_equals,
    contains,
    not_contains,
    greater_than,
    greater_or_equal,
    less_than,
    less_or_equal,
    between,
};

struct EvaluationCondition {
    ComparisonOperation operation{ComparisonOperation::exists};
    StateValue expected;
    std::optional<StateValue> upper_bound;
};

struct EvaluationRule {
    std::optional<EvaluationCondition> when;
    std::string status;
};

struct EvaluationPolicy {
    std::string transport_failure_status{"communication_failure"};
    std::string no_match_status{"unknown"};
    std::vector<EvaluationRule> rules;
};

struct EvaluationOutcome {
    std::string status;
    bool transport_failure{};
    std::optional<std::size_t> matched_rule;
};

class Evaluator {
public:
    [[nodiscard]] std::optional<std::string> validate(
        const EvaluationPolicy& policy) const;
    [[nodiscard]] EvaluationOutcome evaluate(
        const MonitorResult& result,
        const EvaluationPolicy& policy) const;
};

}  // namespace pixelstatus
