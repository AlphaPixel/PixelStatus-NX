"""Bleak transport and frame writer for the MI display."""

from __future__ import annotations

import asyncio
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any, Protocol

from .protocol import (
    BLOCK_INIT_PACKET,
    CHARACTERISTIC_UUID,
    GRAFFITI_INIT_PACKETS,
    PIXEL_COUNT,
    Rgb,
    SERVICE_UUID,
    TARGET_NAME,
    build_block_packets,
    build_pixel_packet,
    validate_rgb,
)


class MiBleError(RuntimeError):
    """BLE discovery, connection, or MI protocol transport failed."""


class GattWriter(Protocol):
    max_write_without_response_size: int | None

    async def write(self, packet: bytes, *, response: bool) -> None: ...


@dataclass(frozen=True, slots=True)
class DeviceSummary:
    name: str | None
    address: str


@dataclass(frozen=True, slots=True)
class ConnectionInfo:
    name: str | None
    address: str
    characteristic_properties: tuple[str, ...]
    max_write_without_response_size: int | None


def _load_bleak() -> tuple[Any, Any]:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as error:
        raise MiBleError(
            "Bleak is not installed; run: python -m pip install -r "
            "tools/requirements-mi-ble.txt"
        ) from error
    return BleakClient, BleakScanner


async def scan_devices(timeout_seconds: float = 5.0) -> tuple[DeviceSummary, ...]:
    if timeout_seconds <= 0:
        raise ValueError("scan timeout must be positive")
    _, scanner = _load_bleak()
    devices = await scanner.discover(timeout=timeout_seconds)
    summaries = {
        device.address: DeviceSummary(name=device.name, address=device.address)
        for device in devices
    }
    return tuple(sorted(summaries.values(), key=lambda item: ((item.name or ""), item.address)))


class BleakConnection:
    def __init__(
        self,
        client: Any,
        characteristic: Any,
        disconnected: asyncio.Event,
        info: ConnectionInfo,
    ) -> None:
        self._client = client
        self._characteristic = characteristic
        self._disconnected = disconnected
        self.info = info

    @classmethod
    async def connect(
        cls,
        *,
        target_name: str = TARGET_NAME,
        address: str | None = None,
        scan_timeout_seconds: float = 8.0,
        connect_timeout_seconds: float = 15.0,
    ) -> "BleakConnection":
        client_type, scanner = _load_bleak()
        if address:
            device = await scanner.find_device_by_address(
                address, timeout=scan_timeout_seconds
            )
        else:
            wanted = target_name.casefold()

            def matches(device: Any, advertisement: Any) -> bool:
                names = (device.name, getattr(advertisement, "local_name", None))
                return any(name and wanted in name.casefold() for name in names)

            device = await scanner.find_device_by_filter(
                matches, timeout=scan_timeout_seconds
            )
        if device is None:
            identity = address or repr(target_name)
            raise MiBleError(f"MI display {identity} was not found while scanning")

        disconnected = asyncio.Event()

        def on_disconnected(_: Any) -> None:
            disconnected.set()

        client = client_type(
            device,
            disconnected_callback=on_disconnected,
            services=(SERVICE_UUID,),
            timeout=connect_timeout_seconds,
        )
        try:
            await client.connect()
            if not client.is_connected:
                raise MiBleError("Bleak returned without connecting to the MI display")
            characteristic = client.services.get_characteristic(CHARACTERISTIC_UUID)
            if characteristic is None:
                raise MiBleError(
                    f"connected display does not expose {CHARACTERISTIC_UUID}"
                )
            properties = tuple(characteristic.properties)
            maximum = getattr(characteristic, "max_write_without_response_size", None)
            info = ConnectionInfo(
                name=device.name,
                address=device.address,
                characteristic_properties=properties,
                max_write_without_response_size=maximum,
            )
            return cls(client, characteristic, disconnected, info)
        except Exception:
            if client.is_connected:
                await client.disconnect()
            raise

    @property
    def max_write_without_response_size(self) -> int | None:
        return self.info.max_write_without_response_size

    @property
    def is_connected(self) -> bool:
        return bool(self._client.is_connected)

    async def write(self, packet: bytes, *, response: bool) -> None:
        if not self.is_connected:
            raise MiBleError("MI display disconnected before a GATT write")
        required_property = "write" if response else "write-without-response"
        if required_property not in self.info.characteristic_properties:
            raise MiBleError(
                f"MI characteristic does not advertise {required_property}; "
                f"available properties: {','.join(self.info.characteristic_properties)}"
            )
        await self._client.write_gatt_char(
            self._characteristic, packet, response=response
        )

    async def wait_disconnected(self) -> None:
        await self._disconnected.wait()

    async def close(self) -> None:
        if self._client.is_connected:
            await self._client.disconnect()

    async def __aenter__(self) -> "BleakConnection":
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.close()


class MiFrameWriter:
    """Serializes complete logical frames over one connected GATT session."""

    def __init__(
        self,
        writer: GattWriter,
        *,
        mode: str = "pixel",
        response: bool = False,
        pixel_delay_seconds: float = 0.02,
        block_delay_seconds: float = 0.005,
        init_delay_seconds: float = 0.1,
    ) -> None:
        if mode not in ("pixel", "block"):
            raise ValueError("MI output mode must be pixel or block")
        if min(pixel_delay_seconds, block_delay_seconds, init_delay_seconds) < 0:
            raise ValueError("MI output delays must not be negative")
        self._writer = writer
        self._mode = mode
        self._response = response
        self._pixel_delay = pixel_delay_seconds
        self._block_delay = block_delay_seconds
        self._init_delay = init_delay_seconds
        self._initialized = False
        self._last_pixels: tuple[Rgb, ...] | None = None

    @property
    def mode(self) -> str:
        return self._mode

    async def initialize(self) -> None:
        if self._initialized:
            return
        if (
            self._mode == "block"
            and not self._response
            and self._writer.max_write_without_response_size is not None
            and self._writer.max_write_without_response_size < 100
        ):
            raise MiBleError(
                "block mode needs a 100-byte write; the characteristic reports "
                f"{self._writer.max_write_without_response_size} bytes without response"
            )
        packets = (
            GRAFFITI_INIT_PACKETS
            if self._mode == "pixel"
            else (BLOCK_INIT_PACKET,)
        )
        for packet in packets:
            await self._writer.write(packet, response=self._response)
        if self._init_delay:
            await asyncio.sleep(self._init_delay)
        self._initialized = True

    async def send_frame(self, pixels: Sequence[Sequence[int]]) -> int:
        validated = tuple(validate_rgb(pixel) for pixel in pixels)
        if len(validated) != PIXEL_COUNT:
            raise ValueError("an MI frame must contain exactly 256 pixels")
        await self.initialize()
        if validated == self._last_pixels:
            return 0

        writes = 0
        try:
            if self._mode == "pixel":
                changed = range(PIXEL_COUNT) if self._last_pixels is None else (
                    index
                    for index, (before, after) in enumerate(
                        zip(self._last_pixels, validated, strict=True)
                    )
                    if before != after
                )
                for index in changed:
                    await self._writer.write(
                        build_pixel_packet(index, validated[index]),
                        response=self._response,
                    )
                    writes += 1
                    if self._pixel_delay:
                        await asyncio.sleep(self._pixel_delay)
            else:
                for packet in build_block_packets(validated):
                    await self._writer.write(packet, response=self._response)
                    writes += 1
                    if self._block_delay:
                        await asyncio.sleep(self._block_delay)
        except Exception:
            self._initialized = False
            self._last_pixels = None
            raise
        self._last_pixels = validated
        return writes
