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

enum class CardTransition {
    instant,
    fade,
    slide_left,
    slide_right,
    slide_up,
    slide_down,
};

struct CardTransitionConfig {
    CardTransition type{CardTransition::instant};
    Duration duration{};
};

struct BitmapCardConfig {
    std::unordered_map<char, Rgb> palette;
    std::vector<std::string> pixels;
};

struct ClockCardConfig {
    Rgb local_color{0x00, 0xB0, 0xFF};
    Rgb utc_color{0xFF, 0xD6, 0x00};
};

struct IndicatorCardConfig {
    std::vector<IndicatorConfig> indicators;
};

enum class ClockTimeZone {
    local,
    utc,
};

struct LayoutClockConfig {
    std::string id;
    std::size_t x{};
    std::size_t y{};
    std::size_t width{15};
    std::size_t height{7};
    ClockTimeZone timezone{ClockTimeZone::local};
    Rgb color{0x00, 0xB0, 0xFF};
};

enum class BarDirection {
    right,
    left,
    up,
    down,
};

struct LayoutBarConfig {
    std::string id;
    std::string source;
    std::size_t x{};
    std::size_t y{};
    std::size_t width{1};
    std::size_t height{1};
    BarDirection direction{BarDirection::right};
    double minimum{};
    double maximum{100.0};
    Rgb track_color{};
};

struct LayoutStatusGridConfig {
    std::string id;
    std::vector<std::string> sources;
    std::size_t x{};
    std::size_t y{};
    std::size_t width{1};
    std::size_t height{1};
    std::size_t columns{1};
    std::size_t gap{};
};

struct LayoutBitmapConfig {
    std::string id;
    std::size_t x{};
    std::size_t y{};
    std::size_t width{1};
    std::size_t height{1};
    std::unordered_map<char, Rgb> palette;
    std::vector<std::string> pixels;
};

using LayoutWidgetConfig = std::variant<
    IndicatorConfig,
    LayoutClockConfig,
    LayoutBarConfig,
    LayoutStatusGridConfig,
    LayoutBitmapConfig>;

struct LayoutCardConfig {
    std::vector<LayoutWidgetConfig> widgets;
};

using CardContentConfig = std::variant<
    BitmapCardConfig,
    ClockCardConfig,
    IndicatorCardConfig,
    LayoutCardConfig>;

struct CardConfig {
    std::string id;
    Duration hold{std::chrono::seconds(5)};
    CardTransitionConfig transition;
    CardContentConfig content;
};

enum class HttpObservation {
    status_code,
    body,
    json_pointer,
    json_array_length,
    json_ratio,
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
    std::string json_denominator_pointer;
    double json_scale{1.0};
};

struct TcpConnectMonitorConfig {
    std::string host;
    std::uint16_t port{};
    Duration timeout{std::chrono::seconds(2)};
};

struct IcmpPingMonitorConfig {
    std::string host;
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
    IcmpPingMonitorConfig,
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
    std::vector<CardConfig> cards;
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
