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

CMake downloads the pinned `nlohmann/json` 3.12.0 release into the ignored build
tree. No dependency is installed globally.

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

The simulator demonstrates:

- four rectangular indicators on a 16x16 logical display;
- solid and blinking appearances;
- a periodic status transition;
- TTL expiration into the distinct `stale` status;
- frame submission through the production `OutputDriver` interface.

## Driver Equivalence

The renderer knows only the `OutputDriver` contract. The Win32 simulator copies and
coalesces submitted logical frames in the same way that a slow BLE driver may need
to do. A future SDL, browser-proxy, direct-LED, or MI BLE implementation can replace
the current driver without changing state, appearance, layout, or renderer code.

Platform-specific event pumping is owned by the host executable and is not part of
the output contract. On ESP32, FreeRTOS tasks will provide the corresponding runtime
scheduling.

## Deliberately Deferred

The host build does not include ESP-IDF, a cross-compiler, NimBLE transport, Wi-Fi,
NVS, LittleFS, GPIO, or OTA support. MI packet construction is host-tested, but actual
BLE behavior remains a hardware validation task.
