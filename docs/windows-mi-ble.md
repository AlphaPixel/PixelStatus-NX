# Windows MI Bluetooth Bridge

PixelStatus NX can mirror any 16×16 frame from its read-only browser-display API to
the Merkury / MI Bluetooth matrix from Windows. This is a host diagnostic and usable
development backend; production ESP32 firmware will implement the same protocol
through native NimBLE rather than Python.

The bridge is implemented and is the active physical-output path in the published
Windows runtime. Physical protocol mapping is validated; a forced mid-transfer
disconnect and longer unattended soak remain open. See
[Implementation Status and Loose Ends](implementation-status.md).

```text
renderer -> HTTP display frame API -> Python/Bleak bridge -> MI GATT display
```

The bridge does not know about monitors, cards, or layouts. It consumes the same
row-major `rgb888` frames as the browser and therefore remains interchangeable with
the Win32 and browser outputs from the renderer's perspective.

## Requirements

- Windows Bluetooth enabled;
- Python 3.10 or later;
- the optional Bleak dependency;
- the official phone application disconnected from the display.

Install the pinned host dependency when needed:

```powershell
python -m pip install -r .\tools\requirements-mi-ble.txt
```

The current host already has Bleak 3.0.2. Its current package metadata lists Windows
11 as supported; discovery, connection, GATT inspection, and real writes have also
been exercised successfully on this project's Windows 10 22H2 development host.

## Discovery and a No-Write Probe

List nearby advertisements:

```powershell
python -m tools.mi_ble scan
```

Connect, inspect the write characteristic, and disconnect without changing pixels:

```powershell
python -m tools.mi_ble probe
```

The bridge discovers `MI Matrix Display` by advertised name. `--address` is an
optional runtime override for unreliable name rediscovery; addresses are environment
specific and must not be stored in public configuration.

## Mirror PixelStatus

For the long-running private operations deck, one command starts both the headless
simulator and this bridge:

```powershell
.\tools\run-operations-demo.ps1
```

The browser display is then available at `http://127.0.0.1:18908/`. Leave the
PowerShell window open; Ctrl+C stops the bridge and its simulator. The command uses
the ignored `examples\operations.local.json` profile and discovers the panel by
name, so it stores neither private network configuration nor a Bluetooth address in
tracked files. It selects the device-validated block mode so full-card changes can
keep pace with the 2.5-second logo and three-second composite cards. All changes in
that hardware profile are instant because the display's observed visible update
rate is about 4 Hz. Pass `-BrowserOnly` to run the same deck without Bluetooth, or
`-MiMode pixel` for conservative sparse writes.

With a simulator browser backend on its default port:

```powershell
.\tools\start-mi-bridge.ps1
```

For the appliance fixture currently using browser port 18938:

```powershell
.\tools\start-mi-bridge.ps1 `
    -DisplayUrl 'http://127.0.0.1:18938/api/v1/display'
```

The command runs until Ctrl+C. A one-frame physical test is:

```powershell
.\tools\start-mi-bridge.ps1 `
    -DisplayUrl 'http://127.0.0.1:18938/api/v1/display' `
    -Once
```

Validate the entire source and packet path without opening Bluetooth:

```powershell
.\tools\start-mi-bridge.ps1 `
    -DisplayUrl 'http://127.0.0.1:18938/api/v1/display' `
    -Once `
    -DryRun
```

The default `pixel` mode uses the device-validated graffiti initialization and
single-pixel packet rule. Its conservative 20 ms write delay takes about five
seconds for an initial 256-pixel restore. Later frames are diffed against the last
completed hardware frame, so a clock digit or status pixel normally needs far fewer
writes.

The HTTP producer continues polling while BLE writes are in progress. Only the
newest pending complete frame is retained. On disconnect, the bridge scans with
bounded exponential backoff and restores the newest complete frame after reconnect;
older animation frames are never queued for replay.

## Calibration and Block Mode

Corner and per-block patterns are available:

```powershell
python -m tools.mi_ble pattern corners
python -m tools.mi_ble pattern blocks --mode block
```

Logical-to-physical calibration options are `--rotation 0|90|180|270`, `--mirror-x`,
`--mirror-y`, and `--color-order RGB|RBG|GRB|GBR|BRG|BGR`.

`block` mode uses the upstream-derived block initializer followed by eight 100-byte
writes. The current characteristic reports a 511-byte write-without-response limit.
On 2026-08-22, the display rendered the eight-block calibration pattern and a live
PixelStatus frame correctly at a 10 ms inter-block delay, and subsequent live
operations use the bridge's explicit 5 ms block-delay default. Block mode remains an
explicit run choice until longer soak and same-connection mode switching establish a
safe crossover. The operations-deck launcher selects it for full-card changes;
there is no automatic sparse/block selection yet.

The current display advertises only `write-without-response`, so `--response` is
rejected with an actionable error rather than silently requesting an unsupported
operation.

## Verification

Pure protocol vectors, frame validation, transforms, initial/sparse writes, block
packetization, write-size guarding, latest-frame coalescing, and atomic heartbeat
publication use the standard-library test suite:

```powershell
python -m unittest discover -s tests -p test_mi_ble.py -v
```

CMake adds this suite to the full host CTest preset whenever a Python 3.10+
interpreter is available; the portable core-only preset remains C++-only.
The 2026-08-21 device session also verified discovery, connection, the expected GATT
service/characteristic, a 511-byte no-response limit, successful delivery of a
complete live PixelStatus frame through 256 single-pixel writes, and acceptance of
the same frame through eight block writes. User-observed calibration on 2026-08-22
confirmed an upper-left origin, row-major RGB order without rotation or mirroring,
contiguous two-row blocks numbered top-to-bottom, and a physical live frame matching
the browser output. The observed visible panel update rate is about 4 Hz. The device
address is intentionally not recorded.

For unattended operation, `bridge --status-file <path>` atomically replaces a JSON
heartbeat containing `scanning`, `connected`, `displaying`, or `retrying` state, the
last displayed source-frame sequence, cumulative GATT write count, update time, and
an optional error type. It never contains the hardware address. The published
Windows runtime writes this under its logs directory, and
`tools\get-windows-runtime-status.ps1` includes it in the combined task health view.

The bridge has demonstrated normal disconnect/retry ownership in unit tests and
ordinary live reconnect behavior. The remaining hardware reliability acceptance is
to force a disconnect during an active block transfer, confirm newest-frame restore,
and complete a longer soak with recorded task and heartbeat behavior.
