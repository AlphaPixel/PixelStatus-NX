# PixelStatus NX

## Project Summary

**PixelStatus NX** is an ESP32-based ambient status-monitoring and visualization system. It is conceived as an intellectual successor to the abandoned 2015 [`eins78/pixelstatus`](https://github.com/eins78/pixelstatus) project, while substantially generalizing its monitoring, state, rendering, and hardware architecture.

The name **NX** means **NeXt**.

The core idea is simple:

> Collect simple status information from LAN, Internet, and peer systems, normalize that information into named states, and render those states as persistent ambient visual indicators.

PixelStatus NX is deliberately **not** intended to become a general-purpose monitoring platform such as Nagios, Zabbix, Prometheus, or Home Assistant. It handles a useful set of simple monitoring primitives itself. Anything requiring sophisticated computation, scripting, authentication workflows, aggregation, or domain-specific logic should be monitored externally and expose its resulting state to PixelStatus NX through a push or query interface.

The current target hardware includes an ESP32 and a **16×16 RGB Bluetooth-connected display**, but neither the monitoring system nor rendering architecture should be coupled to that particular display.

Development begins with a portable C++20 core and a native Win32 display simulator.
This permits configuration, state, timing, appearance, layout, framebuffer, and
output-driver behavior to be developed without ESP32 tools or hardware. See
[Host-First Development](docs/host-development.md) for build and run instructions.
The current host implementation also accepts authenticated status pushes over a
loopback HTTP endpoint; its transport-independent handler is intended to be reused
behind the eventual ESP32 HTTP server. The supported JSON appearance syntax is
documented in [Configuration V1](docs/configuration-v1.md).
The host monitor-engine layer—normalized monitor results, ordered evaluation rules,
and deterministic interval scheduling—is described in
[Host Monitor Engine](docs/monitor-engine.md).
Declarative HTTP monitoring with bounded methods, headers, request/response bodies,
timeouts, status/body/JSON-Pointer observation, and a loopback-tested desktop
adapter now uses those contracts. A
bounded desktop worker pool prevents one slow check from stalling unrelated
monitors or either display backend.
TCP-connect monitoring uses the same scheduler and evaluator while exposing
successful connection latency as a numeric observation.
Bounded TCP-exchange monitoring adds optional text transmission, delimiter-based
response capture, and body or total-latency observation for simple service checks.
Standalone DNS monitoring adds address-family filtering plus address-list, count,
and lookup-latency observations without changing those contracts.
The same rendered framebuffer is also available through a responsive
[Browser Display Backend](docs/http-display.md), including a read-only frame API and
headless host mode.
The planned desktop/ESP32 TLS split and the integration requirements for TrueNAS
CORE, UniFi, Netgear cable modems, and Starlink are described in
[Appliance Monitoring and TLS](docs/appliance-monitoring.md).

---

# 1. Design Goals

PixelStatus NX should be:

- Small enough to run comfortably on an ESP32.
- Reliable enough to operate continuously as an appliance.
- Configurable without recompiling firmware.
- Capable of both **pull-based monitoring** and **push-based status updates**.
- Independent of any particular physical display.
- Capable of driving both remotely connected and directly connected displays.
- Able to express more information than simple red/green status lights.
- Able to define arbitrary named statuses.
- Able to associate each status with a time-varying visual appearance.
- Extensible through well-defined monitor, evaluator, renderer, and output-driver interfaces.
- Understandable enough that configuration remains declarative rather than becoming a programming language.

It should explicitly avoid:

- arbitrary shell execution;
- embedded Unix-like scripting;
- attempting to duplicate full monitoring systems;
- direct coupling between monitoring code and LED hardware;
- hard-coding the current 16×16 physical geometry into the monitoring model;
- forcing all statuses into `OK`/`FAIL`.

---

# 2. Historical Relationship to PixelStatus

The original PixelStatus project implemented approximately:

```text
Task
  ↓
Runner
  ↓
Expectation
  ↓
Pass / Fail
  ↓
Reaction
  ↓
Solid color applied to LED section
```

Its useful architectural idea was the separation between:

1. obtaining information;
2. evaluating that information;
3. deciding what status it represented;
4. displaying the result.

PixelStatus NX retains that conceptual separation while replacing the original linear LED-section model with a substantially more general architecture:

```text
                         ┌────────────────────┐
                         │   Push Sources     │
                         │                    │
                         │ HTTP / MQTT / etc. │
                         └─────────┬──────────┘
                                   │
                                   │
┌───────────────────┐              │
│ Pull Scheduler    │              │
│                   │              │
│ periodic / cron   │              │
└─────────┬─────────┘              │
          │                        │
          v                        │
┌───────────────────┐              │
│ Monitor Runner    │              │
│                   │              │
│ HTTP / Ping /     │              │
│ DNS / SNMP / TCP  │              │
└─────────┬─────────┘              │
          │                        │
          v                        │
┌───────────────────┐              │
│ Evaluator         │              │
│                   │              │
│ raw result →      │              │
│ named status      │              │
└─────────┬─────────┘              │
          │                        │
          └────────────┬───────────┘
                       v
                ┌───────────────┐
                │  State Store  │
                └───────┬───────┘
                        │
                        v
                ┌───────────────┐
                │   Renderer    │
                └───────┬───────┘
                        │
                        v
                ┌───────────────┐
                │ Frame / Scene │
                └───────┬───────┘
                        │
              ┌─────────┴─────────┐
              v                   v
       Bluetooth display    Direct LED driver
```

No source, monitor, or evaluator should need to know how the resulting state is physically displayed.

---

# 3. Hardware Model

## 3.1 Primary Target

Initial development should target an **ESP32**, preferably an **ESP32-S3** with sufficient flash and PSRAM.

The ESP32 provides:

- Wi-Fi;
- Bluetooth LE;
- TCP/IP networking;
- TLS;
- FreeRTOS;
- persistent flash storage;
- timers;
- OTA firmware updating;
- sufficient CPU and memory for the intended workload.

---

## 3.2 Current Display

The current development display is:

- 16×16 pixels;
- RGB;
- 256 total pixels;
- externally controlled;
- connected over Bluetooth Low Energy using GATT writes.

The working single-pixel protocol and candidate full-frame protocol are documented in
[MI LED Display Python Development Handoff](mi-led-display-python-handoff.md). The
single-pixel path has been exercised on the user's display. Full-frame block mode,
physical orientation, throughput, and several connection details still require
device validation.

The current device is an important target, but **must not define the core architecture**.

---

# 4. Modular Output Drivers

Rendering and physical output must be separate layers.

A renderer should produce an abstract frame or scene. An output driver translates that representation into whatever protocol the attached display requires.

Conceptually:

```text
Monitor states
      ↓
Layout / Renderer
      ↓
Logical framebuffer
      ↓
OutputDriver
      ↓
Physical device
```

The first output drivers should include:

### Bluetooth Display Driver

For the existing 16×16 display.

Responsibilities include:

- BLE discovery or configured device binding;
- connection/reconnection;
- protocol encoding;
- frame updates;
- brightness if supported;
- connection-state reporting.

The production firmware implementation should be a native ESP-IDF C/C++ component
using the NimBLE central/GATT-client APIs. The desktop Python/`bleak` implementation
is a protocol reference, diagnostic tool, and test oracle; it is not a runtime
dependency of the ESP32 firmware.

### Direct Addressable LED Driver

Support directly attached hardware such as:

- WS2812 / NeoPixel;
- SK6812;
- possibly APA102 or similar devices.

The direct driver should allow users to build PixelStatus NX hardware without the Bluetooth display.

### Simulator Driver

A host-side or development-mode output that displays the logical framebuffer without physical hardware.

Useful for:

- unit testing;
- layout development;
- appearance-rule development;
- CI;
- screenshots;
- debugging.

Additional output drivers should be possible without altering monitor or renderer code.

The host build currently includes both a native Win32 driver and an HTTP browser
driver. They consume the same logical frame concurrently, and the HTTP driver can
run headlessly for browser-only or remote-display use.

Example interface:

```cpp
enum class FrameSubmitResult {
    accepted,
    coalesced,
    unavailable,
};

class OutputDriver {
public:
    virtual bool begin() = 0;
    virtual FrameSubmitResult submitFrame(const Frame& frame) = 0;
    virtual DriverState state() const = 0;
};
```

Frame submission should not require the renderer to wait for physical transport.
A slow driver may retain or copy the newest frame, replace an older pending frame,
and report that coalescing occurred. Acceptance means that the driver took
responsibility for the frame; it does not guarantee that the physical display has
already received it. Transport errors and reconnect progress are exposed through
`DriverState`.

Output capabilities may differ, so drivers may eventually expose capabilities such as:

```text
dimensions
color depth
maximum frame rate
brightness support
per-pixel support
animation support
orientation
```

The architecture should not assume every output is necessarily 16×16.

---

# 5. Logical Display Model

The renderer should operate on a logical display surface rather than raw physical LED indexes.

For the current hardware:

```text
width  = 16
height = 16
pixels = 256
```

But these values should come from display configuration or driver capabilities.

At minimum:

```cpp
struct RGB {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct Frame {
    unsigned width;
    unsigned height;
    RGB pixels[];
};
```

A more sophisticated scene representation may sit above the framebuffer.

---

# 6. Monitors and Indicators Are Separate Concepts

A **monitor** obtains or receives state information.

An **indicator** determines where and how that information appears.

For example:

```text
Monitor:
    id: web-prod
    type: http

Indicator:
    source: web-prod
    x: 0
    y: 0
    width: 4
    height: 4
```

This separation permits:

- one monitor to drive multiple visual elements;
- one indicator to change layout without changing monitoring;
- multiple indicators to derive from a single state;
- hardware geometry to change independently of monitoring configuration.

---

# 7. Monitoring Philosophy

PixelStatus NX should implement simple checks directly.

A check should be something that can reasonably be described declaratively:

```text
query this thing
obtain this result
compare it against this rule
produce this status
```

It should **not** support arbitrary command execution or general scripting.

For example, this Unix-style monitor:

```bash
curl -sf https://server/api/status |
    jq -e '.database.replication_lag < 30'
```

should either become a declarative HTTP/JSON monitor:

```yaml
type: http_json
url: https://server/api/status

expect:
  path: database.replication_lag
  less_than: 30
```

or, if determining health requires substantially more logic, an external monitoring system should perform that computation and publish:

```json
{
    "id": "database",
    "status": "ok"
}
```

to PixelStatus NX.

---

# 8. Pull Monitoring

Pull monitors execute according to schedules maintained by PixelStatus NX.

Example:

```text
Scheduler
    ↓
Monitor becomes due
    ↓
Work queue
    ↓
Runner
    ↓
Result
    ↓
Evaluator
    ↓
State Store
```

A bounded number of monitors should execute concurrently.

There is little reason to launch dozens of simultaneous TLS sessions from an ESP32.

A small worker pool, perhaps 2–4 simultaneous network operations, is sufficient.

---

# 9. Scheduling

Support two scheduling styles.

## Interval Scheduling

Preferred for normal health checks:

```yaml
interval: 30s
```

Examples:

```text
5s
30s
2m
15m
1h
```

Optional jitter should prevent synchronized bursts when many monitors use identical intervals.

Example:

```yaml
interval: 60s
jitter: 5s
```

---

## Cron-Like Scheduling

Useful for time-dependent checks:

```yaml
schedule: "0 8 * * MON-FRI"
```

Cron syntax need not initially implement every obscure cron feature.

System time should be synchronized using SNTP/NTP.

---

# 10. Initial Pull Monitor Types

## ICMP Ping

Check host reachability.

Possible observations:

- successful replies;
- packet loss;
- average RTT;
- maximum RTT;
- timeout.

Example:

```yaml
type: ping
host: router.local

count: 3
timeout: 1s

expect:
  received:
    greater_or_equal: 2
  avg_rtt:
    less_than: 100ms
```

---

## DNS

Resolve a hostname.

Checks might include:

- successfully resolves;
- resolves to expected IPv4 address;
- resolves to expected IPv6 address;
- response latency.

Example:

```yaml
type: dns
host: internal.example.com

expect:
  ipv4: 192.168.10.20
```

---

## TCP Connect

Attempt a connection to a specified port.

Example:

```yaml
type: tcp_connect
host: server.local
port: 22
timeout: 2s
```

Useful for determining whether:

- SSH;
- SMTP;
- database;
- web;
- custom TCP service

is accepting connections.

---

## TCP Exchange

Connect, optionally transmit a defined payload, read a limited response, and evaluate it.

Example:

```yaml
type: tcp_exchange
host: mail.example.com
port: 25

expect:
  contains: "220"
```

This covers many simple service checks without implementing full application protocols.
The implemented V1 grammar uses a bounded optional `send` string, required
`read_until` delimiter, and either body or total-latency observation; see
[Configuration V1](docs/configuration-v1.md#tcp-exchange) for the runnable contract.

---

## HTTP / HTTPS

This should be one of the most capable built-in monitors.

Configuration may include:

```text
URL
method
headers
authentication
request body
timeout
maximum response size
```

Evaluation may include:

```text
HTTP status
header value
body contains
body equals
response latency
```

Example:

```yaml
type: http
url: https://example.com/health

expect:
  status:
    between: [200, 299]

  body:
    contains: "healthy"

  latency:
    less_than: 2s
```

---

# 11. HTTP JSON Monitoring

JSON APIs deserve first-class handling.

Example:

```yaml
type: http_json
url: https://server.local/api/status

expect:
  path: database.health
  equals: healthy
```

Numeric example:

```yaml
expect:
  path: storage.percent_used
  less_than: 90
```

A simple JSON path grammar is preferable to embedding a full query language.

For example:

```text
database.health
services.web.state
disks[0].percent_used
```

---

# 12. TLS / Certificate Monitoring

TLS checking can be useful independently of HTTP.

Example:

```yaml
type: tls
host: example.com
port: 443

expect:
  valid: true
  expires_in:
    greater_than: 14d
```

Possible checks:

- connection succeeds;
- TLS handshake succeeds;
- certificate chain validates;
- hostname matches;
- certificate expiration threshold.

---

# 13. SNMP

SNMP is practical on ESP32 and useful enough to warrant support.

Initial scope should be deliberately narrow:

- SNMP v1;
- SNMP v2c;
- GET requests;
- common scalar values.

Possible types:

```text
INTEGER
OCTET STRING
Counter32
Counter64
Gauge32
TimeTicks
```

Example:

```yaml
type: snmp
host: switch.local
version: 2c
community: public
oid: 1.3.6.1.2.1.2.2.1.8.5

expect:
  equals: 1
```

Potential uses:

- router state;
- switch interfaces;
- UPS battery;
- temperature;
- printer state;
- toner level;
- storage equipment;
- network appliances.

Initially exclude:

- SET;
- WALK;
- extensive MIB interpretation;
- SNMPv3.

Those can be added if justified.

---

# 14. Additional Possible Pull Monitors

Candidates for later versions include:

```text
UDP request/response
NTP
mDNS service discovery
MQTT query/subscription
Modbus/TCP
```

These should only be added when they provide substantial utility without turning the firmware into a protocol collection.

---

# 15. Push Monitoring

Push sources allow another system to tell PixelStatus NX what state should be displayed.

This is important because it provides the escape hatch for complex monitoring.

Example:

```text
External monitoring software
           ↓
        evaluates
           ↓
  determines service state
           ↓
    PixelStatus NX API
```

PixelStatus NX need not understand how the state was derived.

---

# 16. Local HTTP Push API

PixelStatus NX should expose an HTTP endpoint on the LAN.

For example:

```text
POST /api/v1/status/build
```

Body:

```json
{
    "status": "ok",
    "message": "main branch passed",
    "ttl": 1800
}
```

Or a generic endpoint:

```text
POST /api/v1/status
```

```json
{
    "id": "build",
    "status": "ok",
    "value": 42,
    "message": "main branch passed",
    "ttl": 1800
}
```

Authentication should be available.

At minimum:

```text
API token
```

The ESP32 should generally **not** be exposed directly to the public Internet.

---

# 17. MQTT Push Source

MQTT is a strong candidate for remote push monitoring.

The ESP32 makes an outbound connection to a broker and subscribes to configured topics.

Example:

```text
pixelstatus/status/build
pixelstatus/status/backup
pixelstatus/status/router
```

Message:

```json
{
    "status": "warn",
    "message": "backup overdue",
    "ttl": 3600
}
```

This works across NAT without exposing the ESP32 as a public server.

It also integrates naturally with:

- Home Assistant;
- monitoring servers;
- CI systems;
- custom daemons;
- embedded devices.

---

# 18. State Freshness and TTL

Freshness must be a first-class concept.

A pushed state should not remain healthy forever merely because the reporting system disappeared.

For example:

```json
{
    "status": "ok",
    "ttl": 900
}
```

means:

```text
current time - last update < 900 seconds
    → OK

current time - last update >= 900 seconds
    → STALE
```

Pull monitors similarly become stale if the scheduler or runner cannot obtain sufficiently recent data.

`STALE` should be distinct from an explicit negative result.

---

# 19. Status Model

PixelStatus NX must **not** hard-code only a fixed list of statuses.

There should be a useful default set, such as:

```text
ok
info
active
warn
fail
communication_failure
stale
unknown
disabled
```

but users should be able to define additional statuses.

Examples:

```text
deploying
charging
offline
maintenance
backing_up
degraded
busy
idle
critical
```

A monitor produces a **status identifier**.

The status definition determines its appearance.

This is an important separation:

```text
Monitoring:
    "backup" is OVERDUE

Appearance definition:
    OVERDUE means slowly pulsing amber
```

The monitoring subsystem must not directly encode:

```text
OVERDUE = RGB(255,128,0)
```

---

# 20. Status Appearance Grammar

Each status may define a time-varying LED appearance.

The grammar should be intentionally small, declarative, and deterministic.

It should be capable of expressing:

- solid colors;
- blinking;
- flashing;
- toggling between colors;
- fades;
- pulses;
- repeating sequences;
- potentially finite introductory sequences followed by steady state.

For example:

```yaml
statuses:

  ok:
    appearance:
      solid: "#00FF00"

  fail:
    appearance:
      blink:
        color: "#FF0000"
        on: 500ms
        off: 500ms

  stale:
    appearance:
      pulse:
        color: "#FF8000"
        period: 2s

  communication_failure:
    appearance:
      sequence:
        repeat: true
        steps:
          - color: "#FF00FF"
            duration: 200ms
          - color: "#000000"
            duration: 200ms
          - color: "#FF00FF"
            duration: 200ms
          - color: "#000000"
            duration: 1400ms
```

The grammar should ultimately reduce to a simple function:

```text
appearance(status, elapsed_time) → color
```

or perhaps:

```text
appearance(status, elapsed_time, pixel_context) → visual value
```

if more advanced rendering is later required.

---

# 21. Fundamental Appearance Primitive

A particularly clean internal representation would treat an appearance as a timeline of color keyframes.

Conceptually:

```text
time      color

0 ms      red
200 ms    red
201 ms    black
400 ms    black
401 ms    red
...
```

Interpolation determines whether a transition is:

```text
step
linear
```

This single mechanism can represent most desired effects.

For example, solid green:

```yaml
timeline:
  repeat: true

  keyframes:
    - at: 0ms
      color: "#00FF00"
```

Blink:

```yaml
timeline:
  repeat: true
  duration: 1000ms

  keyframes:
    - at: 0ms
      color: "#FF0000"
      transition: step

    - at: 500ms
      color: "#000000"
      transition: step
```

Fade:

```yaml
timeline:
  repeat: true
  duration: 2000ms

  keyframes:
    - at: 0ms
      color: "#000000"

    - at: 1000ms
      color: "#FF8000"
      transition: linear

    - at: 2000ms
      color: "#000000"
      transition: linear
```

Higher-level syntax such as:

```text
solid
blink
pulse
flash
toggle
```

could simply compile into this timeline representation.

This keeps the rendering engine simple while providing a convenient configuration language.

---

# 22. Appearance Examples

## Solid

```yaml
appearance:
  solid: "#00FF00"
```

---

## Blink

```yaml
appearance:
  blink:
    color: "#FF0000"
    period: 1s
    duty: 50%
```

---

## Toggle

```yaml
appearance:
  toggle:
    colors:
      - "#FF0000"
      - "#0000FF"
    period: 500ms
```

---

## Pulse

```yaml
appearance:
  pulse:
    color: "#FF8000"
    period: 2s
    minimum: 10%
    maximum: 100%
```

---

## Flash Pattern

```yaml
appearance:
  sequence:
    repeat: true

    steps:
      - color: "#FFFFFF"
        duration: 100ms

      - color: "#000000"
        duration: 100ms

      - color: "#FFFFFF"
        duration: 100ms

      - color: "#000000"
        duration: 1700ms
```

---

## Fade Between Colors

```yaml
appearance:
  cycle:
    transition: linear

    steps:
      - color: "#FF0000"
        duration: 1s

      - color: "#FFFF00"
        duration: 1s
```

---

# 23. Appearance State Timing

Each indicator should have a defined animation epoch.

Normally, animation timing should begin when the indicator **enters a status**.

For example:

```text
12:00:00 status changes OK → FAIL
12:00:00 fail animation begins at t=0
```

This allows an appearance to communicate transitions intentionally.

There may eventually be multiple timing modes:

```text
on_status_entry
globally_synchronized
wall_clock
```

Globally synchronized blinking can be useful when many indicators share the same status.

Example:

```text
all failed indicators blink together
```

rather than each blinking at unrelated phases.

---

# 24. Renderer

The renderer consumes:

- current monitor states;
- status appearance definitions;
- indicator layout;
- current time.

It produces the logical display frame.

Conceptually:

```text
StateStore
   +
StatusDefinitions
   +
Layout
   +
Clock
   ↓
Renderer
   ↓
Frame
```

The renderer should not perform network operations.

---

# 25. Indicator Layout

A 16×16 display provides enough pixels for more than simple one-pixel indicators.

Indicators should eventually support regions.

Example:

```yaml
indicators:

  internet:
    source: internet
    x: 0
    y: 0
    width: 4
    height: 4

  backup:
    source: backup
    x: 4
    y: 0
    width: 4
    height: 4
```

A basic indicator may simply fill its assigned area with its status appearance.

Later indicator/render types could include:

```text
solid region
icon
bar
meter
sparkline
text glyph
border
background
```

These should be considered rendering features rather than monitor features.

---

# 26. Potential Rendering Layers

A future renderer could use layers:

```text
background
    ↓
status regions
    ↓
icons
    ↓
alerts
    ↓
global overlays
```

This is not necessary for initial implementation, but the architecture should avoid making it impossible.

---

# 27. Common Monitor Result

Every pull runner should normalize its output.

Conceptually:

```cpp
struct MonitorResult {
    bool transport_success;

    Value value;

    uint32_t latency_ms;

    ErrorCode error;

    std::string detail;

    Timestamp timestamp;
};
```

Different runners may populate different fields.

---

# 28. Evaluator

The evaluator maps a `MonitorResult` into a named status.

Generic comparison operations should include:

```text
exists
not_exists
equals
not_equals
contains
not_contains
greater_than
greater_or_equal
less_than
less_or_equal
between
```

Example:

```yaml
expect:
  value:
    less_than: 90
```

A monitor may need multiple thresholds.

Example:

```yaml
evaluate:

  - when:
      value:
        greater_or_equal: 95
    status: critical

  - when:
      value:
        greater_or_equal: 85
    status: warn

  - otherwise:
      status: ok
```

This enables custom statuses without hard-coding application-specific logic.

---

# 29. Distinguish State From Transport Failure

At minimum, distinguish:

### Explicit Bad State

The remote system answered and reported something unhealthy.

Example:

```text
HTTP 200
JSON health = degraded
```

Result:

```text
warn
```

### Communication Failure

PixelStatus NX could not determine the state.

Examples:

```text
DNS failure
TCP timeout
TLS failure
HTTP timeout
SNMP timeout
```

Result:

```text
communication_failure
```

### Stale

The last known state is too old.

Result:

```text
stale
```

Those three cases should be independently stylable.

---

# 30. State Store

The central State Store should contain current state independent of source type.

Conceptually:

```cpp
struct MonitorState {
    std::string id;

    std::string status;

    Value value;

    std::string message;

    Timestamp observed_at;

    Timestamp updated_at;

    optional<Duration> ttl;
};
```

Push and pull sources both ultimately update this same structure.

---

# 31. Configuration Storage

Configuration should be runtime editable.

Suggested storage split:

```text
NVS
├── Wi-Fi credentials
├── hostname
├── device identity
├── secrets
└── system settings

LittleFS
├── monitors.json
├── statuses.json
├── layout.json
└── display.json
```

JSON is preferable to YAML on the ESP32 itself.

Desktop tools or import utilities can translate YAML into JSON if desirable.

---

# 32. Web Management Interface

PixelStatus NX should eventually expose a local management interface, for example:

```text
http://pixelstatus-nx.local/
```

Possible sections:

```text
Dashboard
Monitors
Push Sources
Statuses
Appearance Editor
Layout
Display
Network
Logs
System
Firmware
```

An appearance editor could preview:

```text
solid
blink
pulse
fade
sequence
```

directly in the browser.

---

# 33. API

A useful API should expose at least:

```text
GET  /api/v1/status
GET  /api/v1/status/{id}

POST /api/v1/status
POST /api/v1/status/{id}

GET  /api/v1/monitors

GET  /api/v1/display
```

Configuration mutation endpoints can be added later.

The API and internal state model should use the same status names.

---

# 34. MI Bluetooth Display Driver

The current display is controlled as a BLE peripheral through GATT writes. Protocol
knowledge has different evidence levels and should not be treated uniformly.

## Validated on the User's Display

- the upstream `draw_pixels.py` path controls the display;
- graffiti-mode initialization uses `BC 00 01 01 55` followed by
  `BC 00 0D 0D 55`;
- the working single-pixel packet rule uses the trailing-byte behavior documented
  in the Python handoff rather than the conflicting upstream protocol note;
- rewriting all 256 pixels through individual writes takes roughly five seconds on
  the current Windows host.

## Derived From the Upstream Implementation

- advertised name: `MI Matrix Display`;
- service UUID: `0000ffd0-0000-1000-8000-00805f9b34fb`;
- write characteristic UUID: `0000ffd1-0000-1000-8000-00805f9b34fb`;
- block-mode initialization: `BC 0F F1 08 08 55`;
- eight 100-byte block packets are expected to transfer a complete frame.

These upstream-derived details are suitable starting points but should be recorded
as device-validated only after the corresponding calibration and block tests pass.

## Still To Validate

- physical pixel ordering, origin, rotation, mirroring, and serpentine behavior;
- RGB color ordering;
- negotiated ATT MTU and reliable maximum write size;
- write-with-response versus write-without-response behavior;
- stable block delay and maximum practical frame rate;
- whether graffiti and block modes can be switched or interleaved safely;
- brightness commands, if supported;
- disconnect, reconnect, and current-frame restoration behavior.

The firmware driver should contain three focused parts:

```text
MiBleOutputDriver
├── frame diffing, coalescing, mode choice, and driver state
├── MI packet encoder with pure byte-building functions
└── ESP-IDF NimBLE GATT transport
```

Sparse/full-frame selection belongs inside this driver. Automatic mode selection
must not be enabled until mode-switching behavior and the crossover threshold have
been measured. The renderer remains unaware of all MI-specific details.

---

# 35. Direct LED Support

Directly connected LEDs should be supported as an alternative output backend.

Potential initial targets:

```text
WS2812B
SK6812
```

Potential later target:

```text
APA102
```

Configuration may include:

```yaml
display:
  driver: ws2812

  width: 16
  height: 16

  pin: 18

  layout:
    serpentine: true
    origin: top_left

  color_order: GRB
```

Mapping `(x,y)` to physical LED index belongs inside the output driver or its geometry adapter.

---

# 36. Display Failure

Output failure should itself be observable internally.

Examples:

```text
BLE disconnected
BLE reconnecting
direct LED driver initialization failed
```

The system should retain monitoring state even while the display is unavailable.

When the display reconnects, the current frame should be restored immediately.

---

# 37. Concurrency Model

A reasonable FreeRTOS decomposition:

```text
Wi-Fi / TCP-IP subsystem

Scheduler task
       ↓
Monitor work queue
       ↓
2–4 monitor workers
       ↓
State Store

HTTP server task ───────────┐
                            │
MQTT task ──────────────────┤
                            v
                       State Store
                            │
                            v
                       Renderer
                            │
                            v
                     Output Driver
```

The renderer may run continuously at a modest frame rate such as:

```text
20–60 FPS
```

when animations are active.

Network monitors operate on much slower schedules.

---

# 38. Resource Discipline

Network operations should have explicit limits:

```text
timeout
maximum body size
maximum JSON size
maximum simultaneous connections
maximum number of monitors
maximum response buffer
```

A monitoring endpoint returning 20 MB of JSON should not exhaust the ESP32.

HTTP/JSON monitors only need enough response data to perform configured checks.

---

# 39. Security

At minimum:

- TLS certificate validation should be enabled by default.
- Secrets should not be exposed through normal API responses.
- Push API should support authentication.
- Management interface should support authentication.
- Internet-facing inbound access should not be assumed.
- MQTT should support TLS and authentication.
- Configuration should distinguish secret values from ordinary values.

The device should normally initiate outbound Internet connections rather than require public inbound access.

---

# 40. OTA Firmware Updates

OTA should be considered a core appliance feature.

Firmware updates should not require physical USB access.

Configuration must survive firmware updates.

A dual-partition OTA strategy with rollback support is preferable.

---

# 41. Boot Behavior

Desired startup sequence:

```text
boot
 ↓
load persistent configuration
 ↓
initialize state store
 ↓
initialize display
 ↓
show startup state
 ↓
connect Wi-Fi
 ↓
synchronize time
 ↓
start push interfaces
 ↓
start scheduler
 ↓
begin monitoring
```

Previously persisted states could optionally be displayed immediately but should clearly become stale until refreshed.

---

# 42. Suggested V1 Scope

This section describes the intended V1 release envelope, not the first implementation
increment. V1 should be built through independently testable vertical slices.

## Platform

- ESP32-S3
- ESP-IDF
- FreeRTOS

## Output

- existing Bluetooth 16×16 RGB display;
- simulator output;
- direct WS2812/SK6812 support.

## Pull Monitors

- Ping;
- DNS;
- TCP connect;
- TCP exchange;
- HTTP/HTTPS;
- HTTP JSON;
- TLS certificate;
- SNMP v1/v2c GET.

## Push Inputs

- HTTP status API;
- MQTT subscriptions.

## Scheduler

- intervals;
- simple cron-like scheduling;
- bounded worker pool.

## States

- built-in defaults;
- user-defined custom statuses.

## Appearance

- solid;
- blink;
- flash;
- toggle;
- fade;
- pulse;
- repeating sequence.

## Display

- logical framebuffer;
- layout independent of physical output;
- animated rendering.

## Persistence

- NVS;
- LittleFS.

## Management

- basic local HTTP API;
- eventually web UI.

## Maintenance

- OTA firmware updates.

## Initial Implementation Milestone

The first implementation milestone should establish one complete path:

```text
versioned JSON configuration
        ↓
in-memory state update
        ↓
state store
        ↓
solid and blink appearances
        ↓
rectangular indicator layout
        ↓
logical framebuffer
        ↓
simulator output
```

This milestone should also define the shared value type, timestamp and TTL semantics,
configuration validation behavior, output-driver ownership/backpressure contract,
and byte-level MI protocol test vectors. It does not require network monitors or
physical display access.

After the simulator path is verified, add the native MI BLE driver as the second
output while keeping the same framebuffer and renderer contracts.

---

# 43. Explicitly Out of Scope for V1

Do not initially implement:

- arbitrary shell commands;
- Python execution in production firmware;
- Lua;
- JavaScript execution;
- SSH command execution;
- full SNMP MIB browser;
- SNMPv3;
- Prometheus server;
- historical time-series database;
- complex alert escalation;
- email sending;
- SMS sending;
- general monitoring-agent functionality;
- sophisticated dashboard graphics.

These belong either to external systems or later extensions.

---

# 44. Suggested Source Architecture

A possible repository organization:

```text
pixelstatus-nx/
├── firmware/
│   ├── main/
│   │
│   └── components/
│       ├── core/
│       │   ├── state_store/
│       │   ├── scheduler/
│       │   ├── evaluator/
│       │   └── config/
│       │
│       ├── monitors/
│       │   ├── ping/
│       │   ├── dns/
│       │   ├── tcp/
│       │   ├── http/
│       │   ├── http_json/
│       │   ├── tls/
│       │   └── snmp/
│       │
│       ├── push/
│       │   ├── http_api/
│       │   └── mqtt/
│       │
│       ├── rendering/
│       │   ├── framebuffer/
│       │   ├── appearance/
│       │   ├── layout/
│       │   └── renderer/
│       │
│       └── output/
│           ├── ble_display/
│           ├── ws2812/
│           └── simulator/
│
├── simulator/
├── tools/
├── docs/
├── examples/
└── README.md
```

Exact structure can evolve, but subsystem boundaries should remain clear.

---

# 45. Core Interfaces

A likely conceptual interface set:

```cpp
class MonitorRunner {
public:
    virtual MonitorResult run(const MonitorConfig&) = 0;
};
```

```cpp
class Evaluator {
public:
    virtual Status evaluate(
        const MonitorResult& result,
        const EvaluationConfig& config
    ) = 0;
};
```

```cpp
class Appearance {
public:
    virtual RGB sample(Duration elapsed) const = 0;
};
```

```cpp
class Renderer {
public:
    virtual void render(
        const StateStore& states,
        Timestamp now,
        Frame& output
    ) = 0;
};
```

```cpp
class OutputDriver {
public:
    virtual bool begin() = 0;
    virtual FrameSubmitResult submitFrame(const Frame&) = 0;
    virtual DriverState state() const = 0;
};
```

---

# 46. Architectural Invariant

The most important constraint should remain:

```text
Monitoring produces state.

State has meaning.

Appearance represents state.

Layout places appearance.

Rendering generates pixels.

Output drivers transport pixels.
```

No layer should collapse those concepts unnecessarily.

In particular:

```text
HTTP monitor
```

should never contain logic equivalent to:

```cpp
if (http_status != 200)
    pixels[37] = RED;
```

Instead:

```text
HTTP result
    ↓
FAIL
    ↓
status definition
    ↓
flashing red
    ↓
indicator layout
    ↓
pixels
    ↓
output driver
```

This separation is what allows PixelStatus NX to evolve beyond both its current Bluetooth 16×16 display and the much more constrained architecture of the original PixelStatus.

---

# 47. Initial Implementation Approval Plan

Implementation should proceed through the following approval gates.

## Gate 1: Contracts and Repository Skeleton

- create the ESP-IDF project and component directories;
- select and pin the initial ESP-IDF toolchain version;
- define the frame, value, monitor-state, status, time, and output-driver types;
- define versioned minimal JSON schemas and validation rules;
- add host-runnable tests for pure core logic and MI packet vectors.

Deliverable: a building skeleton with tests, but no networking or hardware access.

## Gate 2: Simulator Vertical Slice

- implement state transitions and TTL/stale behavior;
- compile solid and blink appearances to the timeline representation;
- render rectangular indicators into a logical framebuffer;
- display or export frames through a simulator driver.

Deliverable: configuration-to-pixels behavior that can be tested without hardware.

Status: complete on the native Win32 target. The timeline compiler now covers the
full initial appearance grammar, not only solid and blink.

## Gate 3: MI Hardware Validation and Native Driver

- run the Python calibration, block, MTU, response-mode, delay, and reconnect tests;
- record results in the MI handoff;
- implement the native NimBLE transport and MI packet encoder;
- add frame coalescing and conservative reconnect behavior;
- enable sparse/full-frame selection only if the measurements justify it.

Deliverable: the same simulator-rendered frames displayed on the MI matrix.

## Gate 4: First External State Input

- add the authenticated local HTTP status endpoint;
- apply TTL and validation limits;
- confirm that pushed states update both simulator and MI outputs identically.

Deliverable: an end-to-end ambient status appliance with one push interface.

Status: the hardware-independent portion is complete and host-tested. The portable
authenticated handler updates the simulator through localhost HTTP. Confirming the
same path through the MI output remains part of Gate 3 hardware validation.

## Later V1 Gates

Add scheduling and pull monitors incrementally, followed by MQTT, direct LEDs,
persistence hardening, management UI, and OTA. Each monitor and output backend should
enter through the established state, rendering, and driver contracts.

Status: the portable interval scheduler, runner boundary, normalized result, generic
evaluator, strict monitor JSON grammar, and desktop HTTP/JSON (including request
methods, headers, and bodies), TCP-connect,
TCP-exchange, and DNS runners are host-tested. The Win32 host also has a bounded
multi-worker executor with per-monitor in-flight exclusion. HTTPS, additional
network runners, secret-provider integration, jitter/cron support, and in-flight
cancellation remain incremental follow-on work.
