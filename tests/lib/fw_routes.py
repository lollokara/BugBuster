"""Firmware HTTP route table, parsed from the S3 sources.

Shared by the route-existence contract test and the read-only fixture walker,
so both agree on what the device serves.
"""

from __future__ import annotations

import re

from tests.lib.srcread import read_source

WEBSERVER = "Firmware/ESP32/src/web/webserver.cpp"
ADAPTER = "Firmware/ESP32/src/web/http_adapter.cpp"
# WebSocket endpoints registered outside webserver.cpp.
WS_SOURCES = ("Firmware/ESP32/src/mp/repl_ws.cpp", "Firmware/ESP32/src/web/ws_stream.cpp")
PLACEHOLDER = "\x00"

_URI_METHOD = re.compile(r'\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*HTTP_([A-Z]+)')


def firmware_routes() -> list[tuple[str, str]]:
    """(METHOD, uri) pairs under /api/. Matching follows
    ``httpd_uri_match_wildcard``: a trailing ``*`` matches any suffix."""
    out = [(m, u) for u, m in _URI_METHOD.findall(read_source(WEBSERVER))]
    out += [(m, u) for u, m in re.findall(
        r'\{\s*"(/api/[^"]+)"\s*,\s*HTTP_([A-Z]+)', read_source(ADAPTER))]
    for ws in WS_SOURCES:
        out += [(m, u) for u, m in _URI_METHOD.findall(read_source(ws))]
    # The SPA fallback `GET /*` would otherwise match every path.
    return [(m, u) for m, u in out if u.startswith("/api/")]


def _path_regex(path: str) -> re.Pattern:
    """A called path with placeholders -> regex; a placeholder matches any
    non-empty run of non-slash characters (it may be part of a segment, as in
    `/api/ota/upload_${target}`)."""
    return re.compile("".join("[^/]+" if c == PLACEHOLDER else re.escape(c) for c in path))


def route_registered(method: str, path: str, routes) -> bool:
    rx = _path_regex(path)
    for m, uri in routes:
        if m != method:
            continue
        if uri.endswith("*"):
            if path.startswith(uri[:-1]):
                return True
        elif rx.fullmatch(uri):
            return True
    return False
