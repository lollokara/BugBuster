#!/usr/bin/env python3
"""Generate the BBP error tables for every host surface from bbp.h (FEAT-6).

bbp.h is the single source: each ``#define BBP_ERR_<NAME> 0xNN  // message``
yields one entry. Three marked blocks are rewritten:

  python/bugbuster/constants.py          ErrorCode enum members
  python/bugbuster_mcp/error_mapping.py  ERROR_MESSAGES dict
  DesktopApp/BugBuster/src-tauri/src/bbp.rs  ERR_* consts + error_to_string arms

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

BEGIN, END = "BEGIN GENERATED: bbp-errors", "END GENERATED: bbp-errors"


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


def splice(path: Path, blocks: list[list[str]]) -> tuple[str, str]:
    """Replace the Nth marked block in `path` with blocks[N]. Returns (old, new)."""
    old = path.read_bytes().decode("utf-8")   # keeps CRLF/LF as-is (py3.11-safe)
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
    return old, nl.join(out)


def main() -> int:
    check = "--check" in sys.argv
    rows = parse()
    targets = [
        (PY_CONST, [py_enum(rows)]),
        (PY_MCP, [py_mcp(rows)]),
        (RUST, [rust_consts(rows), rust_arms(rows)]),
    ]
    stale = []
    for path, blocks in targets:
        old, new = splice(path, blocks)
        if old != new:
            stale.append(path.relative_to(ROOT).as_posix())
            if not check:
                path.write_bytes(new.encode("utf-8"))
    if check and stale:
        print("FAIL  generated BBP error tables are stale:")
        for s in stale:
            print("  " + s)
        print("Run: python Firmware/tools/gen_error_tables.py")
        return 1
    print(("OK  " if check else "wrote ") + f"{len(rows)} error codes"
          + ("" if check else f" ({len(stale)} file(s) changed)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
