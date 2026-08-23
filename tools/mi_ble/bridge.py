"""Latest-frame-wins bridge from the HTTP display API to BLE."""

from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass

from .display import BleakConnection, MiFrameWriter
from .frame_source import DisplayFrame, FrameSourceError, HttpFrameSource
from .protocol import TARGET_NAME, transform_frame
from .status_file import BridgeStatusFile

LOGGER = logging.getLogger("pixelstatus.mi_ble")


class LatestFrameSlot:
    """One-frame asynchronous mailbox that replaces obsolete pending frames."""

    def __init__(self) -> None:
        self._condition = asyncio.Condition()
        self._generation = 0
        self._frame: DisplayFrame | None = None

    async def put(self, frame: DisplayFrame) -> int:
        async with self._condition:
            if self._frame is not None and self._frame.pixels == frame.pixels:
                return self._generation
            self._frame = frame
            self._generation += 1
            self._condition.notify_all()
            return self._generation

    async def wait_after(self, generation: int) -> tuple[int, DisplayFrame]:
        async with self._condition:
            await self._condition.wait_for(
                lambda: self._generation > generation and self._frame is not None
            )
            assert self._frame is not None
            return self._generation, self._frame

    async def current(self) -> tuple[int, DisplayFrame]:
        return await self.wait_after(0)


@dataclass(frozen=True, slots=True)
class BridgeOptions:
    target_name: str = TARGET_NAME
    address: str | None = None
    scan_timeout_seconds: float = 8.0
    connect_timeout_seconds: float = 15.0
    poll_seconds: float = 0.1
    reconnect_initial_seconds: float = 0.5
    reconnect_maximum_seconds: float = 10.0
    mode: str = "pixel"
    response: bool = False
    pixel_delay_seconds: float = 0.02
    block_delay_seconds: float = 0.005
    init_delay_seconds: float = 0.1
    rotation: int = 0
    mirror_x: bool = False
    mirror_y: bool = False
    color_order: str = "RGB"
    status_file: str | None = None


async def poll_source(
    source: HttpFrameSource,
    slot: LatestFrameSlot,
    options: BridgeOptions,
) -> None:
    last_error: str | None = None
    while True:
        try:
            frame = await source.fetch()
            if frame is not None:
                await slot.put(frame)
            if last_error is not None:
                LOGGER.info("Display frame endpoint recovered")
                last_error = None
        except FrameSourceError as error:
            if str(error) != last_error:
                LOGGER.warning("%s", error)
                last_error = str(error)
        await asyncio.sleep(options.poll_seconds)


async def _wait_for_frame_or_disconnect(
    slot: LatestFrameSlot,
    generation: int,
    connection: BleakConnection,
) -> tuple[int, DisplayFrame]:
    frame_task = asyncio.create_task(slot.wait_after(generation))
    disconnected_task = asyncio.create_task(connection.wait_disconnected())
    done, pending = await asyncio.wait(
        (frame_task, disconnected_task), return_when=asyncio.FIRST_COMPLETED
    )
    for task in pending:
        task.cancel()
    await asyncio.gather(*pending, return_exceptions=True)
    if disconnected_task in done:
        if frame_task in done:
            frame_task.result()
        raise ConnectionError("MI display disconnected")
    return frame_task.result()


async def output_frames(slot: LatestFrameSlot, options: BridgeOptions) -> None:
    generation, frame = await slot.current()
    retry_delay = options.reconnect_initial_seconds
    status = BridgeStatusFile(options.status_file)
    while True:
        connection: BleakConnection | None = None
        try:
            status.update("scanning", connected=False)
            LOGGER.info("Scanning for %s", options.address or options.target_name)
            connection = await BleakConnection.connect(
                target_name=options.target_name,
                address=options.address,
                scan_timeout_seconds=options.scan_timeout_seconds,
                connect_timeout_seconds=options.connect_timeout_seconds,
            )
            info = connection.info
            LOGGER.info(
                "Connected to %s [%s]; properties=%s; max_no_response=%s",
                info.name,
                info.address,
                ",".join(info.characteristic_properties),
                info.max_write_without_response_size,
            )
            status.update("connected", connected=True)
            retry_delay = options.reconnect_initial_seconds
            writer = MiFrameWriter(
                connection,
                mode=options.mode,
                response=options.response,
                pixel_delay_seconds=options.pixel_delay_seconds,
                block_delay_seconds=options.block_delay_seconds,
                init_delay_seconds=options.init_delay_seconds,
            )
            while True:
                physical = transform_frame(
                    frame.pixels,
                    rotation=options.rotation,
                    mirror_x=options.mirror_x,
                    mirror_y=options.mirror_y,
                    color_order=options.color_order,
                )
                writes = await writer.send_frame(physical)
                LOGGER.info(
                    "Displayed source frame %s using %s GATT write%s",
                    frame.sequence,
                    writes,
                    "" if writes == 1 else "s",
                )
                status.update(
                    "displaying",
                    connected=True,
                    source_frame=frame.sequence,
                    gatt_writes=writes,
                )
                generation, frame = await _wait_for_frame_or_disconnect(
                    slot, generation, connection
                )
        except asyncio.CancelledError:
            raise
        except Exception as error:
            LOGGER.warning("BLE output unavailable: %s", error)
            status.update(
                "retrying",
                connected=False,
                error_type=type(error).__name__,
            )
            await asyncio.sleep(retry_delay)
            retry_delay = min(retry_delay * 2.0, options.reconnect_maximum_seconds)
            generation, frame = await slot.current()
        finally:
            if connection is not None:
                await connection.close()


async def run_bridge(source: HttpFrameSource, options: BridgeOptions) -> None:
    if options.poll_seconds <= 0:
        raise ValueError("frame polling interval must be positive")
    if options.reconnect_initial_seconds <= 0 or options.reconnect_maximum_seconds <= 0:
        raise ValueError("reconnect delays must be positive")
    slot = LatestFrameSlot()
    tasks = (
        asyncio.create_task(poll_source(source, slot, options)),
        asyncio.create_task(output_frames(slot, options)),
    )
    try:
        await asyncio.gather(*tasks)
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
