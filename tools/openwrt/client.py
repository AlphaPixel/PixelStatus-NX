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


class OpenWrtError(RuntimeError):
    """A sanitized OpenWrt authentication, RPC, or transport failure."""


class CertificatePinError(OpenWrtError):
    """The bridge did not present the explicitly configured certificate."""


def _environment_name(name: str) -> str:
    suffix = "".join(character.upper() if character.isalnum() else "_" for character in name)
    return f"PIXELSTATUS_SECRET_{suffix}"


def resolve_named_secret(name: str) -> str:
    if not name or len(name) > 128 or re.fullmatch(r"[A-Za-z0-9._-]+", name) is None:
        raise OpenWrtError("secret name is invalid")

    environment_value = os.environ.get(_environment_name(name))
    if environment_value is not None:
        if not environment_value:
            raise OpenWrtError(f"named secret {name} is empty")
        return environment_value

    if sys.platform != "win32":
        raise OpenWrtError(f"named secret {name} is unavailable")

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
        raise OpenWrtError(f"named secret {name} is unavailable")

    try:
        credential = credential_pointer.contents
        size = int(credential.CredentialBlobSize)
        if size == 0:
            raise OpenWrtError(f"named secret {name} is empty")
        raw = ctypes.string_at(credential.CredentialBlob, size)
        looks_utf16 = size % 2 == 0 and any(raw[index] == 0 for index in range(1, size, 2))
        try:
            value = raw.decode("utf-16-le" if looks_utf16 else "utf-8").rstrip("\x00")
        except UnicodeDecodeError as error:
            raise OpenWrtError(f"named secret {name} is not valid text") from error
        if not value:
            raise OpenWrtError(f"named secret {name} is empty")
        return value
    finally:
        cred_free(credential_pointer)


def parse_certificate_sha256(value: str) -> bytes:
    normalized = value.strip().replace(":", "")
    if re.fullmatch(r"[0-9A-Fa-f]{64}", normalized) is None:
        raise OpenWrtError("certificate SHA-256 must contain exactly 64 hexadecimal digits")
    return bytes.fromhex(normalized)


class PinnedUbusClient:
    def __init__(
        self,
        url: str,
        certificate_sha256: str,
        username: str,
        password: str,
        *,
        timeout_seconds: float = 10.0,
        maximum_response_bytes: int = 256 * 1024,
        connection_factory: Callable[..., Any] | None = None,
    ) -> None:
        parsed = urlsplit(url)
        if (
            parsed.scheme != "https"
            or parsed.hostname is None
            or parsed.username is not None
            or parsed.password is not None
            or parsed.query
            or parsed.fragment
        ):
            raise OpenWrtError("OpenWrt URL must be HTTPS without credentials, query, or fragment")
        if not parsed.path or parsed.path == "/":
            raise OpenWrtError("OpenWrt URL must include the ubus endpoint path")
        if not username or any(character in username for character in "\r\n"):
            raise OpenWrtError("OpenWrt username is empty or invalid")
        if not password or any(character in password for character in "\r\n"):
            raise OpenWrtError("OpenWrt password is empty or invalid")
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise OpenWrtError("timeout must be greater than zero and at most 30 seconds")
        if maximum_response_bytes < 1 or maximum_response_bytes > 1024 * 1024:
            raise OpenWrtError("response limit must be between 1 byte and 1 MiB")

        self._host = parsed.hostname
        self._port = parsed.port or 443
        self._target = parsed.path
        self._certificate_sha256 = parse_certificate_sha256(certificate_sha256)
        self._username = username
        self._password = password
        self._timeout_seconds = timeout_seconds
        self._maximum_response_bytes = maximum_response_bytes
        self._connection_factory = connection_factory or http.client.HTTPSConnection
        self._next_identifier = 1

    def _request(self, method: str, params: list[Any]) -> Any:
        request = {
            "jsonrpc": "2.0",
            "id": self._next_identifier,
            "method": method,
            "params": params,
        }
        self._next_identifier += 1
        body = json.dumps(request, separators=(",", ":")).encode("utf-8")

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
                raise CertificatePinError("OpenWrt bridge did not present a certificate")
            actual_sha256 = hashlib.sha256(certificate).digest()
            if not hmac.compare_digest(actual_sha256, self._certificate_sha256):
                raise CertificatePinError("OpenWrt bridge certificate pin does not match")

            connection.request(
                "POST",
                self._target,
                body=body,
                headers={
                    "Accept": "application/json",
                    "Content-Type": "application/json",
                    "Content-Length": str(len(body)),
                    "User-Agent": "PixelStatus-NX-OpenWrt/0.1",
                },
            )
            response = connection.getresponse()
            response_body = response.read(self._maximum_response_bytes + 1)
            if len(response_body) > self._maximum_response_bytes:
                raise OpenWrtError("OpenWrt RPC response exceeded the configured byte limit")
            if response.status < 200 or response.status >= 300:
                raise OpenWrtError(f"OpenWrt RPC returned HTTP {response.status}")
            try:
                document = json.loads(response_body.decode("utf-8-sig"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise OpenWrtError("OpenWrt RPC returned invalid JSON") from error
            if not isinstance(document, dict):
                raise OpenWrtError("OpenWrt RPC response is not an object")
            if document.get("error") is not None:
                raise OpenWrtError("OpenWrt JSON-RPC request failed")
            return document.get("result")
        except (OSError, http.client.HTTPException, ssl.SSLError) as error:
            raise OpenWrtError(f"OpenWrt RPC transport failed: {type(error).__name__}") from error
        finally:
            connection.close()

    @staticmethod
    def _result_payload(result: Any, operation: str) -> Any:
        if not isinstance(result, list) or not result or not isinstance(result[0], int):
            raise OpenWrtError(f"OpenWrt {operation} returned an invalid result")
        if result[0] != 0:
            raise OpenWrtError(f"OpenWrt {operation} failed with ubus status {result[0]}")
        return result[1] if len(result) > 1 else {}

    def login(self) -> str:
        result = self._request(
            "call",
            [
                "00000000000000000000000000000000",
                "session",
                "login",
                {"username": self._username, "password": self._password},
            ],
        )
        payload = self._result_payload(result, "login")
        if not isinstance(payload, dict):
            raise OpenWrtError("OpenWrt login returned no session object")
        session = payload.get("ubus_rpc_session")
        if not isinstance(session, str) or re.fullmatch(r"[0-9A-Fa-f]{32}", session) is None:
            raise OpenWrtError("OpenWrt login returned no valid session token")
        return session

    def call(
        self,
        session: str,
        object_name: str,
        method: str,
        arguments: dict[str, Any] | None = None,
    ) -> Any:
        if re.fullmatch(r"[0-9A-Fa-f]{32}", session) is None:
            raise OpenWrtError("OpenWrt session token is invalid")
        if re.fullmatch(r"[A-Za-z0-9._-]+", object_name) is None:
            raise OpenWrtError("OpenWrt ubus object name is invalid")
        if re.fullmatch(r"[A-Za-z0-9._-]+", method) is None:
            raise OpenWrtError("OpenWrt ubus method name is invalid")
        result = self._request("call", [session, object_name, method, arguments or {}])
        return self._result_payload(result, f"{object_name}.{method}")


def _object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise OpenWrtError(f"OpenWrt {label} response is not an object")
    return value


def _number(value: Any) -> int | float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def _load_average(value: Any) -> float | None:
    number = _number(value)
    if number is None:
        return None
    # OpenWrt system.info exposes Linux load averages as 16.16 fixed-point values.
    return round(float(number) / 65536.0, 2)


def _interface_records(value: Any) -> list[dict[str, Any]]:
    records = _object(value, "network interface dump").get("interface")
    if not isinstance(records, list) or any(not isinstance(record, dict) for record in records):
        raise OpenWrtError("OpenWrt network interface dump has no valid interface array")
    return records


def _default_route_present(interface: dict[str, Any]) -> bool:
    routes = interface.get("route")
    if not isinstance(routes, list):
        return False
    for route in routes:
        if not isinstance(route, dict):
            continue
        target = route.get("target")
        mask = route.get("mask")
        if target in ("0.0.0.0", "0.0.0.0/0") and (mask in (None, 0)):
            return True
    return False


def _ipv4_present(interface: dict[str, Any]) -> bool:
    addresses = interface.get("ipv4-address")
    return isinstance(addresses, list) and any(isinstance(item, dict) for item in addresses)


def _connected_wireless(info: dict[str, Any], associations: dict[str, Any]) -> bool:
    bssid = info.get("bssid")
    if isinstance(bssid, str) and bssid not in ("", "00:00:00:00:00:00"):
        return True
    results = associations.get("results")
    if isinstance(results, list) and len(results) > 0:
        return True
    return False


def _select_station(
    observations: list[tuple[str, dict[str, Any], dict[str, Any]]],
    interface_device: str | None,
) -> tuple[str, dict[str, Any], dict[str, Any]] | None:
    if interface_device is not None:
        for observation in observations:
            if observation[0] == interface_device:
                return observation
    for observation in observations:
        mode = str(observation[1].get("mode", "")).lower()
        if mode in ("client", "station", "sta"):
            return observation
    return None


def _release_description(board: dict[str, Any]) -> str | None:
    release = board.get("release")
    if not isinstance(release, dict):
        return None
    description = release.get("description")
    return description if isinstance(description, str) else None


def build_snapshot(
    client: PinnedUbusClient,
    *,
    uplink_interface: str = "wwan",
    now: dt.datetime | None = None,
) -> dict[str, Any]:
    session = client.login()
    board = _object(client.call(session, "system", "board"), "system board")
    system = _object(client.call(session, "system", "info"), "system info")
    interfaces = _interface_records(client.call(session, "network.interface", "dump"))

    matches = [record for record in interfaces if record.get("interface") == uplink_interface]
    if len(matches) != 1:
        raise OpenWrtError(f"OpenWrt uplink interface {uplink_interface} was not found uniquely")
    uplink = matches[0]
    interface_device = uplink.get("device")
    if not isinstance(interface_device, str) or not interface_device:
        interface_device = None

    devices_document = _object(client.call(session, "iwinfo", "devices"), "iwinfo devices")
    device_names = devices_document.get("devices")
    if not isinstance(device_names, list) or any(not isinstance(name, str) for name in device_names):
        raise OpenWrtError("OpenWrt iwinfo devices response has no valid device array")

    observations: list[tuple[str, dict[str, Any], dict[str, Any]]] = []
    for name in device_names:
        wireless_info = _object(
            client.call(session, "iwinfo", "info", {"device": name}),
            "iwinfo info",
        )
        associations = _object(
            client.call(session, "iwinfo", "assoclist", {"device": name}),
            "iwinfo association list",
        )
        observations.append((name, wireless_info, associations))

    station = _select_station(observations, interface_device)
    station_info = station[1] if station is not None else {}
    station_associations = station[2] if station is not None else {}
    wireless_connected = station is not None and _connected_wireless(
        station_info,
        station_associations,
    )

    device_status: dict[str, Any] = {}
    if interface_device is not None:
        device_status = _object(
            client.call(
                session,
                "network.device",
                "status",
                {"name": interface_device},
            ),
            "network device status",
        )

    interface_up = uplink.get("up") is True
    address_assigned = _ipv4_present(uplink)
    default_route = _default_route_present(uplink)
    path_ready = interface_up and address_assigned and default_route
    bridge_ready = path_ready and wireless_connected
    if bridge_ready:
        bridge_status = "ok"
    elif path_ready:
        bridge_status = "warn"
    else:
        bridge_status = "fail"
    starlink_status = "ok" if path_ready else "fail"

    current = now or dt.datetime.now(dt.timezone.utc)
    load = system.get("load")
    load_values = load if isinstance(load, list) else []
    memory = system.get("memory")
    memory_values = memory if isinstance(memory, dict) else {}
    current_memory = _number(memory_values.get("available"))
    total_memory = _number(memory_values.get("total"))
    memory_utilization: float | None = None
    if current_memory is not None and total_memory not in (None, 0):
        memory_utilization = round((1.0 - float(current_memory) / float(total_memory)) * 100.0, 1)

    signal = _number(station_info.get("signal"))
    noise = _number(station_info.get("noise"))
    if noise == 0:
        noise = None
    quality = _number(station_info.get("quality"))
    quality_max = _number(station_info.get("quality_max"))
    return {
        "schema_version": 1,
        "source": "openwrt-starlink-bridge",
        "updated_at": current.astimezone(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "monitor": {"status": "ok" if bridge_ready and path_ready else "fail"},
        "bridge": {
            "monitor": {"status": bridge_status},
            "model": board.get("model"),
            "firmware": _release_description(board),
            "uptime_sec": _number(system.get("uptime")),
            "load_1m": _load_average(load_values[0]) if load_values else None,
            "memory_utilization_pct": memory_utilization,
            "wireless": {
                "station_present": station is not None,
                "connected": wireless_connected,
                "signal_dbm": signal,
                "noise_dbm": noise,
                "quality": quality,
                "quality_max": quality_max,
            },
        },
        "starlink": {
            "monitor": {"status": starlink_status},
            "path_state": "ready" if path_ready else "unavailable",
            "interface_up": interface_up,
            "address_assigned": address_assigned,
            "default_route_present": default_route,
            "interface_uptime_sec": _number(uplink.get("uptime")),
            "device_up": device_status.get("up") if isinstance(device_status.get("up"), bool) else None,
            "telemetry_scope": "router-path",
        },
    }
