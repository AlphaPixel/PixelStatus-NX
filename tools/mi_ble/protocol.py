"""Pure MI display protocol construction and logical-frame transforms."""

from __future__ import annotations

from collections.abc import Iterable, Sequence

WIDTH = 16
HEIGHT = 16
PIXEL_COUNT = WIDTH * HEIGHT
PIXELS_PER_BLOCK = 32
BLOCK_COUNT = 8

TARGET_NAME = "MI Matrix Display"
SERVICE_UUID = "0000ffd0-0000-1000-8000-00805f9b34fb"
CHARACTERISTIC_UUID = "0000ffd1-0000-1000-8000-00805f9b34fb"

GRAFFITI_INIT_PACKETS = (
    bytes.fromhex("bc00010155"),
    bytes.fromhex("bc000d0d55"),
)
BLOCK_INIT_PACKET = bytes.fromhex("bc0ff1080855")

Rgb = tuple[int, int, int]


def _validate_channel(value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 255:
        raise ValueError("RGB channels must be integers from 0 through 255")
    return value


def validate_rgb(color: Sequence[int]) -> Rgb:
    if len(color) != 3:
        raise ValueError("RGB colors must contain exactly three channels")
    return (
        _validate_channel(color[0]),
        _validate_channel(color[1]),
        _validate_channel(color[2]),
    )


def unpack_rgb888(value: int) -> Rgb:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFFFFFF:
        raise ValueError("Packed RGB values must be integers from 0 through 0xFFFFFF")
    return ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF)


def xy_to_index(x: int, y: int) -> int:
    if isinstance(x, bool) or isinstance(y, bool) or not 0 <= x < WIDTH or not 0 <= y < HEIGHT:
        raise ValueError("MI display coordinates must be inside the 16x16 matrix")
    return y * WIDTH + x


def build_pixel_packet(pixel_index: int, color: Sequence[int]) -> bytes:
    if (
        isinstance(pixel_index, bool)
        or not isinstance(pixel_index, int)
        or not 0 <= pixel_index < PIXEL_COUNT
    ):
        raise ValueError("pixel_index must be from 0 through 255")
    red, green, blue = validate_rgb(color)
    trailing_index = 0xFF if pixel_index == 0 else (pixel_index + 1) & 0xFF
    return bytes(
        (
            0xBC,
            0x01,
            0x01,
            0x00,
            pixel_index,
            red,
            green,
            blue,
            trailing_index,
            0x55,
        )
    )


def build_block_packet(block_index: int, pixels: Sequence[Sequence[int]]) -> bytes:
    if (
        isinstance(block_index, bool)
        or not isinstance(block_index, int)
        or not 0 <= block_index < BLOCK_COUNT
    ):
        raise ValueError("block_index must be from 0 through 7")
    if len(pixels) != PIXELS_PER_BLOCK:
        raise ValueError("an MI block must contain exactly 32 pixels")
    packet = bytearray((0xBC, 0x0F, block_index + 1))
    for pixel in pixels:
        packet.extend(validate_rgb(pixel))
    packet.append(0x55)
    return bytes(packet)


def build_block_packets(pixels: Sequence[Sequence[int]]) -> tuple[bytes, ...]:
    if len(pixels) != PIXEL_COUNT:
        raise ValueError("an MI frame must contain exactly 256 pixels")
    return tuple(
        build_block_packet(
            block,
            pixels[block * PIXELS_PER_BLOCK : (block + 1) * PIXELS_PER_BLOCK],
        )
        for block in range(BLOCK_COUNT)
    )


def _validate_color_order(color_order: str) -> str:
    normalized = color_order.upper()
    if len(normalized) != 3 or set(normalized) != {"R", "G", "B"}:
        raise ValueError("color_order must be a permutation of RGB")
    return normalized


def _reorder_color(color: Rgb, color_order: str) -> Rgb:
    channels = {"R": color[0], "G": color[1], "B": color[2]}
    return (
        channels[color_order[0]],
        channels[color_order[1]],
        channels[color_order[2]],
    )


def transform_frame(
    pixels: Iterable[Sequence[int]],
    *,
    rotation: int = 0,
    mirror_x: bool = False,
    mirror_y: bool = False,
    color_order: str = "RGB",
) -> tuple[Rgb, ...]:
    """Map a row-major logical frame into physical MI pixel-index order."""

    source = tuple(validate_rgb(pixel) for pixel in pixels)
    if len(source) != PIXEL_COUNT:
        raise ValueError("an MI frame must contain exactly 256 pixels")
    if rotation not in (0, 90, 180, 270):
        raise ValueError("rotation must be 0, 90, 180, or 270 degrees")
    order = _validate_color_order(color_order)

    output: list[Rgb] = [(0, 0, 0)] * PIXEL_COUNT
    for source_y in range(HEIGHT):
        for source_x in range(WIDTH):
            if rotation == 0:
                target_x, target_y = source_x, source_y
            elif rotation == 90:
                target_x, target_y = WIDTH - 1 - source_y, source_x
            elif rotation == 180:
                target_x, target_y = WIDTH - 1 - source_x, HEIGHT - 1 - source_y
            else:
                target_x, target_y = source_y, HEIGHT - 1 - source_x
            if mirror_x:
                target_x = WIDTH - 1 - target_x
            if mirror_y:
                target_y = HEIGHT - 1 - target_y
            output[xy_to_index(target_x, target_y)] = _reorder_color(
                source[xy_to_index(source_x, source_y)], order
            )
    return tuple(output)
