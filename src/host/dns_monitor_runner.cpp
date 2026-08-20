#include "pixelstatus/host/dns_monitor_runner.hpp"

#include "socket_transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace pixelstatus::host {
namespace {

constexpr Duration maximum_timeout = std::chrono::seconds(30);

class AddressListGuard {
public:
    explicit AddressListGuard(addrinfo* addresses) : addresses_(addresses) {}

    ~AddressListGuard() {
        if (addresses_) {
            freeaddrinfo(addresses_);
        }
    }

    AddressListGuard(const AddressListGuard&) = delete;
    AddressListGuard& operator=(const AddressListGuard&) = delete;

private:
    addrinfo* addresses_;
};

int native_family(DnsAddressFamily family) {
    switch (family) {
        case DnsAddressFamily::any:
            return AF_UNSPEC;
        case DnsAddressFamily::ipv4:
            return AF_INET;
        case DnsAddressFamily::ipv6:
            return AF_INET6;
    }
    return -1;
}

bool valid_observation(DnsObservation observation) {
    switch (observation) {
        case DnsObservation::addresses:
        case DnsObservation::address_count:
        case DnsObservation::latency_ms:
            return true;
    }
    return false;
}

TimePoint finish_result(MonitorResult& result, std::chrono::steady_clock::time_point begin) {
    const auto finished = std::chrono::steady_clock::now();
    result.observed_at = finished;
    result.latency = std::chrono::duration_cast<Duration>(finished - begin);
    return finished;
}

std::string join_addresses(const std::vector<std::string>& addresses) {
    std::string joined;
    for (const auto& address : addresses) {
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += address;
    }
    return joined;
}

class DnsMonitorRunner final : public MonitorRunner {
public:
    explicit DnsMonitorRunner(DnsMonitorConfig config)
        : config_(std::move(config)) {}

    MonitorResult run(TimePoint) override {
        const auto begin = std::chrono::steady_clock::now();
        MonitorResult result;

        if (const auto runtime_error = detail::socket_runtime_error(); runtime_error != 0) {
            result.error = MonitorError::internal;
            result.detail = "Socket runtime initialization failed ("
                + std::to_string(runtime_error) + ')';
            finish_result(result, begin);
            return result;
        }

        addrinfo hints{};
        hints.ai_family = native_family(config_.family);
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* raw_addresses{};
        const auto resolution_error = getaddrinfo(
            config_.host.c_str(), nullptr, &hints, &raw_addresses);
        AddressListGuard address_guard(raw_addresses);
        const auto finished = finish_result(result, begin);

        if (finished - begin > config_.timeout) {
            result.error = MonitorError::timeout;
            result.detail = "DNS lookup for " + config_.host
                + " exceeded the configured timeout";
            return result;
        }
        if (resolution_error != 0 || !raw_addresses) {
            result.error = MonitorError::name_resolution;
            result.detail = "DNS lookup failed for " + config_.host
                + " (" + std::to_string(resolution_error) + ')';
            return result;
        }

        std::vector<std::string> addresses;
        for (auto* address = raw_addresses; address; address = address->ai_next) {
            if (address->ai_family != AF_INET && address->ai_family != AF_INET6) {
                continue;
            }
            std::array<char, NI_MAXHOST> numeric{};
            if (getnameinfo(
                    address->ai_addr,
                    static_cast<socklen_t>(address->ai_addrlen),
                    numeric.data(),
                    static_cast<socklen_t>(numeric.size()),
                    nullptr,
                    0,
                    NI_NUMERICHOST) == 0) {
                addresses.emplace_back(numeric.data());
            }
        }
        std::sort(addresses.begin(), addresses.end());
        addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
        if (addresses.empty()) {
            result.error = MonitorError::invalid_response;
            result.detail = "DNS lookup returned no supported IP addresses for " + config_.host;
            return result;
        }

        result.transport_success = true;
        result.detail = "DNS resolved " + config_.host + " to "
            + std::to_string(addresses.size()) + " address(es)";
        switch (config_.observation) {
            case DnsObservation::addresses:
                result.value = join_addresses(addresses);
                break;
            case DnsObservation::address_count:
                result.value = static_cast<std::int64_t>(addresses.size());
                break;
            case DnsObservation::latency_ms:
                result.value = static_cast<std::int64_t>(result.latency.count());
                break;
        }
        return result;
    }

private:
    DnsMonitorConfig config_;
};

}  // namespace

MonitorRunnerCreationResult create_dns_monitor_runner(DnsMonitorConfig config) {
    if (!detail::valid_network_host(config.host)) {
        return {nullptr, "DNS host is invalid"};
    }
    if (native_family(config.family) < 0) {
        return {nullptr, "DNS address family is invalid"};
    }
    if (!valid_observation(config.observation)) {
        return {nullptr, "DNS observation is invalid"};
    }
    if (config.timeout <= Duration::zero() || config.timeout > maximum_timeout) {
        return {nullptr, "DNS timeout must be between 1ms and 30s"};
    }
    return {std::make_unique<DnsMonitorRunner>(std::move(config)), {}};
}

}  // namespace pixelstatus::host
