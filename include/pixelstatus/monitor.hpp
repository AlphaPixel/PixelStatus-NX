#pragma once

#include "pixelstatus/state.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace pixelstatus {

enum class MonitorError {
    none,
    timeout,
    name_resolution,
    connection,
    tls,
    protocol,
    response_too_large,
    invalid_response,
    internal,
};

[[nodiscard]] std::string_view monitor_error_name(MonitorError error) noexcept;

struct MonitorResult {
    bool transport_success{};
    StateValue value;
    Duration latency{};
    MonitorError error{MonitorError::none};
    std::string detail;
    std::optional<TimePoint> observed_at;
};

class MonitorRunner {
public:
    virtual ~MonitorRunner() = default;

    [[nodiscard]] virtual MonitorResult run(TimePoint started_at) = 0;
};

}  // namespace pixelstatus
