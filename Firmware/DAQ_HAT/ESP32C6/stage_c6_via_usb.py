#!/usr/bin/env python3
"""
stage_c6_via_usb.py — Stage a C6 firmware image onto the P4's `staging`
partition over the P4's own USB-HS vendor-bulk port (fast bulk transfer,
independent of the slow debug-console `c6flash` path in flash_via_p4.py).

This only STAGES the image (USB_CMD_OTA_BEGIN/DATA/END, target=C6) and asks
the P4 to verify it (SHA-256). It does NOT push it to the C6 -- the P4->C6
leg still goes over the existing UART flasher (c6_flasher.c), which is the
same one flash_via_p4.py drives, just triggered on-device instead of from a
slow host<->debug-console transfer. After this script reports "staged and
verified", run on the P4's debug console (USB-Serial-JTAG, e.g. via Tabby):

    daq> c6relay status     # confirm state=STAGED, staged_bytes == image_size
    daq> c6relay push       # actually pushes to the C6 (resumable, NVS-tracked)

Usage:
    python stage_c6_via_usb.py [BUILD_DIR] [--full]

    BUILD_DIR — PlatformIO build output directory
                (default: <script_dir>/.pio/build/esp32c6)
    --full    — Stage bootloader + partition table + app merged from offset
                0x0 (matches relay_c6_push()'s assumption that the staged
                image starts at C6 flash offset 0). This is the only mode
                relay_c6_push() supports today (no app-only staging).

Requires: pyusb (pip install pyusb), and libusb installed (macOS: brew install libusb).
"""

import hashlib
import os
import struct
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    print("ERROR: pyusb not installed -- run: pip install pyusb")
    sys.exit(1)

DAQ_VID = 0x303A
DAQ_PID = 0x4001
DAQ_IFACE = 0
DAQ_EP_OUT = 0x01

BOOTLOADER_OFFSET = 0x00000
PARTITIONS_OFFSET = 0x08000
APP_OFFSET = 0x10000

USB_CMD_OTA_BEGIN = 0x8C
USB_CMD_OTA_DATA = 0x8D
USB_CMD_OTA_END = 0x8E

RELAY_TARGET_C6 = 1

# Must fit usb_stream_t's control-frame reassembly buffer (usb_stream.h
# rx_buf, sized USB_FRAME_OVERHEAD + 512 bytes of PAYLOAD) -- payload here is
# a 4-byte offset + this many data bytes, so keep headroom. A too-large chunk
# is silently dropped firmware-side (no error logged), which only surfaces
# later as a SHA-256/size mismatch at OTA_END.
DATA_CHUNK = 500

FW_PRODUCT_ID = b"DAQHAT_C6"  # informational only -- relay_stage_begin() does not validate it


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_command(seq: int, cmd_type: int, payload: bytes) -> bytes:
    frame = bytearray()
    frame += bytes([0xBB, 0x50, 2, cmd_type, 0, 0])  # magic, version=2, type, flags, reserved
    frame += struct.pack("<I", seq)
    frame += struct.pack("<H", len(payload))
    frame += payload
    crc = crc16_ccitt_false(bytes(frame[2:]))
    frame += struct.pack("<H", crc)
    return bytes(frame)


def make_merged_image(build_dir: str, full: bool) -> bytes:
    files = {
        "bootloader": os.path.join(build_dir, "bootloader.bin"),
        "partitions": os.path.join(build_dir, "partitions.bin"),
        "firmware": os.path.join(build_dir, "firmware.bin"),
    }
    for name, path in files.items():
        if not os.path.exists(path):
            print(f"ERROR: {name} not found: {path}")
            sys.exit(1)

    bl = open(files["bootloader"], "rb").read()
    pt = open(files["partitions"], "rb").read()
    app = open(files["firmware"], "rb").read()

    if not full:
        print("ERROR: relay_c6_push() assumes a full merged image starting at C6 "
              "flash offset 0 -- pass --full (app-only staging isn't supported by "
              "the relay path).")
        sys.exit(1)

    total = APP_OFFSET + len(app)
    image = bytearray(b"\xff" * total)
    image[BOOTLOADER_OFFSET: BOOTLOADER_OFFSET + len(bl)] = bl
    image[PARTITIONS_OFFSET: PARTITIONS_OFFSET + len(pt)] = pt
    image[APP_OFFSET: APP_OFFSET + len(app)] = app
    print(f"  bootloader : {len(bl):>7} B  @ 0x{BOOTLOADER_OFFSET:05X}")
    print(f"  partitions : {len(pt):>7} B  @ 0x{PARTITIONS_OFFSET:05X}")
    print(f"  app        : {len(app):>7} B  @ 0x{APP_OFFSET:05X}")
    print(f"  merged     : {total:>7} B  (0x{total:X})")
    return bytes(image)


def open_daq_usb():
    dev = usb.core.find(idVendor=DAQ_VID, idProduct=DAQ_PID)
    if dev is None:
        print(f"ERROR: P4 DAQ USB device not found (VID={DAQ_VID:04X} PID={DAQ_PID:04X})")
        sys.exit(1)
    try:
        if dev.is_kernel_driver_active(DAQ_IFACE):
            dev.detach_kernel_driver(DAQ_IFACE)
    except (NotImplementedError, usb.core.USBError):
        pass  # macOS: no kernel driver claiming a vendor interface, nothing to detach
    usb.util.claim_interface(dev, DAQ_IFACE)
    return dev


def main():
    args = sys.argv[1:]
    full = "--full" in args
    args = [a for a in args if a != "--full"]
    script_dir = os.path.dirname(os.path.abspath(__file__))
    build_dir = args[0] if args else os.path.join(script_dir, ".pio", "build", "esp32c6")

    print(f"Build dir: {build_dir}")
    image = make_merged_image(build_dir, full)
    sha256 = hashlib.sha256(image).digest()

    dev = open_daq_usb()
    print(f"Opened P4 DAQ USB device, claimed interface {DAQ_IFACE}")

    seq = 0

    # ota_meta_t: u32 image_size, u32 version_u32, u8 sha256[32], char product_id[16]
    product_id = FW_PRODUCT_ID[:16].ljust(16, b"\x00")
    meta = struct.pack("<II32s16s", len(image), 0, sha256, product_id)
    payload = meta + bytes([RELAY_TARGET_C6])
    dev.write(DAQ_EP_OUT, encode_command(seq, USB_CMD_OTA_BEGIN, payload))
    seq += 1
    print(f"Sent OTA_BEGIN: image_size={len(image)} sha256={sha256.hex()[:16]}... target=C6")

    t0 = time.time()
    offset = 0
    while offset < len(image):
        chunk = image[offset: offset + DATA_CHUNK]
        payload = struct.pack("<I", offset) + chunk
        dev.write(DAQ_EP_OUT, encode_command(seq, USB_CMD_OTA_DATA, payload))
        seq += 1
        offset += len(chunk)
        if seq % 64 == 0 or offset == len(image):
            pct = 100.0 * offset / len(image)
            elapsed = time.time() - t0
            rate = (offset / 1024.0) / elapsed if elapsed > 0 else 0
            print(f"  staged {offset}/{len(image)} ({pct:5.1f}%)  {rate:6.1f} KB/s", end="\r")

    print()
    dev.write(DAQ_EP_OUT, encode_command(seq, USB_CMD_OTA_END, b""))
    print("Sent OTA_END (P4 now verifying SHA-256 over the staged image)")
    print()
    print("Next: on the P4 debug console, run:")
    print("    c6relay status   # confirm state=STAGED, staged_bytes == image_size")
    print("    c6relay push     # push the staged image to the C6")


if __name__ == "__main__":
    main()
