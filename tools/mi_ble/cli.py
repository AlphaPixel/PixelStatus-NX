"""Command-line entry point for MI display discovery, validation, and bridging."""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from collections.abc import Sequence

from .bridge import BridgeOptions, run_bridge
from .display import BleakConnection, MiBleError, MiFrameWriter, scan_devices
from .frame_source import DisplayFrame, FrameSourceError, HttpFrameSource
from .patterns import blocks, corners
from .protocol import TARGET_NAME, build_block_packets, build_pixel_packet, transform_frame


def _nonnegative_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must not be negative")
    return parsed


def _positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def _add_device_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--name", default=TARGET_NAME, help="advertised display name")
    parser.add_argument("--address", help="optional runtime BLE address override")
    parser.add_argument("--scan-seconds", type=_positive_float, default=8.0)
    parser.add_argument("--connect-seconds", type=_positive_float, default=15.0)


def _add_output_arguments(parser: argparse.ArgumentParser) -> None:
    _add_device_arguments(parser)
    parser.add_argument(
        "--mode",
        choices=("pixel", "block"),
        default="pixel",
        help="pixel is device-validated; block remains opt-in until calibrated",
    )
    parser.add_argument(
        "--response",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="use GATT writes with response",
    )
    parser.add_argument("--pixel-delay-ms", type=_nonnegative_float, default=20.0)
    parser.add_argument("--block-delay-ms", type=_nonnegative_float, default=5.0)
    parser.add_argument("--init-delay-ms", type=_nonnegative_float, default=100.0)
    parser.add_argument("--rotation", type=int, choices=(0, 90, 180, 270), default=0)
    parser.add_argument("--mirror-x", action="store_true")
    parser.add_argument("--mirror-y", action="store_true")
    parser.add_argument(
        "--color-order",
        choices=("RGB", "RBG", "GRB", "GBR", "BRG", "BGR"),
        default="RGB",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Mirror PixelStatus NX frames to a Merkury / MI BLE matrix"
    )
    parser.add_argument("--verbose", action="store_true")
    commands = parser.add_subparsers(dest="command", required=True)

    scan = commands.add_parser("scan", help="list BLE advertisements without connecting")
    scan.add_argument("--seconds", type=_positive_float, default=6.0)

    probe = commands.add_parser("probe", help="connect and inspect the MI GATT endpoint")
    _add_device_arguments(probe)

    bridge = commands.add_parser(
        "bridge", help="mirror the read-only PixelStatus display-frame API"
    )
    _add_output_arguments(bridge)
    bridge.add_argument(
        "--url",
        default="http://127.0.0.1:8788/api/v1/display",
        help="PixelStatus browser-display frame endpoint",
    )
    bridge.add_argument("--poll-ms", type=_positive_float, default=100.0)
    bridge.add_argument("--request-seconds", type=_positive_float, default=3.0)
    bridge.add_argument("--reconnect-min-ms", type=_positive_float, default=500.0)
    bridge.add_argument("--reconnect-max-ms", type=_positive_float, default=10000.0)
    bridge.add_argument(
        "--status-file",
        help="optional atomic JSON heartbeat path for unattended operation",
    )
    bridge.add_argument(
        "--once", action="store_true", help="send one complete frame and exit"
    )
    bridge.add_argument(
        "--dry-run",
        action="store_true",
        help="fetch and packetize one frame without accessing Bluetooth",
    )

    pattern = commands.add_parser(
        "pattern", help="send a deterministic physical calibration pattern"
    )
    _add_output_arguments(pattern)
    pattern.add_argument("kind", choices=("corners", "blocks"))
    return parser


def _options(arguments: argparse.Namespace) -> BridgeOptions:
    return BridgeOptions(
        target_name=arguments.name,
        address=arguments.address,
        scan_timeout_seconds=arguments.scan_seconds,
        connect_timeout_seconds=arguments.connect_seconds,
        poll_seconds=getattr(arguments, "poll_ms", 100.0) / 1000.0,
        reconnect_initial_seconds=getattr(arguments, "reconnect_min_ms", 500.0)
        / 1000.0,
        reconnect_maximum_seconds=getattr(arguments, "reconnect_max_ms", 10000.0)
        / 1000.0,
        mode=arguments.mode,
        response=arguments.response,
        pixel_delay_seconds=arguments.pixel_delay_ms / 1000.0,
        block_delay_seconds=arguments.block_delay_ms / 1000.0,
        init_delay_seconds=arguments.init_delay_ms / 1000.0,
        rotation=arguments.rotation,
        mirror_x=arguments.mirror_x,
        mirror_y=arguments.mirror_y,
        color_order=arguments.color_order,
        status_file=getattr(arguments, "status_file", None),
    )


def _physical_pixels(frame: DisplayFrame, options: BridgeOptions):
    return transform_frame(
        frame.pixels,
        rotation=options.rotation,
        mirror_x=options.mirror_x,
        mirror_y=options.mirror_y,
        color_order=options.color_order,
    )


async def _write_frame(frame: DisplayFrame, options: BridgeOptions) -> int:
    connection = await BleakConnection.connect(
        target_name=options.target_name,
        address=options.address,
        scan_timeout_seconds=options.scan_timeout_seconds,
        connect_timeout_seconds=options.connect_timeout_seconds,
    )
    async with connection:
        info = connection.info
        print(
            f"Connected: {info.name!r} [{info.address}], "
            f"properties={','.join(info.characteristic_properties)}, "
            f"max_no_response={info.max_write_without_response_size}"
        )
        writer = MiFrameWriter(
            connection,
            mode=options.mode,
            response=options.response,
            pixel_delay_seconds=options.pixel_delay_seconds,
            block_delay_seconds=options.block_delay_seconds,
            init_delay_seconds=options.init_delay_seconds,
        )
        return await writer.send_frame(_physical_pixels(frame, options))


async def _run(arguments: argparse.Namespace) -> None:
    if arguments.command == "scan":
        devices = await scan_devices(arguments.seconds)
        for device in devices:
            marker = " *" if device.name and TARGET_NAME in device.name else ""
            print(f"{device.name!r:32} {device.address}{marker}")
        print(f"{len(devices)} BLE device(s); * marks the default MI target")
        return

    if arguments.command == "probe":
        connection = await BleakConnection.connect(
            target_name=arguments.name,
            address=arguments.address,
            scan_timeout_seconds=arguments.scan_seconds,
            connect_timeout_seconds=arguments.connect_seconds,
        )
        async with connection:
            info = connection.info
            print(f"Connected: {info.name!r} [{info.address}]")
            print(f"Characteristic: {','.join(info.characteristic_properties)}")
            print(f"Maximum write without response: {info.max_write_without_response_size}")
        print("Probe complete; no display commands were written")
        return

    options = _options(arguments)
    if arguments.command == "pattern":
        pixels = corners() if arguments.kind == "corners" else blocks()
        writes = await _write_frame(DisplayFrame(sequence=0, pixels=pixels), options)
        print(f"Calibration pattern sent using {writes} frame-data writes")
        return

    source = HttpFrameSource(arguments.url, timeout_seconds=arguments.request_seconds)
    if arguments.dry_run:
        if not arguments.once:
            raise ValueError("--dry-run requires --once")
        frame = await source.fetch()
        if frame is None:
            raise FrameSourceError("display endpoint returned no initial frame")
        pixels = _physical_pixels(frame, options)
        packets: Sequence[bytes]
        if options.mode == "pixel":
            packets = tuple(
                build_pixel_packet(index, color) for index, color in enumerate(pixels)
            )
        else:
            packets = build_block_packets(pixels)
        print(
            f"Dry run: frame={frame.sequence}, mode={options.mode}, "
            f"data_writes={len(packets)}, data_bytes={sum(map(len, packets))}"
        )
        return
    if arguments.once:
        frame = await source.fetch()
        if frame is None:
            raise FrameSourceError("display endpoint returned no initial frame")
        writes = await _write_frame(frame, options)
        print(f"Displayed source frame {frame.sequence} using {writes} frame-data writes")
        return
    await run_bridge(source, options)


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    arguments = parser.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if arguments.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    try:
        asyncio.run(_run(arguments))
    except KeyboardInterrupt:
        print("Stopped", file=sys.stderr)
        return 130
    except (FrameSourceError, MiBleError, ValueError, OSError) as error:
        print(f"MI bridge error: {error}", file=sys.stderr)
        return 1
    return 0
