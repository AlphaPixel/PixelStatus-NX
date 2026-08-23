# Development Roadmap

This roadmap preserves the host-first strategy: finish and regress configuration,
monitoring, layout, framebuffer, and output behavior on Windows before introducing
an ESP32 toolchain. [Implementation Status and Loose Ends](implementation-status.md)
is the exhaustive capability ledger; this page records sequencing and exit gates.
Dates reflect the latest audit on 2026-08-22. Remaining estimates are active
engineering time and exclude waiting for credentials, routing changes, soak time,
or physical test sessions.

| Stage | Status | Remaining estimate | Deliverable | Remaining exit gate |
| --- | --- | ---: | --- | --- |
| 0. Host foundation | Complete | — | Portable C++20 state/rendering core, schema, Win32/browser outputs, push API, scheduler/evaluator, and five Windows pull transports | None |
| 1. Explicit composite cards | Complete | — | Bounded `layout` card with explicit AABB indicators and clocks | None |
| 2. Split/layered layouts | Complete | — | Row/column splits, stacks, aggregate backgrounds, bars, grids, clocks, indicators, and bitmaps | None |
| 3. Live operations deck | Complete | — | Private three-card live deck with remote and LAN/WAN checks, clocks, card-wide health layers, and deterministic regression | None |
| 4. Appliance adapters | In progress | 3–6 days plus access | Sanitized read-only vendor data behind ordinary monitor sources | TrueNAS API, Netgear decision, and Starlink dish telemetry remain |
| 5. Windows MI output | Operational; reliability gate open | 1–2 days plus soak/device sessions | Browser-frame-to-Bleak bridge using validated pixel and block protocols | Forced reconnect test, longer soak, and mode-switch/crossover decision |
| 6. Windows background runtime | Operational; hardening optional | 1–3 days if selected | Immutable LocalAppData releases, per-user Scheduled Task, logs, and health inspection | Optional retention, rollback, dependency bundling, and installer work |
| 7. ESP32 production port | Not started | 8–15 days plus hardware sessions | ESP-IDF application reusing portable behavior with native networking, storage, and MI/direct-LED outputs | Toolchain selection through final hardware/soak acceptance |

## Completed Host Stages

Stages 0–3 are complete. The core supports validated versioned JSON, custom named
statuses, TTL/stale behavior, all V1 appearance timelines, fixed indicator layouts,
and 1–32-card decks with instant, fade, or four-way slide transitions. Layout cards
resolve either explicit widgets or bounded recursive trees to deterministic AABBs.
Rows and columns support fixed/weighted allocation and gaps; stacks establish
back-to-front layering. Six widget types cover indicators, one-line clocks, numeric
bars, row-major status grids, exact bitmaps, and worst-priority aggregate health.

The Windows monitor path includes normalized results, ordered evaluation, interval
scheduling, one-to-eight workers with per-monitor in-flight exclusion, and HTTP(S),
TCP connect, ICMP, TCP text exchange, and DNS runners. The bearer-authenticated
status API and both display outputs use the same state store and framebuffer.

The ignored operations profile is the live acceptance composition:

- AlphaPixel bitmap for 2.5 seconds;
- eleven remote states plus UTC time for three seconds;
- six LAN/WAN states plus local time for three seconds;
- instant transitions, because the physical MI panel visibly updates at about 4 Hz;
- black healthy, subdued amber warning/stale, and subdued red failure backgrounds
  beneath bright individual state cells.

Its ignored regression validates the 8.5-second framebuffer loop and all configured
live monitors without exposing local addresses or credentials in Git.

## Stage 4: Appliance Data Adapters

The generic HTTP runner already provides named secret headers, bounded requests,
RFC 6901 scalar extraction, array counts, and numeric ratios. Public authenticated
fixtures exercise TrueNAS- and UniFi-shaped data through monitor evaluation and a
composite display.

Two real Windows adapters are complete:

- the official local UniFi Network API collector publishes sanitized gateway,
  application, site, device, and statistics data from a pre-auth certificate-pinned
  connection;
- the restricted OpenWrt rpcd collector publishes sanitized Wi-Fi bridge association
  and Starlink DHCP/default-route state from another pinned connection.

This stage remains open for deeper TrueNAS CORE pool/alert/disk/utilization data, a
decision on model-specific Netgear telemetry, and official Starlink dish gRPC data.
The current TrueNAS and Netgear cells are UI/reachability checks; the Starlink cell
is genuine router-path state but not obstruction, latency, alignment, or outage
telemetry.

## Stage 5: Windows MI Output

The Python/Bleak adapter is a functioning Windows output backend. It discovers
`MI Matrix Display`, validates exact 16×16 `rgb888` source frames, bounds reconnect
backoff, keeps only the newest pending frame, restores a full frame on connection,
diffs sparse pixel updates, and can send eight block packets for full frames. An
atomic heartbeat reports connection state, last source frame, write count, and time
without storing the Bluetooth address.

The actual panel confirmed upper-left row-major RGB mapping, no rotation or mirror,
top-to-bottom two-row blocks, and parity with the browser frame. Pixel and block
paths both work; the private deck explicitly uses block mode. Remaining work is a
forced disconnect during transfer, a longer unattended soak, and testing whether
same-connection pixel/block switching is safe enough to justify automatic mode
selection. A native C++/WinRT replacement is optional.

## Stage 6: Windows Background Runtime

The tested runtime is published as immutable timestamped releases below
`%LOCALAPPDATA%\AlphaPixel\PixelStatus-NX`. An atomic `current.json` pointer selects
the release, `current.previous.json` retains the prior selection, and a per-user
interactive-logon task runs it independently of the working tree. Publishing stops
and restarts an existing task safely. Logs rotate once at 5 MiB, and one status
command checks the task, collectors, browser frame, and MI heartbeat.

The live task was registered and verified on 2026-08-22. Optional hardening is a
safe old-release retention command, one-command rollback, bundled Python/Bleak or a
versioned installer, and automated long-duration health/restart testing. Logged-out
service execution is not a goal while BLE and user-scoped credentials require the
interactive user.

## Stage 7: ESP32 Production Port

Do not start this stage until the remaining Windows behavior that affects the
portable contracts is settled. Then:

1. Select and pin ESP-IDF and create the component/application build without
   weakening the host test path.
2. Add Wi-Fi provisioning, SNTP, FreeRTOS execution, NVS/LittleFS, protected
   secrets, recovery, watchdog, logs, and OTA rollback.
3. Implement native HTTP/ESP-TLS, ping, and any approved appliance adapters behind
   the existing runner contract.
4. Implement the MI central/GATT output through ESP-IDF NimBLE using the validated
   packet vectors and full-restore behavior.
5. Add WS2812/SK6812 output if it remains a V1 requirement.
6. Run resource, disconnect, power-loss, and long-duration tests on hardware.

Python is appropriate for the Windows vendor and BLE adapters, but it is not part of
the firmware plan. Most shared/core and firmware code remains C++20; small host
adapters and operations tooling remain Python and PowerShell where native host APIs
make those languages the simpler boundary.
