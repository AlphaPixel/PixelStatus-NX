# OpenWrt Starlink Bridge Monitoring

The Windows host can query a deliberately restricted OpenWrt rpcd account through
the Python collector in `tools/openwrt`. It publishes one bounded, sanitized JSON
document at `http://127.0.0.1:18951/health`. Two ordinary PixelStatus HTTP monitors
can consume independent JSON pointers from that document:

- `/bridge/monitor/status` describes the OpenWrt Wi-Fi station and routed uplink;
- `/starlink/monitor/status` describes the DHCP/default-route path supplied by the
  Starlink router.

This adapter is implemented, covered by six standard-library unit tests, and
live-validated against the user's bridge. It supplies the last two cells of the
current LAN/WAN card. Dish-specific work is listed in
[Implementation Status and Loose Ends](implementation-status.md).

The collector is loopback-only and uses only the Python standard library. It
verifies an explicit SHA-256 pin for the OpenWrt HTTPS leaf certificate before
sending the rpcd username or password. Redirects are not followed. The public JSON
omits IP addresses, routes, SSID, BSSID, MAC addresses, serial numbers, session
tokens, ACL details, and credentials.

## Restricted rpcd Account

Save the rpcd password in Windows Credential Manager as the generic credential
`PixelStatus-NX/openwrt-api-password`. Its username is normally `pixelstatus`.
Never put the password in configuration, a launch command, or a tracked file.

The account needs read access only to these calls:

| Object | Methods | Purpose |
| --- | --- | --- |
| `system` | `board`, `info` | Model, firmware, uptime, load, and memory |
| `network.interface` | `dump`, `status` | Uplink state, address presence, route presence, and uptime |
| `network.device` | `status` | Uplink device state |
| `iwinfo` | `devices`, `info`, `assoclist` | Station association and radio quality |

Do not grant UCI, file execution, service, reboot, interface up/down, or wireless
configuration access. In particular, the collector does not call the broad
wireless-status method because that response can contain a configured PSK.

Obtain the bridge's leaf-certificate fingerprint from a trusted local observation
and compare it before accepting it. A certificate renewal deliberately stops
authenticated collection until the replacement fingerprint is verified.

Probe once:

```powershell
.\tools\start-openwrt-monitor.ps1 `
    -Probe `
    -Url 'https://bridge.example/ubus' `
    -CertificateSha256 '<64 hexadecimal digits>'
```

Run the loopback collector:

```powershell
.\tools\start-openwrt-monitor.ps1 `
    -Url 'https://bridge.example/ubus' `
    -CertificateSha256 '<64 hexadecimal digits>'
```

## Status Semantics

Bridge `ok` means the configured uplink interface is up, owns an IPv4 address, has
a default route, and the associated Wi-Fi station is observable. If the routed
path is ready but the radio association detail is incomplete, bridge status is
`warn`; loss of the routed path is `fail`.

Starlink `ok` currently means OpenWrt has a live DHCP/default-route path toward the
Starlink router. This is real path state rather than a placeholder, but it is not
dish telemetry. The document labels this boundary as
`telemetry_scope: "router-path"`. Obstruction, dish latency, alignment, and outage
details require a later, narrowly scoped proxy from the Starlink-side networks to
the host collector. A proxy is preferable here because the Starlink dish commonly
uses `192.168.100.1`, which can overlap a cable-modem management address on the
primary LAN.

On an ESP32, a native adapter should make the same HTTPS ubus calls through
ESP-TLS/mbedTLS, retrieve its credential from protected NVS, and publish the same
sanitized shape. Python remains a desktop development adapter, not a firmware
dependency.

References:

- [OpenWrt ubus session and ACL documentation](https://openwrt.org/docs/guide-developer/ubus/session)
- [OpenWrt ubus technical reference](https://openwrt.org/docs/techref/ubus)
