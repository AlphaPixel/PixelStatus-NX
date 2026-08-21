#pragma once

#include "pixelstatus/config.hpp"
#include "pixelstatus/host/monitor_runner_factory.hpp"

namespace pixelstatus::host {

[[nodiscard]] MonitorRunnerCreationResult create_icmp_ping_monitor_runner(
    IcmpPingMonitorConfig config);

}  // namespace pixelstatus::host
