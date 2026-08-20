#pragma once

#include "pixelstatus/appearance.hpp"
#include "pixelstatus/color.hpp"
#include "pixelstatus/evaluator.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pixelstatus {

struct DisplayConfig {
    std::size_t width{16};
    std::size_t height{16};
    Rgb background{};
};

struct IndicatorConfig {
    std::string id;
    std::string source;
    std::size_t x{};
    std::size_t y{};
    std::size_t width{1};
    std::size_t height{1};
};

enum class HttpObservation {
    status_code,
    body,
    json_pointer,
};

enum class HttpMethod {
    get,
    head,
    post,
    put,
    patch,
    delete_,
};

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpMonitorConfig {
    std::string url;
    HttpMethod method{HttpMethod::get};
    std::vector<HttpHeader> headers;
    std::string body;
    Duration timeout{std::chrono::seconds(2)};
    std::size_t maximum_response_bytes{4U * 1024U};
    HttpObservation observation{HttpObservation::status_code};
    std::string json_pointer;
};

struct TcpConnectMonitorConfig {
    std::string host;
    std::uint16_t port{};
    Duration timeout{std::chrono::seconds(2)};
};

enum class TcpExchangeObservation {
    body,
    latency_ms,
};

struct TcpExchangeMonitorConfig {
    std::string host;
    std::uint16_t port{};
    Duration timeout{std::chrono::seconds(2)};
    std::string send;
    std::string read_until;
    std::size_t maximum_response_bytes{4U * 1024U};
    TcpExchangeObservation observation{TcpExchangeObservation::body};
};

enum class DnsAddressFamily {
    any,
    ipv4,
    ipv6,
};

enum class DnsObservation {
    addresses,
    address_count,
    latency_ms,
};

struct DnsMonitorConfig {
    std::string host;
    DnsAddressFamily family{DnsAddressFamily::any};
    DnsObservation observation{DnsObservation::addresses};
    Duration timeout{std::chrono::seconds(2)};
};

using MonitorSourceConfig = std::variant<
    HttpMonitorConfig,
    TcpConnectMonitorConfig,
    TcpExchangeMonitorConfig,
    DnsMonitorConfig>;

struct PullMonitorConfig {
    std::string id;
    Duration interval{};
    std::optional<Duration> ttl;
    EvaluationPolicy evaluation;
    MonitorSourceConfig source;
};

struct AppConfig {
    int schema_version{1};
    DisplayConfig display;
    std::unordered_map<std::string, TimelineAppearance> statuses;
    std::vector<PullMonitorConfig> monitors;
    std::vector<IndicatorConfig> indicators;
};

struct ConfigLoadResult {
    std::optional<AppConfig> config;
    std::vector<std::string> errors;

    [[nodiscard]] explicit operator bool() const noexcept {
        return config.has_value() && errors.empty();
    }
};

[[nodiscard]] ConfigLoadResult load_config_file(const std::filesystem::path& path);
[[nodiscard]] std::optional<Duration> parse_duration(std::string_view value);

}  // namespace pixelstatus
