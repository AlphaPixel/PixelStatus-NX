"""Bounded reader for the PixelStatus NX browser-display frame API."""

from __future__ import annotations

import asyncio
import json
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any

from .protocol import HEIGHT, PIXEL_COUNT, WIDTH, Rgb, unpack_rgb888

MAXIMUM_DOCUMENT_BYTES = 128 * 1024


class FrameSourceError(RuntimeError):
    """A display-frame response was unavailable or invalid."""


@dataclass(frozen=True, slots=True)
class DisplayFrame:
    sequence: int
    pixels: tuple[Rgb, ...]
    width: int = WIDTH
    height: int = HEIGHT


def _required_integer(document: dict[str, Any], name: str) -> int:
    value = document.get(name)
    if isinstance(value, bool) or not isinstance(value, int):
        raise FrameSourceError(f"display frame field {name!r} must be an integer")
    return value


def parse_display_document(payload: bytes) -> DisplayFrame:
    if len(payload) > MAXIMUM_DOCUMENT_BYTES:
        raise FrameSourceError("display frame response exceeds the 128 KiB limit")
    try:
        document = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FrameSourceError("display frame response is not valid UTF-8 JSON") from error
    if not isinstance(document, dict):
        raise FrameSourceError("display frame response must be a JSON object")
    if _required_integer(document, "schema_version") != 1:
        raise FrameSourceError("unsupported display frame schema_version")
    width = _required_integer(document, "width")
    height = _required_integer(document, "height")
    if width != WIDTH or height != HEIGHT:
        raise FrameSourceError(
            f"MI output requires a {WIDTH}x{HEIGHT} frame, received {width}x{height}"
        )
    sequence = _required_integer(document, "sequence")
    if sequence < 0:
        raise FrameSourceError("display frame sequence must not be negative")
    if document.get("format") != "rgb888":
        raise FrameSourceError("display frame format must be rgb888")
    packed_pixels = document.get("pixels")
    if not isinstance(packed_pixels, list) or len(packed_pixels) != PIXEL_COUNT:
        raise FrameSourceError("display frame must contain exactly 256 pixels")
    try:
        pixels = tuple(unpack_rgb888(pixel) for pixel in packed_pixels)
    except ValueError as error:
        raise FrameSourceError(str(error)) from error
    return DisplayFrame(sequence=sequence, pixels=pixels)


class HttpFrameSource:
    def __init__(self, url: str, *, timeout_seconds: float = 3.0) -> None:
        if not url.startswith(("http://", "https://")):
            raise ValueError("display URL must use http:// or https://")
        if timeout_seconds <= 0:
            raise ValueError("display request timeout must be positive")
        self._url = url
        self._timeout_seconds = timeout_seconds
        self._etag: str | None = None

    async def fetch(self) -> DisplayFrame | None:
        return await asyncio.to_thread(self._fetch_sync)

    def _fetch_sync(self) -> DisplayFrame | None:
        headers = {"Accept": "application/json"}
        if self._etag:
            headers["If-None-Match"] = self._etag
        request = urllib.request.Request(self._url, headers=headers, method="GET")
        try:
            with urllib.request.urlopen(request, timeout=self._timeout_seconds) as response:
                payload = response.read(MAXIMUM_DOCUMENT_BYTES + 1)
                if len(payload) > MAXIMUM_DOCUMENT_BYTES:
                    raise FrameSourceError("display frame response exceeds the 128 KiB limit")
                frame = parse_display_document(payload)
                self._etag = response.headers.get("ETag")
                return frame
        except urllib.error.HTTPError as error:
            if error.code == 304:
                return None
            raise FrameSourceError(f"display frame endpoint returned HTTP {error.code}") from error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise FrameSourceError(f"display frame request failed: {error}") from error
