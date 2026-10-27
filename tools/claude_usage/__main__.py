"""Publish Claude plan usage at http://127.0.0.1:<port>/health.

The collector reads Claude Code's OAuth access token from its credentials file on
every refresh and never writes, refreshes, logs, or republishes it. The usage
endpoint is the undocumented one behind Claude Code's /usage screen, so its shape
may change without notice.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
MAXIMUM_RESPONSE_BYTES = 256 * 1024
WINDOWS = ("five_hour", "seven_day")


class UsageError(RuntimeError):
    """A sanitized credential, transport, or response failure."""


def credentials_path() -> Path:
    root = os.environ.get("CLAUDE_CONFIG_DIR") or Path.home() / ".claude"
    return Path(root) / ".credentials.json"


def read_access_token(path: Path, now_ms: int) -> str:
    try:
        oauth = json.loads(path.read_text(encoding="utf-8"))["claudeAiOauth"]
        token = oauth["accessToken"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise UsageError("Claude Code OAuth credentials are unavailable") from error
    if not isinstance(token, str) or not token or "\r" in token or "\n" in token:
        raise UsageError("Claude Code OAuth access token is invalid")
    expires_at = oauth.get("expiresAt")
    if isinstance(expires_at, (int, float)) and expires_at <= now_ms:
        raise UsageError("Claude Code OAuth token expired; run Claude Code to refresh it")
    return token


def fetch_usage(token: str, timeout_seconds: float) -> dict[str, Any]:
    request = urllib.request.Request(
        USAGE_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": "oauth-2025-04-20",
            "Accept": "application/json",
            "User-Agent": "PixelStatus-NX-ClaudeUsage/0.1",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
            body = response.read(MAXIMUM_RESPONSE_BYTES + 1)
    except urllib.error.HTTPError as error:
        raise UsageError(f"Claude usage endpoint returned HTTP {error.code}") from None
    except (OSError, urllib.error.URLError) as error:
        raise UsageError(f"Claude usage request failed: {type(error).__name__}") from None
    if len(body) > MAXIMUM_RESPONSE_BYTES:
        raise UsageError("Claude usage response exceeded the byte limit")
    try:
        document = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        raise UsageError("Claude usage response is not valid JSON") from error
    if not isinstance(document, dict):
        raise UsageError("Claude usage response is not an object")
    return document


def build_snapshot(usage: dict[str, Any]) -> dict[str, Any]:
    snapshot: dict[str, Any] = {"schema_version": 1, "source": "claude-usage"}
    for name in WINDOWS:
        window = usage.get(name)
        if not isinstance(window, dict):
            raise UsageError(f"Claude usage response has no {name} window")
        utilization = window.get("utilization")
        if isinstance(utilization, bool) or not isinstance(utilization, (int, float)):
            raise UsageError(f"Claude usage {name} utilization is not numeric")
        resets_at = window.get("resets_at")
        try:
            epoch = int(dt.datetime.fromisoformat(resets_at).timestamp())
        except (TypeError, ValueError):
            epoch = None  # an idle window has no reset time yet
        snapshot[name] = {"utilization": float(utilization), "resets_at_epoch": epoch}
    return snapshot


class SnapshotStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._snapshot: dict[str, Any] | None = None
        self._error: str | None = "collector has not completed its first refresh"
        self._last_attempt_at: str | None = None
        self._last_success_at: str | None = None

    def update(self, snapshot: dict[str, Any] | None, error: str | None) -> None:
        timestamp = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
        with self._lock:
            self._last_attempt_at = timestamp
            self._error = error
            if error is None:
                self._snapshot = snapshot
                self._last_success_at = timestamp

    def response(self) -> dict[str, Any]:
        with self._lock:
            # Failed refreshes drop the values so monitors report a failure
            # instead of silently showing old percentages.
            document = (
                dict(self._snapshot)
                if self._error is None and self._snapshot is not None
                else {"schema_version": 1, "source": "claude-usage"}
            )
            document["collector"] = {
                "status": "healthy" if self._error is None else "error",
                "last_attempt_at": self._last_attempt_at,
                "last_success_at": self._last_success_at,
                "error": self._error,
            }
            return document


def refresh_once(
    store: SnapshotStore,
    *,
    path: Path,
    timeout_seconds: float,
    fetch: Callable[[str, float], dict[str, Any]] = fetch_usage,
) -> None:
    try:
        token = read_access_token(path, int(time.time() * 1000))
        store.update(build_snapshot(fetch(token, timeout_seconds)), None)
    except UsageError as error:
        store.update(None, str(error))


def serve(port: int, interval_seconds: float, timeout_seconds: float, path: Path) -> None:
    store = SnapshotStore()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
            if self.path != "/health":
                self.send_error(404)
                return
            body = json.dumps(store.response(), separators=(",", ":")).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args: object) -> None:
            return

    stop = threading.Event()

    def loop() -> None:
        while not stop.is_set():
            refresh_once(store, path=path, timeout_seconds=timeout_seconds)
            stop.wait(interval_seconds)

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    worker = threading.Thread(target=loop, daemon=True, name="claude-usage-refresh")
    worker.start()
    print(f"Claude usage monitor: http://127.0.0.1:{port}/health", flush=True)
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("probe", "serve"):
        command = subparsers.add_parser(name)
        command.add_argument("--credentials", type=Path, default=credentials_path())
        command.add_argument("--timeout-seconds", type=float, default=10.0)
        if name == "serve":
            command.add_argument("--port", type=int, default=18952)
            command.add_argument("--interval-seconds", type=float, default=120.0)
    args = parser.parse_args(argv)
    if not 0 < args.timeout_seconds <= 30:
        parser.error("timeout must be greater than zero and at most 30 seconds")
    if args.command == "probe":
        try:
            token = read_access_token(args.credentials, int(time.time() * 1000))
            print(json.dumps(build_snapshot(fetch_usage(token, args.timeout_seconds)), indent=2))
            return 0
        except UsageError as error:
            print(f"Claude usage monitor error: {error}", file=sys.stderr)
            return 1
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    # The 60-second floor keeps polling polite toward an undocumented endpoint.
    if not 60 <= args.interval_seconds <= 3600:
        parser.error("refresh interval must be between 60 and 3600 seconds")
    serve(args.port, args.interval_seconds, args.timeout_seconds, args.credentials)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
