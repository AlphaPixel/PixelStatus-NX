#pragma once

#include "pixelstatus/monitor_engine.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace pixelstatus::host {

inline constexpr std::size_t maximum_monitor_worker_count = 8U;

struct MonitorExecutorOptions {
    std::size_t worker_count{2U};
    Duration idle_poll_interval{10};
};

struct MonitorExecutorStats {
    std::size_t pump_calls{};
    std::size_t jobs_executed{};
    std::size_t state_updates{};
    std::size_t transport_failures{};
    std::size_t runner_exceptions{};
};

struct MonitorExecutorImpl;

class MonitorExecutor {
public:
    explicit MonitorExecutor(
        MonitorEngine& engine,
        MonitorExecutorOptions options = {});
    ~MonitorExecutor();

    MonitorExecutor(const MonitorExecutor&) = delete;
    MonitorExecutor& operator=(const MonitorExecutor&) = delete;

    [[nodiscard]] bool start();
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::size_t worker_count() const;
    [[nodiscard]] MonitorExecutorStats stats() const;
    [[nodiscard]] std::string error() const;

private:
    std::unique_ptr<MonitorExecutorImpl> impl_;
};

}  // namespace pixelstatus::host
