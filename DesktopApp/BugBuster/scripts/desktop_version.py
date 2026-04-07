#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UI_CARGO = ROOT / "Cargo.toml"
TAURI_CARGO = ROOT / "src-tauri" / "Cargo.toml"
TAURI_CONF = ROOT / "src-tauri" / "tauri.conf.json"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def replace_toml_version(path: Path, version: str) -> None:
    content = read_text(path)
    updated, count = re.subn(
        r'(?m)^version = "[^"]+"$',
        f'version = "{version}"',
        content,
        count=1,
    )
    if count != 1:
        raise RuntimeError(f"Could not update version in {path}")
    path.write_text(updated, encoding="utf-8")


def read_toml_version(path: Path) -> str:
    match = re.search(r'(?m)^version = "([^"]+)"$', read_text(path))
    if match is None:
        raise RuntimeError(f"Could not read version from {path}")
    return match.group(1)


def read_versions() -> dict[str, str]:
    tauri_conf = json.loads(read_text(TAURI_CONF))
    return {
        "ui": read_toml_version(UI_CARGO),
        "tauri": read_toml_version(TAURI_CARGO),
        "config": tauri_conf["version"],
    }


def set_version(version: str) -> None:
    replace_toml_version(UI_CARGO, version)
    replace_toml_version(TAURI_CARGO, version)

    tauri_conf = json.loads(read_text(TAURI_CONF))
    tauri_conf["version"] = version
    TAURI_CONF.write_text(json.dumps(tauri_conf, indent=2) + "\n", encoding="utf-8")


def check_versions(expect: str | None) -> int:
    versions = read_versions()
    unique = set(versions.values())
    if len(unique) != 1:
        print("Desktop version mismatch:", file=sys.stderr)
        for name, value in versions.items():
            print(f"  {name}: {value}", file=sys.stderr)
        return 1

    version = next(iter(unique))
    if expect is not None and version != expect:
        print(f"Desktop version is {version}, expected {expect}", file=sys.stderr)
        return 1

    print(version)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Manage synced BugBuster desktop app versions.")
    parser.add_argument("version", nargs="?", help="Set the desktop app version across all release files.")
    parser.add_argument("--check", action="store_true", help="Verify all desktop version files match.")
    parser.add_argument("--expect", help="Expected version when running with --check.")
    args = parser.parse_args()

    if args.version and args.check:
        parser.error("Use either VERSION or --check, not both.")
    if args.expect and not args.check:
        parser.error("--expect requires --check.")

    if args.check:
        return check_versions(args.expect)

    if not args.version:
        parser.error("Missing VERSION or --check.")

    set_version(args.version)
    print(args.version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
