"""M0.5: every HTTP route the Python client calls must be registered by the
firmware with the same method.

Python side: AST of ``python/bugbuster/client.py`` - first argument of
``self._http_get/_http_post/_http_delete`` and ``self._t.get/post/delete``.
f-string fields become a placeholder segment. The client calls paths without
the ``/api`` prefix (the transport adds it).

Firmware side: ``.uri = "...", .method = HTTP_X`` in ``webserver.cpp`` and the
``{ "/api/...", HTTP_X, ...}`` table in ``http_adapter.cpp``. Matching follows
``httpd_uri_match_wildcard``: a trailing ``*`` matches any suffix.
"""

from __future__ import annotations

import ast
import re

import pytest

from tests.lib.srcread import REPO_ROOT, read_source

CLIENT = "python/bugbuster/client.py"
WEBSERVER = "Firmware/ESP32/src/web/webserver.cpp"
ADAPTER = "Firmware/ESP32/src/web/http_adapter.cpp"
# WebSocket endpoints registered outside webserver.cpp.
WS_SOURCES = ("Firmware/ESP32/src/mp/repl_ws.cpp", "Firmware/ESP32/src/web/ws_stream.cpp")
PLACEHOLDER = "\x00"

_METHODS = {
    "_http_get": "GET", "_http_post": "POST", "_http_delete": "DELETE", "_http_put": "PUT",
    "get": "GET", "post": "POST", "delete": "DELETE", "put": "PUT",
}

# (METHOD, path-as-called) -> audit ID. Delete an entry when its fix lands.
KNOWN_BROKEN: dict[tuple[str, str], str] = {}


def firmware_routes() -> list[tuple[str, str]]:
    out = [(m, u) for u, m in re.findall(
        r'\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*HTTP_([A-Z]+)', read_source(WEBSERVER))]
    out += [(m, u) for u, m in re.findall(
        r'\{\s*"(/api/[^"]+)"\s*,\s*HTTP_([A-Z]+)', read_source(ADAPTER))]
    for ws in WS_SOURCES:
        out += [(m, u) for u, m in re.findall(
            r'\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*HTTP_([A-Z]+)', read_source(ws))]
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


def _path_literal(node: ast.expr) -> str | None:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr):
        parts = []
        for i, v in enumerate(node.values):
            if isinstance(v, ast.Constant):
                parts.append(str(v.value))
            elif i == len(node.values) - 1 and parts and not parts[-1].endswith("/"):
                break  # trailing query-string field, e.g. f"/scripts/eval{qs}"
            else:
                parts.append(PLACEHOLDER)
        return "".join(parts)
    return None


def client_calls() -> list[tuple[str, str, int]]:
    tree = ast.parse(read_source(CLIENT))
    calls = []
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and node.func.attr in _METHODS and node.args):
            continue
        recv = node.func.value
        is_self_helper = (isinstance(recv, ast.Name) and recv.id == "self"
                          and node.func.attr.startswith("_http_"))
        is_transport = (isinstance(recv, ast.Attribute) and recv.attr == "_t"
                        and not node.func.attr.startswith("_"))
        if not (is_self_helper or is_transport):
            continue
        path = _path_literal(node.args[0])
        if path is None or not path.startswith("/"):
            continue
        path = path.split("?")[0]
        if not path.startswith("/api/"):
            path = "/api" + path
        calls.append((_METHODS[node.func.attr], path, node.lineno))
    return calls


def _params():
    out = []
    for method, path, line in client_calls():
        marks = []
        if (method, path) in KNOWN_BROKEN:
            marks.append(pytest.mark.xfail(strict=True, reason=KNOWN_BROKEN[(method, path)]))
        out.append(pytest.param(method, path, marks=marks,
                                id=f"client.py:{line}:{method} {path.replace(PLACEHOLDER, '{}')}"))
    return out


def test_collectors_find_both_sides():
    assert len(firmware_routes()) > 100
    assert len(client_calls()) > 80


def test_matcher_negative_controls():
    """The matcher once passed everything via the SPA fallback `GET /*`."""
    routes = firmware_routes()
    assert not route_registered("GET", "/api/definitely/not/a/route", routes)
    assert not route_registered("DELETE", "/api/status", routes)
    assert route_registered("POST", "/api/channel/" + PLACEHOLDER + "/ilimit", routes)
    assert route_registered("POST", "/api/ota/upload_" + PLACEHOLDER, routes)
    assert not route_registered("POST", "/api/ota/nope_" + PLACEHOLDER, routes)


@pytest.mark.parametrize("method,path", _params())
def test_python_client_route_is_registered(method, path):
    assert route_registered(method, path, firmware_routes()), (
        f"{method} {path} is not registered in webserver.cpp/http_adapter.cpp")


# ---------------------------------------------------------------------------
# Web UI (TS/TSX) and iOS (Swift): method-agnostic, from "/api/..." literals.
# Template/interpolated segments become a placeholder. The web IO-lease module
# has its own method-aware vitest (io_lease.test.ts).
# ---------------------------------------------------------------------------

WEB_SRC = REPO_ROOT / "Firmware" / "ESP32" / "web" / "src"
IOS_SRC = REPO_ROOT / "iOSApp" / "Sources"

KNOWN_BROKEN_LITERALS: dict[tuple[str, str], str] = {
    ("io_lease.ts", "/api/io/owner/claim"): "IO-11",
    ("io_lease.ts", "/api/io/owner/release"): "IO-11",
}

_LIT = re.compile(r"""["'`](/api/[^"'`\s?#]*)""")


def _normalise(lit: str) -> str:
    lit = re.sub(r"\$\{[^}]*\}|\\\([^)]*\)", PLACEHOLDER, lit)  # TS ${..} / Swift \(..)
    return lit


def ui_literals() -> list[tuple[str, int, str]]:
    out = []
    files = [p for p in WEB_SRC.rglob("*.ts*") if not p.name.endswith(".test.ts")]
    files += list(IOS_SRC.rglob("*.swift"))
    for p in sorted(files):
        for i, line in enumerate(read_source(p).splitlines(), 1):
            if line.lstrip().startswith(("//", "*")):
                continue
            for m in _LIT.finditer(line):
                out.append((p.name, i, _normalise(m.group(1))))
    return out


def _any_method_registered(path: str, routes) -> bool:
    return any(route_registered(m, path, routes) for m in {m for m, _ in routes})


def _ui_params():
    out = []
    seen = set()
    for fname, line, path in ui_literals():
        if path.endswith("/") or (fname, path) in seen:
            continue  # prefix-building fragments, duplicates
        seen.add((fname, path))
        marks = []
        if (fname, path) in KNOWN_BROKEN_LITERALS:
            marks.append(pytest.mark.xfail(strict=True, reason=KNOWN_BROKEN_LITERALS[(fname, path)]))
        out.append(pytest.param(path, marks=marks,
                                id=f"{fname}:{line}:{path.replace(PLACEHOLDER, '{}')}"))
    return out


def test_ui_collector_finds_literals():
    assert len(ui_literals()) > 50


@pytest.mark.parametrize("path", _ui_params())
def test_ui_route_literal_is_registered(path):
    assert _any_method_registered(path, firmware_routes()), (
        f"{path} is not registered (any method) in webserver.cpp/http_adapter.cpp")
