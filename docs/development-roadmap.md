# Development Roadmap

This roadmap preserves the host-first development strategy: configuration,
monitoring, layout, and framebuffer behavior are completed and regression-tested on
Windows before adding Bluetooth or ESP32 toolchains. Estimates are active engineering
time and exclude delays obtaining credentials, network access, or physical hardware.

| Stage | Estimate | Deliverable | Exit gate |
| --- | ---: | --- | --- |
| 0. Host monitoring foundation | Complete | Portable state/rendering core; Win32 and browser outputs; HTTP(S), TCP, DNS, and ICMP host monitors | Public tests and the ignored local operations deck pass |
| 1. Explicit-AABB composite cards | 2–3 days | Additive `layout` card containing bounded indicator and one-line local/UTC clock widgets | Schema, parser, renderer, example, and deterministic portable tests agree |
| 2. Split layout and richer widgets | Complete | Row/column weighted splits with gaps, horizontal/vertical utilization bars, status grids, and bounded bitmap regions | The proposed 16×16 storage/WAN/VPS/UTC card renders from synthetic states without hand-calculated child coordinates |
| 3. Live composite operations deck | Complete | Apply the layout system to the local deck and exercise transitions, stale states, and failures through Win32 and browser outputs | A repeatable live smoke test observes every composite region and transition |
| 4. Appliance data adapters | 5–8 days | Read-only TrueNAS and UniFi integrations, followed by the best available Netgear and Starlink signals | Real pool/disk/WAN values drive the same tested widgets; credentials remain named secrets |
| 5. Windows Bluetooth hardware path | 5–8 days plus device sessions | Send PixelStatus framebuffers from Windows to the physical MI display | Calibration, block writes, pacing, reconnect, and complete-frame restoration are device-validated |
| 6. Bluetooth output hardening | 3–5 days | Frame coalescing, sparse/full-frame policy, health reporting, and soak tests | Slow or disconnected BLE never blocks rendering and the newest complete frame wins |
| 7. ESP32 production port | 8–15 days | ESP-IDF application using the portable core and a native NimBLE central/GATT output transport | The same configuration and golden frames pass on host and hardware without Python in firmware |

## Stage 1: Explicit-AABB Composite Cards

Status: complete on the Windows host and portable core test targets as of 2026-08-21.

A `layout` card is an ordered list
of widgets with explicit `x`, `y`, `width`, and `height` bounds. The first widget
types are:

- `indicator`, which paints a status appearance from a named state source;
- `clock`, which paints one centered 15×7 local or UTC time inside its bounds.

All bounds are checked while loading configuration. Existing `bitmap`, `clock`, and
`indicators` cards remain valid. Widget order is paint order, permitting deliberate
overlays while keeping non-overlapping subsections straightforward.

## Stage 2: Split Layout and Richer Widgets

Status: complete on the Windows host and portable core test targets as of 2026-08-21.

Explicit AABBs remain the canonical resolved form. A recursive `row` or `column`
tree is configuration convenience that deterministically allocates fixed `size` and
proportional `weight` children, including gaps, before rendering. Allocation uses
cumulative integer division so every platform resolves the same remainder pixels.
Output drivers never interpret the tree.

Layout cards now support numeric bars in four fill directions, row-major status
grids, exact-size bounded palette bitmaps, status indicators, and bounded local or
UTC clocks. Parser limits bound nesting, total nodes, leaf widgets, and all resolved
geometry. [`split-layout.example.json`](../examples/split-layout.example.json)
implements the proposed 16×16 storage/WAN/VPS/UTC composition, and
`tools/test-split-layout.ps1` drives its numeric and status sources through the push
API before checking the browser framebuffer.

## Stage 3: Live Composite Operations Deck

Status: complete on the Windows host as of 2026-08-21.

The ignored local operations profile now renders infrastructure as a 3×2 status
grid and all eleven VPS sources as one four-column grid with a blank twelfth cell.
Its ignored integration test validates the live LAN and Internet monitor results,
all four cards, and intermediate framebuffer states from the fade and slide
transitions. It then injects a controlled `fail` state and a one-second TTL state,
confirming both failure and animated stale rendering before the deck wraps.

The acceptance run verified UniFi, TrueNAS CORE, the Netgear cable modem, the HP
printer, ten HTTPS VPS endpoints, and the Lima ICMP endpoint. The Starlink slot
remains an explicit unknown placeholder until a management route or proxy becomes
available. Addresses, hostnames, observations, and the test itself remain in
Git-ignored local files.

## Stage 5: Windows Bluetooth Hardware Path

The first Windows hardware pass uses Python and `bleak` as a diagnostic transport,
not as production firmware. It will consume the current PixelStatus framebuffer,
discover `MI Matrix Display`, and validate the documented GATT protocol on the actual
display. Required experiments are:

1. corner, rotation, mirroring, pixel-order, and RGB-order calibration;
2. eight-block full-frame writes and negotiated write-size behavior;
3. write-with-response versus write-without-response pacing;
4. graffiti/block mode switching and an evidence-based crossover threshold;
5. disconnect, reconnect, and restoration of the newest complete frame.

The quickest usable bridge can poll the read-only display-frame API and forward
frames with `bleak`. If long-running Windows deployment is valuable after protocol
validation, a native C++/WinRT BLE output driver can replace the Python transport
without changing monitoring, layout, renderer, or framebuffer contracts. The later
ESP32 driver reuses packet vectors and behavioral evidence through ESP-IDF NimBLE,
not the Windows transport implementation.
