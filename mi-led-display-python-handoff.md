# MI LED Display Python Development Handoff

Created: 2026-08-17; implementation status reconciled 2026-08-22

Project context: Python control of a Merkury / MI 16x16 Bluetooth LED matrix display from a Microsoft Surface Book 3 running Windows, based on the reverse-engineered `offe/mi-led-display` repository.

Repository: https://github.com/offe/mi-led-display

## Document role and evidence labels

This document is the hardware workbench and protocol evidence record for the MI
display. The Python code is intended for discovery, calibration, benchmarking, and
generation of protocol test vectors. PixelStatus NX production firmware will not run
Python or `bleak`; it will use a native ESP-IDF NimBLE output driver.

For current commands and operational behavior, use
[`docs/windows-mi-ble.md`](docs/windows-mi-ble.md). For the project-wide completion
boundary and remaining reliability work, use
[`docs/implementation-status.md`](docs/implementation-status.md). Standalone scripts
below are retained as protocol-development history; `tools/mi_ble` is the maintained
implementation.

Protocol claims use these evidence levels:

- **device-validated**: observed on the user's physical display;
- **upstream-derived**: taken from `offe/mi-led-display` but not yet independently
  confirmed on the user's device;
- **working assumption**: plausible behavior that requires a calibration test.

## 1. Hardware and host assumptions

### Display

The display appears to be the Merkury Innovations / MI multicolor 16x16 matrix LED display targeted by `offe/mi-led-display`.

Relevant BLE identity from the repo:

- Advertised name: `MI Matrix Display`
- GATT service UUID: `0000ffd0-0000-1000-8000-00805f9b34fb`
- GATT write characteristic UUID: `0000ffd1-0000-1000-8000-00805f9b34fb`

The display is controlled via Bluetooth Low Energy GATT writes. It is not a serial display, USB framebuffer, HID display, or Wi-Fi display.

### Host

- Machine: Microsoft Surface Book 3
- OS assumption: Windows 10/11
- Python BLE library: `bleak`
- Image dependency for file/image mode: `Pillow`

Recommended Python environment:

```powershell
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install bleak pillow
```

The official repo README may mention `pil`; use `pillow` instead. The import remains `from PIL import Image`.

### PixelStatus NX bridge status

The repository now contains the host reference package at `tools/mi_ble`, the
`python -m tools.mi_ble` command, and `tools/start-mi-bridge.ps1`. It reads the
existing browser-display frame API, validates an exact 16×16 `rgb888` frame, and
forwards it without coupling BLE behavior to the renderer.

The 2026-08-21 device session established:

- Windows/Bleak discovery and connection to the advertised `MI Matrix Display`;
- the expected FFD1 characteristic was present;
- it advertised `write-without-response`, but not `write`;
- Bleak reported a 511-byte maximum write-without-response size;
- one complete live PixelStatus frame was accepted through the device-validated
  graffiti initializer and 256 single-pixel writes;
- the block initializer and eight 100-byte packets for the same live frame completed
  without a GATT error at a 10 ms inter-block delay.

User-observed calibration on 2026-08-22 additionally confirmed:

- pixel zero is the upper-left corner;
- logical rows are row-major without rotation or mirroring;
- the device consumes RGB channel order as encoded;
- block numbers 1 through 8 map to contiguous two-row horizontal bands from top to
  bottom;
- the block-mode live frame visually matched the browser backend;
- returning to graffiti mode on a fresh connection restored the same live frame and
  continued sparse clock updates.

The BLE address is deliberately not recorded because it is a local runtime identity.
Sustained block pacing, same-connection mode switching, and an interrupted-transfer
reconnect remain calibration work rather than completed claims.

## 2. BLE workflow

Basic scan pattern:

```python
from bleak import BleakScanner

devices = await BleakScanner.discover(timeout=5.0)
for d in devices:
    if d.name and "MI Matrix Display" in d.name:
        return d
```

Basic connect pattern:

```python
from bleak import BleakClient

async with BleakClient(device) as client:
    await client.write_gatt_char(CHARACTERISTIC_UUID, packet)
```

Useful troubleshooting notes:

- Close the official phone app before connecting from Python. BLE peripherals like this often tolerate only one central connection.
- Pairing from Windows Settings is not necessarily required, but can help if writes fail with authorization errors.
- Passing the discovered `BleakDevice` object is preferable to hard-coding an address on Windows.
- Windows BLE behavior may be slower or more conservative than Linux BLE behavior.

## 3. Protocol summary

There are at least two relevant protocol modes:

1. Graffiti / single-pixel mode
2. Full-image / block mode

### 3.1 Single-pixel graffiti mode

Initialization sequence known to work from `draw_pixels.py`:

```text
BC 00 01 01 55
BC 00 0D 0D 55
```

Single-pixel packet shape:

```text
BC 01 01 00 PP RR GG BB QQ 55
```

Where:

- `PP` = pixel index, 0 through 255
- `RR GG BB` = RGB color bytes
- `QQ` = trailing index/check byte
- `55` = packet terminator

Important: `protocol.txt` and working `draw_pixels.py` differ on `QQ`.

The user's actual display works with the `draw_pixels.py` behavior:

```python
def pixel_command(pixel_index: int, r: int, g: int, b: int) -> bytes:
    end_index = (pixel_index + 1) % 256
    if pixel_index == 0:
        end_index = 0xFF

    return bytes([
        0xBC, 0x01, 0x01, 0x00,
        pixel_index & 0xFF,
        r & 0xFF, g & 0xFF, b & 0xFF,
        end_index & 0xFF,
        0x55,
    ])
```

The earlier alternate interpretation, `QQ = (pixel_index - 1) & 0xFF`, did not work for this user.

### 3.2 Pixel layout

The working assumption is row-major:

```python
def pixel_index(x: int, y: int) -> int:
    return y * 16 + x
```

Coordinates:

- `x`: 0 to 15
- `y`: 0 to 15
- pixel 0: likely upper-left
- pixel 255: lower-right

This should be validated with a diagnostic pattern because inexpensive LED matrices sometimes use serpentine wiring or rotated/mirrored orientation.

### 3.3 Full-frame block mode

Initialization command from `draw_file.py`:

```text
BC 0F F1 08 08 55
```

Packet shape for each block:

```text
BC 0F <block_number> <32 RGB triples> 55
```

Where:

- `block_number`: 1 through 8
- each block contains 32 pixels
- full frame = 8 block writes
- full frame data = 256 RGB pixels
- each block packet length = 3 header bytes + 96 pixel bytes + 1 terminator = 100 bytes
- full frame payload = 800 bytes across 8 BLE writes

Block-command builder:

```python
def make_block_command(block_index: int, r: int, g: int, b: int) -> bytes:
    if not (0 <= block_index < 8):
        raise ValueError("block_index must be 0..7")

    out = bytearray()
    out.extend([0xBC, 0x0F, block_index + 1])

    for _ in range(32):
        out.extend([r & 0xFF, g & 0xFF, b & 0xFF])

    out.append(0x55)
    return bytes(out)
```

If it validates successfully on the user's device, full-frame block mode is expected
to be the preferred path for any frame where many pixels change.

## 4. Performance findings and expectations

The user measured `draw_pixels.py` full-display updates at roughly 5 seconds for all 256 pixels on the Surface Book 3 / Windows setup.

Measured single-pixel path estimate:

```text
256 pixels / 5 seconds ~= 51 pixel writes/sec
```

For sparse animation, such as a single Pong ball:

```text
clear old pixel + draw new pixel = 2 writes per visible move
51 writes/sec / 2 ~= 25 visible moves/sec
```

That is viable for sparse sprite animation.

For full-screen animation, single-pixel mode is not viable. Use block mode:

```text
Single-pixel full frame: 256 writes, 2560 bytes
Block full frame:          8 writes,  800 bytes
```

The full-frame block mode reduces write count by 32x and byte count by about 3.2x.

The repo's example code uses sleeps between writes. Removing sleeps may improve throughput, but may also cause dropped or corrupted frames depending on the display firmware and Windows BLE stack.

Test settings:

```python
USE_RESPONSE = False
BLOCK_DELAY = 0.0
```

Fallback settings if corruption occurs:

```python
USE_RESPONSE = True
BLOCK_DELAY = 0.0
```

or:

```python
USE_RESPONSE = False
BLOCK_DELAY = 0.001
```

## 5. API choice rules

Use single-pixel graffiti writes when changing only a few pixels:

- Pong ball
- cursor
- dot indicators
- sparse particles
- simple status glyph updates where only a few LEDs change

Use full-frame block writes when changing many pixels:

- full-screen flash
- text redraw
- scrolling text
- icons
- image display
- gradients
- animations involving more than roughly 16-32 changed pixels per frame

Raw byte break-even is around 80 changed pixels:

```text
80 changed pixels * 10 bytes ~= 800 bytes full-frame block mode
```

But BLE write count matters more than bytes, so block mode may win earlier than the byte-only estimate suggests.

There is no known hardware sprite mode, dirty rectangle mode, palette mode, framebuffer streaming API, or command batching API beyond the 8-block full-frame mode.

## 6. Candidate full-screen block-mode test

The packet structure began as **upstream-derived**, but the initializer, eight
100-byte packets, two-row block ordering, and visual output are now
**device-validated**. This standalone script is retained as the original experiment;
prefer `python -m tools.mi_ble pattern blocks --mode block` or the live bridge.

Save as `flash_fullscreen.py`.

```python
import asyncio
import time
from bleak import BleakScanner, BleakClient

TARGET_NAME = "MI Matrix Display"
CHARACTERISTIC_UUID = "0000ffd1-0000-1000-8000-00805f9b34fb"

# The observed characteristic supports writes without response only.
# If frames corrupt or drop, increase BLOCK_DELAY.
USE_RESPONSE = False
BLOCK_DELAY = 0.005

# Safety limit. Set to None to run forever.
RUN_SECONDS = 30


async def find_display():
    devices = await BleakScanner.discover(timeout=5.0)

    for d in devices:
        print(f"Found: {d.name!r} [{d.address}]")
        if d.name and TARGET_NAME in d.name:
            return d

    return None


def make_block_command(block_index: int, r: int, g: int, b: int) -> bytes:
    """
    Full-frame block packet used by offe/mi-led-display draw_file.py:

        BC 0F <block-number> <32 RGB triples> 55

    block_index: 0..7
    """
    if not (0 <= block_index < 8):
        raise ValueError("block_index must be 0..7")

    out = bytearray()
    out.extend([0xBC, 0x0F, block_index + 1])

    for _ in range(32):
        out.extend([r & 0xFF, g & 0xFF, b & 0xFF])

    out.append(0x55)
    return bytes(out)


BLACK_FRAME = [make_block_command(i, 0, 0, 0) for i in range(8)]
WHITE_FRAME = [make_block_command(i, 255, 255, 255) for i in range(8)]


async def init_block_mode(client: BleakClient):
    # This is the mode-init packet used by draw_file.py.
    await client.write_gatt_char(
        CHARACTERISTIC_UUID,
        bytes.fromhex("bc0ff1080855"),
        response=USE_RESPONSE,
    )

    # Give the display firmware a moment to enter the mode.
    await asyncio.sleep(0.1)


async def send_frame(client: BleakClient, frame_packets: list[bytes]):
    for packet in frame_packets:
        await client.write_gatt_char(
            CHARACTERISTIC_UUID,
            packet,
            response=USE_RESPONSE,
        )

        if BLOCK_DELAY:
            await asyncio.sleep(BLOCK_DELAY)


async def flash_test(client: BleakClient):
    frames = 0
    writes = 0
    start = time.perf_counter()
    next_report = start + 2.0

    white = False

    while True:
        now = time.perf_counter()

        if RUN_SECONDS is not None and now - start >= RUN_SECONDS:
            break

        frame = WHITE_FRAME if white else BLACK_FRAME
        white = not white

        await send_frame(client, frame)

        frames += 1
        writes += 8

        now = time.perf_counter()
        if now >= next_report:
            elapsed = now - start
            print(
                f"{frames / elapsed:.2f} full frames/sec, "
                f"{writes / elapsed:.1f} BLE writes/sec, "
                f"{frames} frames total"
            )
            next_report = now + 2.0

    elapsed = time.perf_counter() - start
    print()
    print("Done.")
    print(f"Elapsed:          {elapsed:.3f} sec")
    print(f"Full frames:      {frames}")
    print(f"Full frames/sec:  {frames / elapsed:.2f}")
    print(f"BLE writes:       {writes}")
    print(f"BLE writes/sec:   {writes / elapsed:.1f}")

    # Leave display black.
    await send_frame(client, BLACK_FRAME)


async def main():
    dev = await find_display()
    if not dev:
        raise SystemExit("MI Matrix Display not found.")

    print(f"Connecting to {dev.name} [{dev.address}]")

    async with BleakClient(dev) as client:
        if not client.is_connected:
            raise SystemExit("Failed to connect.")

        print("Connected.")
        print(f"USE_RESPONSE={USE_RESPONSE}, BLOCK_DELAY={BLOCK_DELAY}")

        await init_block_mode(client)
        await flash_test(client)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
```

Run:

```powershell
python flash_fullscreen.py
```

Interpretation:

- `full frames/sec` = complete 16x16 rewrites per second
- visible black/white flash rate = half the full frame rate
- `10 full frames/sec` means 5 white flashes/sec

## 7. Single-pixel example using the device-validated packet rule

This uses sparse graffiti-mode updates: clear the old pixel and draw the new pixel.
The underlying `draw_pixels.py` packet rule is device-validated. The Pong animation
and its motion behavior remain example code rather than a protocol validation result.

Key ideas:

- Initialize graffiti mode with `bc00010155`, `bc000d0d55`
- Use `draw_pixels.py` command rule for individual pixels
- Maintain floating-point ball position and velocity
- Round to nearest integer LED coordinate
- On bounce, reflect velocity and add incidence-weighted angular jitter
- Glancing hits get more random deviation; square-on hits get little deviation

Core pixel write helpers:

```python
CHARACTERISTIC_UUID = "0000ffd1-0000-1000-8000-00805f9b34fb"

WIDTH = 16
HEIGHT = 16


def pixel_index(x: int, y: int) -> int:
    if not (0 <= x < WIDTH and 0 <= y < HEIGHT):
        raise ValueError(f"Pixel out of range: {x}, {y}")
    return y * WIDTH + x


def get_set_pixel_command(pixel: int, r: int, g: int, b: int) -> bytearray:
    if not (0 <= pixel <= 255):
        raise ValueError(f"Pixel index out of range: {pixel}")

    end_index = (pixel + 1) % 256
    if pixel == 0:
        end_index = 0xFF

    return bytearray([
        0xBC, 0x01, 0x01, 0x00,
        pixel & 0xFF,
        r & 0xFF,
        g & 0xFF,
        b & 0xFF,
        end_index & 0xFF,
        0x55
    ])


async def write_pixel(client, x: int, y: int, rgb: tuple[int, int, int]):
    p = pixel_index(x, y)
    cmd = get_set_pixel_command(p, rgb[0], rgb[1], rgb[2])
    await client.write_gatt_char(CHARACTERISTIC_UUID, cmd)
```

Incidence-weighted bounce behavior:

```python
import math
import random

MIN_BOUNCE_JITTER_DEG = 1.0
MAX_BOUNCE_JITTER_DEG = 18.0
MIN_COMPONENT = 0.18


def rotate_vector(vx: float, vy: float, radians: float) -> tuple[float, float]:
    c = math.cos(radians)
    s = math.sin(radians)
    return vx * c - vy * s, vx * s + vy * c


def normalize_speed(vx: float, vy: float, speed: float) -> tuple[float, float]:
    mag = math.hypot(vx, vy)
    if mag == 0:
        return speed, 0.0
    return vx / mag * speed, vy / mag * speed


def keep_components_reasonable(vx: float, vy: float, speed: float) -> tuple[float, float]:
    vx, vy = normalize_speed(vx, vy, speed)

    if abs(vx) < MIN_COMPONENT:
        vx = math.copysign(MIN_COMPONENT, vx if vx != 0 else random.choice([-1, 1]))

    if abs(vy) < MIN_COMPONENT:
        vy = math.copysign(MIN_COMPONENT, vy if vy != 0 else random.choice([-1, 1]))

    return normalize_speed(vx, vy, speed)


def bounce_with_incidence_jitter(
    vx: float,
    vy: float,
    hit_vertical_wall: bool,
    hit_horizontal_wall: bool,
    speed: float,
) -> tuple[float, float]:
    old_vx, old_vy = vx, vy

    if hit_vertical_wall:
        vx = -vx

    if hit_horizontal_wall:
        vy = -vy

    glancing_scores = []

    if hit_vertical_wall:
        normal = abs(old_vx)
        tangent = abs(old_vy)
        glancing_scores.append(tangent / (normal + tangent + 1e-9))

    if hit_horizontal_wall:
        normal = abs(old_vy)
        tangent = abs(old_vx)
        glancing_scores.append(tangent / (normal + tangent + 1e-9))

    glancing = max(glancing_scores) if glancing_scores else 0.0

    jitter_deg = MIN_BOUNCE_JITTER_DEG + glancing * (MAX_BOUNCE_JITTER_DEG - MIN_BOUNCE_JITTER_DEG)
    jitter_rad = math.radians(random.uniform(-jitter_deg, jitter_deg))

    vx, vy = rotate_vector(vx, vy, jitter_rad)
    vx, vy = keep_components_reasonable(vx, vy, speed)

    return vx, vy
```

## 8. Development recommendations for an LLM coding agent

### 8.1 Create a host-side reference driver module

This package should remain a desktop diagnostic and test-oracle tool. It is not the
production ESP32 output driver.

Create a Python module, for example:

```text
mi_led_display/
  __init__.py
  transport.py
  protocol.py
  display.py
  examples/
    scan.py
    flash_fullscreen.py
    pong_pixel.py
    image_file.py
```

Suggested object model:

```python
class MiLedDisplay:
    async def connect(self): ...
    async def disconnect(self): ...
    async def init_graffiti_mode(self): ...
    async def init_block_mode(self): ...
    async def set_pixel(self, x, y, r, g, b): ...
    async def send_frame(self, framebuffer): ...
    async def clear(self): ...
```

Keep protocol construction pure and testable:

```python
def build_pixel_packet(pixel_index, r, g, b) -> bytes: ...
def build_block_packet(block_index, rgb_pixels_32) -> bytes: ...
def xy_to_index(x, y) -> int: ...
```

### 8.2 Add a framebuffer abstraction

Use a 16x16 RGB framebuffer:

```python
class Framebuffer16:
    width = 16
    height = 16
    pixels: list[tuple[int, int, int]]

    def clear(self, rgb=(0, 0, 0)): ...
    def set(self, x, y, rgb): ...
    def get(self, x, y): ...
    def blocks(self): ...
```

The framebuffer should support:

- full-frame send using 8 block packets
- diffing against previous framebuffer for sparse updates
- auto-selecting sparse vs block mode

Auto-selection rule:

```text
if changed_pixels <= threshold:
    send sparse pixel writes
else:
    send full frame via block writes
```

Initial threshold to test: 16 changed pixels.

### 8.3 Add calibration utilities

Create diagnostics:

1. Coordinate corners:
   - pixel `(0,0)` red
   - `(15,0)` green
   - `(0,15)` blue
   - `(15,15)` white

2. Index walk:
   - light one pixel at a time from 0 to 255
   - user observes whether layout is row-major, serpentine, mirrored, or rotated

3. Block test:
   - block 1 red
   - block 2 green
   - block 3 blue
   - etc.

These tests should produce a `layout.json` or hard-coded mapping function later.

### 8.4 Add benchmark suite

Benchmarks needed:

- single-pixel writes/sec with `response=True`
- single-pixel writes/sec with `response=False`
- full-frame block FPS with `response=True`
- full-frame block FPS with `response=False`
- block FPS with delays: 0, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02

Benchmark output should include:

```text
mode
response flag
block delay
pixel delay
writes/sec
frames/sec
dropped/corrupt observed? manual flag
```

Because dropped frames may be visual rather than programmatically detectable, include a manual `--mark-good` or simple CSV log for observed settings.

### 8.5 Robustness

Handle:

- display not found
- connection failure
- phone app already connected
- BLE write failure
- Windows pairing/authentication failure
- keyboard interrupt
- leaving display black on exit

Always provide a conservative mode:

```python
response=True
small delay between writes
```

and a fast mode:

```python
response=False
no delay
```

### 8.6 Avoid bad assumptions

Do not assume:

- the repo's `protocol.txt` is more accurate than working code
- packet `QQ` follows a clean checksum rule
- full-frame block mode can sustain video frame rates
- Windows BLE accepts unlimited no-response writes
- the matrix orientation is definitely row-major
- the display can buffer multiple frames cleanly

Validated facts from the user conversation:

- `draw_pixels.py` works on the user's display.
- A protocol-note-based `mi_display_pixel.py` did not work.
- Full single-pixel rewrite via `draw_pixels.py` takes about 5 seconds on the user's setup.
- User wants Python access and wants to develop interactive/display-control experiments.

### 8.7 ESP32 production integration decision

The ESP32-S3 firmware should use ESP-IDF, FreeRTOS, and the native NimBLE
central/GATT-client APIs. The desktop `bleak` transport cannot be reused directly.
Reuse the protocol knowledge through shared byte-level test vectors instead.

Recommended native component split:

```text
output/mi_ble/
├── mi_protocol        pure packet construction and coordinate mapping
├── mi_ble_transport   scan, connect, discover, write, and reconnect
└── mi_output_driver   framebuffer diff, coalescing, mode choice, and state
```

The driver should queue or copy the latest submitted frame and must not block the
renderer while BLE transfer is in progress. If frames arrive faster than the device
can accept them, replace older pending frames with the newest complete frame.

A MicroPython proof of concept is technically possible because MicroPython exposes
BLE central and GATT-client operations. It is not the selected production path and
would still require a new transport implementation rather than running `bleak`.

## 9. Completed and Remaining Experiments

Completed:

- the maintained `tools/mi_ble` package consumes exact PixelStatus browser frames,
  validates transforms, constructs fixed pixel/block packet vectors, and has pure
  tests;
- the physical characteristic was inspected and advertised no-response writes with
  a reported 511-byte maximum;
- pixel mode, eight-packet block mode, upper-left row-major RGB geometry, no
  rotation/mirroring, and top-to-bottom two-row block order were visually confirmed;
- pixel diffing, newest-frame coalescing, bounded reconnect, complete restoration on
  connection, and an atomic privacy-preserving heartbeat are implemented;
- PixelStatus's C++ appearance grammar and renderer already produce the animated
  framebuffer; the BLE layer intentionally does not reimplement status visuals.

Remaining:

1. Force a disconnect during an active block transfer and verify that reconnect
   restores the newest complete source frame.
2. Run a longer unattended soak and record stability, GATT writes, reconnects,
   task restarts, and the useful visible update ceiling. The current panel appears
   to refresh at about 4 Hz.
3. Test graffiti/block switching on one connection and whether reinitialization
   preserves or replaces untouched pixels.
4. Enable automatic sparse/block selection only if the switching result and measured
   crossover justify it. Until then, mode is explicit for each run.
5. Test brightness commands only if controlling brightness becomes a requirement.
6. Reuse the validated packet vectors when the native ESP-IDF NimBLE component is
   created; no ESP32 transport exists yet.

## 10. Strategic design note for PixelStatus NX

The MI LED display is useful as a Bluetooth output target, but should not define the project architecture.

Recommended architecture:

```text
status sources -> status rules -> appearance grammar -> framebuffer -> output driver
```

Output drivers should be modular:

- MI BLE 16x16 display driver
- ESP32 BLE driver
- ESP32 direct LED matrix driver
- simulator/window driver
- terminal/text debug driver
- web preview driver

The BLE MI display should be treated as one low-resolution, low-throughput output backend.

For richer animation, ESP32 direct LED driving will likely outperform BLE-to-MI-display control. For simple statuses and sparse changes, the MI display is adequate.
