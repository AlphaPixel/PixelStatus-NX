#pragma once

#include "pixelstatus/config.hpp"
#include "pixelstatus/host/monitor_runner_factory.hpp"

namespace pixelstatus::host {

[[nodiscard]] MonitorRunnerCreationResult create_tcp_connect_monitor_runner(
    TcpConnectMonitorConfig config);

}  // namespace pixelstatus::host
