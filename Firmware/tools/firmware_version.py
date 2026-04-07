#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ESP32_BBP = ROOT / "Firmware" / "esp32_ad74416h" / "src" / "bbp.h"
RP2040_CMAKE = ROOT / "Firmware" / "RP2040" / "CMakeLists.txt"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def esp32_version() -> str:
    text = read_text(ESP32_BBP)

    def macro(name: str) -> str:
        match = re.search(rf"(?m)^#define\s+{name}\s+(\d+)\s*$", text)
        if match is None:
            raise RuntimeError(f"Could not read {name} from {ESP32_BBP}")
        return match.group(1)

    return ".".join(
        [
            macro("BBP_FW_VERSION_MAJOR"),
            macro("BBP_FW_VERSION_MINOR"),
            macro("BBP_FW_VERSION_PATCH"),
        ]
    )


def rp2040_version() -> str:
    text = read_text(RP2040_CMAKE)
    match = re.search(r'PROBE_VERSION\s+\\"bb-hat-([^"]+)\\"', text)
    if match is None:
        raise RuntimeError(f'Could not read PROBE_VERSION from {RP2040_CMAKE}')
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description="Read BugBuster firmware versions from source.")
    parser.add_argument("target", choices=["esp32", "rp2040"])
    parser.add_argument("--expect", help="Fail if the parsed version does not match this value.")
    args = parser.parse_args()

    version = esp32_version() if args.target == "esp32" else rp2040_version()
    if args.expect is not None and version != args.expect:
        print(f"{args.target} version is {version}, expected {args.expect}", file=sys.stderr)
        return 1

    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
