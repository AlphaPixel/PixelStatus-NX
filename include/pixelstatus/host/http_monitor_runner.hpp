#pragma once

#include "pixelstatus/config.hpp"
#include "pixelstatus/host/monitor_runner_factory.hpp"
#include "pixelstatus/host/secret_resolver.hpp"

namespace pixelstatus::host {

[[nodiscard]] MonitorRunnerCreationResult create_http_monitor_runner(
    HttpMonitorConfig config,
    const SecretResolver& secret_resolver = {});

}  // namespace pixelstatus::host
