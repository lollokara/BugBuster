#!/usr/bin/env python3
"""Generate the BBP error, command and event tables for every host surface from bbp.h (FEAT-6).

bbp.h is the single source: each ``#define BBP_ERR_<NAME> 0xNN  // message``
yields one error entry, each ``#define BBP_CMD_<NAME> 0xNN`` one command ID and
each ``#define BBP_EVT_<NAME> 0xNN`` one event ID. The marked blocks rewritten:

  python/bugbuster/constants.py          ErrorCode members, CmdId command members
  python/bugbuster_mcp/error_mapping.py  ERROR_MESSAGES dict
  DesktopApp/BugBuster/src-tauri/src/bbp.rs  ERR_* consts + error_to_string arms,
                                             CMD_* consts, EVT_* consts

Adding a BBP command: add the #define to bbp.h, then run this script.

Usage:
  python Firmware/tools/gen_error_tables.py           # rewrite the blocks
  python Firmware/tools/gen_error_tables.py --check   # CI: fail if stale
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BBP_H = ROOT / "Firmware" / "ESP32" / "src" / "bbp" / "bbp.h"
PY_CONST = ROOT / "python" / "bugbuster" / "constants.py"
PY_MCP = ROOT / "python" / "bugbuster_mcp" / "error_mapping.py"
RUST = ROOT / "DesktopApp" / "BugBuster" / "src-tauri" / "src" / "bbp.rs"

# Firmware abbreviations that Python spells out (wire value is identical).
PY_ALIASES = {"INVALID_CH": "INVALID_CHANNEL"}

# Python spellings that predate the firmware names (wire value is identical).
PY_CMD_ALIASES = {
    "SET_CH_FUNC": "SET_CHANNEL_FUNC", "SET_ILIMIT": "SET_CURRENT_LIMIT",
    "SET_AVDD_SEL": "SET_AVDD_SELECT", "CLEAR_CH_ALERT": "CLEAR_CHAN_ALERT",
    "REG_READ": "REGISTER_READ", "REG_WRITE": "REGISTER_WRITE",
    "HAT_SET_IO_VOLTAGE": "HAT_SET_IO_VOLT",
}


def parse_ids(kind: str) -> list[tuple[str, int, str]]:
    """Every ``#define BBP_<kind>_<NAME> 0xNN [// note]`` in header order."""
    text = BBP_H.read_text(encoding="utf-8")
    rows = [(n, int(v, 16), (m or "").strip()) for n, v, m in re.findall(
        rf"^#define\s+BBP_{kind}_(\w+)\s+0x([0-9A-Fa-f]{{2}})\b[ \t]*(?://[ \t]*(.*))?$", text, re.M)]
    if not rows:
        raise SystemExit(f"bbp.h: no BBP_{kind}_* defines found")
    values = [v for _, v, _ in rows]
    dup = sorted({f"0x{v:02X}" for v in values if values.count(v) > 1})
    if dup:
        raise SystemExit(f"bbp.h: BBP_{kind}_* values used twice: {', '.join(dup)}")
    return rows


def py_cmds(rows) -> list[str]:
    w = max(len(PY_CMD_ALIASES.get(n, n)) for n, _, _ in rows)
    return [f"    {PY_CMD_ALIASES.get(n, n):<{w}} = 0x{v:02X}" + (f"  # {m}" if m else "")
            for n, v, m in rows]


def rust_ids(prefix: str, rows) -> list[str]:
    return [f"pub const {prefix}_{n}: u8 = 0x{v:02X};" + (f" // {m}" if m else "") for n, v, m in rows]


def parse() -> list[tuple[str, int, str]]:
    text = BBP_H.read_text(encoding="utf-8")
    rows = []
    for name, val, msg in re.findall(
            r"^#define\s+BBP_ERR_(\w+)\s+0x([0-9A-Fa-f]+)[ \t]*(?://[ \t]*(.*))?$", text, re.M):
        if not msg.strip():
            raise SystemExit(f"bbp.h: BBP_ERR_{name} has no // message comment")
        rows.append((name, int(val, 16), msg.strip()))
    if not rows:
        raise SystemExit("bbp.h: no BBP_ERR_* defines found")
    return rows


def py_enum(rows) -> list[str]:
    w = max(len(PY_ALIASES.get(n, n)) for n, _, _ in rows)
    return [f"    {PY_ALIASES.get(n, n):<{w}} = 0x{v:02X}  # {m}" for n, v, m in rows]


def py_mcp(rows) -> list[str]:
    out = []
    for n, v, m in rows:
        key = PY_ALIASES.get(n, n).lower()
        msg = m.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'    0x{v:02X}: ("{key}", "{msg}"),')
    return out


def rust_consts(rows) -> list[str]:
    return [f"pub const ERR_{n}: u8 = 0x{v:02X};" for n, v, _ in rows]


def rust_arms(rows) -> list[str]:
    out = []
    for n, _, m in rows:
        msg = m.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'        ERR_{n} => "{msg}".to_string(),')
    return out


def splice(old: str, path: Path, tag: str, blocks: list[list[str]]) -> str:
    """Replace the Nth block marked `tag` in the text of `path` with blocks[N]."""
    BEGIN, END = f"BEGIN GENERATED: {tag}", f"END GENERATED: {tag}"
    nl = "\r\n" if "\r\n" in old else "\n"
    lines = old.split(nl)
    out, i, k = [], 0, 0
    while i < len(lines):
        out.append(lines[i])
        if BEGIN in lines[i]:
            if k >= len(blocks):
                raise SystemExit(f"{path}: more generated blocks than expected")
            out.extend(blocks[k])
            k += 1
            i += 1
            while i < len(lines) and END not in lines[i]:
                i += 1
            if i == len(lines):
                raise SystemExit(f"{path}: unterminated generated block")
            out.append(lines[i])
        i += 1
    if k != len(blocks):
        raise SystemExit(f"{path}: expected {len(blocks)} generated block(s), found {k}")
    return nl.join(out)


def main() -> int:
    check = "--check" in sys.argv
    rows, cmds, evts = parse(), parse_ids("CMD"), parse_ids("EVT")
    targets = [
        (PY_CONST, [("bbp-errors", [py_enum(rows)]), ("bbp-commands", [py_cmds(cmds)])]),
        (PY_MCP, [("bbp-errors", [py_mcp(rows)])]),
        (RUST, [("bbp-errors", [rust_consts(rows), rust_arms(rows)]),
                ("bbp-commands", [rust_ids("CMD", cmds)]),
                ("bbp-events", [rust_ids("EVT", evts)])]),
    ]
    stale = []
    for path, tagged in targets:
        old = new = path.read_bytes().decode("utf-8")   # keeps CRLF/LF as-is (py3.11-safe)
        for tag, blocks in tagged:
            new = splice(new, path, tag, blocks)
        if old != new:
            stale.append(path.relative_to(ROOT).as_posix())
            if not check:
                path.write_bytes(new.encode("utf-8"))
    if check and stale:
        print("FAIL  generated BBP tables are stale:")
        for s in stale:
            print("  " + s)
        print("Run: python Firmware/tools/gen_error_tables.py")
        return 1
    print(("OK  " if check else "wrote ") + f"{len(rows)} error codes, {len(cmds)} commands, {len(evts)} events"
          + ("" if check else f" ({len(stale)} file(s) changed)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
