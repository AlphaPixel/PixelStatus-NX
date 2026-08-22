"""Local HTTP fixture for the PixelStatus NX desktop monitor smoke test."""

from __future__ import annotations

import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class HealthHandler(BaseHTTPRequestHandler):
    slow_delay_seconds = 2.5
    slow_started = threading.Event()

    def do_GET(self) -> None:  # noqa: N802 - stdlib handler API
        if self.path == "/appliance/truenas":
            if self.headers.get("Authorization") != "Bearer fixture-truenas-token":
                self.send_json({"error": "unauthorized"}, status=401)
                return
            self.send_json(
                {
                    "pools": [
                        {
                            "name": "tank",
                            "status": "ONLINE",
                            "used_bytes": 750,
                            "total_bytes": 1000,
                        }
                    ],
                    "alerts": [{"level": "WARNING"}],
                }
            )
            return
        if self.path == "/appliance/unifi":
            if self.headers.get("X-API-Key") != "fixture-unifi-token":
                self.send_json({"error": "unauthorized"}, status=401)
                return
            self.send_json(
                {
                    "state": "ONLINE",
                    "wan": {"tx_bps": 300_000_000, "capacity_bps": 1_000_000_000},
                }
            )
            return
        if self.path == "/slow":
            type(self).slow_started.set()
            time.sleep(type(self).slow_delay_seconds)
            self.send_json({"value": 1})
            return
        if self.path == "/fast":
            self.send_json({"value": 2})
            return
        if self.path == "/stats":
            self.send_json({"slow_started": type(self).slow_started.is_set()})
            return
        if self.path != "/health":
            self.send_error(404)
            return

        self.send_json({"database": {"replication_lag": 12}, "healthy": True})

    def do_POST(self) -> None:  # noqa: N802 - stdlib handler API
        if self.path != "/query":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400)
            return
        if length < 0 or length > 16 * 1024:
            self.send_error(413)
            return
        try:
            document = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, UnicodeDecodeError):
            self.send_error(400)
            return
        accepted = (
            self.headers.get("X-PixelStatus-Probe") == "desktop-example"
            and self.headers.get_content_type() == "application/json"
            and document == {"operation": "health"}
        )
        self.send_json({"accepted": accepted})

    def send_json(self, document: object, status: int = 200) -> None:
        payload = json.dumps(document, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format: str, *args: object) -> None:
        return


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--slow-delay-ms", type=int, default=2500)
    options = parser.parse_args()
    if options.slow_delay_ms < 0:
        parser.error("--slow-delay-ms must not be negative")

    HealthHandler.slow_delay_seconds = options.slow_delay_ms / 1000
    HealthHandler.slow_started.clear()
    server = ThreadingHTTPServer(("127.0.0.1", options.port), HealthHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
