#include "pixelstatus/monitor_engine.hpp"

#include "pixelstatus/validation.hpp"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

namespace pixelstatus {

struct MonitorEngine::Job {
    MonitorDefinition definition;
    std::unique_ptr<MonitorRunner> runner;
    TimePoint next_due{};
};

MonitorEngine::MonitorEngine(StateStore& states) : states_(states) {}

MonitorEngine::~MonitorEngine() = default;

MonitorRegistrationResult MonitorEngine::add(
    MonitorDefinition definition,
    std::unique_ptr<MonitorRunner> runner,
    TimePoint first_due) {
    if (!is_valid_identifier(definition.id)) {
        return {false, "monitor id is invalid"};
    }
    if (definition.interval <= Duration::zero()) {
        return {false, "monitor interval must be greater than zero"};
    }
    if (definition.ttl && *definition.ttl <= Duration::zero()) {
        return {false, "monitor TTL must be greater than zero"};
    }
    if (!runner) {
        return {false, "monitor runner is required"};
    }
    if (jobs_.size() >= maximum_monitor_count) {
        return {false, "monitor limit reached"};
    }
    if (std::any_of(jobs_.begin(), jobs_.end(), [&definition](const auto& job) {
            return job->definition.id == definition.id;
        })) {
        return {false, "monitor id is already registered"};
    }
    if (const auto error = evaluator_.validate(definition.evaluation)) {
        return {false, *error};
    }

    jobs_.push_back(std::make_unique<Job>(Job{
        std::move(definition),
        std::move(runner),
        first_due,
    }));
    return {true, {}};
}

MonitorPumpReport MonitorEngine::run_due(TimePoint now, std::size_t maximum_runs) {
    MonitorPumpReport report;
    std::vector<Job*> due;
    for (const auto& job : jobs_) {
        if (job->next_due <= now) {
            due.push_back(job.get());
        }
    }
    std::sort(due.begin(), due.end(), [](const Job* left, const Job* right) {
        if (left->next_due != right->next_due) {
            return left->next_due < right->next_due;
        }
        return left->definition.id < right->definition.id;
    });

    report.due = due.size();
    const auto run_count = std::min(maximum_runs, due.size());
    for (std::size_t index = 0; index < run_count; ++index) {
        auto& job = *due[index];
        const auto overdue = std::chrono::duration_cast<Duration>(now - job.next_due);
        const auto periods = overdue.count() / job.definition.interval.count() + 1;
        job.next_due += job.definition.interval * periods;
        ++report.executed;

        MonitorResult result;
        try {
            result = job.runner->run(now);
        } catch (const std::exception& error) {
            result.transport_success = false;
            result.error = MonitorError::internal;
            result.detail = std::string("Monitor runner exception: ") + error.what();
            ++report.runner_exceptions;
        } catch (...) {
            result.transport_success = false;
            result.error = MonitorError::internal;
            result.detail = "Monitor runner exception: unknown error";
            ++report.runner_exceptions;
        }

        if (!result.transport_success) {
            ++report.transport_failures;
            if (result.detail.empty()) {
                result.detail = std::string(monitor_error_name(result.error));
            }
        }
        const auto outcome = evaluator_.evaluate(result, job.definition.evaluation);
        const auto observed_at = result.observed_at.value_or(now);

        MonitorState state;
        state.id = job.definition.id;
        state.status = outcome.status;
        state.value = std::move(result.value);
        state.message = std::move(result.detail);
        state.observed_at = observed_at;
        state.updated_at = observed_at;
        state.ttl = job.definition.ttl;
        if (states_.upsert(std::move(state))) {
            ++report.state_updates;
        }
    }
    return report;
}

std::optional<TimePoint> MonitorEngine::next_due(const std::string& id) const {
    const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&id](const auto& job) {
        return job->definition.id == id;
    });
    if (found == jobs_.end()) {
        return std::nullopt;
    }
    return (*found)->next_due;
}

std::size_t MonitorEngine::size() const noexcept {
    return jobs_.size();
}

}  // namespace pixelstatus
