#pragma once

#include "pixelstatus/config.hpp"
#include "pixelstatus/host/monitor_runner_factory.hpp"

namespace pixelstatus::host {

[[nodiscard]] MonitorRunnerCreationResult create_dns_monitor_runner(
    DnsMonitorConfig config);

}  // namespace pixelstatus::host
