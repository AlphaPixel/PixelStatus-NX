from __future__ import annotations

import copy
import datetime as dt
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from .client import OpenWrtError, PinnedUbusClient, build_snapshot


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


class SnapshotStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._document: dict[str, Any] | None = None
        self._last_success_at: str | None = None
        self._last_attempt_at: str | None = None
        self._error: str | None = "collector has not completed its first refresh"

    def update_success(self, document: dict[str, Any]) -> None:
        timestamp = _utc_now()
        with self._lock:
            self._document = copy.deepcopy(document)
            self._last_success_at = timestamp
            self._last_attempt_at = timestamp
            self._error = None

    def update_error(self, error: Exception) -> None:
        with self._lock:
            self._last_attempt_at = _utc_now()
            self._error = str(error)

    def response(self) -> dict[str, Any]:
        with self._lock:
            document = copy.deepcopy(self._document) if self._document is not None else {
                "schema_version": 1,
                "source": "openwrt-starlink-bridge",
                "bridge": {"monitor": {"status": "communication_failure"}},
                "starlink": {"monitor": {"status": "communication_failure"}},
            }
            healthy = self._error is None
            if not healthy:
                document["monitor"] = {"status": "communication_failure"}
                document.setdefault("bridge", {})["monitor"] = {
                    "status": "communication_failure"
                }
                document.setdefault("starlink", {})["monitor"] = {
                    "status": "communication_failure"
                }
            document["collector"] = {
                "status": "healthy" if healthy else "error",
                "last_attempt_at": self._last_attempt_at,
                "last_success_at": self._last_success_at,
                "error": self._error,
            }
            return document


def refresh_once(
    client: PinnedUbusClient,
    store: SnapshotStore,
    *,
    uplink_interface: str,
) -> bool:
    try:
        store.update_success(build_snapshot(client, uplink_interface=uplink_interface))
        return True
    except OpenWrtError as error:
        store.update_error(error)
        return False


def refresh_loop(
    client: PinnedUbusClient,
    store: SnapshotStore,
    stop: threading.Event,
    *,
    interval_seconds: float,
    uplink_interface: str,
) -> None:
    while not stop.wait(interval_seconds):
        refresh_once(client, store, uplink_interface=uplink_interface)


class HealthHandler(BaseHTTPRequestHandler):
    store: SnapshotStore

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
        if self.path != "/health":
            self.send_error(404)
            return
        body = json.dumps(self.store.response(), separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: object) -> None:
        return


def serve(
    client: PinnedUbusClient,
    *,
    port: int,
    interval_seconds: float,
    uplink_interface: str,
) -> None:
    store = SnapshotStore()
    refresh_once(client, store, uplink_interface=uplink_interface)

    handler = type("ConfiguredHealthHandler", (HealthHandler,), {"store": store})
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    stop = threading.Event()
    worker = threading.Thread(
        target=refresh_loop,
        args=(client, store, stop),
        kwargs={
            "interval_seconds": interval_seconds,
            "uplink_interface": uplink_interface,
        },
        daemon=True,
        name="openwrt-refresh",
    )
    worker.start()
    print(f"OpenWrt monitor: http://127.0.0.1:{port}/health", flush=True)
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)
