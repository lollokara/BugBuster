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

MD_HEADER = """\
# BugBuster — Release Version Bump

| Component | Tag | CI workflow |
|---|---|---|
| ESP32 firmware | `esp-fw-vX.Y.Z` | `esp32-firmware.yml` |
| RP2040 HAT | `hat-fw-vX.Y` | `rp2040-firmware.yml` |
| Desktop app | `desktop-vX.Y.Z` | `desktop-release.yml` |
| BBP wire protocol | *(no tag — sync only)* | `proto-version-check.yml` |

**Three steps:** run **Setup**, fill in **Configure**, run **Preview**, run **Execute**.
"""

# ── Setup (auto-run, read-only) ───────────────────────────────────────────────
CODE_SETUP = """\
import re, sys, json, subprocess, tempfile, os
from pathlib import Path
from datetime import date

ROOT = Path.cwd()
while not (ROOT / "Firmware").exists() and ROOT.parent != ROOT:
    ROOT = ROOT.parent
assert (ROOT / "Firmware").exists(), "Cannot find repo root — open from within BugBuster repo"

def _macro(text: str, name: str) -> str:
    m = re.search(rf'(?m)^#define\\s+{re.escape(name)}\\s+(\\d+)\\s*$', text)
    return m.group(1) if m else "?"

def read_esp32() -> str:
    t = (ROOT / "Firmware/ESP32/src/bbp/bbp.h").read_text()
    return f"{_macro(t,'BBP_FW_VERSION_MAJOR')}.{_macro(t,'BBP_FW_VERSION_MINOR')}.{_macro(t,'BBP_FW_VERSION_PATCH')}"

def read_rp2040() -> str:
    t = (ROOT / "Firmware/RP2040/CMakeLists.txt").read_text()
    m = re.search(r'set\\s*\\(\\s*PROBE_VERSION\\s+"bb-hat-([0-9]+\\.[0-9]+)"\\s*\\)', t)
    return m.group(1) if m else "?"

def read_desktop() -> str:
    r = subprocess.run(
        [sys.executable, "DesktopApp/BugBuster/scripts/desktop_version.py", "--check"],
        capture_output=True, text=True, cwd=ROOT,
    )
    return r.stdout.strip() or f"ERR: {r.stderr.strip()}"

def read_proto() -> str:
    t = (ROOT / "Firmware/ESP32/src/bbp/bbp.h").read_text()
    return _macro(t, "BBP_PROTO_VERSION")

esp32_ver   = read_esp32()
rp2040_ver  = read_rp2040()
desktop_ver = read_desktop()
proto_ver   = read_proto()

W = 40
print("┌" + "─"*W + "┐")
print("│" + " BugBuster — Current Versions ".center(W) + "│")
print("├" + "─"*W + "┤")
print(f"│  ESP32 firmware    {esp32_ver:>18}  │")
print(f"│  RP2040 HAT fw     {rp2040_ver:>18}  │")
print(f"│  Desktop app       {desktop_ver:>18}  │")
print(f"│  BBP proto         {'v'+proto_ver:>18}  │")
print("└" + "─"*W + "┘")
"""

# ── Step 1 — Configure ────────────────────────────────────────────────────────
MD_CONFIGURE = """\
## Step 1 — Configure

Edit the variables below. **Version strings must be quoted.** Set unused components to `None`.
"""

CODE_CONFIGURE = """\
# ─── EDIT THIS CELL ────────────────────────────────────────────────────────
DRY_RUN = True           # ← flip to False when ready to commit & push

# Version strings MUST be quoted ("3.5.0"), integers for BBP proto (9)
ESP32_NEW     = None     # e.g. "3.5.0"   MAJOR.MINOR.PATCH
RP2040_NEW    = None     # e.g. "3.3"     MAJOR.MINOR only
DESKTOP_NEW   = None     # e.g. "1.2.0"   MAJOR.MINOR.PATCH
BBP_PROTO_NEW = None     # e.g. 9         integer — syncs 4 files, no separate tag

# Becomes the CHANGELOG section header and GitHub release title
# e.g. "1.4.0"  /  "ESP32 3.5.0"  /  "ESP32 3.5.0 + Desktop 1.2.0"
CHANGELOG_LABEL = ""
# ─── END EDIT ──────────────────────────────────────────────────────────────

for _n, _v in [("ESP32_NEW", ESP32_NEW), ("RP2040_NEW", RP2040_NEW), ("DESKTOP_NEW", DESKTOP_NEW)]:
    if _v is not None and not isinstance(_v, str):
        raise TypeError(f'{_n} must be a quoted string, e.g.  {_n} = "3.4.1"  — got {_v!r} ({type(_v).__name__})')
if BBP_PROTO_NEW is not None and not isinstance(BBP_PROTO_NEW, int):
    raise TypeError(f'BBP_PROTO_NEW must be an integer, e.g.  BBP_PROTO_NEW = 9  — got {BBP_PROTO_NEW!r}')

print(f"DRY_RUN        = {DRY_RUN}")
print(f"ESP32_NEW      = {ESP32_NEW!r}")
print(f"RP2040_NEW     = {RP2040_NEW!r}")
print(f"DESKTOP_NEW    = {DESKTOP_NEW!r}")
print(f"BBP_PROTO_NEW  = {BBP_PROTO_NEW!r}")
print(f"CHANGELOG_LABEL= {CHANGELOG_LABEL!r}")
"""

# ── Step 2 — Preview ─────────────────────────────────────────────────────────
MD_PREVIEW = """\
## Step 2 — Preview

Run this cell to validate the configuration and see exactly what will change.
Fix any errors before proceeding.
"""

CODE_PREVIEW = """\
errors: list[str] = []
changes: list[str] = []
_tags: list[tuple[str, str]] = []  # (tag, title) — computed here, reused in Execute

if ESP32_NEW is not None:
    if not re.fullmatch(r'\\d+\\.\\d+\\.\\d+', ESP32_NEW):
        errors.append(f"ESP32_NEW must be MAJOR.MINOR.PATCH — got {ESP32_NEW!r}")
    elif ESP32_NEW == esp32_ver:
        errors.append(f"ESP32_NEW {ESP32_NEW!r} is already the current version")
    else:
        changes.append(f"ESP32 firmware    {esp32_ver}  →  {ESP32_NEW}")
        _tags.append((f"esp-fw-v{ESP32_NEW}", f"ESP32 firmware v{ESP32_NEW}"))

if RP2040_NEW is not None:
    if not re.fullmatch(r'\\d+\\.\\d+', RP2040_NEW):
        errors.append(f"RP2040_NEW must be MAJOR.MINOR — got {RP2040_NEW!r}")
    elif RP2040_NEW == rp2040_ver:
        errors.append(f"RP2040_NEW {RP2040_NEW!r} is already the current version")
    else:
        changes.append(f"RP2040 HAT fw     {rp2040_ver}  →  {RP2040_NEW}")
        _tags.append((f"hat-fw-v{RP2040_NEW}", f"RP2040 HAT firmware v{RP2040_NEW}"))

if DESKTOP_NEW is not None:
    if not re.fullmatch(r'\\d+\\.\\d+\\.\\d+', DESKTOP_NEW):
        errors.append(f"DESKTOP_NEW must be MAJOR.MINOR.PATCH — got {DESKTOP_NEW!r}")
    elif DESKTOP_NEW == desktop_ver:
        errors.append(f"DESKTOP_NEW {DESKTOP_NEW!r} is already the current version")
    else:
        changes.append(f"Desktop app       {desktop_ver}  →  {DESKTOP_NEW}")
        _tags.append((f"desktop-v{DESKTOP_NEW}", f"BugBuster Desktop v{DESKTOP_NEW}"))

if BBP_PROTO_NEW is not None:
    if str(BBP_PROTO_NEW) == proto_ver:
        errors.append(f"BBP_PROTO_NEW {BBP_PROTO_NEW} is already the current proto version")
    else:
        changes.append(f"BBP proto         v{proto_ver}  →  v{BBP_PROTO_NEW}  (4 files synced, no tag)")

if CHANGELOG_LABEL:
    today_str = date.today().strftime("%Y-%m-%d")
    changes.append(f"CHANGELOG.MD      [Unreleased]  →  [{CHANGELOG_LABEL}] — {today_str}")
    if _tags:
        changes.append(f"GitHub releases   {len(_tags)} draft(s) created from changelog section")

if not changes and not errors:
    errors.append("Nothing selected — set at least one of ESP32_NEW / RP2040_NEW / DESKTOP_NEW / BBP_PROTO_NEW")

if errors:
    print("ERRORS — fix before running Execute:")
    for e in errors:
        print(f"  ✖  {e}")
else:
    print("Planned changes:")
    for c in changes:
        print(f"  •  {c}")
    print()
    print(f"  Tags : {[t for t,_ in _tags] or '(none)'}")
    print()
    if DRY_RUN:
        print("  DRY_RUN=True — Execute will print actions but not write, commit, or push")
    else:
        print("  DRY_RUN=False — Execute WILL write files, commit, tag, and push")
"""

# ── Step 3 — Execute ─────────────────────────────────────────────────────────
MD_EXECUTE = """\
## Step 3 — Execute

Applies all changes in one shot: edit files → validate → update changelog → commit → push → create draft GitHub releases.

Set `DRY_RUN = False` in the Configure cell first.
"""

CODE_EXECUTE = """\
assert not errors, "Fix errors shown in Preview before running Execute"

_tag = "[DRY]" if DRY_RUN else "[DONE]"

# ── 1. Apply version file edits ───────────────────────────────────────────────
print("── 1/5  Applying version files ──────────────────────────")

if ESP32_NEW is not None:
    path = ROOT / "Firmware/ESP32/src/bbp/bbp.h"
    t = path.read_text()
    major, minor, patch = ESP32_NEW.split(".")
    t = re.sub(r'(?m)^(#define\\s+BBP_FW_VERSION_MAJOR\\s+)\\d+', rf'\\g<1>{major}', t)
    t = re.sub(r'(?m)^(#define\\s+BBP_FW_VERSION_MINOR\\s+)\\d+', rf'\\g<1>{minor}', t)
    t = re.sub(r'(?m)^(#define\\s+BBP_FW_VERSION_PATCH\\s+)\\d+', rf'\\g<1>{patch}', t)
    if not DRY_RUN: path.write_text(t)
    print(f"  {_tag}  bbp.h  → ESP32 {ESP32_NEW}")

if RP2040_NEW is not None:
    major, minor = RP2040_NEW.split(".")
    cmake = ROOT / "Firmware/RP2040/CMakeLists.txt"
    t = cmake.read_text()
    t = re.sub(r'(set\\s*\\(\\s*PROBE_VERSION\\s+"bb-hat-)[0-9]+\\.[0-9]+(")' , rf'\\g<1>{RP2040_NEW}\\g<2>', t)
    if not DRY_RUN: cmake.write_text(t)
    print(f"  {_tag}  CMakeLists.txt  → PROBE_VERSION bb-hat-{RP2040_NEW}")

    bb = ROOT / "Firmware/RP2040/src/bb_main.c"
    t = bb.read_text()
    t = re.sub(r'(#ifndef BB_HAT_FW_MAJOR\\s*\\n#define BB_HAT_FW_MAJOR\\s+)\\d+', rf'\\g<1>{major}', t)
    t = re.sub(r'(#ifndef BB_HAT_FW_MINOR\\s*\\n#define BB_HAT_FW_MINOR\\s+)\\d+', rf'\\g<1>{minor}', t)
    if not DRY_RUN: bb.write_text(t)
    print(f"  {_tag}  bb_main.c  → BB_HAT_FW_MAJOR/MINOR {RP2040_NEW}")

if DESKTOP_NEW is not None:
    r = subprocess.run(
        [sys.executable, "DesktopApp/BugBuster/scripts/desktop_version.py", DESKTOP_NEW],
        capture_output=True, text=True, cwd=ROOT,
    )
    if r.returncode != 0:
        raise RuntimeError(f"desktop_version.py failed:\\n{r.stderr.strip()}")
    if DRY_RUN:
        subprocess.run(["git", "checkout", "--",
            "DesktopApp/BugBuster/Cargo.toml",
            "DesktopApp/BugBuster/src-tauri/Cargo.toml",
            "DesktopApp/BugBuster/src-tauri/tauri.conf.json",
        ], cwd=ROOT)
    print(f"  {_tag}  desktop_version.py  → Desktop {DESKTOP_NEW}")

if BBP_PROTO_NEW is not None:
    _proto_files = [
        (ROOT / "Firmware/ESP32/src/bbp/bbp.h",
         r'(?m)^(#define\\s+BBP_PROTO_VERSION\\s+)\\d+'),
        (ROOT / "python/bugbuster/constants.py",
         r'(?m)^(BBP_PROTO_VERSION\\s*=\\s*)\\d+'),
        (ROOT / "python/bugbuster/protocol.py",
         r'(?m)^(BBP_PROTO_VERSION\\s*=\\s*)\\d+'),
        (ROOT / "DesktopApp/BugBuster/src-tauri/src/bbp.rs",
         r'(?m)^(pub const PROTO_VERSION:\\s*u8\\s*=\\s*)\\d+'),
    ]
    for _p, _pat in _proto_files:
        _t = _p.read_text()
        if not DRY_RUN: _p.write_text(re.sub(_pat, rf'\\g<1>{BBP_PROTO_NEW}', _t))
    print(f"  {_tag}  BBP_PROTO_VERSION  → v{BBP_PROTO_NEW}  (4 files)")

# ── 2. Validate ───────────────────────────────────────────────────────────────
print("\\n── 2/5  Validating ──────────────────────────────────────")
if DRY_RUN:
    print("  [DRY]  skipped")
else:
    _val_ok = True
    if ESP32_NEW is not None:
        r = subprocess.run([sys.executable, "Firmware/tools/firmware_version.py",
                            "esp32", "--expect", ESP32_NEW],
                           capture_output=True, text=True, cwd=ROOT)
        _ok = r.returncode == 0
        _val_ok &= _ok
        print(f"  {'✔' if _ok else '✖'}  ESP32   {r.stdout.strip() or r.stderr.strip()}")
    if RP2040_NEW is not None:
        r = subprocess.run([sys.executable, "Firmware/tools/firmware_version.py",
                            "rp2040", "--expect", RP2040_NEW],
                           capture_output=True, text=True, cwd=ROOT)
        _ok = r.returncode == 0
        _val_ok &= _ok
        print(f"  {'✔' if _ok else '✖'}  RP2040  {r.stdout.strip() or r.stderr.strip()}")
    if DESKTOP_NEW is not None:
        r = subprocess.run([sys.executable,
                            "DesktopApp/BugBuster/scripts/desktop_version.py",
                            "--check", "--expect", DESKTOP_NEW],
                           capture_output=True, text=True, cwd=ROOT)
        _ok = r.returncode == 0
        _val_ok &= _ok
        print(f"  {'✔' if _ok else '✖'}  Desktop {r.stdout.strip() or r.stderr.strip()}")
    assert _val_ok, "Validation failed — fix version files before continuing"

# ── 3. Update CHANGELOG.MD ────────────────────────────────────────────────────
print("\\n── 3/5  Updating changelog ──────────────────────────────")
_changelog_notes = ""
if CHANGELOG_LABEL:
    _cl_path = ROOT / "CHANGELOG.MD"
    _cl_text = _cl_path.read_text()
    _today = date.today().strftime("%Y-%m-%d")
    _new_header = f"## [{CHANGELOG_LABEL}] — {_today}"
    _m = re.search(r'## \\[Unreleased\\][^\\n]*', _cl_text)
    assert _m, "Could not find '## [Unreleased]' in CHANGELOG.MD"
    _cl_text_new = _cl_text[:_m.start()] + "## [Unreleased]\\n\\n*(no changes yet)*\\n\\n" + _new_header + _cl_text[_m.end():]
    if not DRY_RUN:
        _cl_path.write_text(_cl_text_new)
    print(f"  {_tag}  [Unreleased]  →  {_new_header}")

    # Extract the section we just wrote (used for GitHub release notes)
    _src = _cl_text_new if not DRY_RUN else _cl_text_new  # same in both cases
    _sec = re.search(
        rf'(## \\[{re.escape(CHANGELOG_LABEL)}\\][^\\n]*\\n)(.*?)(?=\\n## |\\Z)',
        _src, re.DOTALL,
    )
    _changelog_notes = _sec.group(0).strip() if _sec else f"Release {CHANGELOG_LABEL}"
else:
    print("  (skipped — CHANGELOG_LABEL is empty)")

# ── 4. Commit ─────────────────────────────────────────────────────────────────
print("\\n── 4/5  Committing ──────────────────────────────────────")
_parts = []
if ESP32_NEW:    _parts.append(f"ESP32 v{ESP32_NEW}")
if RP2040_NEW:   _parts.append(f"RP2040 v{RP2040_NEW}")
if DESKTOP_NEW:  _parts.append(f"Desktop v{DESKTOP_NEW}")
if BBP_PROTO_NEW is not None: _parts.append(f"BBP proto v{BBP_PROTO_NEW}")
_commit_msg = "Release: " + ", ".join(_parts)

_staged = []
if ESP32_NEW:    _staged += ["Firmware/ESP32/src/bbp/bbp.h"]
if RP2040_NEW:   _staged += ["Firmware/RP2040/CMakeLists.txt", "Firmware/RP2040/src/bb_main.c"]
if DESKTOP_NEW:  _staged += ["DesktopApp/BugBuster/Cargo.toml",
                              "DesktopApp/BugBuster/src-tauri/Cargo.toml",
                              "DesktopApp/BugBuster/src-tauri/tauri.conf.json"]
if BBP_PROTO_NEW is not None:
    _staged += ["Firmware/ESP32/src/bbp/bbp.h",
                "python/bugbuster/constants.py",
                "python/bugbuster/protocol.py",
                "DesktopApp/BugBuster/src-tauri/src/bbp.rs"]
if CHANGELOG_LABEL: _staged += ["CHANGELOG.MD"]
_staged = sorted(set(_staged))

print(f"  {_tag}  {_commit_msg!r}")
_commit_sha: str | None = None
if not DRY_RUN:
    subprocess.run(["git", "add"] + _staged, cwd=ROOT, check=True)
    r = subprocess.run(["git", "commit", "-m", _commit_msg],
                       capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        raise RuntimeError(f"git commit failed:\\n{r.stderr.strip()}")
    _commit_sha = subprocess.run(["git", "rev-parse", "HEAD"],
                                 capture_output=True, text=True, cwd=ROOT).stdout.strip()
    print(f"         committed {_commit_sha[:12]}")

# ── 5. Push + tag + GitHub draft releases ────────────────────────────────────
print("\\n── 5/5  Pushing & creating GitHub draft releases ────────")
if DRY_RUN:
    print(f"  [DRY]  would push main + tags: {[t for t,_ in _tags]}")
    if CHANGELOG_LABEL and _tags:
        print(f"  [DRY]  would create {len(_tags)} draft release(s) with changelog notes")
else:
    assert _commit_sha, "No commit SHA — did the Commit step succeed?"
    subprocess.run(["git", "push", "origin", "main"], cwd=ROOT, check=True)
    print("  ✔  pushed main")

    for _tag_name, _tag_title in _tags:
        subprocess.run(["git", "tag", "-a", _tag_name, "-m", _tag_title, _commit_sha],
                       cwd=ROOT, check=True)
    if _tags:
        subprocess.run(["git", "push", "origin"] + [t for t, _ in _tags], cwd=ROOT, check=True)
        print(f"  ✔  pushed {len(_tags)} tag(s): {[t for t,_ in _tags]}")

    # Create a draft GitHub release for each tag, populated with changelog notes
    if CHANGELOG_LABEL and _tags:
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md", delete=False) as _f:
            _f.write(_changelog_notes)
            _notes_file = _f.name
        try:
            for _tag_name, _tag_title in _tags:
                subprocess.run([
                    "gh", "release", "create", _tag_name,
                    "--draft",
                    "--title", _tag_title,
                    "--notes-file", _notes_file,
                ], cwd=ROOT, check=True)
                print(f"  ✔  draft release: {_tag_name}")
        finally:
            os.unlink(_notes_file)
    elif _tags:
        print("  (skipped GitHub releases — CHANGELOG_LABEL is empty)")

    print("\\nDone. Review & publish drafts at:")
    print("  https://github.com/lollokara/BugBuster/releases")
"""

# ─────────────────────────────────────────────────────────────────────────────
cells = [
    md(MD_HEADER),
    md("## Setup — run once per session"),
    code(CODE_SETUP),
    md(MD_CONFIGURE),
    code(CODE_CONFIGURE),
    md(MD_PREVIEW),
    code(CODE_PREVIEW),
    md(MD_EXECUTE),
    code(CODE_EXECUTE),
]

notebook = {
    "nbformat": 4,
    "nbformat_minor": 5,
    "metadata": {
        "kernelspec": {"display_name": "Python 3", "language": "python", "name": "python3"},
        "language_info": {"name": "python", "version": "3.11.0"},
    },
    "cells": cells,
}

out = Path(__file__).parent / "release_version_bump.ipynb"
out.write_text(json.dumps(notebook, indent=1), encoding="utf-8")
print(f"Written: {out}  ({len(cells)} cells)")
