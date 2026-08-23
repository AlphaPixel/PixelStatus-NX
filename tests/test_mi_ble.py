from __future__ import annotations

import asyncio
import json
import tempfile
import unittest
from pathlib import Path

from tools.mi_ble.bridge import LatestFrameSlot
from tools.mi_ble.display import MiBleError, MiFrameWriter
from tools.mi_ble.frame_source import DisplayFrame, FrameSourceError, parse_display_document
from tools.mi_ble.protocol import (
    BLOCK_INIT_PACKET,
    GRAFFITI_INIT_PACKETS,
    PIXEL_COUNT,
    build_block_packet,
    build_pixel_packet,
    transform_frame,
    xy_to_index,
)
from tools.mi_ble.status_file import BridgeStatusFile


class StatusFileTests(unittest.TestCase):
    def test_status_is_atomically_replaced_without_hardware_identifiers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "mi-status.json"
            status = BridgeStatusFile(str(path))
            status.update("connected", connected=True)
            status.update(
                "displaying",
                connected=True,
                source_frame=42,
                gatt_writes=8,
            )

            document = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(document["status"], "displaying")
            self.assertEqual(document["source_frame"], 42)
            self.assertEqual(document["gatt_writes"], 8)
            self.assertNotIn("address", document)
            self.assertFalse(path.with_name("mi-status.json.next").exists())


class FakeGattWriter:
    def __init__(self, maximum: int | None = 512) -> None:
        self.max_write_without_response_size = maximum
        self.writes: list[tuple[bytes, bool]] = []

    async def write(self, packet: bytes, *, response: bool) -> None:
        self.writes.append((packet, response))


class ProtocolTests(unittest.TestCase):
    def test_device_validated_pixel_vectors_match_cpp(self) -> None:
        self.assertEqual(
            build_pixel_packet(0, (255, 0, 0)),
            bytes((0xBC, 0x01, 0x01, 0x00, 0x00, 0xFF, 0, 0, 0xFF, 0x55)),
        )
        packet = build_pixel_packet(255, (1, 2, 3))
        self.assertEqual(packet[4], 0xFF)
        self.assertEqual(packet[8], 0x00)

    def test_block_vector_matches_cpp(self) -> None:
        pixels = [(0, 0, 0)] * 32
        pixels[0] = (1, 2, 3)
        pixels[-1] = (4, 5, 6)
        packet = build_block_packet(7, pixels)
        self.assertEqual(len(packet), 100)
        self.assertEqual(packet[:6], bytes((0xBC, 0x0F, 0x08, 1, 2, 3)))
        self.assertEqual(packet[96:99], bytes((4, 5, 6)))
        self.assertEqual(packet[-1], 0x55)

    def test_rotation_mirroring_and_color_order(self) -> None:
        source = [(0, 0, 0)] * PIXEL_COUNT
        source[xy_to_index(0, 0)] = (1, 2, 3)
        rotated = transform_frame(source, rotation=90, color_order="GRB")
        self.assertEqual(rotated[xy_to_index(15, 0)], (2, 1, 3))
        mirrored = transform_frame(source, mirror_x=True, mirror_y=True)
        self.assertEqual(mirrored[xy_to_index(15, 15)], (1, 2, 3))


class FrameSourceTests(unittest.TestCase):
    def test_display_document_validation(self) -> None:
        document = {
            "schema_version": 1,
            "width": 16,
            "height": 16,
            "sequence": 42,
            "format": "rgb888",
            "pixels": [0x010203] * PIXEL_COUNT,
        }
        frame = parse_display_document(json.dumps(document).encode())
        self.assertEqual(frame.sequence, 42)
        self.assertEqual(frame.pixels[0], (1, 2, 3))

        document["width"] = 8
        with self.assertRaises(FrameSourceError):
            parse_display_document(json.dumps(document).encode())

    def test_boolean_pixel_is_rejected(self) -> None:
        document = {
            "schema_version": 1,
            "width": 16,
            "height": 16,
            "sequence": 0,
            "format": "rgb888",
            "pixels": [False] * PIXEL_COUNT,
        }
        with self.assertRaises(FrameSourceError):
            parse_display_document(json.dumps(document).encode())


class FrameWriterTests(unittest.IsolatedAsyncioTestCase):
    async def test_pixel_mode_full_then_sparse(self) -> None:
        transport = FakeGattWriter()
        writer = MiFrameWriter(
            transport,
            mode="pixel",
            pixel_delay_seconds=0,
            init_delay_seconds=0,
        )
        black = [(0, 0, 0)] * PIXEL_COUNT
        self.assertEqual(await writer.send_frame(black), 256)
        self.assertEqual(len(transport.writes), len(GRAFFITI_INIT_PACKETS) + 256)
        self.assertEqual(await writer.send_frame(black), 0)

        changed = list(black)
        changed[17] = (10, 20, 30)
        self.assertEqual(await writer.send_frame(changed), 1)
        self.assertEqual(transport.writes[-1][0], build_pixel_packet(17, (10, 20, 30)))

    async def test_block_mode_and_write_size_guard(self) -> None:
        transport = FakeGattWriter()
        writer = MiFrameWriter(
            transport,
            mode="block",
            block_delay_seconds=0,
            init_delay_seconds=0,
        )
        self.assertEqual(await writer.send_frame([(1, 2, 3)] * PIXEL_COUNT), 8)
        self.assertEqual(transport.writes[0][0], BLOCK_INIT_PACKET)
        self.assertEqual(len(transport.writes), 9)

        too_small = MiFrameWriter(
            FakeGattWriter(20),
            mode="block",
            block_delay_seconds=0,
            init_delay_seconds=0,
        )
        with self.assertRaises(MiBleError):
            await too_small.send_frame([(0, 0, 0)] * PIXEL_COUNT)

    async def test_latest_frame_slot_coalesces(self) -> None:
        slot = LatestFrameSlot()
        frame_one = DisplayFrame(1, ((0, 0, 0),) * PIXEL_COUNT)
        frame_two = DisplayFrame(2, ((1, 1, 1),) * PIXEL_COUNT)
        await slot.put(frame_one)
        await slot.put(frame_two)
        generation, current = await asyncio.wait_for(slot.current(), timeout=0.1)
        self.assertEqual(generation, 2)
        self.assertEqual(current.sequence, 2)

        same_pixels_new_sequence = DisplayFrame(3, frame_two.pixels)
        self.assertEqual(await slot.put(same_pixels_new_sequence), 2)


if __name__ == "__main__":
    unittest.main()
