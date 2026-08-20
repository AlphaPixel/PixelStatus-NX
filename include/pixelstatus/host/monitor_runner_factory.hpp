#pragma once

#include "pixelstatus/config.hpp"
#include "pixelstatus/monitor.hpp"

#include <memory>
#include <string>

namespace pixelstatus::host {

struct MonitorRunnerCreationResult {
    std::unique_ptr<MonitorRunner> runner;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return runner != nullptr && error.empty();
    }
};

[[nodiscard]] MonitorRunnerCreationResult create_monitor_runner(
    MonitorSourceConfig config);

}  // namespace pixelstatus::host
