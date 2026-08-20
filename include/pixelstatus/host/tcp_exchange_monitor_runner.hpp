#pragma once

#include "pixelstatus/config.hpp"
#include "pixelstatus/host/monitor_runner_factory.hpp"

namespace pixelstatus::host {

[[nodiscard]] MonitorRunnerCreationResult create_tcp_exchange_monitor_runner(
    TcpExchangeMonitorConfig config);

}  // namespace pixelstatus::host
