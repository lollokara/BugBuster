#!/usr/bin/env python3
"""Fail if a tracked doc states a count that disagrees with the source.

Every surface audited on 2026-08-20 had count drift, and the 2026-08-07
documentation reset had already corrected all of it - it came back within two
weeks because nothing gated it. This script derives each number from source and
compares it against every doc that states it.

A derivation lives here and nowhere else. If you need a count, run:

    python Firmware/tools/check_doc_counts.py --print

Adding a claim to a doc? Add it to CLAIMS below, or the gate will not see it.
Removing one? Remove it here too - a claim that no longer matches its file is a
hard failure, because a silently skipped claim is how drift returns.

One exception, and it is narrow: if the claim's whole top-level directory is
absent from the checkout, the claim is skipped and the skip is printed. `.mex/`
is gitignored, so a developer always has it and CI never does. A file missing
from a tree that *does* exist is still a hard failure.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="ignore")


def derive() -> dict[str, int]:
    counts: dict[str, int] = {}

    tool_dir = ROOT / "python/bugbuster_mcp/tools"
    tool_files = sorted(p for p in tool_dir.glob("*.py") if p.name != "__init__.py")
    # waveform.py registers via mcp.tool()(fn) rather than the decorator, so a
    # plain "@mcp.tool" grep under-counts by 10.
    counts["mcp_tools"] = sum(
        len(re.findall(r"@mcp\.tool|mcp\.tool\(\)\(", p.read_text(encoding="utf-8")))
        for p in tool_files
    )
    counts["mcp_groups"] = len(tool_files)

    bbp_h = _read("Firmware/ESP32/src/bbp/bbp.h")
    counts["bbp_commands"] = len(re.findall(r"(?m)^#define\s+BBP_CMD_\w+", bbp_h))
    counts["bbp_errors"] = len(re.findall(r"(?m)^#define\s+BBP_ERR_\w+", bbp_h))

    # Count tab TUPLES, not lines. `grep -c` was used for this once and reported
    # 20 because three tabs share a single line - that wrong number then shipped
    # into three READMEs.
    app_rs = _read("DesktopApp/BugBuster/src/app.rs")
    counts["desktop_tabs"] = len(re.findall(r'\("[a-z_0-9]+",\s*"', app_rs))

    counts["tauri_commands"] = sum(
        len(re.findall(r"#\[tauri::command\]", p.read_text(encoding="utf-8", errors="ignore")))
        for p in (ROOT / "DesktopApp/BugBuster/src-tauri/src").rglob("*.rs")
    )

    regs = 0
    uris: set[str] = set()
    for p in (ROOT / "Firmware/ESP32/src").rglob("*.cpp"):
        text = p.read_text(encoding="utf-8", errors="ignore")
        regs += len(re.findall(r"httpd_register_uri_handler", text))
        uris |= set(re.findall(r'\.uri\s*=\s*"([^"]+)"', text))
    counts["http_registrations"] = regs
    counts["http_unique_uris"] = len(uris)
    counts["http_api_uris"] = len([u for u in uris if u.startswith("/api/")])

    return counts


# (doc path, regex, metric name per capturing group)
CLAIMS: list[tuple[str, str, list[str]]] = [
    (
        "README.md",
        r"MCP-(\d+)%20tools%20%C2%B7%20(\d+)%20groups",
        ["mcp_tools", "mcp_groups"],
    ),
    ("README.md", r"exposes \*\*(\d+) tools\*\*", ["mcp_tools"]),
    ("README.md", r"Leptos 0\.7, (\d+) tabs", ["desktop_tabs"]),
    ("README.md", r"Leptos 0\.7 \((\d+) tabs\)", ["desktop_tabs"]),
    ("README.md", r"MCP server \((\d+) tools, (\d+) groups\)", ["mcp_tools", "mcp_groups"]),
    (
        "python/bugbuster_mcp/README.md",
        r"\*\*(\d+) tools in (\d+) groups",
        ["mcp_tools", "mcp_groups"],
    ),
    ("DesktopApp/BugBuster/README.md", r"(?m)^(\d+) tabs in five categories", ["desktop_tabs"]),
    (
        "DesktopApp/BugBuster/README.md",
        r"commands\.rs\s+(\d+) Tauri commands",
        ["tauri_commands"],
    ),
    ("Docs/simulated-device.md", r"(\d+) BBP commands are registered", ["bbp_commands"]),
    (
        "Docs/io-ownership.md",
        r"of \*\*(\d+)\*\* tools in (\d+) groups",
        ["mcp_tools", "mcp_groups"],
    ),
    (
        ".mex/context/python-mcp.md",
        r"(\d+) AI-callable tools, (\d+) groups",
        ["mcp_tools", "mcp_groups"],
    ),
    (
        ".mex/context/python-mcp.md",
        r"## MCP Tools \((\d+) tools, (\d+) groups\)",
        ["mcp_tools", "mcp_groups"],
    ),
    (
        ".mex/context/architecture.md",
        r"MCP server exposing (\d+) tools across (\d+) groups",
        ["mcp_tools", "mcp_groups"],
    ),
    (".mex/context/stack.md", r"\((\d+) tools via FastMCP\)", ["mcp_tools"]),
    (
        ".mex/manifests/mcp-server.manifest.md",
        r"as (\d+) AI-callable MCP tools across (\d+) modules",
        ["mcp_tools", "mcp_groups"],
    ),
    (
        ".mex/manifests/mcp-server.manifest.md",
        r"\*\*(\d+) AI-callable tools\*\* across",
        ["mcp_tools"],
    ),
    (".mex/manifests/mcp-server.manifest.md", r"### MCP tools \((\d+)\)", ["mcp_tools"]),
]


def main() -> int:
    counts = derive()

    if "--print" in sys.argv:
        width = max(len(k) for k in counts)
        for key, value in counts.items():
            print(f"{key:<{width}}  {value}")
        return 0

    failures: list[str] = []
    skipped: list[str] = []
    checked = 0
    sites = 0

    for rel, pattern, metrics in CLAIMS:
        path = ROOT / rel
        if not path.exists():
            # A whole tree that is absent from this checkout is not drift.
            # `.mex/` is gitignored, so CI never sees it while a developer
            # always does. Distinguish that from a single file that vanished,
            # which IS drift and must fail.
            root = ROOT / PurePosixPath(rel).parts[0]
            if not root.exists():
                skipped.append(rel)
                continue
            failures.append(f"{rel}: file not found (claim registered in CLAIMS)")
            continue
        sites += 1
        match = re.search(pattern, path.read_text(encoding="utf-8", errors="ignore"))
        if match is None:
            failures.append(
                f"{rel}: claim pattern no longer matches -> {pattern!r}\n"
                f"      Either the doc was reworded (update CLAIMS) or the claim "
                f"was dropped (remove it from CLAIMS)."
            )
            continue
        for idx, metric in enumerate(metrics, start=1):
            checked += 1
            stated = int(match.group(idx))
            actual = counts[metric]
            if stated != actual:
                failures.append(f"{rel}: states {metric} = {stated}, source says {actual}")

    if failures:
        print("FAIL  documentation counts disagree with source:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print(
            "\nDerive the real values with:\n"
            "  python Firmware/tools/check_doc_counts.py --print",
            file=sys.stderr,
        )
        return 1

    # Never let a skip be invisible - an unnoticed skip is how drift returns.
    for rel in sorted(set(skipped)):
        print(f"SKIP  {rel} is not present in this checkout (tree is untracked)")

    print(f"OK  {checked} documented count(s) match source across {sites} claim site(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
