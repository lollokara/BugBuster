"""PLT-01 gate: every mutating HTTP route must check the admin token.

A route is protected when its handler calls ``check_admin_auth`` itself, or
when it only delegates and every ``handle_*`` it calls is protected (the
``/api/*`` dispatch handlers). Intentionally open routes go in ``OPEN_ROUTES``
with a reason; an unexplained entry is how a hole gets hidden.
"""

import re

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source

SOURCES = (
    "Firmware/ESP32/src/web/webserver.cpp",
    "Firmware/ESP32/src/web/http_adapter.cpp",
)

# (method, uri) -> reason
OPEN_ROUTES: dict[tuple[str, str], str] = {
    ("POST", "/api/daq/bs"): "battsim status/list/dir/read/profile are reads; SET_EPOCH only stamps "
                             "the host clock once per run (same data class as the open GET routes)",
    ("POST", "/api/daq/bs/read"): "battsim history file read, POST only to carry the request body",
}

_ROUTE_RES = (
    re.compile(r'\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*HTTP_(POST|PUT|DELETE)\s*,'
               r'\s*\.handler\s*=\s*(\w+)'),
    re.compile(r'\{\s*"([^"]+)"\s*,\s*HTTP_(POST|PUT|DELETE)\s*,\s*(\w+)\s*,'),
)


def _routes():
    for path in SOURCES:
        src = read_source(path)
        for rx in _ROUTE_RES:
            for uri, method, handler in rx.findall(src):
                yield path, method, uri, handler


def _protected(path: str, handler: str, seen: frozenset = frozenset()) -> bool:
    if handler in seen:
        return False
    body = extract_function(path, rf"^static esp_err_t {handler}\(")
    if "check_admin_auth" in body:
        return True
    callees = set(re.findall(r"\b(handle_\w+)\s*\(", body)) - {handler}
    return bool(callees) and all(_protected(path, c, seen | {handler}) for c in callees)


def test_route_table_was_parsed():
    found = {(m, u) for _, m, u, _ in _routes()}
    assert ("POST", "/api/wifi/connect") in found
    assert len(found) > 50


def test_mutating_routes_require_auth():
    open_ = sorted(
        f"{method} {uri} ({handler})"
        for path, method, uri, handler in _routes()
        if (method, uri) not in OPEN_ROUTES and not _protected(path, handler)
    )
    assert open_ == [], f"mutating routes without admin auth: {open_}"


def test_open_route_allow_list_is_current():
    routes = {(m, u): (p, h) for p, m, u, h in _routes()}
    stale = [k for k in OPEN_ROUTES if k not in routes or _protected(*routes[k])]
    assert stale == [], f"remove from OPEN_ROUTES: {stale}"
