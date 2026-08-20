#pragma once

#include "pixelstatus/config.hpp"
#include "pixelstatus/monitor.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace pixelstatus::host {

struct HttpTransportResponse {
    bool success{};
    std::uint32_t status_code{};
    std::string body;
    Duration latency{};
    MonitorError error{MonitorError::none};
    std::string detail;
    std::optional<TimePoint> observed_at;
};

#ifdef _WIN32
[[nodiscard]] HttpTransportResponse perform_winhttp_request(
    const HttpMonitorConfig& config);
#endif

}  // namespace pixelstatus::host
