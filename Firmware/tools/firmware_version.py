#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ESP32_BBP = ROOT / "Firmware" / "ESP32" / "src" / "bbp" / "bbp.h"
RP2040_CMAKE = ROOT / "Firmware" / "RP2040" / "CMakeLists.txt"
RP2040_MAIN = ROOT / "Firmware" / "RP2040" / "src" / "bb_main.c"


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
    cmake_text = read_text(RP2040_CMAKE)
    # Match: set(PROBE_VERSION "bb-hat-X.Y")
    match = re.search(r'set\s*\(\s*PROBE_VERSION\s+"bb-hat-([0-9]+)\.([0-9]+)"\s*\)', cmake_text)
    if match is None:
        # Legacy fallback: file(WRITE ...) embedded the version string
        match = re.search(r'PROBE_VERSION\s+\\"bb-hat-([^"]+)\\"', cmake_text)
        if match is None:
            raise RuntimeError(f'Could not read PROBE_VERSION from {RP2040_CMAKE}')
        return match.group(1)
    cmake_major, cmake_minor = match.group(1), match.group(2)
    version = f"{cmake_major}.{cmake_minor}"

    # Cross-check bb_main.c defines against CMakeLists.txt PROBE_VERSION.
    # Two forms are handled:
    #   1. Bare #define (non-guarded): must agree with CMake value.
    #   2. #ifndef-guarded block (#ifndef BB_HAT_FW_MAJOR / #define BB_HAT_FW_MAJOR <N>):
    #      the guarded value is a sentinel (0 = not injected by CMake); if a non-zero
    #      sentinel is present it must still agree with CMakeLists.txt.
    main_text = read_text(RP2040_MAIN)
    for name, cmake_val in (("BB_HAT_FW_MAJOR", cmake_major), ("BB_HAT_FW_MINOR", cmake_minor)):
        # Detect a bare (non-guarded) literal #define line
        bare_match = re.search(
            rf'(?m)^#define\s+{name}\s+([0-9]+)\s*$', main_text
        )
        if bare_match is not None:
            # Bare define found — confirm it matches CMakeLists
            if bare_match.group(1) != cmake_val:
                raise RuntimeError(
                    f"{RP2040_MAIN}: {name} is {bare_match.group(1)!r} "
                    f"but CMakeLists.txt says {cmake_val!r}. "
                    f"Edit PROBE_VERSION in CMakeLists.txt only and remove "
                    f"the bare #define from bb_main.c (use #ifndef guard instead)."
                )
        else:
            # Check for #ifndef-guarded block: #ifndef <NAME>\n#define <NAME> <N>
            guarded_match = re.search(
                rf'(?m)^#ifndef\s+{name}\s*\n#define\s+{name}\s+([0-9]+)', main_text
            )
            if guarded_match is not None:
                guarded_val = guarded_match.group(1)
                # Sentinel value 0 means CMake injection is expected at build time — skip.
                if guarded_val != "0" and guarded_val != cmake_val:
                    raise RuntimeError(
                        f"{RP2040_MAIN}: #ifndef-guarded {name} default is {guarded_val!r} "
                        f"but CMakeLists.txt PROBE_VERSION says {cmake_val!r}. "
                        f"Update the guarded default or PROBE_VERSION so they agree."
                    )

    return version


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
