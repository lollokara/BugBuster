#!/usr/bin/env python3
"""daq_push.py -- push a locally built P4 or C6 image to the DAQ HAT over HTTP.

The C6 needs a MERGED image from flash offset 0 (bootloader + partition table
+ app) because relay_c6_push() starts the ROM-loader write at 0. The PlatformIO
build only emits the three pieces separately, so this merges them with esptool
before uploading. Pushing the app-only firmware.bin would brick the C6; the
device rejects it, but merging here means you never hit that.

Usage:
    python3 tests/tools/daq_push.py p4 --host 192.168.3.18 --token $TOK
    python3 tests/tools/daq_push.py c6 --host 192.168.3.18 --token $TOK
    python3 tests/tools/daq_push.py c6 --image merged.bin --host ... --token ...
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from bugbuster.ota import OTAClient, OTAError  # noqa: E402
from bugbuster.transport import HTTPTransport  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
P4_BIN = REPO / "Firmware/DAQ_HAT/ESP32P4/.pio/build/esp32p4/firmware.bin"
C6_DIR = REPO / "Firmware/DAQ_HAT/ESP32C6/.pio/build/esp32c6"


def _find_esptool() -> list[str]:
    """Return an argv prefix that invokes esptool, or exit with a clear error.

    esptool may be installed as the ``esptool.py`` script, as ``esptool``,
    reachable via ``python3 -m esptool``, or bundled inside a local PlatformIO
    installation. Try each in turn rather than letting subprocess raise a
    FileNotFoundError traceback.
    """
    for name in ("esptool.py", "esptool"):
        found = shutil.which(name)
        if found:
            return [found]

    for candidate in (
        Path.home() / ".platformio" / "penv" / "bin" / "esptool.py",
        Path.home() / ".platformio" / "penv" / "bin" / "esptool",
    ):
        if candidate.is_file():
            return [str(candidate)]

    probe = subprocess.run(
        [sys.executable, "-m", "esptool", "version"],
        capture_output=True, text=True,
    )
    if probe.returncode == 0:
        return [sys.executable, "-m", "esptool"]

    sys.exit(
        "esptool not found. Install it with `pip install esptool`, or make sure "
        "PlatformIO's bundled esptool (~/.platformio/penv/bin/esptool.py) is present."
    )


def merge_c6(out: Path) -> Path:
    """esptool merge_bin over the three C6 artefacts, at their flash offsets."""
    parts = {
        "0x0": C6_DIR / "bootloader.bin",
        "0x8000": C6_DIR / "partitions.bin",
        "0x10000": C6_DIR / "firmware.bin",
    }
    missing = [str(p) for p in parts.values() if not p.is_file()]
    if missing:
        sys.exit("missing C6 build artefacts (run `pio run -e esp32c6` first):\n  "
                 + "\n  ".join(missing))

    esptool = _find_esptool()
    cmd = esptool + ["--chip", "esp32c6", "merge_bin", "-o", str(out)]
    for off, path in parts.items():
        cmd += [off, str(path)]
    subprocess.run(cmd, check=True)

    _verify_merged(out)
    return out


def _verify_merged(path: Path) -> None:
    """Check the two magic bytes the device itself validates before flashing.

    Catching a bad merge here (wrong offsets, truncated artefact) is far
    cheaper than finding out from a device-side rejection or, worse, a brick.
    """
    data = path.read_bytes()
    if len(data) < 0x8002:
        sys.exit(f"merged image {path} is too small ({len(data)} bytes) to be valid")
    if data[0] != 0xE9:
        sys.exit(
            f"merged image {path} does not start with the ESP image magic byte "
            f"0xE9 (got 0x{data[0]:02X}) -- merge is corrupt, do not push this"
        )
    if data[0x8000:0x8002] != b"\xAA\x50":
        sys.exit(
            f"merged image {path} is missing the partition-table magic AA 50 "
            f"at offset 0x8000 (got {data[0x8000:0x8002].hex()}) -- merge is "
            f"corrupt, do not push this"
        )


def render(rec: dict) -> None:
    stage = rec.get("stage", "?")
    if stage in ("upload", "stage", "relay") and rec.get("total"):
        done, total = rec["done"], rec["total"]
        pct = 100.0 * done / total
        bar = "#" * int(pct / 2.5)
        print(f"\r  {stage:<6} [{bar:<40}] {pct:5.1f}%  {done}/{total}", end="", flush=True)
    else:
        print(f"\r  {stage:<6}{' ' * 58}")
        print(f"  -> {rec}")


def _make_ota(host: str, token: str) -> OTAClient:
    transport = HTTPTransport(host, admin_token=token)
    return OTAClient(transport)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("target", choices=["p4", "c6"])
    ap.add_argument("--host", required=True)
    ap.add_argument("--token", required=True, help="admin token (CLI: `token`)")
    ap.add_argument("--image", help="override the built image path")
    args = ap.parse_args()

    ota = _make_ota(args.host, args.token)

    if args.target == "p4":
        image = Path(args.image) if args.image else P4_BIN
        if not image.is_file():
            sys.exit(f"no P4 image at {image} (run `pio run -e esp32p4`)")
        print(f"pushing P4 image {image} ({image.stat().st_size} bytes)")
        fn = ota.upload_p4
    else:
        if args.image:
            image = Path(args.image)
        else:
            tmp = Path(tempfile.mkdtemp()) / "c6-merged.bin"
            print("merging C6 bootloader + partitions + app ...")
            image = merge_c6(tmp)
        print(f"pushing C6 image {image} ({image.stat().st_size} bytes)")
        fn = ota.upload_c6

    try:
        final = fn(str(image), on_event=render)
    except OTAError as e:
        print(f"\nFAILED: {e}")
        return 1
    except requests.exceptions.RequestException as e:
        print(f"\nFAILED: could not reach {args.host}: {e}")
        return 1
    print(f"\nOK: {final}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
