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

The equivalent one-command workflow is:

```powershell
.\tools\build-host.ps1
```

After building, run the repeatable simulator/API integration smoke test with:

```powershell
.\tools\test-host-api.ps1
```

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
```

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

The core library now also contains host-tested runner, evaluator, and deterministic
interval-engine contracts. Scripted runners exercise the complete path from a due
monitor through threshold evaluation, state publication, TTL, and rendering without
performing network I/O. See [Host Monitor Engine](monitor-engine.md) for comparison
semantics, scheduling behavior, and the concrete runners still to be added.

The simulator supports:

- four rectangular indicators on a 16x16 logical display;
- solid, blink, toggle, fade, pulse, sequence, and color-cycle appearances;
- a periodic status transition;
- TTL expiration into the distinct `stale` status;
- authenticated status pushes over a loopback HTTP endpoint;
- frame submission through the production `OutputDriver` interface.

## Driver Equivalence

The renderer knows only the `OutputDriver` contract. The Win32 simulator copies and
coalesces submitted logical frames in the same way that a slow BLE driver may need
to do. A future SDL, browser-proxy, direct-LED, or MI BLE implementation can replace
the current driver without changing state, appearance, layout, or renderer code.

The HTTP transport is similarly thin: it translates native HTTP requests into the
portable `StatusApi` request/response contract. The validation and state-update
logic can therefore be reused by an ESP32 HTTP server without bringing the desktop
HTTP library into firmware.

Platform-specific event pumping is owned by the host executable and is not part of
the output contract. On ESP32, FreeRTOS tasks will provide the corresponding runtime
scheduling.

## Deliberately Deferred

The host build does not include ESP-IDF, a cross-compiler, NimBLE transport, Wi-Fi,
NVS, LittleFS, GPIO, or OTA support. MI packet construction is host-tested, but actual
BLE behavior remains a hardware validation task. Concrete pull transports and their
JSON configuration are also deferred; their portable execution contracts are now in
place and tested.
