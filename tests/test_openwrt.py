from __future__ import annotations

import datetime as dt
import hashlib
import json
import unittest

from tools.openwrt.client import (
    CertificatePinError,
    OpenWrtError,
    PinnedUbusClient,
    build_snapshot,
    parse_certificate_sha256,
)
from tools.openwrt.server import SnapshotStore


class FakeSocket:
    def __init__(self, certificate: bytes) -> None:
        self._certificate = certificate

    def getpeercert(self, *, binary_form: bool = False) -> bytes:
        if not binary_form:
            raise AssertionError("the client must request the binary certificate")
        return self._certificate


class FakeResponse:
    def __init__(self, body: bytes, status: int = 200) -> None:
        self._body = body
        self.status = status

    def read(self, maximum: int) -> bytes:
        return self._body[:maximum]


class FakeConnection:
    def __init__(self, certificate: bytes, body: bytes) -> None:
        self.sock = FakeSocket(certificate)
        self.response = FakeResponse(body)
        self.connected = False
        self.closed = False
        self.requests: list[tuple[str, str, bytes, dict[str, str]]] = []

    def connect(self) -> None:
        self.connected = True

    def request(
        self,
        method: str,
        target: str,
        *,
        body: bytes,
        headers: dict[str, str],
    ) -> None:
        if not self.connected:
            raise AssertionError("request sent before TLS connection")
        self.requests.append((method, target, body, headers))

    def getresponse(self) -> FakeResponse:
        return self.response

    def close(self) -> None:
        self.closed = True


class PinnedTransportTests(unittest.TestCase):
    def test_pin_is_checked_before_password_is_sent(self) -> None:
        certificate = b"test certificate"
        response = {
            "jsonrpc": "2.0",
            "id": 1,
            "result": [0, {"ubus_rpc_session": "ab" * 16}],
        }
        connection = FakeConnection(certificate, json.dumps(response).encode())
        client = PinnedUbusClient(
            "https://bridge.test/ubus",
            hashlib.sha256(certificate).hexdigest(),
            "pixelstatus",
            "secret-password",
            connection_factory=lambda *args, **kwargs: connection,
        )

        self.assertEqual(client.login(), "ab" * 16)
        self.assertEqual(len(connection.requests), 1)
        method, target, body, headers = connection.requests[0]
        self.assertEqual(method, "POST")
        self.assertEqual(target, "/ubus")
        self.assertEqual(headers["Content-Type"], "application/json")
        request = json.loads(body)
        self.assertEqual(request["params"][3]["password"], "secret-password")
        self.assertTrue(connection.closed)

    def test_pin_mismatch_sends_no_http_request(self) -> None:
        connection = FakeConnection(b"unexpected", b"{}")
        client = PinnedUbusClient(
            "https://bridge.test/ubus",
            hashlib.sha256(b"expected").hexdigest(),
            "pixelstatus",
            "secret-password",
            connection_factory=lambda *args, **kwargs: connection,
        )

        with self.assertRaises(CertificatePinError):
            client.login()
        self.assertEqual(connection.requests, [])
        self.assertTrue(connection.closed)

    def test_pin_parser_accepts_colons_and_rejects_bad_values(self) -> None:
        digest = "AA:" * 31 + "AA"
        self.assertEqual(parse_certificate_sha256(digest), bytes([0xAA] * 32))
        with self.assertRaises(OpenWrtError):
            parse_certificate_sha256("not-a-pin")


class FakeOpenWrtClient:
    def __init__(self, *, wireless_connected: bool = True) -> None:
        self.wireless_connected = wireless_connected
        self.calls: list[tuple[str, str, dict[str, object]]] = []

    def login(self) -> str:
        return "12" * 16

    def call(
        self,
        session: str,
        object_name: str,
        method: str,
        arguments: dict[str, object] | None = None,
    ) -> object:
        self.calls.append((object_name, method, arguments or {}))
        key = (object_name, method)
        responses: dict[tuple[str, str], object] = {
            ("system", "board"): {
                "model": "Test Bridge",
                "release": {"description": "OpenWrt test"},
                "serial": "private-serial",
            },
            ("system", "info"): {
                "uptime": 12345,
                "load": [100, 200, 300],
                "memory": {"total": 1000, "available": 400},
            },
            ("network.interface", "dump"): {
                "interface": [
                    {
                        "interface": "wwan",
                        "up": True,
                        "uptime": 456,
                        "device": "phy1-sta0",
                        "ipv4-address": [{"address": "192.0.2.2", "mask": 24}],
                        "route": [
                            {
                                "target": "0.0.0.0",
                                "mask": 0,
                                "nexthop": "192.0.2.1",
                            }
                        ],
                    }
                ]
            },
            ("iwinfo", "devices"): {"devices": ["phy0-ap0", "phy1-sta0"]},
            ("network.device", "status"): {"up": True, "macaddr": "00:11:22:33:44:55"},
        }
        if key == ("iwinfo", "info"):
            device = (arguments or {}).get("device")
            if device == "phy1-sta0":
                return {
                    "mode": "Client",
                    "ssid": "private-network",
                    "bssid": "AA:BB:CC:DD:EE:FF" if self.wireless_connected else "",
                    "signal": -54,
                    "noise": -95,
                    "quality": 62,
                    "quality_max": 70,
                    "encryption": {"enabled": True},
                }
            return {"mode": "Master", "ssid": "private-lan"}
        if key == ("iwinfo", "assoclist"):
            return {"results": []}
        return responses[key]


class SnapshotTests(unittest.TestCase):
    def test_snapshot_reports_bridge_and_starlink_path_without_private_identifiers(self) -> None:
        client = FakeOpenWrtClient()
        snapshot = build_snapshot(
            client,  # type: ignore[arg-type]
            now=dt.datetime(2026, 8, 22, 22, 0, tzinfo=dt.timezone.utc),
        )

        self.assertEqual(snapshot["bridge"]["monitor"]["status"], "ok")
        self.assertEqual(snapshot["starlink"]["monitor"]["status"], "ok")
        self.assertEqual(snapshot["starlink"]["path_state"], "ready")
        self.assertEqual(snapshot["bridge"]["wireless"]["signal_dbm"], -54)
        self.assertEqual(snapshot["bridge"]["memory_utilization_pct"], 60.0)
        serialized = json.dumps(snapshot)
        for private_value in (
            "192.0.2.1",
            "192.0.2.2",
            "private-network",
            "private-lan",
            "AA:BB:CC:DD:EE:FF",
            "00:11:22:33:44:55",
            "private-serial",
            "12121212121212121212121212121212",
        ):
            self.assertNotIn(private_value, serialized)

        method_names = {(object_name, method) for object_name, method, _ in client.calls}
        self.assertEqual(
            method_names,
            {
                ("system", "board"),
                ("system", "info"),
                ("network.interface", "dump"),
                ("network.device", "status"),
                ("iwinfo", "devices"),
                ("iwinfo", "info"),
                ("iwinfo", "assoclist"),
            },
        )

    def test_path_can_be_ready_while_wireless_details_warn(self) -> None:
        client = FakeOpenWrtClient(wireless_connected=False)
        snapshot = build_snapshot(client)  # type: ignore[arg-type]

        self.assertEqual(snapshot["bridge"]["monitor"]["status"], "warn")
        self.assertEqual(snapshot["starlink"]["monitor"]["status"], "ok")

    def test_store_marks_both_statuses_as_communication_failures(self) -> None:
        store = SnapshotStore()
        store.update_success(
            {
                "bridge": {"monitor": {"status": "ok"}},
                "starlink": {"monitor": {"status": "ok"}},
            }
        )
        store.update_error(OpenWrtError("transport unavailable"))

        response = store.response()
        self.assertEqual(response["bridge"]["monitor"]["status"], "communication_failure")
        self.assertEqual(response["starlink"]["monitor"]["status"], "communication_failure")
        self.assertEqual(response["collector"]["error"], "transport unavailable")


if __name__ == "__main__":
    unittest.main()
