"""Deterministic hardware-calibration frame patterns."""

from __future__ import annotations

from .protocol import HEIGHT, PIXEL_COUNT, WIDTH, Rgb, xy_to_index


def corners() -> tuple[Rgb, ...]:
    pixels: list[Rgb] = [(0, 0, 0)] * PIXEL_COUNT
    pixels[xy_to_index(0, 0)] = (255, 0, 0)
    pixels[xy_to_index(WIDTH - 1, 0)] = (0, 255, 0)
    pixels[xy_to_index(0, HEIGHT - 1)] = (0, 0, 255)
    pixels[xy_to_index(WIDTH - 1, HEIGHT - 1)] = (255, 255, 255)
    return tuple(pixels)


def blocks() -> tuple[Rgb, ...]:
    colors: tuple[Rgb, ...] = (
        (255, 0, 0),
        (0, 255, 0),
        (0, 0, 255),
        (255, 255, 255),
        (255, 255, 0),
        (0, 255, 255),
        (255, 0, 255),
        (64, 64, 64),
    )
    return tuple(color for color in colors for _ in range(32))
