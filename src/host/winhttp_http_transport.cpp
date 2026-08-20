#include "http_transport.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace pixelstatus::host {
namespace {

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
    ~InternetHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept
        : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HINTERNET get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    HINTERNET handle_{};
};

struct WinHttpUrl {
    bool secure{};
    INTERNET_PORT port{};
    std::wstring host;
    std::wstring target;
};

std::optional<std::wstring> utf8_to_wide(std::string_view value) {
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    if (value.empty()) {
        return std::wstring{};
    }
    const auto length = static_cast<int>(value.size());
    const auto required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            length,
            converted.data(),
            required)
        != required) {
        return std::nullopt;
    }
    return converted;
}

std::optional<WinHttpUrl> crack_url(std::string_view value) {
    const auto wide = utf8_to_wide(value);
    if (!wide || wide->size() > (std::numeric_limits<DWORD>::max)()) {
        return std::nullopt;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(
            wide->c_str(),
            static_cast<DWORD>(wide->size()),
            0,
            &components)) {
        return std::nullopt;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP
        && components.nScheme != INTERNET_SCHEME_HTTPS) {
        return std::nullopt;
    }

    WinHttpUrl result;
    result.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    result.port = components.nPort;
    result.host.assign(components.lpszHostName, components.dwHostNameLength);
    if (components.dwUrlPathLength == 0U) {
        result.target = L"/";
    } else {
        result.target.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.dwExtraInfoLength != 0U) {
        result.target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    return result;
}

std::wstring_view method_name(HttpMethod method) {
    switch (method) {
        case HttpMethod::get:
            return L"GET";
        case HttpMethod::head:
            return L"HEAD";
        case HttpMethod::post:
            return L"POST";
        case HttpMethod::put:
            return L"PUT";
        case HttpMethod::patch:
            return L"PATCH";
        case HttpMethod::delete_:
            return L"DELETE";
    }
    return {};
}

bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character + ('a' - 'A'))
                : character;
        };
        if (lower(left[index]) != lower(right[index])) {
            return false;
        }
    }
    return true;
}

MonitorError map_winhttp_error(DWORD error, bool secure) {
    switch (error) {
        case ERROR_WINHTTP_TIMEOUT:
            return MonitorError::timeout;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return MonitorError::name_resolution;
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return MonitorError::connection;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        case ERROR_WINHTTP_CLIENT_CERT_NO_PRIVATE_KEY:
        case ERROR_WINHTTP_CLIENT_CERT_NO_ACCESS_PRIVATE_KEY:
        case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED_PROXY:
        case ERROR_WINHTTP_SECURE_FAILURE_PROXY:
            return MonitorError::tls;
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
            return secure ? MonitorError::tls : MonitorError::protocol;
        default:
            return MonitorError::protocol;
    }
}

HttpTransportResponse failure(
    std::chrono::steady_clock::time_point begin,
    DWORD error,
    bool secure,
    std::string_view operation) {
    const auto finished = std::chrono::steady_clock::now();
    HttpTransportResponse result;
    result.latency = std::chrono::duration_cast<Duration>(finished - begin);
    result.observed_at = finished;
    result.error = map_winhttp_error(error, secure);
    result.detail = secure ? "HTTPS " : "HTTP ";
    result.detail.append(operation);
    result.detail += " failed (WinHTTP error " + std::to_string(error) + ")";
    return result;
}

}  // namespace

HttpTransportResponse perform_winhttp_request(const HttpMonitorConfig& config) {
    const auto begin = std::chrono::steady_clock::now();
    const auto url = crack_url(config.url);
    if (!url) {
        return failure(begin, ERROR_WINHTTP_INVALID_URL, false, "URL parsing");
    }

    const InternetHandle session(WinHttpOpen(
        L"PixelStatus-NX/0.1",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        return failure(begin, GetLastError(), url->secure, "session creation");
    }

    const auto timeout = static_cast<int>(config.timeout.count());
    if (!WinHttpSetTimeouts(
            session.get(), timeout, timeout, timeout, timeout)) {
        return failure(begin, GetLastError(), url->secure, "timeout setup");
    }

    const InternetHandle connection(WinHttpConnect(
        session.get(), url->host.c_str(), url->port, 0));
    if (!connection) {
        return failure(begin, GetLastError(), url->secure, "connection setup");
    }

    const auto method = method_name(config.method);
    const InternetHandle request(WinHttpOpenRequest(
        connection.get(),
        std::wstring(method).c_str(),
        url->target.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        url->secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        return failure(begin, GetLastError(), url->secure, "request creation");
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(
            request.get(),
            WINHTTP_OPTION_REDIRECT_POLICY,
            &redirect_policy,
            sizeof(redirect_policy))) {
        return failure(begin, GetLastError(), url->secure, "redirect setup");
    }

    bool has_accept_header{};
    for (const auto& header : config.headers) {
        has_accept_header =
            has_accept_header || ascii_iequals(header.name, "accept");
        const auto name = utf8_to_wide(header.name);
        const auto value = utf8_to_wide(header.value);
        if (!name || !value) {
            return failure(begin, ERROR_NO_UNICODE_TRANSLATION, url->secure, "header conversion");
        }
        const auto line = *name + L": " + *value;
        if (!WinHttpAddRequestHeaders(
                request.get(),
                line.c_str(),
                static_cast<DWORD>(line.size()),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            return failure(begin, GetLastError(), url->secure, "header setup");
        }
    }
    if (config.observation == HttpObservation::json_pointer && !has_accept_header) {
        constexpr std::wstring_view accept = L"Accept: application/json";
        if (!WinHttpAddRequestHeaders(
                request.get(),
                accept.data(),
                static_cast<DWORD>(accept.size()),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            return failure(begin, GetLastError(), url->secure, "header setup");
        }
    }

    auto* body = config.body.empty()
        ? WINHTTP_NO_REQUEST_DATA
        : const_cast<char*>(config.body.data());
    const auto body_size = static_cast<DWORD>(config.body.size());
    if (!WinHttpSendRequest(
            request.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            body,
            body_size,
            body_size,
            0)) {
        return failure(begin, GetLastError(), url->secure, "request send");
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        return failure(begin, GetLastError(), url->secure, "response receive");
    }

    DWORD status_code{};
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        return failure(begin, GetLastError(), url->secure, "status parsing");
    }

    std::string response_body;
    response_body.reserve(config.maximum_response_bytes);
    std::array<char, 4096U> buffer{};
    while (true) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            return failure(begin, GetLastError(), url->secure, "response reading");
        }
        if (available == 0U) {
            break;
        }
        if (available > config.maximum_response_bytes - response_body.size()) {
            const auto finished = std::chrono::steady_clock::now();
            HttpTransportResponse too_large;
            too_large.latency =
                std::chrono::duration_cast<Duration>(finished - begin);
            too_large.observed_at = finished;
            too_large.error = MonitorError::response_too_large;
            too_large.detail =
                "HTTP response exceeded the configured body limit";
            return too_large;
        }
        const auto requested = (std::min)(
            available, static_cast<DWORD>(buffer.size()));
        DWORD received{};
        if (!WinHttpReadData(request.get(), buffer.data(), requested, &received)) {
            return failure(begin, GetLastError(), url->secure, "response reading");
        }
        if (received == 0U) {
            break;
        }
        response_body.append(buffer.data(), received);
    }

    const auto finished = std::chrono::steady_clock::now();
    HttpTransportResponse result;
    result.success = true;
    result.status_code = status_code;
    result.body = std::move(response_body);
    result.latency = std::chrono::duration_cast<Duration>(finished - begin);
    result.observed_at = finished;
    return result;
}

}  // namespace pixelstatus::host
