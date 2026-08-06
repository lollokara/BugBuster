"""Source-file reader for the static source-inspection tests.

Many unit tests assert on the *text* of firmware/desktop/iOS sources rather than
on runtime behaviour. Reading those files with ``Path.read_text()`` uses the
platform default encoding, which is cp1252 on Windows and fails on the
box-drawing and typographic characters used throughout the firmware comment
banners. Every such read must go through :func:`read_source` so the suite is
byte-identical on Linux, macOS and Windows.
"""

from __future__ import annotations

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read_source(path: str | Path) -> str:
    """Read a repository source file as UTF-8, resolved against the repo root."""
    p = Path(path)
    if not p.is_absolute():
        p = REPO_ROOT / p
    return p.read_text(encoding="utf-8")
