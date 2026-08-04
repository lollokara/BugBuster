"""Static parity between api_core's route table and webserver's registrations.

No hardware. This guards a bug class that has shipped twice: a route exists in
`api_core_handle()` (so BLE can reach it) but has no URI handler registered in
`webserver.cpp` (so HTTP 404s), or vice versa. `539c0d9` was the expensive
version -- `/api/update/apply` parsed differently in the two dispatchers and
would have flashed the wrong chips.
"""
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[2]
API_CORE = REPO / "Firmware/ESP32/src/net/api_core.cpp"
WEBSERVER = REPO / "Firmware/ESP32/src/web/webserver.cpp"

# Routes api_core_handle() dispatches on (reachable over BLE) but with no
# matching URI handler registered in webserver.cpp (404 over HTTP). Confirmed
# by source inspection on 2026-08-04 -- each entry was checked against every
# registration AND every wildcard/dispatch-prefix handler in webserver.cpp,
# not just a literal string search:
#
#   /api/hat/rail/enable         -- only the /api/hat/v2/rail/enable variant is
#   /api/hat/rail/voltage           registered. The catch-all POST /api/hat/*
#                                    dispatcher (handle_hat_post_dispatch) only
#                                    recognizes .../config, .../reset and
#                                    .../detect substrings, so these 404 through
#                                    it rather than reaching a handler.
#   /api/hat/v2/swd/detect       -- no registration, and it does not match any
#                                    substring the /api/hat/* wildcard checks
#                                    for (only /api/hat/v2/swd/setup exists).
#   /api/ota/apply                -- webserver.cpp instead registers
#   /api/ota/status                  /api/update/apply and /api/update/status,
#                                     which are a *different* operation, not a
#                                     delegating alias: handle_post_update_apply
#                                     parses targets itself instead of calling
#                                     api_core_handle("...", "/api/ota/apply",
#                                     ...) -- see test_route_parity.py's
#                                     ALIAS_KNOWN_DIVERGENCES for that gap. The
#                                     literal /api/ota/apply and /api/ota/status
#                                     paths have no HTTP handler at all.
#   /api/ota/releases            -- no registration and no alias anywhere in
#                                    webserver.cpp; HTTP has no way to list
#                                    release options at all.
#
# Remove an entry from this list when its handler is registered -- the test
# will then fail if it regresses.
KNOWN_HTTP_MISSING = {
    "/api/hat/rail/enable",
    "/api/hat/rail/voltage",
    "/api/hat/v2/swd/detect",
    "/api/ota/apply",
    "/api/ota/status",
    "/api/ota/releases",
}


def _api_core_routes(text):
    """Exact-match routes api_core_handle() dispatches on."""
    return set(re.findall(r'strcmp\(path,\s*"(/api/[^"]+)"\)\s*==\s*0', text))


def _webserver_routes(text):
    """Routes webserver.cpp mentions at all (registration or delegation)."""
    return set(re.findall(r'"(/api/[^"*?]+)"', text))


def test_source_files_exist():
    assert API_CORE.is_file(), "missing %s" % API_CORE
    assert WEBSERVER.is_file(), "missing %s" % WEBSERVER


def test_every_api_core_route_is_reachable_over_http():
    core = _api_core_routes(API_CORE.read_text())
    web = _webserver_routes(WEBSERVER.read_text())
    assert core, "parsed no routes out of api_core.cpp — has the dispatch style changed?"

    missing = {r for r in core if r not in web} - KNOWN_HTTP_MISSING
    assert not missing, (
        "routes dispatched by api_core_handle() but absent from webserver.cpp — "
        "reachable over BLE, 404 over HTTP: %s" % sorted(missing))


def test_known_missing_routes_are_still_missing():
    """If a known gap gets fixed, tighten the guard rather than leaving it stale."""
    core = _api_core_routes(API_CORE.read_text())
    web = _webserver_routes(WEBSERVER.read_text())
    fixed = {r for r in KNOWN_HTTP_MISSING if r in web or r not in core}
    assert not fixed, (
        "these are no longer missing from webserver.cpp — remove them from "
        "KNOWN_HTTP_MISSING so the guard covers them: %s" % sorted(fixed))


def test_update_apply_is_parsed_consistently():
    """Both dispatchers must understand the p4/c6 update targets.

    `dacb220` migrated only one parser; `539c0d9` fixed it. If either file
    handles /api/update/apply, both must mention p4 and c6.

    Whole-file rather than a windowed search: in webserver.cpp the handler
    body (`handle_post_update_apply`, ~line 4041) sits ~53 KB before its
    `httpd_uri_t` registration (~line 5397) because handlers are all defined
    up front and every route is registered together in one setup function
    near the end of the file. A +/-4000 char window around the route string
    would miss the handler entirely and fail even though p4/c6 are handled
    correctly — confirmed by grepping both files: `"p4"`/`"c6"` each appear
    exactly once, both inside the update-apply target-parsing code, so a
    whole-file check carries no meaningful false-positive risk here.
    """
    core = API_CORE.read_text()
    web = WEBSERVER.read_text()
    for name, text in (("api_core.cpp", core), ("webserver.cpp", web)):
        if "/api/update/apply" not in text:
            continue
        for target in ("p4", "c6"):
            assert '"%s"' % target in text, (
                "%s handles /api/update/apply but never mentions %r — the two "
                "dispatchers can disagree about update targets, which is how "
                "539c0d9 nearly flashed the wrong chips" % (name, target))
