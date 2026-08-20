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

    def send_json(self, document: object) -> None:
        payload = json.dumps(document, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
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
