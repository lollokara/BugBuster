#!/usr/bin/env python3
"""Fail if a key in sdkconfig.defaults did not make it into the generated sdkconfig.

PlatformIO does NOT merge newly added `sdkconfig.defaults` keys into an
already-generated `sdkconfig`. The key is simply ignored, silently, and the
build ships the old value.

That trap has already cost this project once: `sdkconfig.defaults` asked for
`CONFIG_TINYUSB_VENDOR_TX_BUFSIZE=32768`, the build shipped 8192, and because
`WAVE_I` frames are 16038 bytes the DAQ USB waveform stream dropped 100 percent
of frames for an unknown length of time. Nothing failed - a connected host just
saw only the 10 Hz summary frames.

Usage:
    python Firmware/tools/check_sdkconfig_effective.py
    python Firmware/tools/check_sdkconfig_effective.py --verbose

A project whose sdkconfig has not been generated yet is skipped, because a
clean checkout has not built anything. Everything that HAS been generated is
checked.

If this fails, delete the generated sdkconfig for that environment and rebuild:
    rm Firmware/DAQ_HAT/ESP32P4/sdkconfig.esp32p4 && pio run -e esp32p4
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

PROJECTS = [
    "Firmware/ESP32",
    "Firmware/DAQ_HAT/ESP32P4",
    "Firmware/DAQ_HAT/ESP32C6",
]

SET_RE = re.compile(r"(?m)^(CONFIG_[A-Z0-9_]+)=(.*)$")
UNSET_RE = re.compile(r"(?m)^#\s*(CONFIG_[A-Z0-9_]+)\s+is not set\s*$")

# (project, key) -> why this key is allowed to be ineffective.
# A waiver needs a reason. "It was already failing" is not one - if a key does
# nothing, the honest options are to waive it here or delete it from defaults.
WAIVERS: dict[tuple[str, str], str] = {
    (
        "Firmware/DAQ_HAT/ESP32C6",
        "CONFIG_PARTITION_TABLE_CUSTOM",
    ): (
        "PlatformIO owns the partition table for this project via "
        "board_build.partitions (partitions_c6.csv for C6, partitions.csv for "
        "C3) and generates partitions.bin directly, bypassing Kconfig. The "
        "generated sdkconfig therefore keeps SINGLE_APP and it does not matter."
    ),
}


def parse(text: str) -> dict[str, str]:
    """Map every CONFIG_ key to its value. An unset key maps to the sentinel 'n'."""
    values = {key: value.strip() for key, value in SET_RE.findall(text)}
    for key in UNSET_RE.findall(text):
        values.setdefault(key, "n")
    return values


def normalise(value: str) -> str:
    """Kconfig writes 'n' as a comment and 'y' literally; ints may be hex or dec."""
    value = value.strip()
    if value in ("", "n"):
        return "n"
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    try:
        return str(int(value, 0))
    except ValueError:
        return value


def main() -> int:
    verbose = "--verbose" in sys.argv or "-v" in sys.argv
    failures: list[str] = []
    unknown: list[str] = []
    checked_projects = 0

    for rel in PROJECTS:
        project = ROOT / rel
        defaults_path = project / "sdkconfig.defaults"
        if not defaults_path.exists():
            continue

        generated = sorted(
            p for p in project.glob("sdkconfig.*") if p.name != "sdkconfig.defaults"
        )
        if not generated:
            if verbose:
                print(f"  {rel}: no generated sdkconfig yet, skipping")
            continue

        wanted = parse(defaults_path.read_text(encoding="utf-8", errors="ignore"))

        # A key that no generated config for this project recognises is almost
        # always a typo or a symbol renamed by an IDF upgrade. It is not an
        # effectiveness failure (nothing claims it should apply), but it is dead
        # weight that silently does nothing - CONFIG_SPIRAM_ALLOW_BSS_SEG sat in
        # the S3 defaults like this while the real symbol was
        # CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY.
        known: set[str] = set()
        for gen in generated:
            known |= set(parse(gen.read_text(encoding="utf-8", errors="ignore")))
        for key in sorted(set(wanted) - known):
            unknown.append(f"{rel}/sdkconfig.defaults: {key} is not a symbol any generated config defines")

        for gen in generated:
            checked_projects += 1
            actual = parse(gen.read_text(encoding="utf-8", errors="ignore"))
            mismatches = 0
            waived = 0
            for key, want in wanted.items():
                # A key the target does not define at all (a C6 key in a P4
                # build, say) is not drift - Kconfig never offered it.
                if key not in actual:
                    continue
                if normalise(actual[key]) != normalise(want):
                    if (rel, key) in WAIVERS:
                        waived += 1
                        if verbose:
                            print(f"    waived {key}: {WAIVERS[(rel, key)]}")
                        continue
                    mismatches += 1
                    failures.append(
                        f"{rel}/{gen.name}: {key} wanted {want!r}, effective {actual[key]!r}"
                    )
            if verbose:
                status = "OK" if mismatches == 0 else f"{mismatches} mismatch(es)"
                extra = f", {waived} waived" if waived else ""
                print(f"  {rel}/{gen.name}: {len(wanted)} default key(s) -> {status}{extra}")

    if failures:
        print("FAIL  sdkconfig.defaults keys that never took effect:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print(
            "\nPlatformIO does not merge new defaults into an existing sdkconfig.\n"
            "Delete the generated sdkconfig for that environment and rebuild.",
            file=sys.stderr,
        )
        return 1

    for u in unknown:
        print(f"WARN  {u}")
    print(f"OK  every sdkconfig.defaults key is effective in {checked_projects} generated config(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
