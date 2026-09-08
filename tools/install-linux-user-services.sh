#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: bash tools/install-linux-user-services.sh [options]

Options:
  --config PATH       configuration file (default: examples/pixelstatus.sample.json)
  --build-dir PATH    Linux build directory (default: out/build/linux-release)
  --bind ADDRESS      browser bind address (default: 127.0.0.1)
  --port PORT         browser port (default: 8788)
  --with-ble          also install and start the MI Bluetooth bridge
  --mi-mode MODE      MI transfer mode: pixel or block (default: block)
  --no-start          install units without enabling or starting them
  --help              show this help
EOF
}

repository_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
config="${repository_root}/examples/pixelstatus.sample.json"
build_directory="${repository_root}/out/build/linux-release"
web_bind="127.0.0.1"
web_port="8788"
with_ble=0
mi_mode="block"
start_services=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            config="${2:?--config requires a path}"
            shift 2
            ;;
        --build-dir)
            build_directory="${2:?--build-dir requires a path}"
            shift 2
            ;;
        --bind)
            web_bind="${2:?--bind requires an address}"
            shift 2
            ;;
        --port)
            web_port="${2:?--port requires a port}"
            shift 2
            ;;
        --with-ble)
            with_ble=1
            shift
            ;;
        --mi-mode)
            mi_mode="${2:?--mi-mode requires pixel or block}"
            shift 2
            ;;
        --no-start)
            start_services=0
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$mi_mode" != pixel && "$mi_mode" != block ]]; then
    printf 'Invalid --mi-mode: %s\n' "$mi_mode" >&2
    exit 2
fi
if [[ ! "$web_port" =~ ^[0-9]+$ ]] || (( web_port < 1 || web_port > 65535 )); then
    printf 'Invalid --port: %s\n' "$web_port" >&2
    exit 2
fi

config="$(realpath -- "$config")"
build_directory="$(realpath -- "$build_directory")"
simulator="${build_directory}/pixelstatus_simulator"
python="${PIXELSTATUS_PYTHON:-${repository_root}/.venv/bin/python}"

if [[ ! -x "$simulator" ]]; then
    printf 'Linux simulator is missing: %s\n' "$simulator" >&2
    exit 1
fi
if [[ ! -f "$config" ]]; then
    printf 'Configuration is missing: %s\n' "$config" >&2
    exit 1
fi
if (( with_ble )) && [[ ! -x "$python" ]]; then
    printf 'BLE Python environment is missing: %s\n' "$python" >&2
    printf 'Create .venv and install tools/requirements-mi-ble.txt first.\n' >&2
    exit 1
fi
if (( with_ble )) && ! "$python" -c 'import bleak' >/dev/null 2>&1; then
    printf 'The selected Python environment cannot import Bleak: %s\n' "$python" >&2
    exit 1
fi

escape_unit_value() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//%/%%}"
    printf '%s' "$value"
}

escape_working_directory() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value// /\\x20}"
    value="${value//$'\t'/\\x09}"
    value="${value//%/%%}"
    printf '%s' "$value"
}

render_unit() {
    local source="$1"
    local destination="$2"
    local content
    content="$(<"$source")"
    content="${content//@PIXELSTATUS_ROOT@/$(escape_unit_value "$repository_root")}"
    content="${content//@PIXELSTATUS_WORKING_DIRECTORY@/$(escape_working_directory "$repository_root")}"
    content="${content//@PIXELSTATUS_SIMULATOR@/$(escape_unit_value "$simulator")}"
    content="${content//@PIXELSTATUS_CONFIG@/$(escape_unit_value "$config")}"
    content="${content//@PIXELSTATUS_WEB_BIND@/$(escape_unit_value "$web_bind")}"
    content="${content//@PIXELSTATUS_WEB_PORT@/$web_port}"
    content="${content//@PIXELSTATUS_PYTHON@/$(escape_unit_value "$python")}"
    content="${content//@PIXELSTATUS_MI_MODE@/$mi_mode}"
    printf '%s\n' "$content" >"$destination"
}

unit_directory="${XDG_CONFIG_HOME:-${HOME}/.config}/systemd/user"
mkdir -p -- "$unit_directory"
render_unit \
    "${repository_root}/packaging/systemd/pixelstatus-nx.service.in" \
    "${unit_directory}/pixelstatus-nx.service"
if (( with_ble )); then
    render_unit \
        "${repository_root}/packaging/systemd/pixelstatus-nx-mi.service.in" \
        "${unit_directory}/pixelstatus-nx-mi.service"
fi

systemctl --user daemon-reload
if (( start_services )); then
    systemctl --user enable --now pixelstatus-nx.service
    if (( with_ble )); then
        systemctl --user enable --now pixelstatus-nx-mi.service
    fi
fi

printf 'Installed PixelStatus NX user service in %s\n' "$unit_directory"
printf 'Browser display: http://%s:%s/\n' "$web_bind" "$web_port"
if (( ! start_services )); then
    printf 'Start it with: systemctl --user enable --now pixelstatus-nx.service\n'
fi
