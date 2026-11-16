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
    """Read a repository source file as UTF-8, resolved against the repo root.

    A path containing ``*`` is a glob: the matching files are returned joined in
    sorted order (see :data:`WEBSERVER_SOURCES`)."""
    p = Path(path)
    if not p.is_absolute():
        p = REPO_ROOT / p
    if "*" in p.name:
        files = sorted(p.parent.glob(p.name))
        if not files:
            raise FileNotFoundError(str(p))
        return "\n".join(f.read_text(encoding="utf-8") for f in files)
    return p.read_text(encoding="utf-8")


# The S3 HTTP server: webserver.cpp (helpers, route table) plus the
# webserver_*.cpp handler files it was split into.
WEBSERVER_SOURCES = "Firmware/ESP32/src/web/webserver*.cpp"


def client_source_files() -> list[Path]:
    """client.py plus the client_*.py modules the BugBuster class is split across."""
    return sorted((REPO_ROOT / "python" / "bugbuster").glob("client*.py"))


def read_client_sources() -> str:
    return "\n".join(read_source(p) for p in client_source_files())
