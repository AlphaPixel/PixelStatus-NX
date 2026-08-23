from __future__ import annotations

import copy
import datetime as dt
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from .client import PinnedHttpsJsonClient, UnifiError, build_snapshot


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
                "source": "unifi-network",
                "gateway": {"state": "UNKNOWN"},
                "wans": [],
            }
            healthy = self._error is None
            if not healthy:
                document["monitor"] = {"status": "communication_failure"}
            elif not isinstance(document.get("monitor"), dict):
                document["monitor"] = {"status": "fail"}
            document["collector"] = {
                "status": "healthy" if healthy else "error",
                "last_attempt_at": self._last_attempt_at,
                "last_success_at": self._last_success_at,
                "error": self._error,
            }
            return document


def refresh_once(
    client: PinnedHttpsJsonClient,
    store: SnapshotStore,
    *,
    site_id: str | None,
    device_id: str | None,
) -> bool:
    try:
        store.update_success(build_snapshot(client, site_id=site_id, device_id=device_id))
        return True
    except UnifiError as error:
        store.update_error(error)
        return False


def refresh_loop(
    client: PinnedHttpsJsonClient,
    store: SnapshotStore,
    stop: threading.Event,
    *,
    interval_seconds: float,
    site_id: str | None,
    device_id: str | None,
) -> None:
    while not stop.wait(interval_seconds):
        refresh_once(client, store, site_id=site_id, device_id=device_id)


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
    client: PinnedHttpsJsonClient,
    *,
    port: int,
    interval_seconds: float,
    site_id: str | None,
    device_id: str | None,
) -> None:
    store = SnapshotStore()
    refresh_once(client, store, site_id=site_id, device_id=device_id)

    handler = type("ConfiguredHealthHandler", (HealthHandler,), {"store": store})
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    stop = threading.Event()
    worker = threading.Thread(
        target=refresh_loop,
        args=(client, store, stop),
        kwargs={
            "interval_seconds": interval_seconds,
            "site_id": site_id,
            "device_id": device_id,
        },
        daemon=True,
        name="unifi-refresh",
    )
    worker.start()
    print(f"UniFi monitor: http://127.0.0.1:{port}/health", flush=True)
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)
