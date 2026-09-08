# Linux Host: Browser Display and MI Bluetooth LE

PixelStatus NX has a native Linux headless target. It runs the same C++20 monitor,
state, layout, renderer, and framebuffer code as Windows, serves the responsive
browser display directly, and can mirror that framebuffer to the MI Matrix Display
through the existing Python/Bleak bridge.

The Linux target does not build a desktop window. The browser is the native Linux
display backend; `--no-window` is therefore the default as well as an accepted
cross-platform option.

## Supported Boundary

| Capability | Linux status |
| --- | --- |
| Core, configuration, cards, layouts, rendering | Native C++20; same code and tests as Windows |
| Browser display and read-only frame API | Native C++ with `cpp-httplib` |
| HTTP, HTTPS, TCP connect/exchange, and DNS monitors | Native C++; HTTPS uses OpenSSL 3 and the system CA paths |
| Named monitor secrets | `PIXELSTATUS_SECRET_<NAME>` environment variables |
| MI Bluetooth LE output | Existing Python bridge through Bleak's BlueZ D-Bus backend |
| ICMP monitor | Not yet implemented on Linux; configurations using `icmp_ping` fail at startup |
| Native desktop window | Not implemented; use the browser backend |
| UniFi and OpenWrt sidecars | Portable Python, but their Linux secret/service packaging has not yet been live-validated |

Windows continues to use WinHTTP/Schannel for HTTPS, Credential Manager as an
optional secret source, and the Win32 window. These choices do not affect the
portable monitor or output contracts.

## Clean Ubuntu/Debian Bootstrap

The documented reference host is Ubuntu 24.04 LTS or a current Debian derivative.
Equivalent packages work on other distributions. Start from a normal unprivileged
user account:

```bash
sudo apt update
sudo apt install --yes \
    git g++ cmake ninja-build libssl-dev \
    python3 python3-venv python3-pip \
    bluez

git clone https://github.com/AlphaPixel/PixelStatus-NX.git
cd PixelStatus-NX
```

The repository requires CMake 3.25+, a C++20 compiler, Ninja, OpenSSL 3 development
files for Linux HTTPS, and Python 3.10+ for the optional BLE path. Confirm the host
before building:

```bash
cmake --version
c++ --version
python3 --version
openssl version
```

Configure, build, and run the complete public regression:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

CMake downloads the hash-pinned `nlohmann/json` and `cpp-httplib` sources into the
ignored build tree. It does not install project dependencies globally. The Linux
preset requires OpenSSL instead of silently producing a build that rejects HTTPS.

Start the sample browser display in the foreground:

```bash
bash tools/run-linux-demo.sh
```

Open <http://127.0.0.1:8788/>. Supply another configuration as the first argument,
and pass normal simulator options after it:

```bash
bash tools/run-linux-demo.sh examples/card-deck.example.json --web-refresh-ms 250
```

The launcher respects these optional environment variables:

```text
PIXELSTATUS_BUILD_DIR   build containing pixelstatus_simulator
PIXELSTATUS_CONFIG      default configuration path
PIXELSTATUS_WEB_BIND    browser bind address (default 127.0.0.1)
PIXELSTATUS_WEB_PORT    browser port (default 8788)
```

For another computer on a trusted LAN, bind to all interfaces and allow the selected
TCP port through the host firewall:

```bash
PIXELSTATUS_WEB_BIND=0.0.0.0 PIXELSTATUS_WEB_PORT=8788 \
    bash tools/run-linux-demo.sh
```

The browser endpoint is read-only but has no viewer authentication or TLS. Keep it
on loopback unless the network is trusted; use an authenticated TLS reverse proxy
for permanent remote or Internet access. The mutation/status API remains bound to
loopback independently of the browser bind address.

## HTTPS and Secrets

Linux HTTPS monitors use `cpp-httplib` with OpenSSL 3. Server-certificate and
hostname verification remain enabled, and OpenSSL uses the host's configured trust
paths. Install a private appliance CA into the operating system trust store when
appropriate. Per-monitor custom CAs and certificate/SPKI pins remain backlog items.

Linux resolves named configuration references from environment variables only. A
reference such as `${secret:truenas-api-key}` maps to:

```bash
export PIXELSTATUS_SECRET_TRUENAS_API_KEY='replace-me'
```

For systemd, place assignments in `~/.config/pixelstatus-nx/environment`, restrict
the file, and restart an installed service after changes. Use an editor so the
secret is not copied into shell history:

```bash
install -d -m 700 ~/.config/pixelstatus-nx
umask 077
${EDITOR:-nano} ~/.config/pixelstatus-nx/environment
chmod 600 ~/.config/pixelstatus-nx/environment
systemctl --user restart pixelstatus-nx.service
```

The file uses one `NAME=value` assignment per line, for example
`PIXELSTATUS_SECRET_TRUENAS_API_KEY=replace-me`.

Do not commit that file or a site-specific configuration. Repository-local `.env`
files, `secrets/`, and `examples/operations.local.json` are Git-ignored.

## MI Display Through BlueZ

Bleak 3.0.2 requires Python 3.10+ and a Linux distribution with BlueZ 5.55 or newer.
Its Linux backend talks to the system BlueZ daemon over D-Bus; PixelStatus does not
use raw HCI sockets or a Linux-specific MI protocol implementation.

Create an isolated environment and install the repository-pinned bridge dependency:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install -r tools/requirements-mi-ble.txt
```

Enable the host adapter and verify that it is powered:

```bash
bluetoothctl --version
sudo systemctl enable --now bluetooth.service
bluetoothctl show
```

Then exercise discovery and GATT access before installing a service:

```bash
.venv/bin/python -m tools.mi_ble scan
.venv/bin/python -m tools.mi_ble probe
.venv/bin/python -m tools.mi_ble bridge \
    --url http://127.0.0.1:8788/api/v1/display \
    --mode block \
    --poll-ms 250
```

The target normally does not need to be paired. It must be powered, in radio range,
and disconnected from the Merkury application or another bridge. Bleak selects the
BlueZ backend automatically. Set `BLEAK_LOGGING=1` for detailed discovery, D-Bus,
and GATT diagnostics. If `bluetoothctl show` reports no controller, correct the
adapter, driver, firmware, hardware/radio-block, or virtual-machine passthrough
before debugging PixelStatus.

Containers and remapped user namespaces add a separate host-D-Bus authentication
boundary. They are not the recommended deployment; if unavoidable, follow Bleak's
`BLEAK_DBUS_AUTH_UID` guidance and expose the host BlueZ D-Bus deliberately rather
than granting a privileged container broad device access.

The protocol and framebuffer paths are covered by platform-independent Python tests.
The current MI panel has been physically validated on Windows; Linux discovery,
connection, and sustained writes still require acceptance on an actual BlueZ host.

## Persistent User Services

After the release build succeeds, the supplied installer renders path-specific
systemd user units from `packaging/systemd/` and starts the browser service:

```bash
bash tools/install-linux-user-services.sh \
    --config "$PWD/examples/card-deck.example.json"
```

Include the independently supervised BLE bridge after the interactive scan and
probe succeed:

```bash
bash tools/install-linux-user-services.sh \
    --config "$PWD/examples/card-deck.example.json" \
    --with-ble \
    --mi-mode block
```

Useful service commands are:

```bash
systemctl --user status pixelstatus-nx.service pixelstatus-nx-mi.service
journalctl --user -u pixelstatus-nx.service -u pixelstatus-nx-mi.service -f
systemctl --user restart pixelstatus-nx.service pixelstatus-nx-mi.service
```

User services normally stop when the last login session ends. To start them at boot
and keep them after logout, an administrator can enable lingering for this account:

```bash
sudo loginctl enable-linger "$USER"
```

The generated units use `Restart=on-failure` for the C++ runtime and
`Restart=always` for the BLE bridge. The bridge also performs bounded in-process BLE
reconnection and restores a complete newest frame after reconnect. Installation is
source-tree based: do not delete or move the clone, build directory, configuration,
or `.venv` while the units refer to them. Re-run the installer after rebuilding in a
new location or changing the bind, port, configuration, Python environment, or BLE
mode.

## Scratch Acceptance Checklist

On a new Linux machine, completion means all of the following pass:

1. `cmake --preset linux-release`, build, and `ctest` finish successfully.
2. `curl --fail http://127.0.0.1:8788/api/v1/display` returns an `rgb888` frame whose
   dimensions and pixel count match the configuration.
3. The browser page scales correctly and its sequence advances.
4. A public HTTPS monitor classifies a valid system-trusted endpoint successfully.
5. If BLE is required, `scan`, `probe`, and a displayed test frame succeed as the
   intended service user, followed by a restart/reconnect check.
6. If persistence is required, both selected user units are active after logout or
   reboot with lingering enabled.

The public Linux CI job compiles the full headless target and runs the deterministic
C++ and Python suites on Ubuntu. It cannot prove Bluetooth radio access or render a
physical MI panel; those are deliberately called out as host acceptance steps.

## Recorded Linux Validation

On 2026-08-23, the documented build was exercised in a newly provisioned Ubuntu
x86-64 WSL environment with GCC 15.2, CMake 4.2, OpenSSL 3.5, Python 3.14, and
BlueZ 5.85:

- the release preset built without warnings and all four CTest suites passed;
- the card-deck browser endpoint returned HTTP 200, a 16×16 `rgb888` frame, exactly
  256 pixels, and an advancing sequence;
- the public HTTPS example rendered its healthy color through OpenSSL system trust;
- a fresh temporary virtual environment installed and imported pinned Bleak 3.0.2;
- `systemd-analyze --user verify` accepted both rendered user units.

That WSL instance did not expose a Bluetooth controller and its BlueZ service was
inactive, so it cannot count as physical Linux MI validation or a reboot/lingering
service test. Those remain explicit native-host acceptance items.
