#include "pixelstatus/host/http_monitor_runner.hpp"

#include "http_transport.hpp"
#include "pixelstatus/http_url.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace pixelstatus::host {
namespace {

using Json = nlohmann::json;
constexpr Duration maximum_timeout = std::chrono::seconds(30);
constexpr std::size_t maximum_response_bytes = 64U * 1024U;
constexpr std::size_t maximum_request_body_bytes = 16U * 1024U;
constexpr std::size_t maximum_header_count = 32U;
constexpr std::size_t maximum_header_name_bytes = 128U;
constexpr std::size_t maximum_header_value_bytes = 2U * 1024U;
constexpr std::size_t maximum_headers_bytes = 8U * 1024U;
constexpr std::size_t maximum_json_pointer_bytes = 512U;
constexpr std::size_t maximum_secret_name_bytes = 128U;

std::string_view http_method_name(HttpMethod method) {
    switch (method) {
        case HttpMethod::get:
            return "GET";
        case HttpMethod::head:
            return "HEAD";
        case HttpMethod::post:
            return "POST";
        case HttpMethod::put:
            return "PUT";
        case HttpMethod::patch:
            return "PATCH";
        case HttpMethod::delete_:
            return "DELETE";
    }
    return {};
}

std::string ascii_lower(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const auto character : value) {
        lowered.push_back(character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A'))
            : character);
    }
    return lowered;
}

bool valid_header_name(std::string_view name) {
    if (name.empty() || name.size() > maximum_header_name_bytes) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        const auto alpha_numeric = (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9');
        return alpha_numeric || character == '!' || character == '#'
            || character == '$' || character == '%' || character == '&'
            || character == static_cast<char>(0x27) || character == '*'
            || character == '+' || character == '-' || character == '.'
            || character == '^' || character == '_'
            || character == static_cast<char>(0x60) || character == '|'
            || character == '~';
    });
}

bool valid_header_value(std::string_view value) {
    if (value.size() > maximum_header_value_bytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte != 0x7FU;
    });
}

bool transport_owns_header(std::string_view lowered_name) {
    return lowered_name == "host" || lowered_name == "content-length"
        || lowered_name == "transfer-encoding" || lowered_name == "connection";
}

bool valid_secret_name(std::string_view name) {
    return !name.empty() && name.size() <= maximum_secret_name_bytes
        && std::all_of(name.begin(), name.end(), [](char character) {
            return (character >= 'A' && character <= 'Z')
                || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '.' || character == '_' || character == '-';
        });
}

std::optional<std::string> resolve_secret_template(
    std::string_view value,
    const SecretResolver& resolver,
    std::string& unresolved_name) {
    constexpr std::string_view prefix = "${secret:";
    std::string resolved;
    resolved.reserve(value.size());
    std::size_t offset{};
    while (true) {
        const auto begin = value.find(prefix, offset);
        if (begin == std::string_view::npos) {
            resolved.append(value.substr(offset));
            return resolved;
        }
        resolved.append(value.substr(offset, begin - offset));
        const auto name_begin = begin + prefix.size();
        const auto end = value.find('}', name_begin);
        const auto name = end == std::string_view::npos
            ? std::string_view{}
            : value.substr(name_begin, end - name_begin);
        if (end == std::string_view::npos || !valid_secret_name(name)) {
            unresolved_name = "<invalid>";
            return std::nullopt;
        }
        if (!resolver) {
            unresolved_name = std::string(name);
            return std::nullopt;
        }
        auto secret = resolver(name);
        if (!secret) {
            unresolved_name = std::string(name);
            return std::nullopt;
        }
        resolved.append(*secret);
        offset = end + 1U;
    }
}

bool valid_json_pointer(std::string_view pointer) {
    if (pointer.size() > maximum_json_pointer_bytes
        || (!pointer.empty() && pointer.front() != '/')) {
        return false;
    }
    for (std::size_t index = 0; index < pointer.size(); ++index) {
        if (pointer[index] == '~') {
            if (++index >= pointer.size()
                || (pointer[index] != '0' && pointer[index] != '1')) {
                return false;
            }
        }
    }
    return true;
}

MonitorError map_transport_error(httplib::Error error) {
    switch (error) {
        case httplib::Error::ConnectionTimeout:
        case httplib::Error::Timeout:
            return MonitorError::timeout;
        case httplib::Error::Connection:
        case httplib::Error::ProxyConnection:
        case httplib::Error::ConnectionClosed:
            return MonitorError::connection;
        case httplib::Error::SSLConnection:
        case httplib::Error::SSLLoadingCerts:
        case httplib::Error::SSLServerVerification:
        case httplib::Error::SSLServerHostnameVerification:
            return MonitorError::tls;
        case httplib::Error::ExceedMaxPayloadSize:
        case httplib::Error::ResourceExhaustion:
            return MonitorError::response_too_large;
        default:
            return MonitorError::protocol;
    }
}

std::optional<StateValue> json_scalar(const Json& value) {
    if (value.is_null()) {
        return StateValue{};
    }
    if (value.is_boolean()) {
        return StateValue{value.get<bool>()};
    }
    if (value.is_number_integer()) {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::nullopt;
            }
            return StateValue{static_cast<std::int64_t>(number)};
        }
        return StateValue{value.get<std::int64_t>()};
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number)) {
            return std::nullopt;
        }
        return StateValue{number};
    }
    if (value.is_string()) {
        return StateValue{value.get<std::string>()};
    }
    return std::nullopt;
}

std::optional<long double> json_number(const Json& value) {
    if (value.is_number_unsigned()) {
        return static_cast<long double>(value.get<std::uint64_t>());
    }
    if (value.is_number_integer()) {
        return static_cast<long double>(value.get<std::int64_t>());
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (std::isfinite(number)) {
            return static_cast<long double>(number);
        }
    }
    return std::nullopt;
}

HttpTransportResponse perform_httplib_request(
    const HttpMonitorConfig& config,
    const ParsedHttpUrl& url) {
    const auto begin = std::chrono::steady_clock::now();
    HttpTransportResponse result;
    httplib::Client client(url.base);
    if (!client.is_valid()) {
        result.error = MonitorError::connection;
        result.detail = "HTTP client could not parse or initialize the configured URL";
        result.observed_at = std::chrono::steady_clock::now();
        return result;
    }
    client.set_connection_timeout(config.timeout);
    client.set_read_timeout(config.timeout);
    client.set_write_timeout(config.timeout);
    client.set_max_timeout(config.timeout);
    client.set_follow_location(false);

    httplib::Headers headers;
    bool has_accept_header{};
    for (const auto& header : config.headers) {
        headers.emplace(header.name, header.value);
        has_accept_header = has_accept_header || ascii_lower(header.name) == "accept";
    }
    if (config.observation != HttpObservation::status_code
        && config.observation != HttpObservation::body && !has_accept_header) {
        headers.emplace("Accept", "application/json");
    }
    result.body.reserve(config.maximum_response_bytes);
    bool response_too_large{};
    httplib::Request request;
    request.method = http_method_name(config.method);
    request.path = url.target;
    request.headers = std::move(headers);
    request.body = config.body;
    request.response_handler =
        [&config, &response_too_large](const httplib::Response& incoming) {
            if (incoming.has_header("Content-Length")
                && incoming.get_header_value_u64("Content-Length")
                    > config.maximum_response_bytes) {
                response_too_large = true;
                return false;
            }
            return true;
        };
    request.content_receiver =
        [&config, &result, &response_too_large](
            const char* data,
            std::size_t length,
            std::size_t,
            std::size_t) {
            if (length > config.maximum_response_bytes - result.body.size()) {
                response_too_large = true;
                return false;
            }
            result.body.append(data, length);
            return true;
        };
    const auto response = client.send(request);
    const auto finished = std::chrono::steady_clock::now();
    result.observed_at = finished;
    result.latency = std::chrono::duration_cast<Duration>(finished - begin);
    if (!response) {
        if (response_too_large) {
            result.error = MonitorError::response_too_large;
            result.detail = "HTTP response exceeded the configured body limit";
            return result;
        }
        result.error = map_transport_error(response.error());
        result.detail = "HTTP request failed: " + httplib::to_string(response.error());
        return result;
    }
    result.success = true;
    result.status_code = static_cast<std::uint32_t>(response->status);
    return result;
}

class HttpMonitorRunner final : public MonitorRunner {
public:
    HttpMonitorRunner(HttpMonitorConfig config, ParsedHttpUrl url)
        : config_(std::move(config)), url_(std::move(url)) {}

    MonitorResult run(TimePoint) override {
        auto transport = url_.scheme == HttpScheme::http
            ? perform_httplib_request(config_, url_)
#ifdef _WIN32
            : perform_winhttp_request(config_);
#else
            : HttpTransportResponse{};
#endif
        MonitorResult result;
        result.observed_at = transport.observed_at;
        result.latency = transport.latency;
        if (!transport.success) {
            result.error = transport.error;
            result.detail = std::move(transport.detail);
            return result;
        }
        result.transport_success = true;
        result.detail = "HTTP " + std::to_string(transport.status_code);
        if (config_.observation == HttpObservation::status_code) {
            result.value = static_cast<std::int64_t>(transport.status_code);
            return result;
        }
        if (config_.observation == HttpObservation::body) {
            result.value = std::move(transport.body);
            return result;
        }

        const auto document = Json::parse(transport.body, nullptr, false, true);
        if (document.is_discarded()) {
            result.transport_success = false;
            result.error = MonitorError::invalid_response;
            result.detail += "; response body is not valid JSON";
            return result;
        }
        try {
            const auto pointer = Json::json_pointer(config_.json_pointer);
            if (!document.contains(pointer)) {
                result.value = std::monostate{};
                result.detail += "; JSON pointer was not found";
                return result;
            }
            const auto& selected = document.at(pointer);
            if (config_.observation == HttpObservation::json_array_length) {
                if (!selected.is_array()) {
                    result.transport_success = false;
                    result.error = MonitorError::invalid_response;
                    result.detail += "; selected JSON value is not an array";
                    return result;
                }
                result.value = static_cast<std::int64_t>(selected.size());
                return result;
            }
            if (config_.observation == HttpObservation::json_ratio) {
                const auto denominator_pointer =
                    Json::json_pointer(config_.json_denominator_pointer);
                if (!document.contains(denominator_pointer)) {
                    result.value = std::monostate{};
                    result.detail += "; JSON denominator pointer was not found";
                    return result;
                }
                const auto numerator = json_number(selected);
                const auto denominator = json_number(document.at(denominator_pointer));
                if (!numerator || !denominator || *denominator == 0.0L) {
                    result.transport_success = false;
                    result.error = MonitorError::invalid_response;
                    result.detail += "; JSON ratio requires numeric values and a nonzero denominator";
                    return result;
                }
                const auto ratio = *numerator / *denominator
                    * static_cast<long double>(config_.json_scale);
                const auto value = static_cast<double>(ratio);
                if (!std::isfinite(value)) {
                    result.transport_success = false;
                    result.error = MonitorError::invalid_response;
                    result.detail += "; JSON ratio result is not finite";
                    return result;
                }
                result.value = value;
                return result;
            }
            auto value = json_scalar(selected);
            if (!value) {
                result.transport_success = false;
                result.error = MonitorError::invalid_response;
                result.detail += "; selected JSON value is not a supported scalar";
                return result;
            }
            result.value = std::move(*value);
            return result;
        } catch (const std::exception& error) {
            result.transport_success = false;
            result.error = MonitorError::invalid_response;
            result.detail += "; JSON pointer evaluation failed: ";
            result.detail += error.what();
            return result;
        }
    }

private:
    HttpMonitorConfig config_;
    ParsedHttpUrl url_;
};

}  // namespace

MonitorRunnerCreationResult create_http_monitor_runner(
    HttpMonitorConfig config,
    const SecretResolver& secret_resolver) {
    auto url = parse_http_url(config.url);
    if (!url) {
        return {nullptr, "HTTP URL is invalid or unsupported by the desktop adapter"};
    }
#ifndef _WIN32
    if (url->scheme != HttpScheme::http) {
        return {nullptr, "HTTPS is not enabled in the current desktop HTTP adapter"};
    }
#endif
    if (http_method_name(config.method).empty()) {
        return {nullptr, "HTTP method is invalid"};
    }
    if (!config.body.empty()
        && (config.method == HttpMethod::get || config.method == HttpMethod::head)) {
        return {nullptr, "HTTP request body is not allowed with GET or HEAD"};
    }
    if (config.body.size() > maximum_request_body_bytes) {
        return {nullptr, "HTTP request body exceeds 16384 bytes"};
    }
    if (config.headers.size() > maximum_header_count) {
        return {nullptr, "HTTP request has more than 32 headers"};
    }
    std::unordered_set<std::string> header_names;
    std::size_t header_bytes{};
    for (auto& header : config.headers) {
        const auto lowered_name = ascii_lower(header.name);
        if (!valid_header_name(header.name) || transport_owns_header(lowered_name)
            || !header_names.emplace(lowered_name).second) {
            return {nullptr, "HTTP request contains an invalid header"};
        }
        std::string unresolved_name;
        auto resolved = resolve_secret_template(
            header.value, secret_resolver, unresolved_name);
        if (!resolved) {
            return {
                nullptr,
                "HTTP request header " + header.name
                    + " references unresolved secret " + unresolved_name,
            };
        }
        header.value = std::move(*resolved);
        if (!valid_header_value(header.value)) {
            return {nullptr, "HTTP request contains an invalid resolved header"};
        }
        header_bytes += header.name.size() + header.value.size();
    }
    if (header_bytes > maximum_headers_bytes) {
        return {nullptr, "HTTP request headers exceed 8192 bytes"};
    }
    if (config.timeout <= Duration::zero() || config.timeout > maximum_timeout) {
        return {nullptr, "HTTP timeout must be between 1ms and 30s"};
    }
    if (config.maximum_response_bytes == 0U
        || config.maximum_response_bytes > maximum_response_bytes) {
        return {nullptr, "HTTP response byte limit must be between 1 and 65536"};
    }
    if (config.observation != HttpObservation::status_code
        && config.observation != HttpObservation::body
        && config.observation != HttpObservation::json_pointer
        && config.observation != HttpObservation::json_array_length
        && config.observation != HttpObservation::json_ratio) {
        return {nullptr, "HTTP observation is invalid"};
    }
    if ((config.observation == HttpObservation::json_pointer
            || config.observation == HttpObservation::json_array_length
            || config.observation == HttpObservation::json_ratio)
        && !valid_json_pointer(config.json_pointer)) {
        return {nullptr, "HTTP JSON pointer is invalid"};
    }
    if (config.observation == HttpObservation::json_ratio
        && (!valid_json_pointer(config.json_denominator_pointer)
            || !std::isfinite(config.json_scale) || config.json_scale <= 0.0
            || config.json_scale > 1'000'000'000.0)) {
        return {nullptr, "HTTP JSON ratio configuration is invalid"};
    }
    return {
        std::make_unique<HttpMonitorRunner>(std::move(config), std::move(*url)),
        {},
    };
}

}  // namespace pixelstatus::host
