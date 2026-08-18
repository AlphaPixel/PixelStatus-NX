#include "pixelstatus/monitor.hpp"

namespace pixelstatus {

std::string_view monitor_error_name(MonitorError error) noexcept {
    switch (error) {
        case MonitorError::none:
            return "none";
        case MonitorError::timeout:
            return "timeout";
        case MonitorError::name_resolution:
            return "name_resolution";
        case MonitorError::connection:
            return "connection";
        case MonitorError::tls:
            return "tls";
        case MonitorError::protocol:
            return "protocol";
        case MonitorError::response_too_large:
            return "response_too_large";
        case MonitorError::invalid_response:
            return "invalid_response";
        case MonitorError::internal:
            return "internal";
    }
    return "internal";
}

}  // namespace pixelstatus
