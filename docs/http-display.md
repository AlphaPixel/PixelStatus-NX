# Browser Display Backend

The host HTTP display is a read-only `OutputDriver` that retains the newest logical
frame and serves it to a browser. It consumes exactly the same `Frame` produced for
the Win32 simulator and future physical drivers; monitor, state, appearance, layout,
and rendering code do not know that a browser is attached.

This backend is implemented, host-tested, and used by both live browser viewing and
the Windows MI bridge. It is a production-useful desktop output, not a configuration
management UI. See [Implementation Status and Loose Ends](implementation-status.md).

The simulator starts this backend by default on loopback port 8788:

```powershell
.\out\build\windows-debug\Debug\pixelstatus_simulator.exe
```

Open `http://127.0.0.1:8788/`. The native and browser windows display the same
logical framebuffer. To run only the browser backend:

```powershell
.\out\build\windows-debug\Debug\pixelstatus_simulator.exe --no-window
```

The relevant command-line options are:

```text
--web-display-port N          listen on a different port (default: 8788)
--web-display-bind ADDRESS    bind to another interface (default: 127.0.0.1)
--web-refresh-ms N            initial client refresh period, 16–300000 ms
--no-web-display              disable the browser backend
--no-window                   disable the native Win32 backend
```

At least one display backend must remain enabled. Binding to `0.0.0.0` makes the
read-only display visible on every available interface; the current display server
does not authenticate viewers or provide TLS, so loopback remains the safe default.
For permanent remote access, put it behind an authenticated TLS reverse proxy or add
equivalent server support; do not treat a temporary port-forward as secure.

## Frame API

`GET /api/v1/display` returns the newest complete frame:

```json
{
  "schema_version": 1,
  "width": 2,
  "height": 1,
  "sequence": 42,
  "format": "rgb888",
  "default_refresh_ms": 100,
  "pixels": [16711680, 65280]
}
```

`pixels` is a row-major array of packed `0xRRGGBB` integers and contains exactly
`width * height` entries. The configured display dimensions determine the grid;
the browser has no 16×16 assumption. Each response includes an `ETag`. A request
whose `If-None-Match` value still names the newest frame receives `304 Not Modified`.

The page, manifest, and icon are available from:

```text
GET /
GET /index.html
GET /manifest.webmanifest
GET /favicon.svg
```

## Browser Behavior

The page renders into a canvas with nearest-neighbor scaling. Its CSS constrains
the canvas by both viewport dimensions, preserves the server-provided aspect ratio,
and therefore keeps logical pixels square on resize.

Moving the pointer into the upper-right corner reveals the refresh selector. Rates
range from five minutes to 60 requests per second and persist locally when browser
storage is available. Polls do not overlap, hidden tabs stop fetching, failed
requests retain the last frame, and the client reconnects automatically.

Ordinary web pages cannot remove a browser's address bar or tab strip. This page
reduces its own chrome to an auto-hiding control and offers two browser-controlled
ways to reduce the remaining browser chrome without forcing fullscreen:

- **Open minimal display window** requests a popup-style resizable window.
- The web app manifest requests `standalone` display when the page is installed as
  an application or shortcut by a supporting browser.

The browser ultimately decides whether either mode removes its navigation controls.

## Verification

The host unit suite tests the frame schema, dimensions, packed colors, sequence,
ETag handling, page assets, lifecycle, and output-driver coalescing. The executable
smoke test starts the simulator without a Win32 window and verifies a rendered 16×16
frame through the HTTP API:

```powershell
.\tools\test-web-display.ps1
```

The private operations runtime serves the same backend on loopback port 18908 and
its Scheduled Task status check verifies that the sequence continues advancing.
