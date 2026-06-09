"""
Post-build memory size gate for BugBuster ESP32-S3 firmware.

Checks:
  1. Static DRAM (.data + .bss) stays below DRAM_STATIC_LIMIT_KB
  2. firmware.bin fits within the smaller OTA partition (app0 / app1)

Usage — standalone (after `pio run`):
    python Firmware/tools/check_memory.py \\
        Firmware/ESP32/.pio/build/esp32s3/firmware.elf \\
        Firmware/ESP32/partitions.csv

Usage — PlatformIO post-build (automatic on every `pio run`):
    Add to platformio.ini:
        extra_scripts = pre:scripts/pio_webfs_partition.py
                        post:../tools/check_memory.py
"""

from __future__ import annotations
import csv
import os
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Thresholds — edit here to tighten or relax limits
# ---------------------------------------------------------------------------
DRAM_STATIC_LIMIT_KB = 160   # .data + .bss — current baseline ~144 KB; fail if +16 KB regression
OTA_FILL_WARN_PCT    = 80    # print WARNING when binary reaches this % of partition
OTA_FILL_FAIL_PCT    = 90    # hard fail when binary reaches this % of partition

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _find_size_tool(cc_path: str | None) -> str | None:
    """Derive xtensa-esp-elf-size from the compiler path, or search PATH."""
    if cc_path:
        size = cc_path.replace("-gcc", "-size").replace("-g++", "-size")
        if Path(size).exists():
            return size
    for candidate in ("xtensa-esp-elf-size", "xtensa-esp32s3-elf-size"):
        try:
            subprocess.run([candidate, "--version"], capture_output=True, check=True)
            return candidate
        except (FileNotFoundError, subprocess.CalledProcessError):
            pass
    return None


def _parse_size_output(output: str) -> dict[str, int]:
    """Parse `size -A` output into {section_name: size_bytes}."""
    sections: dict[str, int] = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("."):
            try:
                sections[parts[0]] = int(parts[1])
            except ValueError:
                pass
    return sections


def _ota_partition_size(partitions_csv: Path) -> int | None:
    """Return the size in bytes of the smaller OTA partition (app0/app1)."""
    sizes = []
    try:
        with partitions_csv.open(newline="") as f:
            for row in csv.reader(f):
                row = [c.strip() for c in row]
                if not row or row[0].startswith("#"):
                    continue
                if len(row) >= 5 and row[1] == "app":
                    try:
                        sizes.append(int(row[4], 0))
                    except ValueError:
                        pass
    except FileNotFoundError:
        return None
    return min(sizes) if sizes else None


# ---------------------------------------------------------------------------
# Core check
# ---------------------------------------------------------------------------

def run_check(elf: Path, partitions_csv: Path, size_tool: str | None) -> bool:
    ok = True
    print("\n" + "=" * 60)
    print("BugBuster ESP32 Memory Check")
    print("=" * 60)

    # --- Static DRAM ---
    if size_tool and elf.exists():
        result = subprocess.run(
            [size_tool, "-A", str(elf)],
            capture_output=True, text=True
        )
        if result.returncode == 0:
            sections = _parse_size_output(result.stdout)
            data_b = sections.get(".dram0.data", sections.get(".data", 0))
            bss_b  = sections.get(".dram0.bss",  sections.get(".bss",  0))
            iram_b = sections.get(".iram0.text",  sections.get(".iram", 0))
            text_b = sections.get(".flash.text",  sections.get(".text", 0))
            rodata_b = sections.get(".flash.rodata", sections.get(".rodata", 0))
            static_kb = (data_b + bss_b) / 1024
            limit_kb  = DRAM_STATIC_LIMIT_KB
            dram_ok   = static_kb <= limit_kb
            status    = "OK" if dram_ok else "FAIL"
            print(f"\nStatic DRAM (.data + .bss):  {static_kb:6.1f} KB  / limit {limit_kb} KB  [{status}]")
            print(f"  .dram0.data   {data_b / 1024:6.1f} KB")
            print(f"  .dram0.bss    {bss_b  / 1024:6.1f} KB")
            print(f"  .iram0.text   {iram_b / 1024:6.1f} KB  (IRAM — fast execute)")
            print(f"  .flash.text   {text_b / 1024:6.1f} KB  (flash code)")
            print(f"  .flash.rodata {rodata_b/1024:6.1f} KB  (flash const data)")
            if not dram_ok:
                print(f"  *** STATIC DRAM EXCEEDS {limit_kb} KB LIMIT — reduce global/static buffers ***")
                ok = False
        else:
            print(f"\nStatic DRAM: could not run {size_tool} ({result.stderr.strip()})")
    else:
        print("\nStatic DRAM: skipped (size tool not found or ELF missing)")

    # --- Estimated flash binary size vs OTA partition ---
    # The firmware .bin is produced by esptool after the ELF is linked, so it
    # may not exist when this post-build hook fires.  Estimate the binary size
    # from the flash ELF sections — this is accurate to within a few KB of the
    # real .bin (esptool adds a small header and padding).
    ota_bytes = _ota_partition_size(partitions_csv)
    if size_tool and elf.exists() and ota_bytes:
        try:
            sections  # already populated above
        except NameError:
            result2 = subprocess.run([size_tool, "-A", str(elf)],
                                     capture_output=True, text=True)
            sections = _parse_size_output(result2.stdout) if result2.returncode == 0 else {}

        flash_text_b   = sections.get(".flash.text",   0)
        flash_rodata_b = sections.get(".flash.rodata",  0)
        flash_desc_b   = sections.get(".flash.appdesc", 0)
        # iram text is also copied into flash at boot
        iram_text_b    = sections.get(".iram0.text",    sections.get(".iram", 0))
        dram_data_b    = sections.get(".dram0.data",    sections.get(".data", 0))
        estimated_b    = flash_text_b + flash_rodata_b + flash_desc_b + iram_text_b + dram_data_b
        ota_kb  = ota_bytes / 1024
        est_kb  = estimated_b / 1024
        pct     = estimated_b / ota_bytes * 100
        if pct >= OTA_FILL_FAIL_PCT:
            status = "FAIL"
            ok = False
        elif pct >= OTA_FILL_WARN_PCT:
            status = "WARN"
        else:
            status = "OK"
        print(f"\nEstimated flash size:  {est_kb:7.1f} KB  / OTA partition {ota_kb:.0f} KB  ({pct:.1f}%)  [{status}]")
        print(f"  (flash.text {flash_text_b/1024:.0f} KB + flash.rodata {flash_rodata_b/1024:.0f} KB"
              f" + iram.text {iram_text_b/1024:.0f} KB + dram.data {dram_data_b/1024:.0f} KB + misc)")
        if pct >= OTA_FILL_FAIL_PCT:
            print(f"  *** ESTIMATED BINARY EXCEEDS {OTA_FILL_FAIL_PCT}% OF OTA PARTITION ***")
        elif pct >= OTA_FILL_WARN_PCT:
            print(f"  Warning: binary nearing {OTA_FILL_WARN_PCT}% of partition")
    elif not ota_bytes:
        print("\nFlash size check: skipped (partitions.csv not found)")

    print("\n" + ("PASS — all memory checks OK" if ok else "FAIL — see above") )
    print("=" * 60 + "\n")
    return ok


# ---------------------------------------------------------------------------
# PlatformIO integration (SCons post-build action)
# ---------------------------------------------------------------------------
try:
    Import("env")           # type: ignore[name-defined]  # SCons global

    def _pio_action(source, target, env):           # noqa: ANN001
        elf  = Path(str(target[0]))
        root = Path(env.subst("$PROJECT_DIR"))
        csv_ = root / "partitions.csv"
        cc   = env.get("CC", "")
        tool = _find_size_tool(cc)
        if not run_check(elf, csv_, tool):
            import SCons.Errors
            raise SCons.Errors.BuildError(
                errstr="Memory check failed — see output above"
            )

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _pio_action)  # type: ignore[name-defined]

except Exception:
    # Not running inside SCons — standalone mode below
    pass


# ---------------------------------------------------------------------------
# Standalone entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    if len(sys.argv) < 2:
        # Try well-known default paths from repo root
        repo = Path(__file__).parent.parent
        elf  = repo / "ESP32/.pio/build/esp32s3/firmware.elf"
        csv_ = repo / "ESP32/partitions.csv"
    else:
        elf  = Path(sys.argv[1])
        csv_ = Path(sys.argv[2]) if len(sys.argv) > 2 else elf.parent.parent.parent / "partitions.csv"

    tool = _find_size_tool(None)
    if not tool:
        print("Warning: xtensa-esp-elf-size not found on PATH — DRAM check skipped")
        print("Install via: the ESP-IDF toolchain or PlatformIO will include it automatically")

    sys.exit(0 if run_check(elf, csv_, tool) else 1)
