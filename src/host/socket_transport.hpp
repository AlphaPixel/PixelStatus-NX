#pragma once

#include "pixelstatus/monitor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#endif

namespace pixelstatus::host::detail {

#ifdef _WIN32
using NativeSocketHandle = SOCKET;
#else
using NativeSocketHandle = int;
#endif

class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(NativeSocketHandle handle);
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] NativeSocketHandle native() const noexcept;

private:
#ifdef _WIN32
    NativeSocketHandle handle_{INVALID_SOCKET};
#else
    NativeSocketHandle handle_{-1};
#endif
};

struct TcpConnectionResult {
    TcpSocket socket;
    MonitorError error{MonitorError::none};
    int native_error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return socket.valid() && error == MonitorError::none;
    }
};

enum class SocketIoStatus {
    success,
    timeout,
    closed,
    error,
};

struct SocketIoResult {
    SocketIoStatus status{SocketIoStatus::error};
    std::size_t bytes{};
    int native_error{};
};

[[nodiscard]] int socket_runtime_error();
[[nodiscard]] bool valid_network_host(std::string_view host);
[[nodiscard]] std::string endpoint(std::string_view host, std::uint16_t port);
[[nodiscard]] TcpConnectionResult connect_tcp(
    std::string_view host,
    std::uint16_t port,
    TimePoint deadline);
[[nodiscard]] SocketIoResult send_some(
    TcpSocket& socket,
    std::string_view data,
    TimePoint deadline);
[[nodiscard]] SocketIoResult receive_some(
    TcpSocket& socket,
    std::span<char> buffer,
    TimePoint deadline);

}  // namespace pixelstatus::host::detail
