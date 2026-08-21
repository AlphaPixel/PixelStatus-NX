# Host-First Development

PixelStatus NX is designed so that monitoring, state, appearance, layout, rendering,
and logical-frame behavior can be completed and tested on a desktop before an ESP32
toolchain or device is introduced.

The current host target is 64-bit Windows using Visual Studio 2022 and CMake. The
simulator uses Win32 GDI and has no runtime graphics dependency.

## Build

From a PowerShell prompt in the repository root:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

To compile and test only the portable core without the desktop network/display
adapters or Win32 simulator, use the companion preset:

```powershell
cmake --preset windows-core-debug
cmake --build --preset windows-core-debug
ctest --preset windows-core-debug
```

The equivalent one-command workflow is:

```powershell
.\tools\build-host.ps1
```

## Visual Studio Debug Profiles

Open the repository folder directly in Visual Studio 2022 and select the
`windows-debug` configure/build preset. The repository-root [`launch.vs.json`](../launch.vs.json)
adds these shared entries to the **Startup Item** dropdown:

| Startup item | Behavior | Endpoints |
| --- | --- | --- |
| `Simulator - Native + Browser` | Sample configuration with both output drivers | Status API `18787`, browser `18788` |
| `Simulator - Browser Only (30 FPS)` | Headless sample display at a 33 ms client default | Status API `18797`, browser `18798` |
| `Simulator - Native Only (Push API)` | Win32 output without the browser server | Status API `18807` |
| `Simulator - HTTP Monitor (Native + Browser)` | HTTP/JSON monitor example with both outputs | Status API `18817`, browser `18818` |
| `Simulator - HTTP Request Monitor` | Authenticated JSON POST with custom headers and body | Status API `18877`, browser `18878` |
| `Simulator - HTTPS Monitor` | Public HTTPS system-trust check | Status API `18887`, browser `18888` |
| `Simulator - Card Deck (Real Monitors)` | Logo, local/UTC clock, LAN/WAN, and public-server cards | Status API `18897`, browser `18898` |
| `Simulator - AABB Layout Card` | Composite status regions and a bounded UTC clock | Status API `18917`, browser `18918` |
| `Simulator - Local Operations` | Optional ignored site-specific profile | Status API `18907`, browser `18908` |
| `Simulator - Concurrent HTTP Monitors` | Headless two-monitor slow/fast concurrency example | Status API `18837`, browser `18838` |
| `Simulator - TCP Connect Monitor` | Headless TCP-connect latency example | Status API `18847`, browser `18848` |
| `Simulator - DNS Monitor` | Headless localhost IPv4 resolution example | Status API `18857`, browser `18858` |
| `Simulator - TCP Exchange Monitor` | Headless bounded request/response example | Status API `18867`, browser `18868` |
| `Simulator - Timed Browser Smoke (10 seconds)` | Headless, API-disabled, 60 FPS browser exercise that exits automatically | Browser `18828` |
| `Tests - Active CMake Preset` | Runs `pixelstatus_tests` under the debugger | None |

The simulator entries require the `windows-debug` preset because the core-only
preset does not build that executable. The tests entry follows the active preset:
use `windows-debug` for the complete host suite or `windows-core-debug` for the
portable suite without host adapter code.

Before starting any HTTP- or TCP-monitor profile, run its local upstream fixture in a
separate terminal:

```powershell
python .\tools\mock-health-server.py
```

The shared development bearer token used by the relevant profiles is
`pixelstatus-vs-debug`. It is deliberately non-secret and is valid only for the
locally launched debug process.

The HTTP Request profile also supplies
`PIXELSTATUS_SECRET_HTTP_REQUEST_TOKEN=desktop-example` as a deliberately
non-secret fixture. It exercises the same named-secret resolution used by
production headers.

Visual Studio normally discovers root launch profiles automatically. If they are
hidden, choose **Show/Hide Debug Targets** from the Startup Item dropdown. A local
`.vs\launch.vs.json` takes precedence over root entries with the same name; the
repository intentionally leaves `.vs` ignored because it also contains per-user
indexes and workspace state.

After building, run the repeatable simulator/API integration smoke test with:

```powershell
.\tools\test-host-api.ps1
```

The pull-monitor smoke test starts a local Python HTTP fixture and confirms that the
configured Win32 simulator publishes its JSON-derived state through the status API:

```powershell
.\tools\test-host-monitor.ps1
```

The authenticated-request smoke test verifies `POST`, explicit headers, a JSON
request body, environment-secret resolution, Boolean JSON extraction, evaluation,
and state publication:

```powershell
.\tools\test-http-request.ps1
```

When Internet access is available, the HTTPS smoke test verifies a successful
WinHTTP/Schannel handshake through the system trust store:

```powershell
.\tools\test-https-monitor.ps1
```

The card-deck smoke test runs the browser backend, confirms that the frame advances
away from the bitmap logo, and waits for both the localhost DNS and public HTTPS
monitors to report healthy states:

```powershell
.\tools\test-card-deck.ps1
```

The AABB layout-card smoke test verifies mixed status and clock widgets through the
browser framebuffer API without network fixtures:

```powershell
.\tools\test-layout-card.ps1
```

Site-specific monitoring can be exercised with the ignored
`examples\operations.local.json` profile and its ignored companion test:

```powershell
.\tools\test-operations.local.ps1
```

Both paths are intentionally excluded from Git. Keep LAN addresses, private
hostnames, credentials, and environment-specific assertions in those local files.

The browser-display smoke test runs the renderer headlessly and verifies its HTML,
manifest, and current-frame API:

```powershell
.\tools\test-web-display.ps1
```

The concurrency smoke test holds one local HTTP request for 2.5 seconds and verifies
that a second worker publishes the fast monitor immediately while the status API and
browser display remain responsive:

```powershell
.\tools\test-monitor-concurrency.ps1
```

The TCP-connect smoke test verifies configuration, connection latency observation,
state publication, and the generic runner factory against the same local fixture:

```powershell
.\tools\test-tcp-monitor.ps1
```

The DNS smoke test verifies address-family selection, resolution, evaluation, and
state publication without requiring an external fixture:

```powershell
.\tools\test-dns-monitor.ps1
```

The TCP-exchange smoke test sends a raw request to the local fixture, reads the
response headers through their delimiter, evaluates the response text, and verifies
the published state:

```powershell
.\tools\test-tcp-exchange.ps1
```

Python is used only for the mock upstream fixture; the monitor implementations and
all application behavior under test are C++20.

CMake downloads the pinned `nlohmann/json` 3.12.0 and `cpp-httplib` 0.51.0
releases into the ignored build tree. No dependency is installed globally.

The canonical configuration contract is
[`schemas/pixelstatus-config-v1.schema.json`](../schemas/pixelstatus-config-v1.schema.json).
Runtime validation also enforces display bounds and unique indicator IDs that cannot
be expressed conveniently in the schema alone.

## Run the Simulator

```powershell
.\out\build\windows-debug\Debug\pixelstatus_simulator.exe
```

The sample configuration is copied beside the executable. A different configuration
can be supplied as the first argument:

```powershell
.\out\build\windows-debug\Debug\pixelstatus_simulator.exe .\examples\pixelstatus.sample.json
```

For an automated graphical smoke test, close the simulator after a fixed interval:

```powershell
.\out\build\windows-debug\Debug\pixelstatus_simulator.exe --run-for-ms 2000
```

The simulator also accepts:

```text
--api-port N        listen on a different loopback port (default: 8787)
--api-token TOKEN   use a stable development bearer token
--no-api            disable the HTTP status input
--web-display-port N          browser-display port (default: 8788)
--web-display-bind ADDRESS    browser-display interface (default: 127.0.0.1)
--web-refresh-ms N            default browser polling period (16–300000 ms)
--no-web-display              disable browser output
--no-window                   run without the native Win32 output
--monitor-workers N           bounded pull-monitor workers (default: 2, maximum: 8)
```

The browser display is available at `http://127.0.0.1:8788/` by default. It can run
beside the native window or as the only output with `--no-window`. See
[Browser Display Backend](http-display.md) for its read-only frame API, refresh
control, responsive scaling, and standalone-window behavior.

## Push Statuses From the Host

The simulator exposes the production-shaped status API only on `127.0.0.1`. If no
token is supplied, it generates and prints a new 64-hex-character token at startup.
For repeatable local testing, start it with an explicit token:

```powershell
.\out\build\windows-debug\Debug\pixelstatus_simulator.exe --api-token local-development-token
```

In a second PowerShell prompt, push and then query a state:

```powershell
$headers = @{ Authorization = 'Bearer local-development-token' }
$body = @{
    status = 'fail'
    value = 7
    message = 'Desktop end-to-end test'
    ttl = 30
} | ConvertTo-Json

Invoke-RestMethod `
    -Uri 'http://127.0.0.1:8787/api/v1/status/build' `
    -Method Post `
    -Headers $headers `
    -ContentType 'application/json' `
    -Body $body

Invoke-RestMethod `
    -Uri 'http://127.0.0.1:8787/api/v1/status/build' `
    -Headers $headers
```

`POST /api/v1/status` also works when the JSON body contains `id`. `ttl` is an
optional positive integer in seconds. The body is limited to 4 KiB and scalar
values are limited to null, Boolean, integer, finite floating point, or string.
The default portable limits allow 256 states, 64-byte IDs and status names, a
512-byte message, a 1 KiB string value, and a maximum TTL of seven days.
See [Configuration V1](configuration-v1.md) for the status and appearance model.

## Portable Pull-Monitor Foundation

The core library contains host-tested runner, evaluator, and deterministic
interval-engine contracts. Scripted runners exercise time and failure edge cases;
in-process tests exercise the concrete HTTP/JSON, TCP-connect, ICMP-ping,
TCP-exchange, and DNS adapters through state publication, TTL, and rendering. See
[Host Monitor Engine](monitor-engine.md) for comparison and scheduling semantics.

The simulator automatically registers an optional `monitors` array from its JSON
configuration and executes those monitors with a bounded background worker pool.
Each configured monitor can have only one request in flight at a time. See
[`http-monitor.example.json`](../examples/http-monitor.example.json) for a complete
single-monitor configuration and
[`http-request.example.json`](../examples/http-request.example.json) for a JSON
`POST` with a named-secret request header and body, and
[`https-monitor.example.json`](../examples/https-monitor.example.json) for the
public system-trust path,
[`concurrent-monitors.example.json`](../examples/concurrent-monitors.example.json)
for the deterministic slow/fast fixture. TCP reachability and bounded text exchanges
are shown in [`tcp-connect.example.json`](../examples/tcp-connect.example.json) and
[`tcp-exchange.example.json`](../examples/tcp-exchange.example.json). Standalone
address resolution is shown in
[`dns-monitor.example.json`](../examples/dns-monitor.example.json).
A local operations profile can use `icmp_ping` for endpoints that expose no
suitable application service.

The simulator supports:

- legacy fixed layouts or timed decks of bitmap, clock, and indicator cards;
- instant, fade, and four-direction slide transitions between cards;
- solid, blink, toggle, fade, pulse, sequence, and color-cycle appearances;
- a periodic status transition;
- TTL expiration into the distinct `stale` status;
- authenticated status pushes over a loopback HTTP endpoint;
- native Win32 and responsive browser outputs fed by the same logical frames;
- frame submission through the production `OutputDriver` interface.

## Driver Equivalence

The renderer knows only the `OutputDriver` contract. The Win32 and HTTP display
drivers independently copy and coalesce submitted logical frames in the same way
that a slow BLE driver may need to do. A future SDL, direct-LED, or MI BLE
implementation can replace either driver without changing state, appearance,
layout, or renderer code.

The HTTP transport is similarly thin: it translates native HTTP requests into the
portable `StatusApi` request/response contract. The validation and state-update
logic can therefore be reused by an ESP32 HTTP server without bringing the desktop
HTTP library into firmware.

Platform-specific event pumping is owned by the host executable and is not part of
the output contract. On ESP32, FreeRTOS tasks will provide the corresponding runtime
scheduling.

## Deliberately Deferred

The host build does not include ESP-IDF, a cross-compiler, NimBLE transport, Wi-Fi,
NVS, LittleFS, GPIO, or OTA support. MI packet construction is host-tested, but
actual BLE behavior remains a hardware validation task. WinHTTP/Schannel HTTPS,
system certificate trust, named desktop secrets, bounded HTTP methods, request
headers and bodies, scalar JSON extraction, TCP connect, bounded TCP text exchange,
ICMP echo, DNS address resolution, evaluation, concurrent interval execution, and display
responsiveness are implemented and host-tested. Per-monitor custom CAs, certificate
pins, and additional pull transports remain deferred. See
[Appliance Monitoring and TLS](appliance-monitoring.md) for the selected Win32 and
ESP32 TLS paths and appliance-integration requirements.
