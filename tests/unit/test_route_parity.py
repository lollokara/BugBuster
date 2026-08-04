"""HTTP routes that also exist in api_core must delegate, not re-implement.

`net/api_core.cpp` is documented as the single transport-agnostic dispatcher:
"do it once here and both HTTP and BLE get it, so the two transports can never
drift apart" (api_core.h). That only holds if `web/webserver.cpp`'s handler for
a shared route calls `api_core_handle()` instead of doing the work itself.

Where it does not hold, the two transports drift, and this repo has been bitten
by it repeatedly:

  * `/api/update/apply` was implemented in both files; a migration updated only
    api_core, so `{"p4":true,"c6":true}` over plain HTTP silently fell through
    to the rp2040+esp32 defaults and would have flashed the mainboard and LA HAT
    instead of the DAQ HAT. It failed to fire on the bench only because the
    worker could not allocate -- luck, not a safeguard.
  * `/api/update/check` kept calling update_manager_check() inline on the 4 KB
    httpd stack, rebooting the board on every request, while the 16 KB-worker
    fix landed on api_core's BLE-only `/api/ota/check`.
  * `/api/idac/cal/points` returns a different document on each transport (the
    HTTP copy omits the entire `hat` calibration object).
  * `/api/gpio` likewise: the HTTP copy emits `name`, `modeName` and `pulldown`;
    the BLE copy omits all three.

This test enumerates both route tables from source and fails on any shared route
whose HTTP handler does not delegate. Known-divergent routes are listed in
KNOWN_DIVERGENCES with the reason; that set should only ever shrink.

See docs/superpowers/reviews/2026-08-03-design-sweep.md findings S1-2, S2-1, S2-3
and .mex/patterns/cross-surface-parity-guard.md.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
WEBSERVER = REPO / "Firmware/ESP32/src/web/webserver.cpp"
API_CORE = REPO / "Firmware/ESP32/src/net/api_core.cpp"

# Routes that are implemented independently in both files and have NOT yet been
# unified. Each entry must carry a reason. Remove entries as they are fixed --
# never add one without a finding to point at.
KNOWN_DIVERGENCES: dict[str, str] = {
    "/api/gpio": (
        "HTTP emits name/modeName/pulldown; the api_core copy omits all three, "
        "so the same GET returns a different document per transport. "
        "Design sweep 2026-08-03."
    ),
    "/api/hat/v2/calibrate/start": (
        "HTTP handler adds hat_detected() and railId range validation that the "
        "api_core copy does not obviously mirror. Needs a behavioural diff "
        "before unifying. Design sweep 2026-08-03."
    ),
}

# Route pairs that are the SAME operation under different names. Exact-path
# matching cannot see these, which is precisely why the /api/update/apply and
# /api/update/check bugs stayed hidden -- grep for the path found one copy each.
ALIASES: dict[str, str] = {
    "/api/update/check": "/api/ota/check",
    "/api/update/apply": "/api/ota/apply",
}

ALIAS_KNOWN_DIVERGENCES: dict[str, str] = {
    "/api/update/apply": (
        "HTTP ignores the `index` field entirely (always applies nightly) while "
        "api_core supports update_manager_apply_release_index(); the two also "
        "differ on empty-body defaults, targets==0 handling and threading. "
        "Design sweep 2026-08-03 finding S2-1."
    ),
}


def _read(path: Path) -> str:
    if not path.is_file():
        pytest.skip(f"{path.relative_to(REPO)} not present in this checkout")
    return path.read_text(encoding="utf-8", errors="replace")


def _handler_bodies(src: str) -> dict[str, str]:
    """Extract each `static esp_err_t name(httpd_req_t *req)` body by brace match."""
    bodies: dict[str, str] = {}
    for m in re.finditer(r"static esp_err_t (\w+)\(httpd_req_t \*req\)\s*\n\{", src):
        name = m.group(1)
        i = m.end()
        depth = 1
        while i < len(src) and depth > 0:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        bodies[name] = src[m.end() : i]
    return bodies


def _registrations(src: str) -> list[tuple[str, str, str]]:
    """(uri, method, handler) for every httpd_uri_t registration."""
    return re.findall(
        r'\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*(\w+)\s*,\s*\.handler\s*=\s*(\w+)',
        src,
    )


def _api_core_routes(src: str) -> set[str]:
    return set(re.findall(r'strcmp\(path,\s*"([^"]+)"\)', src))


def test_route_tables_are_parseable():
    """Guard the guard: if the parse breaks, the test must fail loudly, not vacuously pass."""
    ws = _read(WEBSERVER)
    ac = _read(API_CORE)
    regs = _registrations(ws)
    bodies = _handler_bodies(ws)
    routes = _api_core_routes(ac)

    assert len(regs) > 100, f"only parsed {len(regs)} webserver registrations"
    assert len(bodies) > 100, f"only parsed {len(bodies)} handler bodies"
    assert len(routes) > 30, f"only parsed {len(routes)} api_core routes"


def test_shared_routes_delegate_to_api_core():
    """Any route in BOTH tables must have its HTTP handler call api_core_handle()."""
    ws = _read(WEBSERVER)
    ac = _read(API_CORE)
    bodies = _handler_bodies(ws)
    ac_routes = _api_core_routes(ac)

    offenders = []
    for uri, _method, handler in _registrations(ws):
        if uri not in ac_routes:
            continue
        if uri in KNOWN_DIVERGENCES:
            continue
        body = bodies.get(handler, "")
        if "api_core_handle" not in body:
            offenders.append(f"{uri} -> {handler}() re-implements instead of delegating")

    assert not offenders, (
        "HTTP handlers duplicating an api_core route:\n  "
        + "\n  ".join(offenders)
        + "\n\nDelegate via api_core_handle(method, uri, body), or add the route to "
        "KNOWN_DIVERGENCES with a reason."
    )


def test_aliased_routes_delegate_to_api_core():
    """Same operation under two names -- the case exact matching cannot see."""
    ws = _read(WEBSERVER)
    ac = _read(API_CORE)
    bodies = _handler_bodies(ws)
    ac_routes = _api_core_routes(ac)
    regs = {uri: handler for uri, _m, handler in _registrations(ws)}

    offenders = []
    for http_path, core_path in ALIASES.items():
        if http_path not in regs:
            continue
        assert core_path in ac_routes, (
            f"ALIASES maps {http_path} -> {core_path}, but {core_path} is no longer "
            "an api_core route; update the alias map."
        )
        if http_path in ALIAS_KNOWN_DIVERGENCES:
            continue
        body = bodies.get(regs[http_path], "")
        if "api_core_handle" not in body:
            offenders.append(
                f"{http_path} -> {regs[http_path]}() does not delegate to {core_path}"
            )

    assert not offenders, (
        "Aliased routes not delegating:\n  " + "\n  ".join(offenders)
    )


def test_known_divergences_are_still_real():
    """Stop the allowlist rotting: every entry must still be a live duplicate.

    If a route was unified but left in KNOWN_DIVERGENCES, the allowlist would
    silently keep excusing a route that no longer needs it -- and would go on
    excusing it if it regressed.
    """
    ws = _read(WEBSERVER)
    ac = _read(API_CORE)
    bodies = _handler_bodies(ws)
    ac_routes = _api_core_routes(ac)
    regs = {uri: handler for uri, _m, handler in _registrations(ws)}

    stale = []
    for uri in KNOWN_DIVERGENCES:
        if uri not in regs or uri not in ac_routes:
            stale.append(f"{uri}: no longer registered in both files")
        elif "api_core_handle" in bodies.get(regs[uri], ""):
            stale.append(f"{uri}: now delegates -- remove it from KNOWN_DIVERGENCES")
    for uri in ALIAS_KNOWN_DIVERGENCES:
        handler = regs.get(uri)
        if handler and "api_core_handle" in bodies.get(handler, ""):
            stale.append(f"{uri}: now delegates -- remove it from ALIAS_KNOWN_DIVERGENCES")

    assert not stale, "Stale allowlist entries:\n  " + "\n  ".join(stale)
