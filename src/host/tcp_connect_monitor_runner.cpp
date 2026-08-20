#include "pixelstatus/host/tcp_connect_monitor_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pixelstatus::host {
namespace {

constexpr std::size_t maximum_host_bytes = 253U;
constexpr Duration maximum_timeout = std::chrono::seconds(30);

#ifdef _WIN32

using SocketHandle = SOCKET;
constexpr SocketHandle invalid_socket_handle = INVALID_SOCKET;

class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        error_ = WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~SocketRuntime() {
        if (error_ == 0) {
            WSACleanup();
        }
    }

    [[nodiscard]] int error() const noexcept {
        return error_;
    }

private:
    int error_{};
};

int last_socket_error() {
    return WSAGetLastError();
}

bool set_nonblocking(SocketHandle socket) {
    u_long enabled = 1U;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

void close_socket(SocketHandle socket) {
    closesocket(socket);
}

bool connection_pending(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

bool timeout_error(int error) {
    return error == WSAETIMEDOUT;
}

int wait_for_connect(SocketHandle socket, timeval& timeout) {
    fd_set writable;
    fd_set exceptional;
    FD_ZERO(&writable);
    FD_ZERO(&exceptional);
    FD_SET(socket, &writable);
    FD_SET(socket, &exceptional);
    return select(0, nullptr, &writable, &exceptional, &timeout);
}

int connected_socket_error(SocketHandle socket) {
    int error{};
    int length = sizeof(error);
    if (getsockopt(
            socket,
            SOL_SOCKET,
            SO_ERROR,
            reinterpret_cast<char*>(&error),
            &length) != 0) {
        return last_socket_error();
    }
    return error;
}

#else

using SocketHandle = int;
constexpr SocketHandle invalid_socket_handle = -1;

class SocketRuntime {
public:
    [[nodiscard]] int error() const noexcept {
        return 0;
    }
};

int last_socket_error() {
    return errno;
}

bool set_nonblocking(SocketHandle socket) {
    const auto flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_socket(SocketHandle socket) {
    close(socket);
}

bool connection_pending(int error) {
    return error == EINPROGRESS || error == EWOULDBLOCK;
}

bool timeout_error(int error) {
    return error == ETIMEDOUT;
}

int wait_for_connect(SocketHandle socket, timeval& timeout) {
    fd_set writable;
    fd_set exceptional;
    FD_ZERO(&writable);
    FD_ZERO(&exceptional);
    FD_SET(socket, &writable);
    FD_SET(socket, &exceptional);
    return select(socket + 1, nullptr, &writable, &exceptional, &timeout);
}

int connected_socket_error(SocketHandle socket) {
    int error{};
    socklen_t length = sizeof(error);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return last_socket_error();
    }
    return error;
}

#endif

class SocketGuard {
public:
    explicit SocketGuard(SocketHandle socket) : socket_(socket) {}

    ~SocketGuard() {
        if (socket_ != invalid_socket_handle) {
            close_socket(socket_);
        }
    }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

private:
    SocketHandle socket_;
};

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

bool valid_host(std::string_view host) {
    if (host.empty() || host.size() > maximum_host_bytes) {
        return false;
    }
    return std::none_of(host.begin(), host.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte <= 0x20U || byte == 0x7FU
            || character == '/' || character == '?' || character == '#'
            || character == '[' || character == ']';
    });
}

std::string endpoint(const TcpConnectMonitorConfig& config) {
    if (config.host.find(':') != std::string::npos) {
        return '[' + config.host + "]:" + std::to_string(config.port);
    }
    return config.host + ':' + std::to_string(config.port);
}

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
        const auto deadline = begin + config_.timeout;
        MonitorResult result;

        static SocketRuntime runtime;
        if (runtime.error() != 0) {
            result.error = MonitorError::internal;
            result.detail = "Socket runtime initialization failed ("
                + std::to_string(runtime.error()) + ')';
            finish_result(result, begin);
            return result;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* raw_addresses{};
        const auto service = std::to_string(config_.port);
        const auto resolution_error = getaddrinfo(
            config_.host.c_str(), service.c_str(), &hints, &raw_addresses);
        AddressListGuard addresses(raw_addresses);
        if (resolution_error != 0 || !raw_addresses) {
            result.error = MonitorError::name_resolution;
            result.detail = "TCP name resolution failed for " + config_.host
                + " (" + std::to_string(resolution_error) + ')';
            finish_result(result, begin);
            return result;
        }

        int final_error{};
        bool timed_out{};
        for (auto* address = raw_addresses; address; address = address->ai_next) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                timed_out = true;
                break;
            }

            const auto socket = ::socket(
                address->ai_family,
                address->ai_socktype,
                address->ai_protocol);
            if (socket == invalid_socket_handle) {
                final_error = last_socket_error();
                continue;
            }
            SocketGuard socket_guard(socket);
            if (!set_nonblocking(socket)) {
                final_error = last_socket_error();
                continue;
            }

            if (::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
                result.transport_success = true;
            } else {
                final_error = last_socket_error();
                if (!connection_pending(final_error)) {
                    continue;
                }

                const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining <= std::chrono::microseconds::zero()) {
                    timed_out = true;
                    break;
                }
                timeval timeout{};
                timeout.tv_sec = static_cast<long>(remaining.count() / 1'000'000);
                timeout.tv_usec = static_cast<long>(remaining.count() % 1'000'000);
                const auto selected = wait_for_connect(socket, timeout);
                if (selected == 0) {
                    timed_out = true;
                    break;
                }
                if (selected < 0) {
                    final_error = last_socket_error();
                    continue;
                }
                final_error = connected_socket_error(socket);
                result.transport_success = final_error == 0;
            }

            if (result.transport_success) {
                finish_result(result, begin);
                result.value = static_cast<std::int64_t>(result.latency.count());
                result.detail = "TCP connection established to " + endpoint(config_);
                return result;
            }
        }

        finish_result(result, begin);
        if (timed_out || timeout_error(final_error)
            || std::chrono::steady_clock::now() >= deadline) {
            result.error = MonitorError::timeout;
            result.detail = "TCP connection to " + endpoint(config_) + " timed out";
        } else {
            result.error = MonitorError::connection;
            result.detail = "TCP connection to " + endpoint(config_)
                + " failed (" + std::to_string(final_error) + ')';
        }
        return result;
    }

private:
    TcpConnectMonitorConfig config_;
};

}  // namespace

MonitorRunnerCreationResult create_tcp_connect_monitor_runner(
    TcpConnectMonitorConfig config) {
    if (!valid_host(config.host)) {
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
