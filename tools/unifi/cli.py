from __future__ import annotations

import argparse
import json
import sys

from .client import PinnedHttpsJsonClient, UnifiError, build_snapshot, resolve_named_secret
from .server import serve


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Query UniFi Network through a pinned local TLS connection.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("probe", "serve"):
        command = subparsers.add_parser(name)
        command.add_argument("--base-url", required=True)
        command.add_argument("--certificate-sha256", required=True)
        command.add_argument("--secret-name", default="unifi-api-key")
        command.add_argument("--site-id")
        command.add_argument("--device-id")
        command.add_argument("--timeout-seconds", type=float, default=10.0)
        if name == "serve":
            command.add_argument("--port", type=int, default=18950)
            command.add_argument("--interval-seconds", type=float, default=30.0)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "serve":
            if not 1 <= args.port <= 65535:
                raise UnifiError("port must be between 1 and 65535")
            if not 1 <= args.interval_seconds <= 3600:
                raise UnifiError("refresh interval must be between 1 and 3600 seconds")
        client = PinnedHttpsJsonClient(
            args.base_url,
            args.certificate_sha256,
            resolve_named_secret(args.secret_name),
            timeout_seconds=args.timeout_seconds,
        )
        if args.command == "probe":
            document = build_snapshot(client, site_id=args.site_id, device_id=args.device_id)
            print(json.dumps(document, indent=2, sort_keys=True))
            return 0
        serve(
            client,
            port=args.port,
            interval_seconds=args.interval_seconds,
            site_id=args.site_id,
            device_id=args.device_id,
        )
        return 0
    except UnifiError as error:
        print(f"UniFi monitor error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
