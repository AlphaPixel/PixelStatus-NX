#pragma once

#include "pixelstatus/evaluator.hpp"
#include "pixelstatus/monitor.hpp"
#include "pixelstatus/state.hpp"

#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pixelstatus {

inline constexpr std::size_t maximum_monitor_count = 256U;

struct MonitorDefinition {
    std::string id;
    Duration interval{};
    std::optional<Duration> ttl;
    EvaluationPolicy evaluation;
};

struct MonitorRegistrationResult {
    bool success{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return success;
    }
};

struct MonitorPumpReport {
    std::size_t due{};
    std::size_t executed{};
    std::size_t state_updates{};
    std::size_t transport_failures{};
    std::size_t runner_exceptions{};
};

class MonitorEngine {
public:
    explicit MonitorEngine(StateStore& states);
    ~MonitorEngine();

    MonitorEngine(const MonitorEngine&) = delete;
    MonitorEngine& operator=(const MonitorEngine&) = delete;

    [[nodiscard]] MonitorRegistrationResult add(
        MonitorDefinition definition,
        std::unique_ptr<MonitorRunner> runner,
        TimePoint first_due);
    [[nodiscard]] MonitorPumpReport run_due(
        TimePoint now,
        std::size_t maximum_runs = std::numeric_limits<std::size_t>::max());

    [[nodiscard]] std::optional<TimePoint> next_due(const std::string& id) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Job;

    StateStore& states_;
    Evaluator evaluator_;
    std::vector<std::unique_ptr<Job>> jobs_;
};

}  // namespace pixelstatus
