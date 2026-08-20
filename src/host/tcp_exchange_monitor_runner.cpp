#include "pixelstatus/host/tcp_exchange_monitor_runner.hpp"

#include "socket_transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace pixelstatus::host {
namespace {

constexpr Duration maximum_timeout = std::chrono::seconds(30);
constexpr std::size_t maximum_send_bytes = 4U * 1024U;
constexpr std::size_t maximum_delimiter_bytes = 256U;
constexpr std::size_t maximum_response_bytes = 64U * 1024U;

void finish_result(MonitorResult& result, std::chrono::steady_clock::time_point begin) {
    const auto finished = std::chrono::steady_clock::now();
    result.observed_at = finished;
    result.latency = std::chrono::duration_cast<Duration>(finished - begin);
}

void apply_connection_failure(
    MonitorResult& result,
    const detail::TcpConnectionResult& connected,
    std::string_view host,
    std::uint16_t port) {
    result.error = connected.error;
    const auto target = detail::endpoint(host, port);
    if (connected.error == MonitorError::timeout) {
        result.detail = "TCP exchange connection to " + target + " timed out";
    } else if (connected.error == MonitorError::name_resolution) {
        result.detail = "TCP exchange name resolution failed for " + std::string(host)
            + " (" + std::to_string(connected.native_error) + ')';
    } else if (connected.error == MonitorError::internal) {
        result.detail = "Socket runtime initialization failed ("
            + std::to_string(connected.native_error) + ')';
    } else {
        result.detail = "TCP exchange connection to " + target + " failed ("
            + std::to_string(connected.native_error) + ')';
    }
}

class TcpExchangeMonitorRunner final : public MonitorRunner {
public:
    explicit TcpExchangeMonitorRunner(TcpExchangeMonitorConfig config)
        : config_(std::move(config)) {}

    MonitorResult run(TimePoint) override {
        const auto begin = std::chrono::steady_clock::now();
        const auto deadline = begin + config_.timeout;
        auto connected = detail::connect_tcp(config_.host, config_.port, deadline);
        MonitorResult result;
        if (!connected) {
            finish_result(result, begin);
            apply_connection_failure(result, connected, config_.host, config_.port);
            return result;
        }

        std::size_t sent{};
        while (sent < config_.send.size()) {
            const auto io = detail::send_some(
                connected.socket,
                std::string_view(config_.send).substr(sent),
                deadline);
            if (io.status != detail::SocketIoStatus::success || io.bytes == 0U) {
                finish_result(result, begin);
                if (io.status == detail::SocketIoStatus::timeout) {
                    result.error = MonitorError::timeout;
                    result.detail = "TCP exchange send timed out";
                } else {
                    result.error = MonitorError::connection;
                    result.detail = "TCP exchange send failed ("
                        + std::to_string(io.native_error) + ')';
                }
                return result;
            }
            sent += io.bytes;
        }

        std::string response;
        response.reserve(config_.maximum_response_bytes);
        std::array<char, 1024U> buffer{};
        while (true) {
            if (response.size() == config_.maximum_response_bytes) {
                finish_result(result, begin);
                result.error = MonitorError::response_too_large;
                result.detail = "TCP exchange response exceeded the configured byte limit";
                return result;
            }
            const auto available = std::min(
                buffer.size(), config_.maximum_response_bytes - response.size());
            const auto io = detail::receive_some(
                connected.socket,
                std::span<char>(buffer.data(), available),
                deadline);
            if (io.status == detail::SocketIoStatus::timeout) {
                finish_result(result, begin);
                result.error = MonitorError::timeout;
                result.detail = "TCP exchange receive timed out";
                return result;
            }
            if (io.status == detail::SocketIoStatus::closed) {
                finish_result(result, begin);
                result.error = MonitorError::protocol;
                result.detail = "TCP exchange peer closed before the response delimiter";
                return result;
            }
            if (io.status == detail::SocketIoStatus::error) {
                finish_result(result, begin);
                result.error = MonitorError::connection;
                result.detail = "TCP exchange receive failed ("
                    + std::to_string(io.native_error) + ')';
                return result;
            }

            response.append(buffer.data(), io.bytes);
            const auto delimiter = response.find(config_.read_until);
            if (delimiter != std::string::npos) {
                response.resize(delimiter + config_.read_until.size());
                break;
            }
        }

        finish_result(result, begin);
        result.transport_success = true;
        if (config_.observation == TcpExchangeObservation::body) {
            result.value = response;
        } else {
            result.value = static_cast<std::int64_t>(result.latency.count());
        }
        result.detail = "TCP exchange completed with "
            + detail::endpoint(config_.host, config_.port)
            + "; received " + std::to_string(response.size()) + " bytes";
        return result;
    }

private:
    TcpExchangeMonitorConfig config_;
};

}  // namespace

MonitorRunnerCreationResult create_tcp_exchange_monitor_runner(
    TcpExchangeMonitorConfig config) {
    if (!detail::valid_network_host(config.host)) {
        return {nullptr, "TCP exchange host is invalid"};
    }
    if (config.port == 0U) {
        return {nullptr, "TCP exchange port must be between 1 and 65535"};
    }
    if (config.timeout <= Duration::zero() || config.timeout > maximum_timeout) {
        return {nullptr, "TCP exchange timeout must be between 1ms and 30s"};
    }
    if (config.send.size() > maximum_send_bytes) {
        return {nullptr, "TCP exchange send payload exceeds 4096 bytes"};
    }
    if (config.read_until.empty() || config.read_until.size() > maximum_delimiter_bytes) {
        return {nullptr, "TCP exchange response delimiter must contain 1-256 bytes"};
    }
    if (config.maximum_response_bytes == 0U
        || config.maximum_response_bytes > maximum_response_bytes
        || config.maximum_response_bytes < config.read_until.size()) {
        return {nullptr, "TCP exchange response byte limit is invalid"};
    }
    if (config.observation != TcpExchangeObservation::body
        && config.observation != TcpExchangeObservation::latency_ms) {
        return {nullptr, "TCP exchange observation is invalid"};
    }
    return {std::make_unique<TcpExchangeMonitorRunner>(std::move(config)), {}};
}

}  // namespace pixelstatus::host
