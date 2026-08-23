from __future__ import annotations

import ctypes
import datetime as dt
import hashlib
import hmac
import http.client
import json
import math
import os
import re
import ssl
import sys
from collections.abc import Callable
from typing import Any
from urllib.parse import urlsplit


class UnifiError(RuntimeError):
    """A sanitized UniFi discovery or transport failure."""


class CertificatePinError(UnifiError):
    """The gateway did not present the explicitly configured certificate."""


def _environment_name(name: str) -> str:
    suffix = "".join(character.upper() if character.isalnum() else "_" for character in name)
    return f"PIXELSTATUS_SECRET_{suffix}"


def resolve_named_secret(name: str) -> str:
    if not name or len(name) > 128 or re.fullmatch(r"[A-Za-z0-9._-]+", name) is None:
        raise UnifiError("secret name is invalid")

    environment_value = os.environ.get(_environment_name(name))
    if environment_value is not None:
        if not environment_value:
            raise UnifiError(f"named secret {name} is empty")
        return environment_value

    if sys.platform != "win32":
        raise UnifiError(f"named secret {name} is unavailable")

    from ctypes import wintypes

    class Credential(ctypes.Structure):
        _fields_ = [
            ("Flags", wintypes.DWORD),
            ("Type", wintypes.DWORD),
            ("TargetName", wintypes.LPWSTR),
            ("Comment", wintypes.LPWSTR),
            ("LastWritten", wintypes.FILETIME),
            ("CredentialBlobSize", wintypes.DWORD),
            ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)),
            ("Persist", wintypes.DWORD),
            ("AttributeCount", wintypes.DWORD),
            ("Attributes", ctypes.c_void_p),
            ("TargetAlias", wintypes.LPWSTR),
            ("UserName", wintypes.LPWSTR),
        ]

    credential_pointer = ctypes.POINTER(Credential)()
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    cred_read = advapi32.CredReadW
    cred_read.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.POINTER(ctypes.POINTER(Credential)),
    ]
    cred_read.restype = wintypes.BOOL
    cred_free = advapi32.CredFree
    cred_free.argtypes = [ctypes.c_void_p]
    cred_free.restype = None

    target = f"PixelStatus-NX/{name}"
    if not cred_read(target, 1, 0, ctypes.byref(credential_pointer)):
        raise UnifiError(f"named secret {name} is unavailable")

    try:
        credential = credential_pointer.contents
        size = int(credential.CredentialBlobSize)
        if size == 0:
            raise UnifiError(f"named secret {name} is empty")
        raw = ctypes.string_at(credential.CredentialBlob, size)
        looks_utf16 = size % 2 == 0 and any(raw[index] == 0 for index in range(1, size, 2))
        try:
            value = raw.decode("utf-16-le" if looks_utf16 else "utf-8").rstrip("\x00")
        except UnicodeDecodeError as error:
            raise UnifiError(f"named secret {name} is not valid text") from error
        if not value:
            raise UnifiError(f"named secret {name} is empty")
        return value
    finally:
        cred_free(credential_pointer)


def parse_certificate_sha256(value: str) -> bytes:
    normalized = value.strip().replace(":", "")
    if re.fullmatch(r"[0-9A-Fa-f]{64}", normalized) is None:
        raise UnifiError("certificate SHA-256 must contain exactly 64 hexadecimal digits")
    return bytes.fromhex(normalized)


class PinnedHttpsJsonClient:
    def __init__(
        self,
        base_url: str,
        certificate_sha256: str,
        api_key: str,
        *,
        timeout_seconds: float = 10.0,
        maximum_response_bytes: int = 256 * 1024,
        connection_factory: Callable[..., Any] | None = None,
    ) -> None:
        parsed = urlsplit(base_url)
        if (
            parsed.scheme != "https"
            or parsed.hostname is None
            or parsed.username is not None
            or parsed.password is not None
            or parsed.query
            or parsed.fragment
        ):
            raise UnifiError("UniFi base URL must be an HTTPS URL without credentials, query, or fragment")
        if not api_key or "\r" in api_key or "\n" in api_key:
            raise UnifiError("UniFi API key is empty or invalid")
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise UnifiError("timeout must be greater than zero and at most 30 seconds")
        if maximum_response_bytes < 1 or maximum_response_bytes > 1024 * 1024:
            raise UnifiError("response limit must be between 1 byte and 1 MiB")

        self._host = parsed.hostname
        self._port = parsed.port or 443
        self._base_path = parsed.path.rstrip("/")
        self._certificate_sha256 = parse_certificate_sha256(certificate_sha256)
        self._api_key = api_key
        self._timeout_seconds = timeout_seconds
        self._maximum_response_bytes = maximum_response_bytes
        self._connection_factory = connection_factory or http.client.HTTPSConnection

    def get_json(self, path: str) -> Any:
        if not path.startswith("/") or "\r" in path or "\n" in path:
            raise UnifiError("UniFi API path is invalid")

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        connection = self._connection_factory(
            self._host,
            self._port,
            context=context,
            timeout=self._timeout_seconds,
        )
        try:
            connection.connect()
            socket = getattr(connection, "sock", None)
            certificate = None if socket is None else socket.getpeercert(binary_form=True)
            if not certificate:
                raise CertificatePinError("UniFi gateway did not present a certificate")
            actual_sha256 = hashlib.sha256(certificate).digest()
            if not hmac.compare_digest(actual_sha256, self._certificate_sha256):
                raise CertificatePinError("UniFi gateway certificate pin does not match")

            target = f"{self._base_path}{path}"
            connection.request(
                "GET",
                target,
                headers={
                    "Accept": "application/json",
                    "X-API-Key": self._api_key,
                    "User-Agent": "PixelStatus-NX-UniFi/0.1",
                },
            )
            response = connection.getresponse()
            body = response.read(self._maximum_response_bytes + 1)
            if len(body) > self._maximum_response_bytes:
                raise UnifiError("UniFi API response exceeded the configured byte limit")
            if response.status < 200 or response.status >= 300:
                raise UnifiError(f"UniFi API returned HTTP {response.status}")
            try:
                return json.loads(body.decode("utf-8-sig"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise UnifiError("UniFi API returned invalid JSON") from error
        except (OSError, http.client.HTTPException, ssl.SSLError) as error:
            raise UnifiError(f"UniFi API transport failed: {type(error).__name__}") from error
        finally:
            connection.close()


def _object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise UnifiError(f"UniFi API {label} response is not an object")
    return value


def _data(value: Any, label: str) -> list[dict[str, Any]]:
    document = _object(value, label)
    records = document.get("data")
    if not isinstance(records, list) or any(not isinstance(record, dict) for record in records):
        raise UnifiError(f"UniFi API {label} response has no valid data array")
    return records


def _select_record(
    records: list[dict[str, Any]],
    identifier: str | None,
    label: str,
) -> dict[str, Any]:
    if identifier is not None:
        matches = [record for record in records if record.get("id") == identifier]
        if len(matches) != 1:
            raise UnifiError(f"configured UniFi {label} ID was not found")
        return matches[0]
    if len(records) != 1:
        raise UnifiError(f"UniFi API returned {len(records)} {label} records; configure an explicit ID")
    return records[0]


def _number(value: Any) -> int | float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def build_snapshot(
    client: PinnedHttpsJsonClient,
    *,
    site_id: str | None = None,
    device_id: str | None = None,
    now: dt.datetime | None = None,
) -> dict[str, Any]:
    info = _object(client.get_json("/v1/info"), "application info")
    site = _select_record(_data(client.get_json("/v1/sites?limit=200"), "sites"), site_id, "site")
    selected_site_id = site.get("id")
    if not isinstance(selected_site_id, str) or not selected_site_id:
        raise UnifiError("selected UniFi site has no valid ID")

    devices = _data(
        client.get_json(f"/v1/sites/{selected_site_id}/devices?limit=200"),
        "devices",
    )
    if device_id is None and len(devices) > 1:
        dream_machines = [
            device for device in devices
            if "dream machine" in str(device.get("model", "")).lower()
        ]
        device = _select_record(dream_machines, None, "gateway device")
    else:
        device = _select_record(devices, device_id, "gateway device")
    selected_device_id = device.get("id")
    if not isinstance(selected_device_id, str) or not selected_device_id:
        raise UnifiError("selected UniFi gateway has no valid ID")

    details = _object(
        client.get_json(f"/v1/sites/{selected_site_id}/devices/{selected_device_id}"),
        "device details",
    )
    statistics = _object(
        client.get_json(
            f"/v1/sites/{selected_site_id}/devices/{selected_device_id}/statistics/latest"
        ),
        "device statistics",
    )
    wans = _data(client.get_json(f"/v1/sites/{selected_site_id}/wans"), "WAN interfaces")

    interfaces = details.get("interfaces")
    ports = interfaces.get("ports") if isinstance(interfaces, dict) else []
    if not isinstance(ports, list):
        ports = []
    uplink = statistics.get("uplink")
    if not isinstance(uplink, dict):
        uplink = {}
    state = details.get("state", device.get("state", "UNKNOWN"))
    if not isinstance(state, str):
        state = "UNKNOWN"

    current = now or dt.datetime.now(dt.timezone.utc)
    return {
        "schema_version": 1,
        "source": "unifi-network",
        "updated_at": current.astimezone(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "monitor": {"status": "ok" if state == "ONLINE" else "fail"},
        "application": {"version": info.get("applicationVersion")},
        "site": {
            "name": site.get("name"),
            "internal_reference": site.get("internalReference"),
        },
        "gateway": {
            "name": details.get("name", device.get("name")),
            "model": details.get("model", device.get("model")),
            "state": state,
            "firmware_version": details.get("firmwareVersion"),
            "firmware_updatable": details.get("firmwareUpdatable"),
            "uptime_sec": _number(statistics.get("uptimeSec")),
            "last_heartbeat_at": statistics.get("lastHeartbeatAt"),
            "cpu_utilization_pct": _number(statistics.get("cpuUtilizationPct")),
            "memory_utilization_pct": _number(statistics.get("memoryUtilizationPct")),
            "uplink": {
                "tx_rate_bps": _number(uplink.get("txRateBps")),
                "rx_rate_bps": _number(uplink.get("rxRateBps")),
            },
            "ports": {
                "total": len(ports),
                "up": sum(1 for port in ports if isinstance(port, dict) and port.get("state") == "UP"),
            },
        },
        "wans": [
            {"name": wan.get("name")}
            for wan in wans
            if isinstance(wan.get("name"), str)
        ],
    }
