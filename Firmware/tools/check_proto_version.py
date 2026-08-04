#!/usr/bin/env python3
"""Check that BBP_PROTO_VERSION is in sync across all three source files.

Usage:
    python Firmware/tools/check_proto_version.py
    python Firmware/tools/check_proto_version.py --verbose

Exit 0 if all three files agree; exit 1 with a diff summary if any differ.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (label, file path, regex pattern)
SOURCES: list[tuple[str, Path, str]] = [
    (
        "Firmware/ESP32/src/bbp/bbp.h",
        ROOT / "Firmware" / "ESP32" / "src" / "bbp" / "bbp.h",
        r"(?m)^#define\s+BBP_PROTO_VERSION\s+(\d+)",
    ),
    (
        "python/bugbuster/protocol.py",
        ROOT / "python" / "bugbuster" / "protocol.py",
        r"(?m)^BBP_PROTO_VERSION\s*=\s*(\d+)",
    ),
    (
        "DesktopApp/BugBuster/src-tauri/src/bbp.rs",
        ROOT / "DesktopApp" / "BugBuster" / "src-tauri" / "src" / "bbp.rs",
        r"(?m)^pub\s+const\s+PROTO_VERSION\s*:\s*u8\s*=\s*(\d+)\s*;",
    ),
]


def extract_version(label: str, path: Path, pattern: str) -> int:
    if not path.exists():
        print(f"ERROR: file not found: {path}", file=sys.stderr)
        sys.exit(2)
    text = path.read_text(encoding="utf-8")
    match = re.search(pattern, text)
    if match is None:
        print(f"ERROR: could not find PROTO_VERSION constant in {label}", file=sys.stderr)
        sys.exit(2)
    return int(match.group(1))


def check_no_shadow_copies() -> int:
    """Fail if a second BBP_PROTO_VERSION literal reappears outside SOURCES.

    python/bugbuster/constants.py used to carry its own copy. Nothing imported
    it, so it drifted to 9 unnoticed while the three checked files moved to 10
    -- this gate never saw it. A shadow copy that disagrees with the wire is a
    latent regression waiting for its first importer.
    See docs/superpowers/reviews/2026-08-03-design-sweep.md finding S2-14.
    """
    shadow = ROOT / "python" / "bugbuster" / "constants.py"
    if not shadow.exists():
        return 0
    text = shadow.read_text(encoding="utf-8")
    if re.search(r"(?m)^BBP_PROTO_VERSION\s*=\s*\d+", text):
        print(
            "FAIL  python/bugbuster/constants.py defines its own "
            "BBP_PROTO_VERSION literal.",
            file=sys.stderr,
        )
        print(
            "      The canonical copy is python/bugbuster/protocol.py; import "
            "it from there.",
            file=sys.stderr,
        )
        return 1
    return 0


def main() -> int:
    verbose = "--verbose" in sys.argv or "-v" in sys.argv

    if check_no_shadow_copies() != 0:
        return 1

    versions: list[tuple[str, int]] = []
    for label, path, pattern in SOURCES:
        v = extract_version(label, path, pattern)
        versions.append((label, v))
        if verbose:
            print(f"  {label}: {v}")

    values = {v for _, v in versions}
    if len(values) == 1:
        ver = next(iter(values))
        print(f"OK  BBP_PROTO_VERSION = {ver}  (all 3 files agree)")
        return 0

    print("FAIL  BBP_PROTO_VERSION mismatch across files:", file=sys.stderr)
    for label, v in versions:
        print(f"  {v}  {label}", file=sys.stderr)
    print(
        "\nFix: update all three files to the same integer before committing.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
