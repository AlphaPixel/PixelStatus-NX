#include "pixelstatus/host/monitor_executor.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pixelstatus::host {
namespace {

constexpr Duration minimum_idle_poll_interval{1};
constexpr Duration maximum_idle_poll_interval{1000};

}  // namespace

struct MonitorExecutorImpl {
    MonitorExecutorImpl(MonitorEngine& monitor_engine, MonitorExecutorOptions executor_options)
        : engine(monitor_engine), options(std::move(executor_options)) {}

    MonitorEngine& engine;
    MonitorExecutorOptions options;
    std::vector<std::jthread> workers;
    mutable std::mutex mutex;
    bool is_running{};
    std::atomic_bool failed{};
    std::string last_error;
    std::atomic_size_t pump_calls{};
    std::atomic_size_t jobs_executed{};
    std::atomic_size_t state_updates{};
    std::atomic_size_t transport_failures{};
    std::atomic_size_t runner_exceptions{};
};

MonitorExecutor::MonitorExecutor(MonitorEngine& engine, MonitorExecutorOptions options)
    : impl_(std::make_unique<MonitorExecutorImpl>(engine, std::move(options))) {}

MonitorExecutor::~MonitorExecutor() {
    stop();
}

bool MonitorExecutor::start() {
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->is_running) {
            impl_->last_error = "Monitor executor is already running";
            return false;
        }
        if (impl_->options.worker_count == 0U
            || impl_->options.worker_count > maximum_monitor_worker_count) {
            impl_->last_error = "Monitor worker count must be between 1 and "
                + std::to_string(maximum_monitor_worker_count);
            return false;
        }
        if (impl_->options.idle_poll_interval < minimum_idle_poll_interval
            || impl_->options.idle_poll_interval > maximum_idle_poll_interval) {
            impl_->last_error = "Monitor idle poll interval must be between 1ms and 1s";
            return false;
        }

        impl_->failed.store(false, std::memory_order_relaxed);
        impl_->pump_calls.store(0U, std::memory_order_relaxed);
        impl_->jobs_executed.store(0U, std::memory_order_relaxed);
        impl_->state_updates.store(0U, std::memory_order_relaxed);
        impl_->transport_failures.store(0U, std::memory_order_relaxed);
        impl_->runner_exceptions.store(0U, std::memory_order_relaxed);
        impl_->last_error.clear();
        impl_->is_running = true;
    }

    try {
        impl_->workers.reserve(impl_->options.worker_count);
        for (std::size_t index = 0; index < impl_->options.worker_count; ++index) {
            impl_->workers.emplace_back([this](std::stop_token stop) {
                while (!stop.stop_requested()
                       && !impl_->failed.load(std::memory_order_relaxed)) {
                    try {
                        const auto report = impl_->engine.run_due(
                            std::chrono::steady_clock::now(), 1U);
                        impl_->pump_calls.fetch_add(1U, std::memory_order_relaxed);
                        impl_->jobs_executed.fetch_add(report.executed, std::memory_order_relaxed);
                        impl_->state_updates.fetch_add(report.state_updates, std::memory_order_relaxed);
                        impl_->transport_failures.fetch_add(
                            report.transport_failures, std::memory_order_relaxed);
                        impl_->runner_exceptions.fetch_add(
                            report.runner_exceptions, std::memory_order_relaxed);
                        if (report.executed == 0U) {
                            std::this_thread::sleep_for(impl_->options.idle_poll_interval);
                        }
                    } catch (const std::exception& exception) {
                        {
                            std::scoped_lock lock(impl_->mutex);
                            if (impl_->last_error.empty()) {
                                impl_->last_error = std::string("Monitor executor failure: ")
                                    + exception.what();
                            }
                        }
                        impl_->failed.store(true, std::memory_order_relaxed);
                    } catch (...) {
                        {
                            std::scoped_lock lock(impl_->mutex);
                            if (impl_->last_error.empty()) {
                                impl_->last_error = "Monitor executor failure: unknown exception";
                            }
                        }
                        impl_->failed.store(true, std::memory_order_relaxed);
                    }
                }
            });
        }
    } catch (const std::exception& exception) {
        for (auto& worker : impl_->workers) {
            worker.request_stop();
        }
        for (auto& worker : impl_->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        impl_->workers.clear();
        std::scoped_lock lock(impl_->mutex);
        impl_->is_running = false;
        impl_->last_error = std::string("Unable to start monitor executor: ") + exception.what();
        return false;
    } catch (...) {
        for (auto& worker : impl_->workers) {
            worker.request_stop();
        }
        for (auto& worker : impl_->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        impl_->workers.clear();
        std::scoped_lock lock(impl_->mutex);
        impl_->is_running = false;
        impl_->last_error = "Unable to start monitor executor: unknown exception";
        return false;
    }
    return true;
}

void MonitorExecutor::stop() {
    for (auto& worker : impl_->workers) {
        worker.request_stop();
    }
    for (auto& worker : impl_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    impl_->workers.clear();

    std::scoped_lock lock(impl_->mutex);
    impl_->is_running = false;
}

bool MonitorExecutor::running() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->is_running && !impl_->failed.load(std::memory_order_relaxed);
}

std::size_t MonitorExecutor::worker_count() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->workers.size();
}

MonitorExecutorStats MonitorExecutor::stats() const {
    return {
        impl_->pump_calls.load(std::memory_order_relaxed),
        impl_->jobs_executed.load(std::memory_order_relaxed),
        impl_->state_updates.load(std::memory_order_relaxed),
        impl_->transport_failures.load(std::memory_order_relaxed),
        impl_->runner_exceptions.load(std::memory_order_relaxed),
    };
}

std::string MonitorExecutor::error() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->last_error;
}

}  // namespace pixelstatus::host
