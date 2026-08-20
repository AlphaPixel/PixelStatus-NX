#include "socket_transport.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <string>
#include <utility>

#ifdef _WIN32
#include <WS2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pixelstatus::host::detail {
namespace {

constexpr std::size_t maximum_host_bytes = 253U;

#ifdef _WIN32

constexpr NativeSocketHandle invalid_socket_handle = INVALID_SOCKET;

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

bool set_nonblocking(NativeSocketHandle socket) {
    u_long enabled = 1U;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

void close_socket(NativeSocketHandle socket) {
    closesocket(socket);
}

bool connection_pending(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

bool operation_would_block(int error) {
    return error == WSAEWOULDBLOCK;
}

bool operation_interrupted(int error) {
    return error == WSAEINTR;
}

bool timeout_error(int error) {
    return error == WSAETIMEDOUT;
}

int select_socket(
    NativeSocketHandle socket,
    bool readable,
    fd_set& selected,
    fd_set& exceptional,
    timeval& timeout) {
    static_cast<void>(socket);
    return select(
        0,
        readable ? &selected : nullptr,
        readable ? nullptr : &selected,
        &exceptional,
        &timeout);
}

int socket_error(NativeSocketHandle socket) {
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

int send_native(NativeSocketHandle socket, const char* data, std::size_t size) {
    return send(socket, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
}

int receive_native(NativeSocketHandle socket, char* data, std::size_t size) {
    return recv(socket, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
}

#else

constexpr NativeSocketHandle invalid_socket_handle = -1;

class SocketRuntime {
public:
    [[nodiscard]] int error() const noexcept {
        return 0;
    }
};

int last_socket_error() {
    return errno;
}

bool set_nonblocking(NativeSocketHandle socket) {
    const auto flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_socket(NativeSocketHandle socket) {
    close(socket);
}

bool connection_pending(int error) {
    return error == EINPROGRESS || error == EWOULDBLOCK;
}

bool operation_would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool operation_interrupted(int error) {
    return error == EINTR;
}

bool timeout_error(int error) {
    return error == ETIMEDOUT;
}

int select_socket(
    NativeSocketHandle socket,
    bool readable,
    fd_set& selected,
    fd_set& exceptional,
    timeval& timeout) {
    if (socket >= FD_SETSIZE) {
        errno = EMFILE;
        return -1;
    }
    return select(
        socket + 1,
        readable ? &selected : nullptr,
        readable ? nullptr : &selected,
        &exceptional,
        &timeout);
}

int socket_error(NativeSocketHandle socket) {
    int error{};
    socklen_t length = sizeof(error);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return last_socket_error();
    }
    return error;
}

int send_native(NativeSocketHandle socket, const char* data, std::size_t size) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    return static_cast<int>(send(socket, data, size, flags));
}

int receive_native(NativeSocketHandle socket, char* data, std::size_t size) {
    return static_cast<int>(recv(socket, data, size, 0));
}

#endif

enum class WaitStatus {
    ready,
    timeout,
    error,
};

struct WaitResult {
    WaitStatus status{WaitStatus::error};
    int native_error{};
};

SocketRuntime& socket_runtime() {
    static SocketRuntime runtime;
    return runtime;
}

WaitResult wait_for_socket(
    NativeSocketHandle socket,
    bool readable,
    TimePoint deadline) {
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::microseconds::zero()) {
            return {WaitStatus::timeout, 0};
        }

        fd_set selected;
        fd_set exceptional;
        FD_ZERO(&selected);
        FD_ZERO(&exceptional);
        FD_SET(socket, &selected);
        FD_SET(socket, &exceptional);
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remaining.count() / 1'000'000);
        timeout.tv_usec = static_cast<long>(remaining.count() % 1'000'000);
        const auto result = select_socket(socket, readable, selected, exceptional, timeout);
        if (result == 0) {
            return {WaitStatus::timeout, 0};
        }
        if (result < 0) {
            const auto error = last_socket_error();
            if (operation_interrupted(error)) {
                continue;
            }
            return {WaitStatus::error, error};
        }
        const auto error = socket_error(socket);
        if (error != 0) {
            return {WaitStatus::error, error};
        }
        return {WaitStatus::ready, 0};
    }
}

struct AddressListGuard {
    addrinfo* addresses{};

    ~AddressListGuard() {
        if (addresses) {
            freeaddrinfo(addresses);
        }
    }
};

}  // namespace

TcpSocket::TcpSocket(NativeSocketHandle handle) : handle_(handle) {}

TcpSocket::~TcpSocket() {
    if (valid()) {
        close_socket(handle_);
    }
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = invalid_socket_handle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        if (valid()) {
            close_socket(handle_);
        }
        handle_ = other.handle_;
        other.handle_ = invalid_socket_handle;
    }
    return *this;
}

bool TcpSocket::valid() const noexcept {
    return handle_ != invalid_socket_handle;
}

NativeSocketHandle TcpSocket::native() const noexcept {
    return handle_;
}

int socket_runtime_error() {
    return socket_runtime().error();
}

bool valid_network_host(std::string_view host) {
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

std::string endpoint(std::string_view host, std::uint16_t port) {
    if (host.find(':') != std::string_view::npos) {
        return '[' + std::string(host) + "]:" + std::to_string(port);
    }
    return std::string(host) + ':' + std::to_string(port);
}

TcpConnectionResult connect_tcp(
    std::string_view host,
    std::uint16_t port,
    TimePoint deadline) {
    if (const auto runtime_error = socket_runtime_error(); runtime_error != 0) {
        return {{}, MonitorError::internal, runtime_error};
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* raw_addresses{};
    const auto service = std::to_string(port);
    const auto host_text = std::string(host);
    const auto resolution_error = getaddrinfo(
        host_text.c_str(), service.c_str(), &hints, &raw_addresses);
    AddressListGuard address_guard{raw_addresses};
    if (resolution_error != 0 || !raw_addresses) {
        return {{}, MonitorError::name_resolution, resolution_error};
    }

    int final_error{};
    for (auto* address = raw_addresses; address; address = address->ai_next) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return {{}, MonitorError::timeout, final_error};
        }
        TcpSocket socket(::socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol));
        if (!socket.valid()) {
            final_error = last_socket_error();
            continue;
        }
        if (!set_nonblocking(socket.native())) {
            final_error = last_socket_error();
            continue;
        }

        if (::connect(
                socket.native(),
                address->ai_addr,
                static_cast<int>(address->ai_addrlen)) == 0) {
            return {std::move(socket), MonitorError::none, 0};
        }
        final_error = last_socket_error();
        if (!connection_pending(final_error)) {
            continue;
        }

        const auto waited = wait_for_socket(socket.native(), false, deadline);
        if (waited.status == WaitStatus::ready) {
            return {std::move(socket), MonitorError::none, 0};
        }
        if (waited.status == WaitStatus::timeout) {
            return {{}, MonitorError::timeout, 0};
        }
        final_error = waited.native_error;
    }

    if (timeout_error(final_error) || std::chrono::steady_clock::now() >= deadline) {
        return {{}, MonitorError::timeout, final_error};
    }
    return {{}, MonitorError::connection, final_error};
}

SocketIoResult send_some(
    TcpSocket& socket,
    std::string_view data,
    TimePoint deadline) {
    if (data.empty()) {
        return {SocketIoStatus::success, 0U, 0};
    }
    while (true) {
        const auto sent = send_native(socket.native(), data.data(), data.size());
        if (sent >= 0) {
            return {SocketIoStatus::success, static_cast<std::size_t>(sent), 0};
        }
        const auto error = last_socket_error();
        if (operation_interrupted(error)) {
            continue;
        }
        if (!operation_would_block(error)) {
            return {SocketIoStatus::error, 0U, error};
        }
        const auto waited = wait_for_socket(socket.native(), false, deadline);
        if (waited.status == WaitStatus::timeout) {
            return {SocketIoStatus::timeout, 0U, 0};
        }
        if (waited.status == WaitStatus::error) {
            return {SocketIoStatus::error, 0U, waited.native_error};
        }
    }
}

SocketIoResult receive_some(
    TcpSocket& socket,
    std::span<char> buffer,
    TimePoint deadline) {
    if (buffer.empty()) {
        return {SocketIoStatus::success, 0U, 0};
    }
    while (true) {
        const auto received = receive_native(socket.native(), buffer.data(), buffer.size());
        if (received > 0) {
            return {SocketIoStatus::success, static_cast<std::size_t>(received), 0};
        }
        if (received == 0) {
            return {SocketIoStatus::closed, 0U, 0};
        }
        const auto error = last_socket_error();
        if (operation_interrupted(error)) {
            continue;
        }
        if (!operation_would_block(error)) {
            return {SocketIoStatus::error, 0U, error};
        }
        const auto waited = wait_for_socket(socket.native(), true, deadline);
        if (waited.status == WaitStatus::timeout) {
            return {SocketIoStatus::timeout, 0U, 0};
        }
        if (waited.status == WaitStatus::error) {
            return {SocketIoStatus::error, 0U, waited.native_error};
        }
    }
}

}  // namespace pixelstatus::host::detail
