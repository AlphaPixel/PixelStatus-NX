from __future__ import annotations

import datetime as dt
import hashlib
import json
import unittest

from tools.unifi.client import (
    CertificatePinError,
    PinnedHttpsJsonClient,
    UnifiError,
    build_snapshot,
    parse_certificate_sha256,
)
from tools.unifi.server import SnapshotStore


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
        self.requests: list[tuple[str, str, dict[str, str]]] = []

    def connect(self) -> None:
        self.connected = True

    def request(self, method: str, target: str, *, headers: dict[str, str]) -> None:
        if not self.connected:
            raise AssertionError("request sent before TLS connection")
        self.requests.append((method, target, headers))

    def getresponse(self) -> FakeResponse:
        return self.response

    def close(self) -> None:
        self.closed = True


class PinnedTransportTests(unittest.TestCase):
    def test_pin_is_checked_before_api_key_is_sent(self) -> None:
        certificate = b"test certificate"
        connection = FakeConnection(certificate, b'{"value":42}')
        client = PinnedHttpsJsonClient(
            "https://gateway.test/proxy/network/integration",
            hashlib.sha256(certificate).hexdigest(),
            "secret-key",
            connection_factory=lambda *args, **kwargs: connection,
        )

        self.assertEqual(client.get_json("/v1/info"), {"value": 42})
        self.assertEqual(len(connection.requests), 1)
        method, target, headers = connection.requests[0]
        self.assertEqual(method, "GET")
        self.assertEqual(target, "/proxy/network/integration/v1/info")
        self.assertEqual(headers["X-API-Key"], "secret-key")
        self.assertTrue(connection.closed)

    def test_pin_mismatch_sends_no_http_request(self) -> None:
        connection = FakeConnection(b"unexpected", b"{}")
        client = PinnedHttpsJsonClient(
            "https://gateway.test/integration",
            hashlib.sha256(b"expected").hexdigest(),
            "secret-key",
            connection_factory=lambda *args, **kwargs: connection,
        )

        with self.assertRaises(CertificatePinError):
            client.get_json("/v1/info")
        self.assertEqual(connection.requests, [])
        self.assertTrue(connection.closed)

    def test_pin_parser_accepts_colons_and_rejects_bad_values(self) -> None:
        digest = "AA:" * 31 + "AA"
        self.assertEqual(parse_certificate_sha256(digest), bytes([0xAA] * 32))
        with self.assertRaises(UnifiError):
            parse_certificate_sha256("not-a-pin")


class FakeUnifiClient:
    def __init__(self) -> None:
        self.responses = {
            "/v1/info": {"applicationVersion": "10.5.67"},
            "/v1/sites?limit=200": {
                "data": [{"id": "site-id", "name": "Default", "internalReference": "default"}]
            },
            "/v1/sites/site-id/devices?limit=200": {
                "data": [
                    {
                        "id": "device-id",
                        "name": "Gateway",
                        "model": "UniFi Dream Machine PRO SE",
                        "state": "ONLINE",
                        "ipAddress": "203.0.113.10",
                        "macAddress": "00:11:22:33:44:55",
                    }
                ]
            },
            "/v1/sites/site-id/devices/device-id": {
                "name": "Gateway",
                "model": "UniFi Dream Machine PRO SE",
                "state": "ONLINE",
                "firmwareVersion": "5.1.26",
                "firmwareUpdatable": False,
                "ipAddress": "203.0.113.10",
                "macAddress": "00:11:22:33:44:55",
                "interfaces": {
                    "ports": [{"state": "UP"}, {"state": "DOWN"}, {"state": "UP"}]
                },
            },
            "/v1/sites/site-id/devices/device-id/statistics/latest": {
                "uptimeSec": 123,
                "lastHeartbeatAt": "2026-08-22T20:45:16Z",
                "cpuUtilizationPct": 4.6,
                "memoryUtilizationPct": 42.5,
                "uplink": {"txRateBps": 1000, "rxRateBps": 2000},
            },
            "/v1/sites/site-id/wans": {
                "data": [{"id": "private-id", "name": "Primary"}, {"name": "Backup"}]
            },
        }
        self.paths: list[str] = []

    def get_json(self, path: str) -> object:
        self.paths.append(path)
        return self.responses[path]


class SnapshotTests(unittest.TestCase):
    def test_snapshot_is_stable_and_omits_addresses_and_internal_ids(self) -> None:
        client = FakeUnifiClient()
        snapshot = build_snapshot(
            client,  # type: ignore[arg-type]
            now=dt.datetime(2026, 8, 22, 21, 0, tzinfo=dt.timezone.utc),
        )

        self.assertEqual(snapshot["monitor"]["status"], "ok")
        self.assertEqual(snapshot["gateway"]["state"], "ONLINE")
        self.assertEqual(snapshot["gateway"]["ports"], {"total": 3, "up": 2})
        self.assertEqual(snapshot["gateway"]["uplink"]["rx_rate_bps"], 2000)
        self.assertEqual(snapshot["wans"], [{"name": "Primary"}, {"name": "Backup"}])
        serialized = json.dumps(snapshot)
        self.assertNotIn("203.0.113.10", serialized)
        self.assertNotIn("00:11:22:33:44:55", serialized)
        self.assertNotIn("private-id", serialized)

    def test_multiple_sites_require_an_explicit_site_id(self) -> None:
        client = FakeUnifiClient()
        client.responses["/v1/sites?limit=200"]["data"].append(  # type: ignore[index,union-attr]
            {"id": "other", "name": "Other", "internalReference": "other"}
        )
        with self.assertRaises(UnifiError):
            build_snapshot(client)  # type: ignore[arg-type]

    def test_store_preserves_gateway_failure_and_overrides_collection_failure(self) -> None:
        store = SnapshotStore()
        store.update_success({"monitor": {"status": "fail"}, "gateway": {"state": "OFFLINE"}})
        self.assertEqual(store.response()["monitor"]["status"], "fail")

        store.update_error(UnifiError("transport unavailable"))
        response = store.response()
        self.assertEqual(response["monitor"]["status"], "communication_failure")
        self.assertEqual(response["collector"]["error"], "transport unavailable")


if __name__ == "__main__":
    unittest.main()
