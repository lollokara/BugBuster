#!/usr/bin/env python3
"""Build and verify the two DAQ HAT firmware image formats.

The ESP32-P4 and the ESP32-C6 on the DAQ HAT need DIFFERENT image formats, and
mixing them up is the single most damaging mistake in this pipeline:

  P4  -> app-only OTA image (`.pio/build/esp32p4/firmware.bin` as-is).
         The S3 streams it into `esp_ota_write()`, which targets an A/B slot.
         A merged image here writes bootloader bytes into the app slot; the new
         slot fails to boot and the P4 falls back via A/B rollback.

  C6  -> full merged image from 0x0 (bootloader@0x0 + partitions@0x8000 +
         app@0x10000). `relay_c6_push()` calls
         `c6_flasher_begin(image_size - offset, offset)` with offset = 0 on a
         fresh push, so the ESP-ROM loader writes starting at flash offset 0.
         An app-only image here lays app bytes over the bootloader and BRICKS
         the C6 -- there is no A/B slot and no second MCU to recover it.

Both formats start with the same 0xE9 ESP image magic byte, so a magic-byte
check alone proves nothing. The discriminators that actually work are the
partition-table magic at 0x8000 and the second 0xE9 at 0x10000, which together
can only appear in a merged image -- and a merged image is necessarily longer
than 0x10000 bytes, which an app-only image may also be, so length is a
necessary but NOT sufficient condition on its own.

Reference implementation for the merge layout: the `make_merged_image()`
bench helper in Firmware/DAQ_HAT/ESP32C6/flash_via_p4.py (app_only=False).
This module is the CI-side equivalent, kept import-light (no pyserial) so the
release workflow and the unit tests can both use it.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Flash layout of a full merged ESP-IDF image, as written by the ROM loader.
BOOTLOADER_OFFSET = 0x0
PARTITIONS_OFFSET = 0x8000
APP_OFFSET = 0x10000

# First byte of any ESP-IDF application or bootloader image.
ESP_IMAGE_MAGIC = 0xE9
# ESP_PARTITION_MAGIC (0x50AA) little-endian, at the start of every entry of a
# partition table -- so it is the first two bytes of the table itself.
ESP_PARTITION_MAGIC_LE = b"\xaa\x50"

# A real DAQ HAT app is ~1 MB; anything tiny means a truncated or wrong file.
MIN_APP_SIZE = 64 * 1024


class ImageError(RuntimeError):
    """A built or published image is not in the format it claims to be."""


def looks_merged(data: bytes) -> bool:
    """True if *data* carries the structure only a merged-from-0x0 image has.

    All three conditions together: an app-only image is very often longer than
    0x10000, so the length test alone is not a discriminator -- it is the
    partition-table magic at 0x8000 and the second image magic at 0x10000 that
    make this decisive.
    """
    return (
        len(data) > APP_OFFSET
        and data[BOOTLOADER_OFFSET] == ESP_IMAGE_MAGIC
        and data[PARTITIONS_OFFSET:PARTITIONS_OFFSET + 2] == ESP_PARTITION_MAGIC_LE
        and data[APP_OFFSET] == ESP_IMAGE_MAGIC
    )


def make_merged_image(build_dir: Path) -> bytes:
    """Assemble bootloader + partition table + app into one image from 0x0.

    Gaps are 0xFF (erased flash), matching what esptool's own merge_bin emits
    and what flash_via_p4.py's bench helper produces.
    """
    parts = {
        "bootloader": build_dir / "bootloader.bin",
        "partitions": build_dir / "partitions.bin",
        "firmware": build_dir / "firmware.bin",
    }
    missing = [f"{name} ({path})" for name, path in parts.items() if not path.is_file()]
    if missing:
        raise ImageError("missing build output(s): " + ", ".join(missing))

    bootloader = parts["bootloader"].read_bytes()
    partitions = parts["partitions"].read_bytes()
    app = parts["firmware"].read_bytes()

    if bootloader[:1] != bytes([ESP_IMAGE_MAGIC]):
        raise ImageError(
            f"bootloader.bin starts with 0x{bootloader[0]:02X}, expected "
            f"0x{ESP_IMAGE_MAGIC:02X} -- not an ESP image"
        )
    if app[:1] != bytes([ESP_IMAGE_MAGIC]):
        raise ImageError(
            f"firmware.bin starts with 0x{app[0]:02X}, expected "
            f"0x{ESP_IMAGE_MAGIC:02X} -- not an ESP image"
        )
    if partitions[:2] != ESP_PARTITION_MAGIC_LE:
        raise ImageError(
            "partitions.bin does not start with the ESP_PARTITION_MAGIC 0x50AA"
        )
    if len(bootloader) > PARTITIONS_OFFSET:
        raise ImageError(
            f"bootloader.bin is {len(bootloader)} B and would overrun the "
            f"partition table at 0x{PARTITIONS_OFFSET:X}"
        )
    if PARTITIONS_OFFSET + len(partitions) > APP_OFFSET:
        raise ImageError(
            f"partitions.bin is {len(partitions)} B and would overrun the app "
            f"at 0x{APP_OFFSET:X}"
        )

    image = bytearray(b"\xff" * (APP_OFFSET + len(app)))
    image[BOOTLOADER_OFFSET:BOOTLOADER_OFFSET + len(bootloader)] = bootloader
    image[PARTITIONS_OFFSET:PARTITIONS_OFFSET + len(partitions)] = partitions
    image[APP_OFFSET:APP_OFFSET + len(app)] = app
    return bytes(image)


def verify_merged(data: bytes) -> None:
    """Assert *data* is a full merged image safe to hand the C6 ROM loader."""
    if len(data) <= APP_OFFSET:
        raise ImageError(
            f"image is {len(data)} B, which is <= the 0x{APP_OFFSET:X} app "
            f"offset -- a merged image must extend past it. This looks like an "
            f"app-only image; flashing it at offset 0 would overwrite the C6 "
            f"bootloader and brick the chip."
        )
    if data[BOOTLOADER_OFFSET] != ESP_IMAGE_MAGIC:
        raise ImageError(
            f"byte 0x0 is 0x{data[BOOTLOADER_OFFSET]:02X}, expected the "
            f"0x{ESP_IMAGE_MAGIC:02X} bootloader magic"
        )
    if data[PARTITIONS_OFFSET:PARTITIONS_OFFSET + 2] != ESP_PARTITION_MAGIC_LE:
        raise ImageError(
            f"no ESP_PARTITION_MAGIC (0x50AA) at 0x{PARTITIONS_OFFSET:X}. The "
            f"image is long enough to be merged but has no partition table "
            f"where the ROM loader expects one -- almost certainly an app-only "
            f"image, which would brick the C6."
        )
    if data[APP_OFFSET] != ESP_IMAGE_MAGIC:
        raise ImageError(
            f"byte 0x{APP_OFFSET:X} is 0x{data[APP_OFFSET]:02X}, expected the "
            f"0x{ESP_IMAGE_MAGIC:02X} app magic"
        )


def verify_app_only(data: bytes) -> None:
    """Assert *data* is a bare app image safe to hand `esp_ota_write()`."""
    if len(data) < MIN_APP_SIZE:
        raise ImageError(
            f"image is only {len(data)} B; a real DAQ HAT app is ~1 MB. "
            f"Truncated or wrong file."
        )
    if data[0] != ESP_IMAGE_MAGIC:
        raise ImageError(
            f"byte 0x0 is 0x{data[0]:02X}, expected the "
            f"0x{ESP_IMAGE_MAGIC:02X} app magic"
        )
    if looks_merged(data):
        raise ImageError(
            "this is a MERGED image (partition-table magic at "
            f"0x{PARTITIONS_OFFSET:X} and app magic at 0x{APP_OFFSET:X}), not "
            "an app-only OTA image. Streaming it into esp_ota_write() writes "
            "bootloader bytes into the P4's A/B slot and the new slot will not "
            "boot."
        )


def _cmd_merge(args: argparse.Namespace) -> int:
    image = make_merged_image(Path(args.build_dir))
    verify_merged(image)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(
        f"merged image: {out} ({len(image)} B, 0x{len(image):X}) "
        f"[bootloader@0x0, partitions@0x{PARTITIONS_OFFSET:X}, "
        f"app@0x{APP_OFFSET:X}]"
    )
    return 0


def _cmd_verify(args: argparse.Namespace) -> int:
    path = Path(args.path)
    if not path.is_file():
        print(f"error: no such file: {path}", file=sys.stderr)
        return 1
    data = path.read_bytes()
    checker = verify_merged if args.format == "merged" else verify_app_only
    try:
        checker(data)
    except ImageError as exc:
        print(f"error: {path.name} is not a valid {args.format} image: {exc}",
              file=sys.stderr)
        return 1
    print(f"ok: {path.name} is a valid {args.format} image ({len(data)} B)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    merge = sub.add_parser(
        "merge", help="build a full merged image from a PlatformIO build dir"
    )
    merge.add_argument("--build-dir", required=True,
                       help="directory holding bootloader.bin/partitions.bin/firmware.bin")
    merge.add_argument("--out", required=True, help="output path for the merged image")
    merge.set_defaults(func=_cmd_merge)

    verify = sub.add_parser("verify", help="check a published asset's format")
    verify.add_argument("path")
    verify.add_argument("--format", required=True, choices=["merged", "app-only"])
    verify.set_defaults(func=_cmd_verify)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ImageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
