#!/usr/bin/env bash

set -euo pipefail

repository_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_directory="${PIXELSTATUS_BUILD_DIR:-${repository_root}/out/build/linux-release}"
simulator="${build_directory}/pixelstatus_simulator"
config="${PIXELSTATUS_CONFIG:-${repository_root}/examples/pixelstatus.sample.json}"

if [[ $# -gt 0 && "$1" != --* ]]; then
    config="$1"
    shift
fi

if [[ ! -x "$simulator" ]]; then
    printf 'Linux simulator is missing: %s\n' "$simulator" >&2
    printf 'Build it with: cmake --preset linux-release && cmake --build --preset linux-release\n' >&2
    exit 1
fi
if [[ ! -f "$config" ]]; then
    printf 'Configuration is missing: %s\n' "$config" >&2
    exit 1
fi

exec "$simulator" "$config" \
    --no-window \
    --web-display-bind "${PIXELSTATUS_WEB_BIND:-127.0.0.1}" \
    --web-display-port "${PIXELSTATUS_WEB_PORT:-8788}" \
    "$@"
