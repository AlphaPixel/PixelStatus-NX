#include "pixelstatus/host/tcp_connect_monitor_runner.hpp"

#include "socket_transport.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace pixelstatus::host {
namespace {

constexpr Duration maximum_timeout = std::chrono::seconds(30);

void finish_result(MonitorResult& result, std::chrono::steady_clock::time_point begin) {
    const auto finished = std::chrono::steady_clock::now();
    result.observed_at = finished;
    result.latency = std::chrono::duration_cast<Duration>(finished - begin);
}

class TcpConnectMonitorRunner final : public MonitorRunner {
public:
    explicit TcpConnectMonitorRunner(TcpConnectMonitorConfig config)
        : config_(std::move(config)) {}

    MonitorResult run(TimePoint) override {
        const auto begin = std::chrono::steady_clock::now();
        const auto connected = detail::connect_tcp(
            config_.host,
            config_.port,
            begin + config_.timeout);
        MonitorResult result;
        finish_result(result, begin);
        if (!connected) {
            result.error = connected.error;
            const auto target = detail::endpoint(config_.host, config_.port);
            if (connected.error == MonitorError::timeout) {
                result.detail = "TCP connection to " + target + " timed out";
            } else if (connected.error == MonitorError::name_resolution) {
                result.detail = "TCP name resolution failed for " + config_.host
                    + " (" + std::to_string(connected.native_error) + ')';
            } else if (connected.error == MonitorError::internal) {
                result.detail = "Socket runtime initialization failed ("
                    + std::to_string(connected.native_error) + ')';
            } else {
                result.detail = "TCP connection to " + target + " failed ("
                    + std::to_string(connected.native_error) + ')';
            }
            return result;
        }

        result.transport_success = true;
        result.value = static_cast<std::int64_t>(result.latency.count());
        result.detail = "TCP connection established to "
            + detail::endpoint(config_.host, config_.port);
        return result;
    }

private:
    TcpConnectMonitorConfig config_;
};

}  // namespace

MonitorRunnerCreationResult create_tcp_connect_monitor_runner(
    TcpConnectMonitorConfig config) {
    if (!detail::valid_network_host(config.host)) {
        return {nullptr, "TCP host is invalid"};
    }
    if (config.port == 0U) {
        return {nullptr, "TCP port must be between 1 and 65535"};
    }
    if (config.timeout <= Duration::zero() || config.timeout > maximum_timeout) {
        return {nullptr, "TCP timeout must be between 1ms and 30s"};
    }
    return {std::make_unique<TcpConnectMonitorRunner>(std::move(config)), {}};
}

}  // namespace pixelstatus::host
