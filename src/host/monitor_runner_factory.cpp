#include "pixelstatus/host/monitor_runner_factory.hpp"

#include "pixelstatus/host/dns_monitor_runner.hpp"
#include "pixelstatus/host/http_monitor_runner.hpp"
#include "pixelstatus/host/icmp_ping_monitor_runner.hpp"
#include "pixelstatus/host/tcp_connect_monitor_runner.hpp"
#include "pixelstatus/host/tcp_exchange_monitor_runner.hpp"

#include <type_traits>
#include <utility>
#include <variant>

namespace pixelstatus::host {

MonitorRunnerCreationResult create_monitor_runner(
    MonitorSourceConfig config,
    const SecretResolver& secret_resolver) {
    return std::visit([&secret_resolver](auto source) -> MonitorRunnerCreationResult {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, HttpMonitorConfig>) {
            return create_http_monitor_runner(std::move(source), secret_resolver);
        } else if constexpr (std::is_same_v<Source, TcpConnectMonitorConfig>) {
            return create_tcp_connect_monitor_runner(std::move(source));
        } else if constexpr (std::is_same_v<Source, IcmpPingMonitorConfig>) {
            return create_icmp_ping_monitor_runner(std::move(source));
        } else if constexpr (std::is_same_v<Source, TcpExchangeMonitorConfig>) {
            return create_tcp_exchange_monitor_runner(std::move(source));
        } else {
            return create_dns_monitor_runner(std::move(source));
        }
    }, std::move(config));
}

MonitorRunnerCreationResult create_monitor_runner(MonitorSourceConfig config) {
    const auto resolver = system_secret_resolver();
    return create_monitor_runner(std::move(config), resolver);
}

}  // namespace pixelstatus::host
