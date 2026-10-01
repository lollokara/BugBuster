"""The P4's C6 flasher skips a fixed flash range so a C6 update never wipes NVS.

That range is hard-coded in c6_flasher.c; this pins it to the C6 partition
tables so moving a partition cannot silently re-introduce the NVS wipe.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FLASHER = ROOT / "Firmware/DAQ_HAT/ESP32P4/src/link/c6_flasher.c"
TABLES = [
    ROOT / "Firmware/DAQ_HAT/ESP32C6/partitions_c6.csv",
    ROOT / "Firmware/DAQ_HAT/ESP32C6/partitions.csv",
]


def _flasher_hole() -> tuple[int, int]:
    src = FLASHER.read_text(encoding="utf-8")
    start = re.search(r"#define\s+C6_KEEP_START\s+(0x[0-9A-Fa-f]+)", src)
    end = re.search(r"#define\s+C6_KEEP_END\s+(0x[0-9A-Fa-f]+)", src)
    assert start and end, "C6_KEEP_START/END not found in c6_flasher.c"
    return int(start.group(1), 16), int(end.group(1), 16)


def _partitions(path: Path) -> list[tuple[str, str, str, int, int]]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        name, ptype, subtype, offset, size = cols[:5]
        rows.append((name, ptype, subtype, int(offset, 0), int(size, 0)))
    return rows


def test_flasher_hole_covers_nvs_and_phy_and_stops_at_app() -> None:
    start, end = _flasher_hole()
    for table in TABLES:
        parts = _partitions(table)
        app = [p for p in parts if p[1] == "app"]
        assert app and min(p[3] for p in app) == end, f"{table.name}: app must start at C6_KEEP_END"
        for name, ptype, subtype, offset, size in parts:
            if ptype == "data" and subtype in ("nvs", "phy", "nvs_keys"):
                assert start <= offset and offset + size <= end, (
                    f"{table.name}: {name} 0x{offset:X}+0x{size:X} outside the flasher's keep range"
                )
