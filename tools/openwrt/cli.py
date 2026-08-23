from __future__ import annotations

import argparse
import json
import sys

from .client import OpenWrtError, PinnedUbusClient, build_snapshot, resolve_named_secret
from .server import serve


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Query a read-only OpenWrt ubus endpoint through pinned TLS.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("probe", "serve"):
        command = subparsers.add_parser(name)
        command.add_argument("--url", required=True)
        command.add_argument("--certificate-sha256", required=True)
        command.add_argument("--username", default="pixelstatus")
        command.add_argument("--secret-name", default="openwrt-api-password")
        command.add_argument("--uplink-interface", default="wwan")
        command.add_argument("--timeout-seconds", type=float, default=10.0)
        if name == "serve":
            command.add_argument("--port", type=int, default=18951)
            command.add_argument("--interval-seconds", type=float, default=15.0)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "serve":
            if not 1 <= args.port <= 65535:
                raise OpenWrtError("port must be between 1 and 65535")
            if not 1 <= args.interval_seconds <= 3600:
                raise OpenWrtError("refresh interval must be between 1 and 3600 seconds")
        if not args.uplink_interface or len(args.uplink_interface) > 32:
            raise OpenWrtError("uplink interface name is invalid")
        client = PinnedUbusClient(
            args.url,
            args.certificate_sha256,
            args.username,
            resolve_named_secret(args.secret_name),
            timeout_seconds=args.timeout_seconds,
        )
        if args.command == "probe":
            document = build_snapshot(client, uplink_interface=args.uplink_interface)
            print(json.dumps(document, indent=2, sort_keys=True))
            return 0
        serve(
            client,
            port=args.port,
            interval_seconds=args.interval_seconds,
            uplink_interface=args.uplink_interface,
        )
        return 0
    except OpenWrtError as error:
        print(f"OpenWrt monitor error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
