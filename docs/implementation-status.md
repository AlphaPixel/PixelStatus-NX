# Implementation Status and Loose Ends

This is the canonical progress ledger for PixelStatus NX. The root
[`README.md`](../README.md) remains the architectural specification; this document
records what exists now, what has been exercised against real systems, and what is
still planned. It was reconciled with the source, schema, examples, tests, and local
Windows deployment on 2026-08-22.

The evidence labels used below are:

- **Implemented and host-tested**: present in the repository and covered by
  deterministic Windows or portable-core tests.
- **Live-validated**: exercised against the user's actual network, services, or MI
  Matrix Display. Site-specific addresses, credentials, hardware identifiers, and
  acceptance details remain in Git-ignored local files.
- **Planned**: architectural intent only; configuration examples in the original
  specification are not promises that the feature already exists.

## Current Capability Matrix

| Area | Current state | Evidence and boundary |
| --- | --- | --- |
| Portable core | Implemented and host-tested | C++20 value/state types, steady-clock timing, TTL/stale handling, appearance timelines, renderer, framebuffer, output contract, configuration loader, and deterministic tests |
| Configuration | Implemented and host-tested | Versioned JSON schema plus runtime cross-field, geometry, duration, identifier, resource, and reference validation |
| Appearances | Implemented and host-tested | Solid, blink, toggle, fade, pulse, repeating or finite sequence, and linear or stepped color cycle |
| Presentation | Implemented and host-tested | Permanent indicator layouts or 1–32-card decks; bitmap, two-line clock, indicator, and composite layout cards; instant, fade, and four-direction slide transitions |
| Composite layout | Implemented and host-tested | Explicit AABBs; recursive row/column fixed and weighted splits; gaps; back-to-front stacks; indicator, clock, bar, status-grid, aggregate-status, and exact bitmap widgets |
| Monitor engine | Implemented and host-tested | Normalized results/errors, ordered first-match evaluation, interval scheduling, TTL publication, 256-monitor bound, per-monitor in-flight exclusion, and a reusable one-to-eight-worker Windows executor |
| Pull monitors | Implemented and host-tested on Windows | HTTP/HTTPS, TCP connect, ICMP IPv4 echo, bounded text TCP exchange, and DNS address resolution |
| HTTP observations | Implemented and host-tested | Status code, complete bounded body, RFC 6901 scalar pointer, JSON-array length, and scaled numeric ratio |
| Desktop TLS and secrets | Implemented and host-tested | WinHTTP/Schannel system trust and hostname validation; named header secrets from environment variables or the current user's Windows Credential Manager; redacted failures |
| Push state API | Implemented and host-tested | Bearer-authenticated loopback GET/POST status API with validation, replacement semantics, values, messages, and TTL |
| Desktop outputs | Implemented and host-tested | Native Win32 pixel window and headless/read-only browser display, both consuming the same logical framebuffer |
| Desktop development tooling | Implemented and host-tested | CMake host/core presets, root Visual Studio startup profiles for the display and monitor examples, focused PowerShell smoke tests, and four registered CTest suites |
| Browser backend | Implemented, host-tested, and used live | Arbitrary server-provided width/height, proportional square-pixel canvas, ETag polling, 16 ms–5 minute refresh control, hidden-tab pause, auto-hiding controls, popup request, and installable standalone manifest |
| Card-wide health layers | Implemented and live-validated | A subdued aggregate-status background can sit below bright per-node widgets; the local operations deck uses black/amber/red card health |
| UniFi | Implemented and live-validated on Windows | Loopback-only Python adapter for the official local Network API, explicit pre-auth leaf-certificate pin, Credential Manager API key, sanitized gateway/application/site/statistics output |
| OpenWrt/Starlink path | Implemented and live-validated on Windows | Restricted read-only rpcd adapter, explicit pre-auth certificate pin, sanitized Wi-Fi bridge association and DHCP/default-route health; this is router-path state, not dish telemetry |
| Windows MI display | Implemented and live-validated | Python/Bleak bridge, discovery, GATT validation, exact frame validation, transforms, pixel diffing, latest-frame coalescing, bounded reconnect, full restore, block packetization, and privacy-preserving heartbeat |
| MI physical protocol | Live-validated | Upper-left origin, row-major RGB, no rotation/mirroring, eight top-to-bottom two-row blocks, correct browser/physical parity; the observed panel visually updates at about 4 Hz |
| Windows background runtime | Implemented and live-validated | Immutable versioned releases below `%LOCALAPPDATA%`, atomic current/previous pointers, per-user interactive-logon Scheduled Task, restart policy, log rotation, collector/browser/MI health inspection |
| ESP32 | Planned | No ESP-IDF project, firmware adapters, cross-toolchain build, flash deployment, or hardware validation exists yet |

## Live Operations Profile

The ignored `examples/operations.local.json` profile currently runs an 8.5-second
three-card loop with instant changes, chosen because the physical panel refreshes
too slowly to show useful half-second dissolves:

1. AlphaPixel 16×16 bitmap for 2.5 seconds.
2. Eleven remote-server states in a 4×3 grid with one blank cell, above a UTC clock,
   for three seconds.
3. Six LAN/WAN states in a 3×2 grid above a local clock for three seconds.

The LAN/WAN sources are the UniFi gateway, TrueNAS UI, Netgear modem UI, printer,
OpenWrt Wi-Fi bridge, and Starlink router path. UniFi and OpenWrt are real sanitized
API-derived states; TrueNAS, Netgear, and the printer are currently reachability/UI
checks. Ten remote servers use HTTPS and one uses ICMP. Aggregate backgrounds are
black when healthy, subdued yellow/amber for warning or stale, and subdued red for
reported or communication failure, while individual cells keep their brighter
status colors.

The deck has passed its ignored live regression through all three frame layouts and
all configured monitors. It is also running from a published release through the
per-user task `AlphaPixel PixelStatus-NX`; the status command checks both collectors,
the advancing browser frame, and the MI write heartbeat without disclosing a BLE
address.

## Loose Ends and Unimplemented Work

The following list is ordered by practical value and dependency, not by the order in
which ideas appeared in the original specification.

### 1. Complete the Windows hardware reliability gate

- Force a disconnect during a block transfer and verify bounded rediscovery plus a
  full restore of the newest frame.
- Run a longer unattended BLE/collector/simulator soak and record write, reconnect,
  memory, and task-restart behavior.
- Determine whether graffiti and block modes may safely switch on one connection.
  Until then, mode choice remains explicit per run; no automatic sparse/full-frame
  crossover is implemented.
- Reconfirm the stable block delay and useful update ceiling. The current panel's
  observed visible rate is about 4 Hz, so the private hardware profile deliberately
  uses instant transitions even though the renderer still supports animation.
- A native C++/WinRT bridge is optional, not required for functionality; Python is
  intentionally the Windows adapter and will not be used in ESP32 firmware.

### 2. Deepen appliance telemetry

- Replace the TrueNAS UI reachability check with a least-privilege CORE 13 API
  profile for pool health, active alerts, disk health, and utilization after matching
  the installed `/api/docs/` schemas.
- Decide whether Netgear modem reachability is sufficient. Rich channel or signal
  telemetry requires model/firmware discovery and probably a bounded LAN proxy
  because there is no assumed stable public API.
- Add Starlink dish data—obstruction, latency, alignment, and outages—through the
  official gRPC/protobuf API. Routing or a narrow proxy is needed because the dish
  and cable modem may both use `192.168.100.1` on different paths.
- The tested UniFi API exposes configured WAN identities and aggregate gateway
  statistics, but not independent live health for every WAN in the observed shape.
  Combine direct probes or another bounded source if per-link health is required.

### 3. Finish generic host monitoring policies

- Add per-monitor custom CA selection and certificate/SPKI pin policies to the C++
  HTTP configuration. The UniFi/OpenWrt sidecars already perform their own explicit
  leaf pin before authentication; this gap applies to generic monitors.
- Add deterministic TLS fixtures for trusted, unknown-CA, hostname-mismatch, pin
  match, and pin rollover behavior.
- Cookie/session authentication, client certificates, HTTP header or latency
  observations, DNS record-type queries, binary/multi-step TCP exchange, jitter,
  cron-like schedules, and cancellation of an already-blocking OS request remain
  unimplemented.
- Standalone TLS-certificate-expiry, SNMP v1/v2c GET, MQTT push, UDP, NTP, mDNS,
  and Modbus/TCP are specification backlog, not current monitor types.

### 4. Harden desktop deployment and management

- Published releases accumulate by design. Add an explicit safe retention command
  only if disk use warrants it.
- `current.previous.json` records the prior selection, but there is no one-command
  rollback or automatic health-based rollback.
- Publication records the existing Python executable and verifies Bleak; it does not
  bundle or install Python/Bleak. A future installer could make the runtime
  self-contained while retaining user configuration and credentials outside Program
  Files.
- The Scheduled Task requires an interactive logged-on user for BLE and Credential
  Manager. Logged-out service operation is not implemented.
- The browser display is read-only and unauthenticated. Keep its default loopback
  binding; TLS/authentication or a trusted reverse proxy is required before making
  it a permanent non-local service.
- There is no web configuration editor, monitor-list API, configuration mutation
  API, historical database, or alert-notification system.
- There is no SDL2 output. The native Win32 window and browser backend fulfill the
  current desktop-emulator requirement; SDL2 is optional if a portable native
  desktop output later becomes valuable.

### 5. Build the ESP32 production target

- Select and pin ESP-IDF, create the application/component build, and keep the
  portable C++ behavior covered by shared golden tests.
- Add Wi-Fi provisioning/recovery, SNTP, bounded FreeRTOS scheduling/execution,
  NVS/LittleFS persistence, encrypted secret storage, logging, watchdog behavior,
  and OTA with rollback.
- Implement native network adapters with `esp_http_client`/ESP-TLS/mbedTLS and the
  ESP-IDF ping facilities while preserving the host `MonitorRunner` contract.
- Implement the MI output with ESP-IDF NimBLE, including the Windows-validated
  packet vectors, reconnect/full-restore behavior, and output-health publication.
- Implement direct WS2812/SK6812 output and geometry/color-order mapping if that
  hardware path remains in V1 scope.
- Validate RAM, flash, socket, TLS-session, stack, power-loss, reconnect, and long-
  duration behavior on actual hardware. None of this requires Python in firmware.

## Validation Commands

The public host regression is:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

The configured CTest preset currently includes the portable/C++ suite and the MI
BLE, UniFi, and OpenWrt Python suites. Focused browser, card, layout, monitor, and
fixture smoke tests are listed in [Host-First Development](host-development.md).
The private live regression is `tools\test-operations.local.ps1`; it and its input
profile are intentionally ignored by Git.
