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
P4_VERSION_H = ROOT / "Firmware" / "DAQ_HAT" / "ESP32P4" / "include" / "version.h"
C6_VERSION_H = ROOT / "Firmware" / "DAQ_HAT" / "ESP32C6" / "include" / "version.h"


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
    # Match: set(PROBE_VERSION "bb-hat-X.Y<suffix>")
    match = re.search(r'set\s*\(\s*PROBE_VERSION\s+"bb-hat-([0-9]+)\.([0-9]+)([^"]*)"\s*\)', cmake_text)
    if match is None:
        # Legacy fallback: file(WRITE ...) embedded the version string
        match = re.search(r'PROBE_VERSION\s+\\"bb-hat-([^"]+)\\"', cmake_text)
        if match is None:
            raise RuntimeError(f'Could not read PROBE_VERSION from {RP2040_CMAKE}')
        return match.group(1)
    cmake_major, cmake_minor, suffix = match.group(1), match.group(2), match.group(3)
    version = f"{cmake_major}.{cmake_minor}{suffix}"

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


def version_h_version(path: Path) -> str:
    """Parse MAJOR.MINOR.PATCH out of a DAQ HAT `version.h`.

    Both DAQ HAT chips carry the same header shape (see the P4's for the
    canonical comment). The C6's is CI-only: DDP carries no C6 build ID, so
    nothing reports this value over the wire.
    """
    text = read_text(path)

    def macro(name: str) -> str:
        match = re.search(rf"(?m)^#define\s+{name}\s+(\d+)\s*$", text)
        if match is None:
            raise RuntimeError(f"Could not read {name} from {path}")
        return match.group(1)

    return ".".join(
        [
            macro("FW_VERSION_MAJOR"),
            macro("FW_VERSION_MINOR"),
            macro("FW_VERSION_PATCH"),
        ]
    )


READERS = {
    "esp32": esp32_version,
    "rp2040": rp2040_version,
    "p4": lambda: version_h_version(P4_VERSION_H),
    "c6": lambda: version_h_version(C6_VERSION_H),
}


def main() -> int:
    parser = argparse.ArgumentParser(description="Read BugBuster firmware versions from source.")
    parser.add_argument("target", choices=sorted(READERS))
    parser.add_argument("--expect", help="Fail if the parsed version does not match this value.")
    args = parser.parse_args()

    version = READERS[args.target]()
    if args.expect is not None and version != args.expect:
        print(f"{args.target} version is {version}, expected {args.expect}", file=sys.stderr)
        return 1

    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
