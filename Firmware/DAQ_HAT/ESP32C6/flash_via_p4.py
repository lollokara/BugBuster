#!/usr/bin/env python3
"""
flash_via_p4.py — Flash the ESP32-C6 via the ESP32-P4 debug console.

The P4 uses esp-serial-flasher to program the C6 over UART2.
This script merges the three build artefacts into one image and streams
it to the P4's 'c6flash' CLI command over the USB-Serial-JTAG port.

Usage:
    python flash_via_p4.py <PORT>  [BUILD_DIR]  [--full]  [--attempts N]  [--no-build]

    PORT      — P4 REPL serial port  (e.g. COM15 or /dev/ttyACM0)
    BUILD_DIR — PlatformIO build output directory
                (default: <script_dir>/.pio/build/esp32c6)
    --full    — Flash bootloader + partition table + app from offset 0x0.
                Default (without --full) flashes only the app at 0x10000,
                keeping the existing bootloader and partition table on the chip.
    --attempts N — Retry up to N times on a stall/failure (default 6). Each
                   retry RESUMES from the last 4 KB-aligned point the C6
                   confirmed programming, so a mid-transfer stall does not
                   re-send the whole image.
    --no-build — Skip the PlatformIO build step and flash whatever is already
                 in BUILD_DIR. By default the C6 firmware is rebuilt first so
                 the flashed image is always fresh.

Example:
    python flash_via_p4.py COM15 --full
    python flash_via_p4.py COM15 --full --attempts 5
    python flash_via_p4.py COM15 --no-build
    python flash_via_p4.py /dev/ttyACM0 /path/to/esp32c6/build

Requires: pyserial  (pip install pyserial)
"""

import os
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed — run: pip install pyserial")
    sys.exit(1)

# ── Flash offsets (must match the ESP32-C6 partition table) ─────────────────
BOOTLOADER_OFFSET = 0x00000
PARTITIONS_OFFSET = 0x08000
APP_OFFSET        = 0x10000

CHUNK = 256   # bytes per chunk — must match buf[] in cmd_c6flash on P4


# ── Image builder ────────────────────────────────────────────────────────────

def make_merged_image(build_dir: str, app_only: bool = False) -> tuple:
    """Return (image_bytes, flash_offset).

    app_only=True  → only firmware.bin at 0x10000 (keeps existing bootloader
                      and partition table on the chip — safest for OTA chips).
    app_only=False → full merged image (bootloader + partitions + app) from 0x0.
    """
    files = {
        "bootloader": os.path.join(build_dir, "bootloader.bin"),
        "partitions": os.path.join(build_dir, "partitions.bin"),
        "firmware":   os.path.join(build_dir, "firmware.bin"),
    }
    for name, path in files.items():
        if not os.path.exists(path):
            print(f"ERROR: {name} not found: {path}")
            sys.exit(1)

    bl  = open(files["bootloader"], "rb").read()
    pt  = open(files["partitions"], "rb").read()
    app = open(files["firmware"],   "rb").read()

    # Sanity check: ESP image magic = 0xE9
    for name, data in [("bootloader", bl), ("app (firmware)", app)]:
        if data[0:1] != b'\xe9':
            print(f"WARNING: {name} first byte = 0x{data[0]:02X}, expected 0xE9 "
                  f"(may not be a valid ESP image)")

    if app_only:
        print(f"  Mode       : APP-ONLY (keeping existing bootloader + partition table)")
        print(f"  app        : {len(app):>7} B  @ 0x{APP_OFFSET:05X}"
              f"  [first bytes: {app[:4].hex()}]")
        print(f"  flash size : {len(app):>7} B")
        return bytes(app), APP_OFFSET
    else:
        total = APP_OFFSET + len(app)
        image = bytearray(b'\xff' * total)
        image[BOOTLOADER_OFFSET: BOOTLOADER_OFFSET + len(bl)] = bl
        image[PARTITIONS_OFFSET: PARTITIONS_OFFSET + len(pt)] = pt
        image[APP_OFFSET:        APP_OFFSET        + len(app)] = app
        print(f"  Mode       : FULL (bootloader + partitions + app)")
        print(f"  bootloader : {len(bl):>7} B  @ 0x{BOOTLOADER_OFFSET:05X}"
              f"  [first bytes: {bl[:4].hex()}]")
        print(f"  partitions : {len(pt):>7} B  @ 0x{PARTITIONS_OFFSET:05X}")
        print(f"  app        : {len(app):>7} B  @ 0x{APP_OFFSET:05X}"
              f"  [first bytes: {app[:4].hex()}]")
        print(f"  merged     : {total:>7} B  (0x{total:X})")
        return bytes(image), BOOTLOADER_OFFSET


# ── Serial helpers ───────────────────────────────────────────────────────────

def wait_token(ser: serial.Serial, tokens: list, timeout: float,
               echo: bool = True) -> str:
    """Read until one of *tokens* appears; return the matched token or ''."""
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = ser.read(max(1, ser.in_waiting))
        if data:
            if echo:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
            buf += data
            for t in tokens:
                if t in buf:
                    return t.decode()
        else:
            time.sleep(0.01)
    return ""


# ── Main ─────────────────────────────────────────────────────────────────────

def sync_to_prompt(ser: serial.Serial, timeout: float = 35.0) -> None:
    """Return the P4 REPL to a clean 'daq>' prompt.

    A previous aborted attempt may have left cmd_c6flash still running on the P4
    (it fails fast now, but can take up to ~10 s to time out). Send a bare CR and
    drain until the prompt appears or the line goes quiet, so the next c6flash
    command is parsed as a command — never mistaken for image data.
    """
    ser.reset_input_buffer()
    deadline = time.monotonic() + timeout
    buf = b""
    last_rx = time.monotonic()
    ser.write(b"\r\n")
    while time.monotonic() < deadline:
        n = ser.in_waiting
        data = ser.read(n if n else 1)
        if data:
            buf = (buf + data)[-256:]
            last_rx = time.monotonic()
            if b"daq>" in buf:
                break
        elif time.monotonic() - last_rx > 0.6:
            # Quiet line: nudge once more, then accept whatever state we are in.
            ser.write(b"\r\n")
            time.sleep(0.3)
            if ser.in_waiting == 0:
                break
    ser.reset_input_buffer()


def flash_once(port: str, image: bytes, flash_offset: int, base_off: int,
               attempt: int, attempts: int):
    """Flash image[base_off:] to the C6 at (flash_offset + base_off).

    Returns (success: bool, confirmed_abs: int) where confirmed_abs is the
    absolute number of bytes (from the start of `image`) the C6 has acknowledged
    writing so far. Never raises / never sys.exit()s so the caller can resume.

    Resume is safe because the P4's flash_start erases only from the given offset
    forward, leaving the bytes already programmed below `base_off` untouched, and
    every offset we pass is 4 KB-sector aligned.
    """
    CHUNK = 256          # must match buf[] size in cmd_c6flash on the P4
    ACK_STALL = 15.0     # per-chunk ACK timeout; a stall triggers a resume
    payload = memoryview(image)[base_off:]
    seg_total = len(payload)          # bytes to send this attempt
    seg_offset = flash_offset + base_off
    total = len(image)
    confirmed = base_off              # absolute bytes confirmed on the C6

    tag = f"resume @0x{base_off:X}" if base_off else "full image"
    print(f"\n── Attempt {attempt}/{attempts} ({tag}) ─────────────────")
    try:
        ser = serial.Serial(port, 115200, timeout=2)
    except serial.SerialException as e:
        print(f"ERROR: cannot open {port}: {e}")
        return False, confirmed

    try:
        time.sleep(0.3)
        sync_to_prompt(ser)

        # ── 1. Send the c6flash command ─────────────────────────────────────
        cmd = f"c6flash {seg_total} {seg_offset:#010x}\r\n".encode()
        print(f"Sending: {cmd.strip().decode()}")
        ser.write(cmd)

        # ── 2. READY — P4 starting bootloader entry ─────────────────────────
        print("Waiting for READY ", end="", flush=True)
        if not wait_token(ser, [b"READY"], timeout=6, echo=False):
            print("\nERROR: timed out waiting for READY "
                  "(is the P4 running firmware with the c6flash command?)")
            return False, confirmed
        print("✓")

        # ── 3. BEGIN — C6 ROM bootloader connected ──────────────────────────
        print("Waiting for BEGIN (C6 bootloader ~2 s) ", end="", flush=True)
        match = wait_token(ser, [b"BEGIN", b"FAIL:"], timeout=20, echo=True)
        if match != "BEGIN":
            if match == "FAIL:":
                print("\nERROR: P4 reported a bootloader-connect failure (see above).")
            else:
                print("\nERROR: timed out waiting for BEGIN — "
                      "check C6 power and RST/BOOT pin wiring.")
            return False, confirmed
        print("✓")

        # ── 4. Stream the segment with per-chunk ACK flow control ───────────
        # Send one CHUNK, wait for the P4's '.' ACK, then send the next. The P4
        # also emits PROG:<done>/<seg_total>\n every 16 KB (no '.' in those) which
        # we strip out to track the confirmed-flashed point for resume.
        sent = 0
        start = time.monotonic()
        print(f"Sending {seg_total} bytes @ 0x{seg_offset:X} "
              f"(flow-controlled, {CHUNK} B chunks) …")
        while sent < seg_total:
            ser.write(payload[sent: sent + CHUNK])
            sent += min(CHUNK, seg_total - sent)
            elapsed = max(time.monotonic() - start, 1e-9)
            print(f"\r  TX {base_off + sent:>7}/{total}  "
                  f"({(base_off + sent) * 100 // total:3}%)"
                  f"  Flash {confirmed:>7}B"
                  f"  {sent / elapsed / 1024:5.1f} KB/s   ", end="", flush=True)

            ack_buf = b""
            deadline = time.monotonic() + ACK_STALL
            got_ack = False
            fail_msg = None
            while time.monotonic() < deadline and not got_ack and not fail_msg:
                n = ser.in_waiting
                data = ser.read(n if n else 1)
                if not data:
                    continue
                ack_buf += data
                if b"FAIL:" in ack_buf:
                    fail_msg = ack_buf.decode(errors="replace").strip()
                    break
                # Strip PROG: lines first so the '.' scan sees only ACKs.
                while b"PROG:" in ack_buf and b"\n" in ack_buf:
                    s = ack_buf.index(b"PROG:")
                    e = ack_buf.index(b"\n", s)
                    try:
                        seg_done = int(ack_buf[s:e].split(b":")[1].split(b"/")[0])
                        confirmed = base_off + seg_done
                    except Exception:
                        pass
                    ack_buf = ack_buf[:s] + ack_buf[e + 1:]
                if b"." in ack_buf:
                    got_ack = True

            if fail_msg:
                print(f"\nERROR: P4 reported flash failure: {fail_msg}")
                return False, confirmed
            if not got_ack:
                print(f"\nERROR: no ACK for {ACK_STALL:.0f} s at "
                      f"{base_off + sent}/{total} bytes — treating as a stall.")
                return False, confirmed

        print(f"\r  TX {total:>7}/{total}  (100%)  sent" + " " * 30)

        # ── 5. Wait for OK / FAIL ───────────────────────────────────────────
        print("Waiting for flash result (verify + C6 reboot) …")
        # The P4 does printf("OK\n"), but the USB-Serial-JTAG console converts
        # \n -> \r\n, so the wire bytes are "OK\r\n". Match both a CR- and
        # LF-terminated "OK" so the success line is not missed (a missed OK
        # falsely triggers a resume pass — see wait_token's substring match).
        match = wait_token(ser, [b"OK\r", b"OK\n", b"FAIL:"], timeout=60, echo=True)
        time.sleep(0.4)
        tail = ser.read(ser.in_waiting)
        if tail:
            sys.stdout.buffer.write(tail)
            sys.stdout.buffer.flush()
        if match in ("OK\r", "OK\n"):
            print("\n✓ C6 flashed and verified.")
            return True, total
        if match == "FAIL:":
            print("\nERROR: P4 reported a flash/verify failure (see above).")
        else:
            print("\nERROR: timed out waiting for flash result.")
        return False, confirmed
    finally:
        try:
            ser.close()
        except Exception:
            pass


# ── Main ─────────────────────────────────────────────────────────────────────

def build_c6(project_dir: str, env: str = "esp32c6") -> None:
    """Rebuild the C6 firmware with PlatformIO so the flashed image is fresh.

    Runs `pio run -e <env>` in *project_dir*. Aborts the script on any build
    error (never flash a stale image after a failed build).
    """
    print("=" * 60)
    print(f"  Building ESP32-C6 firmware  (pio run -e {env})")
    print("=" * 60)
    try:
        rc = subprocess.call(["pio", "run", "-e", env], cwd=project_dir)
    except FileNotFoundError:
        print("ERROR: 'pio' (PlatformIO CLI) not found on PATH — cannot build.")
        print("       Install it or re-run with --no-build to flash an existing image.")
        sys.exit(1)
    if rc != 0:
        print(f"ERROR: PlatformIO build failed (exit {rc}) — not flashing.")
        sys.exit(rc)
    print("Build OK.\n")


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    port = sys.argv[1]
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_build = os.path.join(script_dir, ".pio", "build", "esp32c6")
    build_dir = default_build
    full_flash = False
    do_build = True
    attempts = 6
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--full":
            full_flash = True
        elif arg == "--no-build":
            do_build = False
        elif arg == "--attempts":
            i += 1
            attempts = max(1, int(args[i]))
        else:
            build_dir = arg
        i += 1

    print("=" * 60)
    print("  ESP32-C6 via-P4 flasher")
    print("=" * 60)
    print(f"Port      : {port}")
    print(f"Build dir : {build_dir}")
    print(f"Attempts  : {attempts}")
    print(f"Build     : {'yes' if do_build else 'skipped (--no-build)'}")
    print()

    # Rebuild first so we always flash a fresh image. Only meaningful for the
    # default build dir (a custom BUILD_DIR is assumed to be pre-built).
    if do_build:
        if build_dir == default_build:
            build_c6(script_dir)
        else:
            print("NOTE: custom BUILD_DIR given — skipping build (use the default "
                  "dir to auto-build, or pre-build that env yourself).\n")

    image, flash_offset = make_merged_image(build_dir, app_only=not full_flash)

    # Resume-capable retry. A transient P4↔C6 UART glitch can stall a transfer
    # part-way; instead of re-sending the whole image, the next attempt resumes
    # from the last 4 KB-aligned point the C6 confirmed writing. The P4's
    # flash_start erases only from the given offset forward, so bytes already
    # programmed below the resume point are preserved.
    SECTOR = 4096
    total = len(image)
    # Never resume so late that the remaining segment is smaller than one sector
    # (the P4 rejects sizes < 4096) and keep the offset sector-aligned.
    max_base = max(0, ((total - SECTOR) // SECTOR) * SECTOR)
    base = 0
    for attempt in range(1, attempts + 1):
        ok, confirmed = flash_once(port, image, flash_offset, base, attempt, attempts)
        if ok:
            print("\nDone.")
            return
        if attempt < attempts:
            new_base = min((confirmed // SECTOR) * SECTOR, max_base)
            if new_base > base:
                print(f"Resuming from 0x{new_base:X} "
                      f"({new_base}/{total} B already programmed) in 2 s …")
                base = new_base
            else:
                print(f"No new confirmed progress — retrying from 0x{base:X} in 2 s …")
            time.sleep(2.0)

    print(f"\nERROR: C6 flash failed after {attempts} attempt(s).")
    sys.exit(1)


if __name__ == "__main__":
    main()
