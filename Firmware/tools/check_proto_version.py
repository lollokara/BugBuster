#!/usr/bin/env python3
"""Check that every wire-protocol version constant is in sync across its copies.

Three protocol families are gated:

* **BBP** - 3 copies (ESP32 firmware, Python lib, desktop Rust).
* **DAQ USB stream** - 4 copies. The P4 defines it and there are three
  independent host decoders; this is exactly the shape that produced the 0x13
  error-code collision, so it is gated rather than reviewed.
* **DDP** (P4 <-> C6 display link) - one canonical definition in a shared
  header, plus a scan for any shadow copy that could drift away from it.

Usage:
    python Firmware/tools/check_proto_version.py
    python Firmware/tools/check_proto_version.py --verbose

Exit 0 if every family agrees; exit 1 with a diff summary otherwise.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# family -> [(label, relative path, regex with one capturing group)]
FAMILIES: dict[str, list[tuple[str, str, str]]] = {
    "BBP_PROTO_VERSION": [
        (
            "Firmware/ESP32/src/bbp/bbp.h",
            "Firmware/ESP32/src/bbp/bbp.h",
            r"(?m)^#define\s+BBP_PROTO_VERSION\s+(\d+)",
        ),
        (
            "python/bugbuster/protocol.py",
            "python/bugbuster/protocol.py",
            r"(?m)^BBP_PROTO_VERSION\s*=\s*(\d+)",
        ),
        (
            "DesktopApp/BugBuster/src-tauri/src/bbp.rs",
            "DesktopApp/BugBuster/src-tauri/src/bbp.rs",
            r"(?m)^pub\s+const\s+PROTO_VERSION\s*:\s*u8\s*=\s*(\d+)\s*;",
        ),
    ],
    "USB_PROTO_VERSION (DAQ stream)": [
        (
            "Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h",
            "Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h",
            r"(?m)^#define\s+USB_PROTO_VERSION\s+(\d+)u?",
        ),
        (
            "python/bugbuster/daq_stream.py",
            "python/bugbuster/daq_stream.py",
            r"(?m)^PROTO_VERSION\s*=\s*(\d+)",
        ),
        (
            "DesktopApp/BugBuster/src-tauri/src/daq_proto.rs",
            "DesktopApp/BugBuster/src-tauri/src/daq_proto.rs",
            r"(?m)^pub\s+const\s+PROTO_VERSION\s*:\s*u8\s*=\s*(\d+)\s*;",
        ),
        (
            "tests/lib/daq_proto.py",
            "tests/lib/daq_proto.py",
            r"(?m)^PROTO_VERSION\s*=\s*(\d+)",
        ),
    ],
    "DDP_PROTO_VERSION (P4 <-> C6)": [
        (
            "Firmware/DAQ_HAT/common/ddp_proto.h",
            "Firmware/DAQ_HAT/common/ddp_proto.h",
            r"(?m)^#define\s+DDP_PROTO_VERSION\s+(\d+)u?",
        ),
    ],
}

# Files that must NOT define their own copy of a gated constant. A shadow copy
# is invisible to this gate until something imports it, by which point it has
# already drifted. python/bugbuster/constants.py did exactly that with BBP.
SHADOW_BANS: list[tuple[str, str, str]] = [
    (
        "python/bugbuster/constants.py",
        r"(?m)^BBP_PROTO_VERSION\s*=\s*\d+",
        "the canonical copy is python/bugbuster/protocol.py; import it from there",
    ),
]


def extract_version(label: str, path: Path, pattern: str) -> int:
    if not path.exists():
        print(f"ERROR: file not found: {path}", file=sys.stderr)
        sys.exit(2)
    text = path.read_text(encoding="utf-8")
    match = re.search(pattern, text)
    if match is None:
        print(f"ERROR: could not find the version constant in {label}", file=sys.stderr)
        sys.exit(2)
    return int(match.group(1))


def check_no_shadow_copies() -> int:
    failures = 0
    for rel, pattern, advice in SHADOW_BANS:
        path = ROOT / rel
        if not path.exists():
            continue
        if re.search(pattern, path.read_text(encoding="utf-8")):
            print(f"FAIL  {rel} defines its own copy of a gated constant.", file=sys.stderr)
            print(f"      {advice}", file=sys.stderr)
            failures += 1
    return failures


def main() -> int:
    verbose = "--verbose" in sys.argv or "-v" in sys.argv

    failures = check_no_shadow_copies()

    for family, sources in FAMILIES.items():
        versions = [
            (label, extract_version(label, ROOT / rel, pattern))
            for label, rel, pattern in sources
        ]
        if verbose:
            for label, v in versions:
                print(f"  {family}: {label} = {v}")

        values = {v for _, v in versions}
        if len(values) == 1:
            ver = next(iter(values))
            n = len(versions)
            agree = "1 definition" if n == 1 else f"all {n} files agree"
            print(f"OK  {family} = {ver}  ({agree})")
            continue

        failures += 1
        print(f"FAIL  {family} mismatch across files:", file=sys.stderr)
        for label, v in versions:
            print(f"  {v}  {label}", file=sys.stderr)

    if failures:
        print(
            "\nFix: update every listed file to the same integer before committing.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
