#include "pixelstatus/host/http_monitor_runner.hpp"

#include "pixelstatus/http_url.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace pixelstatus::host {
namespace {

using Json = nlohmann::json;

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

class HttpMonitorRunner final : public MonitorRunner {
public:
    HttpMonitorRunner(HttpMonitorConfig config, ParsedHttpUrl url)
        : config_(std::move(config)), url_(std::move(url)) {}

    MonitorResult run(TimePoint) override {
        const auto begin = std::chrono::steady_clock::now();
        MonitorResult result;
        httplib::Client client(url_.base);
        if (!client.is_valid()) {
            result.error = MonitorError::connection;
            result.detail = "HTTP client could not parse or initialize the configured URL";
            result.observed_at = std::chrono::steady_clock::now();
            return result;
        }
        client.set_connection_timeout(config_.timeout);
        client.set_read_timeout(config_.timeout);
        client.set_write_timeout(config_.timeout);
        client.set_max_timeout(config_.timeout);
        client.set_follow_location(false);

        httplib::Headers headers;
        if (config_.observation == HttpObservation::json_pointer) {
            headers.emplace("Accept", "application/json");
        }
        std::string response_body;
        response_body.reserve(config_.maximum_response_bytes);
        bool response_too_large{};
        const auto response = client.Get(
            url_.target,
            headers,
            [this, &response_too_large](const httplib::Response& incoming) {
                if (incoming.has_header("Content-Length")
                    && incoming.get_header_value_u64("Content-Length")
                        > config_.maximum_response_bytes) {
                    response_too_large = true;
                    return false;
                }
                return true;
            },
            [this, &response_body, &response_too_large](const char* data, std::size_t length) {
                if (length > config_.maximum_response_bytes - response_body.size()) {
                    response_too_large = true;
                    return false;
                }
                response_body.append(data, length);
                return true;
            });
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
        result.transport_success = true;
        result.detail = "HTTP " + std::to_string(response->status);
        if (config_.observation == HttpObservation::status_code) {
            result.value = static_cast<std::int64_t>(response->status);
            return result;
        }
        if (config_.observation == HttpObservation::body) {
            result.value = response_body;
            return result;
        }

        const auto document = Json::parse(response_body, nullptr, false, true);
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
            auto value = json_scalar(document.at(pointer));
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

MonitorRunnerCreationResult create_http_monitor_runner(HttpMonitorConfig config) {
    auto url = parse_http_url(config.url);
    if (!url) {
        return {nullptr, "HTTP URL is invalid or unsupported by the desktop adapter"};
    }
    if (url->scheme != HttpScheme::http) {
        return {nullptr, "HTTPS is not enabled in the current desktop HTTP adapter"};
    }
    return {
        std::make_unique<HttpMonitorRunner>(std::move(config), std::move(*url)),
        {},
    };
}

}  // namespace pixelstatus::host
