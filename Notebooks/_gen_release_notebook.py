#!/usr/bin/env python3
"""One-shot generator — run this to (re)create release_version_bump.ipynb."""
import json
import uuid
from pathlib import Path


def cid():
    return uuid.uuid4().hex[:12]


def md(src: str) -> dict:
    return {
        "cell_type": "markdown",
        "id": cid(),
        "metadata": {},
        "source": src.splitlines(keepends=True),
    }


def code(src: str) -> dict:
    return {
        "cell_type": "code",
        "execution_count": None,
        "id": cid(),
        "metadata": {},
        "outputs": [],
        "source": src.splitlines(keepends=True),
    }


# ─────────────────────────────────────────────────────────────────────────────
#  Cell sources
# ─────────────────────────────────────────────────────────────────────────────

MD_HEADER = """\
# BugBuster — Release Version Bump

Bump one or more component versions, update the changelog, commit, and push release tags.

| Component | Tag format | CI workflow |
|---|---|---|
| ESP32 firmware | `esp-fw-vMAJOR.MINOR.PATCH` | `esp32-firmware.yml` |
| RP2040 HAT firmware | `hat-fw-vMAJOR.MINOR` | `rp2040-firmware.yml` |
| Desktop app | `desktop-vMAJOR.MINOR.PATCH` | `desktop-release.yml` |
| BBP wire protocol | *(no separate tag — sync only)* | `proto-version-check.yml` |

**Quick start**
1. Run **Setup** → **Current Versions**
2. Fill in the **Configure** cell
3. Run **Preview** to confirm
4. Set `DRY_RUN = False`, then run **Apply → Validate → Changelog → Commit → Tag & Push**
"""

CODE_SETUP = """\
import re
import sys
import json
import subprocess
from pathlib import Path
from datetime import date

# Walk up from wherever the notebook lives until we find the repo root
ROOT = Path.cwd()
while not (ROOT / "Firmware").exists() and ROOT.parent != ROOT:
    ROOT = ROOT.parent

assert (ROOT / "Firmware").exists(), (
    "Could not find repo root — open this notebook from within the BugBuster repo"
)
print(f"Repo root: {ROOT}")
"""

CODE_VERSIONS = """\
def _macro(text: str, name: str) -> str:
    m = re.search(rf'(?m)^#define\\s+{re.escape(name)}\\s+(\\d+)\\s*$', text)
    return m.group(1) if m else "?"

def read_esp32_version() -> str:
    t = (ROOT / "Firmware/ESP32/src/bbp/bbp.h").read_text()
    return ".".join([
        _macro(t, "BBP_FW_VERSION_MAJOR"),
        _macro(t, "BBP_FW_VERSION_MINOR"),
        _macro(t, "BBP_FW_VERSION_PATCH"),
    ])

def read_rp2040_version() -> str:
    t = (ROOT / "Firmware/RP2040/CMakeLists.txt").read_text()
    m = re.search(r'set\\s*\\(\\s*PROBE_VERSION\\s+"bb-hat-([0-9]+\\.[0-9]+)"\\s*\\)', t)
    return m.group(1) if m else "?"

def read_desktop_version() -> str:
    r = subprocess.run(
        [sys.executable, "DesktopApp/BugBuster/scripts/desktop_version.py", "--check"],
        capture_output=True, text=True, cwd=ROOT,
    )
    return r.stdout.strip() or f"ERR: {r.stderr.strip()}"

def read_bbp_proto() -> str:
    t = (ROOT / "Firmware/ESP32/src/bbp/bbp.h").read_text()
    return _macro(t, "BBP_PROTO_VERSION")

esp32_ver   = read_esp32_version()
rp2040_ver  = read_rp2040_version()
desktop_ver = read_desktop_version()
bbp_proto   = read_bbp_proto()

W = 40
print("┌" + "─" * W + "┐")
print("│" + " BugBuster — Current Versions ".center(W) + "│")
print("├" + "─" * W + "┤")
print(f"│  ESP32 firmware    {esp32_ver:>18}  │")
print(f"│  RP2040 HAT fw     {rp2040_ver:>18}  │")
print(f"│  Desktop app       {desktop_ver:>18}  │")
print(f"│  BBP proto         {'v' + bbp_proto:>18}  │")
print("└" + "─" * W + "┘")
"""

MD_CONFIGURE = """\
## Configure

Edit the cell below, then run it.

- Set each component to `None` to skip it.
- `CHANGELOG_LABEL` becomes the section header in `CHANGELOG.MD`
  (e.g. `"1.4.0"`, `"ESP32 3.5.0"`, or `"ESP32 3.5.0 + Desktop 1.2.0"`).
- Leave `DRY_RUN = True` for a safe preview; flip to `False` when ready to write.
"""

CODE_CONFIGURE = """\
# ─── EDIT THIS CELL ────────────────────────────────────────────────────────
DRY_RUN = True           # ← flip to False when ready to write files & commit

# Version strings MUST be quoted.  Set to None to skip a component.
ESP32_NEW     = None     # e.g. "3.5.0"   (MAJOR.MINOR.PATCH)
RP2040_NEW    = None     # e.g. "3.3"     (MAJOR.MINOR only)
DESKTOP_NEW   = None     # e.g. "1.2.0"   (MAJOR.MINOR.PATCH)
BBP_PROTO_NEW = None     # e.g. 9         (integer — syncs all 4 protocol files; no tag)

# Label used in the CHANGELOG section header — set to "" to skip changelog
CHANGELOG_LABEL = ""    # e.g. "1.4.0" or "ESP32 3.5.0"
# ─── END EDIT ──────────────────────────────────────────────────────────────

# Type guards — catch bare floats like 3.4.1 before they confuse later cells
for _name, _val in [
    ("ESP32_NEW", ESP32_NEW),
    ("RP2040_NEW", RP2040_NEW),
    ("DESKTOP_NEW", DESKTOP_NEW),
]:
    if _val is not None and not isinstance(_val, str):
        raise TypeError(
            f"{_name} must be a quoted string — e.g. {_name} = \\"3.4.1\\"\\n"
            f"Got: {_val!r}  ({type(_val).__name__})"
        )
if BBP_PROTO_NEW is not None and not isinstance(BBP_PROTO_NEW, int):
    raise TypeError(
        f"BBP_PROTO_NEW must be an integer — e.g. BBP_PROTO_NEW = 9\\n"
        f"Got: {BBP_PROTO_NEW!r}  ({type(BBP_PROTO_NEW).__name__})"
    )

print(f"DRY_RUN        = {DRY_RUN}")
print(f"ESP32_NEW      = {ESP32_NEW!r}")
print(f"RP2040_NEW     = {RP2040_NEW!r}")
print(f"DESKTOP_NEW    = {DESKTOP_NEW!r}")
print(f"BBP_PROTO_NEW  = {BBP_PROTO_NEW!r}")
print(f"CHANGELOG_LABEL= {CHANGELOG_LABEL!r}")
"""

CODE_PREVIEW = """\
errors: list[str] = []
changes: list[str] = []

if ESP32_NEW is not None:
    if not re.fullmatch(r'\\d+\\.\\d+\\.\\d+', ESP32_NEW):
        errors.append(f"ESP32_NEW must be MAJOR.MINOR.PATCH, got {ESP32_NEW!r}")
    elif ESP32_NEW == esp32_ver:
        errors.append(f"ESP32_NEW {ESP32_NEW!r} matches the current version — no change")
    else:
        changes.append(f"ESP32 firmware    {esp32_ver}  →  {ESP32_NEW}")

if RP2040_NEW is not None:
    if not re.fullmatch(r'\\d+\\.\\d+', RP2040_NEW):
        errors.append(f"RP2040_NEW must be MAJOR.MINOR, got {RP2040_NEW!r}")
    elif RP2040_NEW == rp2040_ver:
        errors.append(f"RP2040_NEW {RP2040_NEW!r} matches the current version — no change")
    else:
        changes.append(f"RP2040 HAT fw     {rp2040_ver}  →  {RP2040_NEW}")

if DESKTOP_NEW is not None:
    if not re.fullmatch(r'\\d+\\.\\d+\\.\\d+', DESKTOP_NEW):
        errors.append(f"DESKTOP_NEW must be MAJOR.MINOR.PATCH, got {DESKTOP_NEW!r}")
    elif DESKTOP_NEW == desktop_ver:
        errors.append(f"DESKTOP_NEW {DESKTOP_NEW!r} matches the current version — no change")
    else:
        changes.append(f"Desktop app       {desktop_ver}  →  {DESKTOP_NEW}")

if BBP_PROTO_NEW is not None:
    if not isinstance(BBP_PROTO_NEW, int):
        errors.append(f"BBP_PROTO_NEW must be an integer, got {BBP_PROTO_NEW!r}")
    elif str(BBP_PROTO_NEW) == bbp_proto:
        errors.append(f"BBP_PROTO_NEW {BBP_PROTO_NEW} matches current proto version — no change")
    else:
        changes.append(f"BBP proto         v{bbp_proto}  →  v{BBP_PROTO_NEW}  (4 files synced)")

if CHANGELOG_LABEL:
    today_str = date.today().strftime("%Y-%m-%d")
    changes.append(f"CHANGELOG.MD      [Unreleased]  →  [{CHANGELOG_LABEL}] — {today_str}")

if not changes and not errors:
    errors.append("Nothing to do — set at least one of ESP32_NEW / RP2040_NEW / DESKTOP_NEW / BBP_PROTO_NEW")

if errors:
    print("ERRORS:")
    for e in errors:
        print(f"  ✖  {e}")
else:
    print("Planned changes:")
    for c in changes:
        print(f"  •  {c}")
    print()
    if DRY_RUN:
        print("  [DRY_RUN=True — no files will be written until you flip the flag]")
    else:
        print("  [DRY_RUN=False — files WILL be written on Apply]")
"""

CODE_APPLY = """\
assert not errors, "Fix errors in Configure/Preview before applying"

def _apply_esp32(new_ver: str) -> None:
    path = ROOT / "Firmware/ESP32/src/bbp/bbp.h"
    text = path.read_text()
    major, minor, patch = new_ver.split(".")
    text = re.sub(r'(?m)^(#define\\s+BBP_FW_VERSION_MAJOR\\s+)\\d+', rf'\\g<1>{major}', text)
    text = re.sub(r'(?m)^(#define\\s+BBP_FW_VERSION_MINOR\\s+)\\d+', rf'\\g<1>{minor}', text)
    text = re.sub(r'(?m)^(#define\\s+BBP_FW_VERSION_PATCH\\s+)\\d+', rf'\\g<1>{patch}', text)
    if not DRY_RUN:
        path.write_text(text)
    tag = "[DRY]" if DRY_RUN else "[DONE]"
    print(f"  {tag}  bbp.h → ESP32 {new_ver}")


def _apply_rp2040(new_ver: str) -> None:
    major, minor = new_ver.split(".")

    cmake = ROOT / "Firmware/RP2040/CMakeLists.txt"
    text = cmake.read_text()
    text = re.sub(
        r'(set\\s*\\(\\s*PROBE_VERSION\\s+"bb-hat-)[0-9]+\\.[0-9]+(")',
        rf'\\g<1>{new_ver}\\g<2>', text,
    )
    if not DRY_RUN:
        cmake.write_text(text)
    tag = "[DRY]" if DRY_RUN else "[DONE]"
    print(f"  {tag}  CMakeLists.txt → PROBE_VERSION bb-hat-{new_ver}")

    bb_main = ROOT / "Firmware/RP2040/src/bb_main.c"
    text = bb_main.read_text()
    text = re.sub(
        r'(#ifndef BB_HAT_FW_MAJOR\\s*\\n#define BB_HAT_FW_MAJOR\\s+)\\d+',
        rf'\\g<1>{major}', text,
    )
    text = re.sub(
        r'(#ifndef BB_HAT_FW_MINOR\\s*\\n#define BB_HAT_FW_MINOR\\s+)\\d+',
        rf'\\g<1>{minor}', text,
    )
    if not DRY_RUN:
        bb_main.write_text(text)
    print(f"  {tag}  bb_main.c  → BB_HAT_FW_MAJOR/MINOR {new_ver}")


def _apply_desktop(new_ver: str) -> None:
    r = subprocess.run(
        [sys.executable, "DesktopApp/BugBuster/scripts/desktop_version.py", new_ver],
        capture_output=True, text=True, cwd=ROOT,
    )
    if r.returncode != 0:
        raise RuntimeError(f"desktop_version.py failed:\\n{r.stderr.strip()}")
    tag = "[DRY]" if DRY_RUN else "[DONE]"
    if DRY_RUN:
        # desktop_version.py already wrote the files — undo them in dry-run
        subprocess.run(
            ["git", "checkout", "--",
             "DesktopApp/BugBuster/Cargo.toml",
             "DesktopApp/BugBuster/src-tauri/Cargo.toml",
             "DesktopApp/BugBuster/src-tauri/tauri.conf.json"],
            cwd=ROOT,
        )
    print(f"  {tag}  desktop_version.py → Desktop {new_ver}")


def _apply_bbp_proto(new_ver: int) -> None:
    specs = [
        (ROOT / "Firmware/ESP32/src/bbp/bbp.h",
         r'(?m)^(#define\\s+BBP_PROTO_VERSION\\s+)\\d+'),
        (ROOT / "python/bugbuster/constants.py",
         r'(?m)^(BBP_PROTO_VERSION\\s*=\\s*)\\d+'),
        (ROOT / "python/bugbuster/protocol.py",
         r'(?m)^(BBP_PROTO_VERSION\\s*=\\s*)\\d+'),
        (ROOT / "DesktopApp/BugBuster/src-tauri/src/bbp.rs",
         r'(?m)^(pub const PROTO_VERSION:\\s*u8\\s*=\\s*)\\d+'),
    ]
    for path, pattern in specs:
        text = path.read_text()
        updated = re.sub(pattern, rf'\\g<1>{new_ver}', text)
        if not DRY_RUN:
            path.write_text(updated)
    tag = "[DRY]" if DRY_RUN else "[DONE]"
    print(f"  {tag}  BBP_PROTO_VERSION → v{new_ver}  (bbp.h, constants.py, protocol.py, bbp.rs)")


print("Applying version file edits...")
if ESP32_NEW is not None:
    _apply_esp32(ESP32_NEW)
if RP2040_NEW is not None:
    _apply_rp2040(RP2040_NEW)
if DESKTOP_NEW is not None:
    _apply_desktop(DESKTOP_NEW)
if BBP_PROTO_NEW is not None:
    _apply_bbp_proto(BBP_PROTO_NEW)
print("Done.")
"""

CODE_VALIDATE = """\
# Runs the same validation scripts as CI.
# Skipped automatically in DRY_RUN mode (files weren't written).
passed: list[str] = []
failed: list[str] = []

def _run_check(cmd: list[str]) -> None:
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    label = " ".join(str(c) for c in cmd[-3:])  # last 3 args for context
    out = r.stdout.strip() or r.stderr.strip()
    if r.returncode == 0:
        passed.append(f"{label}  →  {out}")
    else:
        failed.append(f"{label}  →  {out}")

if DRY_RUN:
    print("[DRY_RUN — skipping validation; set DRY_RUN=False and re-run to validate]")
else:
    if ESP32_NEW is not None:
        _run_check([sys.executable, "Firmware/tools/firmware_version.py",
                    "esp32", "--expect", ESP32_NEW])
    if RP2040_NEW is not None:
        _run_check([sys.executable, "Firmware/tools/firmware_version.py",
                    "rp2040", "--expect", RP2040_NEW])
    if DESKTOP_NEW is not None:
        _run_check([sys.executable,
                    "DesktopApp/BugBuster/scripts/desktop_version.py",
                    "--check", "--expect", DESKTOP_NEW])

    for p in passed:
        print(f"  ✔  {p}")
    for f in failed:
        print(f"  ✖  {f}")

    assert not failed, "Validation failed — fix version files before continuing"
    if passed:
        print("All validation checks passed.")
"""

CODE_CHANGELOG = """\
# Replaces the [Unreleased] section header with the versioned label + today's date,
# and inserts a fresh empty [Unreleased] block at the top.

assert CHANGELOG_LABEL, "Set CHANGELOG_LABEL in the Configure cell before running this"

changelog_path = ROOT / "CHANGELOG.MD"
text = changelog_path.read_text()

today_str = date.today().strftime("%Y-%m-%d")
new_version_header = f"## [{CHANGELOG_LABEL}] — {today_str}"

# Match the existing [Unreleased] line (with optional date suffix)
m = re.search(r'## \\[Unreleased\\][^\\n]*', text)
assert m, "Could not find '## [Unreleased]' in CHANGELOG.MD"

empty_unreleased = "## [Unreleased]\\n\\n*(no changes yet)*\\n\\n"
text_new = text[: m.start()] + empty_unreleased + new_version_header + text[m.end() :]

print("Changelog diff preview:")
print(f"  BEFORE : {m.group(0)!r}")
print(f"  REPLACE: {new_version_header!r}")
print(f"  INSERT : new empty [Unreleased] block above it")

if not DRY_RUN:
    changelog_path.write_text(text_new)
    print("[DONE]  CHANGELOG.MD updated")
else:
    print("[DRY]   CHANGELOG.MD not written")
"""

CODE_COMMIT = """\
parts: list[str] = []
if ESP32_NEW is not None:    parts.append(f"ESP32 v{ESP32_NEW}")
if RP2040_NEW is not None:   parts.append(f"RP2040 v{RP2040_NEW}")
if DESKTOP_NEW is not None:  parts.append(f"Desktop v{DESKTOP_NEW}")
if BBP_PROTO_NEW is not None: parts.append(f"BBP proto v{BBP_PROTO_NEW}")

assert parts, "Nothing bumped — nothing to commit"

commit_msg = "Release: " + ", ".join(parts)

staged: list[str] = []
if ESP32_NEW is not None:
    staged += ["Firmware/ESP32/src/bbp/bbp.h"]
if RP2040_NEW is not None:
    staged += ["Firmware/RP2040/CMakeLists.txt", "Firmware/RP2040/src/bb_main.c"]
if DESKTOP_NEW is not None:
    staged += [
        "DesktopApp/BugBuster/Cargo.toml",
        "DesktopApp/BugBuster/src-tauri/Cargo.toml",
        "DesktopApp/BugBuster/src-tauri/tauri.conf.json",
    ]
if BBP_PROTO_NEW is not None:
    staged += [
        "Firmware/ESP32/src/bbp/bbp.h",
        "python/bugbuster/constants.py",
        "python/bugbuster/protocol.py",
        "DesktopApp/BugBuster/src-tauri/src/bbp.rs",
    ]
if CHANGELOG_LABEL:
    staged += ["CHANGELOG.MD"]

staged = sorted(set(staged))

print(f"Commit message : {commit_msg!r}")
print(f"Files to stage :")
for f in staged:
    print(f"  {f}")

commit_sha: str | None = None

if not DRY_RUN:
    subprocess.run(["git", "add"] + staged, cwd=ROOT, check=True)
    r = subprocess.run(["git", "commit", "-m", commit_msg],
                       capture_output=True, text=True, cwd=ROOT)
    print(r.stdout.strip())
    if r.returncode != 0:
        print("STDERR:", r.stderr.strip())
        raise RuntimeError("git commit failed")
    r2 = subprocess.run(["git", "rev-parse", "HEAD"],
                        capture_output=True, text=True, cwd=ROOT)
    commit_sha = r2.stdout.strip()
    print(f"[DONE]  Committed {commit_sha[:12]}")
else:
    print("[DRY]   No commit made")
"""

CODE_TAG_PUSH = """\
# Creates annotated tags for each bumped component and pushes main + tags.
# BBP proto gets no separate tag (it's a sync-only change).

tags: list[tuple[str, str]] = []
if ESP32_NEW is not None:
    tags.append((f"esp-fw-v{ESP32_NEW}",    f"ESP32 BBP firmware v{ESP32_NEW}"))
if RP2040_NEW is not None:
    tags.append((f"hat-fw-v{RP2040_NEW}",   f"RP2040 HAT firmware v{RP2040_NEW}"))
if DESKTOP_NEW is not None:
    tags.append((f"desktop-v{DESKTOP_NEW}", f"BugBuster Desktop v{DESKTOP_NEW}"))

print("Tags to create:")
for tag, msg in tags:
    print(f"  {tag!r}  →  {msg!r}")
if not tags:
    print("  (none — only BBP proto bumped, no tag needed)")

if DRY_RUN:
    print("\\n[DRY]   No push or tags performed")
elif commit_sha is None:
    print("\\n[SKIP]  commit_sha not set — run the Commit cell first")
else:
    print("\\nPushing main...")
    subprocess.run(["git", "push", "origin", "main"], cwd=ROOT, check=True)

    for tag, msg in tags:
        subprocess.run(["git", "tag", "-a", tag, "-m", msg, commit_sha],
                       cwd=ROOT, check=True)
        print(f"  Created tag {tag}")

    if tags:
        tag_names = [t for t, _ in tags]
        subprocess.run(["git", "push", "origin"] + tag_names, cwd=ROOT, check=True)
        print(f"\\n[DONE]  Pushed {len(tags)} tag(s).")

    print("\\nWatch CI at:")
    print("  https://github.com/lollokara/BugBuster/actions")
    print("\\nDraft releases will appear at:")
    print("  https://github.com/lollokara/BugBuster/releases")
"""

# ─────────────────────────────────────────────────────────────────────────────
#  Assemble notebook
# ─────────────────────────────────────────────────────────────────────────────

cells = [
    md(MD_HEADER),
    md("## 1 — Setup"),
    code(CODE_SETUP),
    md("## 2 — Current Versions"),
    code(CODE_VERSIONS),
    md(MD_CONFIGURE),
    code(CODE_CONFIGURE),
    md("## 4 — Preview"),
    code(CODE_PREVIEW),
    md("## 5 — Apply file edits\n\nWrites version numbers into source files. Safe to re-run."),
    code(CODE_APPLY),
    md("## 6 — Validate\n\nRuns the same scripts that CI uses to gate each release tag."),
    code(CODE_VALIDATE),
    md("## 7 — Update CHANGELOG.MD"),
    code(CODE_CHANGELOG),
    md("## 8 — Git commit"),
    code(CODE_COMMIT),
    md("## 9 — Tag & push\n\nCreates annotated release tags and pushes `main` + tags to `origin`."),
    code(CODE_TAG_PUSH),
]

notebook = {
    "nbformat": 4,
    "nbformat_minor": 5,
    "metadata": {
        "kernelspec": {
            "display_name": "Python 3",
            "language": "python",
            "name": "python3",
        },
        "language_info": {
            "name": "python",
            "version": "3.11.0",
        },
    },
    "cells": cells,
}

out = Path(__file__).parent / "release_version_bump.ipynb"
out.write_text(json.dumps(notebook, indent=1), encoding="utf-8")
print(f"Written: {out}")
