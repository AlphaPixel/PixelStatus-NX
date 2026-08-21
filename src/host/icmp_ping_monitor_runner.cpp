#include "pixelstatus/host/icmp_ping_monitor_runner.hpp"

#include "socket_transport.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <WS2tcpip.h>
#include <Ipexport.h>
#include <IcmpAPI.h>
#include <Iphlpapi.h>
#endif

namespace pixelstatus::host {
namespace {

constexpr Duration maximum_timeout = std::chrono::seconds(30);

void finish_result(MonitorResult& result, std::chrono::steady_clock::time_point begin) {
    const auto finished = std::chrono::steady_clock::now();
    result.observed_at = finished;
    result.latency = std::chrono::duration_cast<Duration>(finished - begin);
}

#ifdef _WIN32

struct AddressListGuard {
    addrinfo* addresses{};

    ~AddressListGuard() {
        if (addresses != nullptr) {
            freeaddrinfo(addresses);
        }
    }
};

class IcmpHandle {
public:
    IcmpHandle() : handle_(IcmpCreateFile()) {}

    ~IcmpHandle() {
        if (valid()) {
            static_cast<void>(IcmpCloseHandle(handle_));
        }
    }

    IcmpHandle(const IcmpHandle&) = delete;
    IcmpHandle& operator=(const IcmpHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE native() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

MonitorError error_from_status(ULONG status) {
    if (status == IP_REQ_TIMED_OUT || status == ERROR_SEM_TIMEOUT) {
        return MonitorError::timeout;
    }
    return MonitorError::connection;
}

class IcmpPingMonitorRunner final : public MonitorRunner {
public:
    explicit IcmpPingMonitorRunner(IcmpPingMonitorConfig config)
        : config_(std::move(config)) {}

    MonitorResult run(TimePoint) override {
        const auto begin = std::chrono::steady_clock::now();
        MonitorResult result;

        const auto runtime_error = detail::socket_runtime_error();
        if (runtime_error != 0) {
            finish_result(result, begin);
            result.error = MonitorError::internal;
            result.detail = "Socket runtime initialization failed ("
                + std::to_string(runtime_error) + ')';
            return result;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* raw_addresses{};
        const auto resolve_error = getaddrinfo(
            config_.host.c_str(), nullptr, &hints, &raw_addresses);
        AddressListGuard addresses{raw_addresses};
        if (resolve_error != 0 || addresses.addresses == nullptr) {
            finish_result(result, begin);
            result.error = MonitorError::name_resolution;
            result.detail = "ICMP name resolution failed for " + config_.host
                + " (" + std::to_string(resolve_error) + ')';
            return result;
        }

        const auto* destination = reinterpret_cast<const sockaddr_in*>(
            addresses.addresses->ai_addr);
        IcmpHandle handle;
        if (!handle.valid()) {
            const auto error = GetLastError();
            finish_result(result, begin);
            result.error = MonitorError::internal;
            result.detail = "Unable to create ICMP handle ("
                + std::to_string(error) + ')';
            return result;
        }

        constexpr std::array<char, 16U> payload{
            'P', 'i', 'x', 'e', 'l', 'S', 't', 'a', 't', 'u', 's', '-', 'N', 'X', 0, 0,
        };
        std::vector<std::byte> reply_buffer(
            sizeof(ICMP_ECHO_REPLY) + payload.size() + 8U);
        const auto reply_count = IcmpSendEcho(
            handle.native(),
            destination->sin_addr.S_un.S_addr,
            const_cast<char*>(payload.data()),
            static_cast<WORD>(payload.size()),
            nullptr,
            reply_buffer.data(),
            static_cast<DWORD>(reply_buffer.size()),
            static_cast<DWORD>(config_.timeout.count()));
        if (reply_count == 0U) {
            const auto error = GetLastError();
            finish_result(result, begin);
            result.error = error_from_status(error);
            result.detail = "ICMP echo to " + config_.host + " failed ("
                + std::to_string(error) + ')';
            return result;
        }

        const auto* reply = reinterpret_cast<const ICMP_ECHO_REPLY*>(
            reply_buffer.data());
        if (reply->Status != IP_SUCCESS) {
            finish_result(result, begin);
            result.error = error_from_status(reply->Status);
            result.detail = "ICMP echo to " + config_.host + " failed ("
                + std::to_string(reply->Status) + ')';
            return result;
        }

        finish_result(result, begin);
        result.transport_success = true;
        result.value = static_cast<std::int64_t>(reply->RoundTripTime);
        result.detail = "ICMP echo reply from " + config_.host;
        return result;
    }

private:
    IcmpPingMonitorConfig config_;
};

#endif

}  // namespace

MonitorRunnerCreationResult create_icmp_ping_monitor_runner(
    IcmpPingMonitorConfig config) {
    if (!detail::valid_network_host(config.host)) {
        return {nullptr, "ICMP host is invalid"};
    }
    if (config.timeout <= Duration::zero() || config.timeout > maximum_timeout) {
        return {nullptr, "ICMP timeout must be between 1ms and 30s"};
    }
#ifdef _WIN32
    return {std::make_unique<IcmpPingMonitorRunner>(std::move(config)), {}};
#else
    return {nullptr, "ICMP ping is not implemented for this host platform"};
#endif
}

}  // namespace pixelstatus::host
