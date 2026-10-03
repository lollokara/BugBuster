"""Hardware tests: routes the BLE tunnel serves, exercised over HTTP.

There is NO host-side way to drive ``api_core_handle()`` over the BLE tunnel in this
repo: the tunnel is a GATT write/notify pair (``ble_service.cpp``, request
``{"id":N,"path":"/api/...","body":{...}}``, <= 512 bytes) that needs a BLE central
bonded to the board, and neither ``bleak`` nor any BLE client exists in ``python/``
or ``tests/``. So, as documented in COVERAGE-scripting-daq.md, parity is tested in
two halves:

* source guards (``tests/unit/test_route_parity.py``, ``test_scripts_api_parity.py``)
  assert every scripts route is dispatched by ``api_core_handle()`` and that the HTTP
  handler only delegates to it;
* THIS file hits the same routes over HTTP, where the handler delegates to the very
  same ``api_core_handle()`` body the tunnel runs - so a wrong key, status or shape
  here is a wrong key, status or shape over BLE. It also checks the BLE-specific
  constraint that can be tested without a radio: every chunk request, framed exactly
  as the tunnel frames it, fits the 512-byte request buffer.

Covers sub-project 1's tunnel work too: ``/api/selftest/supplies`` (+ ``/cached``),
``/api/wifi``, ``/api/wifi/scan``, ``/api/update/status`` and the VDUT setpoint with
``currentLimitMa``.

    PYTHONPATH=python pytest tests/device/test_hw_ble_tunnel_routes.py -v \
        --device-http=<ip> --device-usb=<port>
"""

import base64
import json

import pytest

from tests.device._hw_scripts import script_name

pytestmark = [pytest.mark.timeout(60), pytest.mark.http_only]

BLE_REQ_MAX = 512        # s_api_req_buf after plan Task 5 (was 256)


def ble_frame(path: str, body: dict, req_id: int = 7) -> bytes:
    """The bytes a BLE central writes to the API-request characteristic."""
    return json.dumps({"id": req_id, "path": path, "body": body}, separators=(",", ":")).encode()


# ---------------------------------------------------------------------------
# every scripts route is reachable over HTTP (registered AND delegating)
# ---------------------------------------------------------------------------

ROUTES = [
    ("GET", "/api/scripts/status", None),
    ("GET", "/api/scripts/logs?since=0", None),
    ("GET", "/api/scripts/files", None),
    ("GET", "/api/scripts/files/get?name=bbt_hw_nosuch.py", None),
    ("POST", "/api/scripts/files/delete?name=bbt_hw_nosuch.py", None),
    ("POST", "/api/scripts/files/chunk", {"name": "../x.py", "off": 0, "b64": "", "final": False}),
    ("POST", "/api/scripts/run-file?name=bbt_hw_nosuch.py", None),
    ("POST", "/api/scripts/stop", None),
    ("GET", "/api/scripts/autorun/status", None),
]


@pytest.mark.parametrize("method,path,body", ROUTES, ids=[r[1].split("?")[0] for r in ROUTES])
def test_scripts_route_is_registered_over_http(scripts, method, path, body):
    r = (scripts.get(path) if method == "GET"
         else scripts.post(path, **({"json": body} if body is not None else {})))
    # A refusal of a bad request is fine (400/404 with {"ok":false,"error":...} proves the
    # handler ran); a bare 404/405/501 or a crash means the route is not wired.
    is_api_error = False
    if "json" in r.headers.get("Content-Type", ""):
        try:
            j = r.json()
            # {"ok":false,"error":...} on any refusal; a 404 whose body carries an
            # "error" key (e.g. {"error":"script not found"}) is a valid not-found
            # from a wired handler, not a missing route.
            is_api_error = isinstance(j, dict) and "error" in j and (
                j.get("ok") is False or r.status_code == 404)
        except ValueError:
            pass
    assert r.status_code < 500 and (r.status_code not in (404, 405) or is_api_error), \
        "%s %s -> HTTP %d: route missing or crashed: %s" % (method, path, r.status_code, r.text[:200])
    if r.status_code != 200:
        assert is_api_error, "error replies must carry {\"ok\":false,\"error\":...}: %r" % r.text


def test_autorun_enable_and_disable_routes_exist_without_changing_autorun(scripts):
    """enable is probed with an invalid name (must be refused); disable only when autorun is already off."""
    before = scripts.get("/api/scripts/autorun/status").json()
    r = scripts.post("/api/scripts/autorun/enable", params={"name": "../not-a-script.py"})
    assert r.status_code not in (404, 405, 500, 501), "autorun/enable: HTTP %d %s" % (r.status_code, r.text[:200])
    assert r.status_code == 400, "an invalid autorun name must be refused, got %d: %s" % (r.status_code, r.text)
    if not before["enabled"]:
        r = scripts.post("/api/scripts/autorun/disable")      # idempotent while already off
        assert r.status_code == 200, "autorun/disable: HTTP %d %s" % (r.status_code, r.text[:200])
    assert scripts.get("/api/scripts/autorun/status").json() == before


def test_files_get_and_list_agree_with_the_chunked_upload_over_http(scripts):
    name = script_name("parity")
    body = b"# parity\nprint('p')\n" * 40
    scripts.upload(name, body, chunk_bytes=300)
    assert name in scripts.list_files()
    r = scripts.get("/api/scripts/files/get", params={"name": name})
    assert r.status_code == 200 and r.content == body
    assert "python" in r.headers.get("Content-Type", "") or "text" in r.headers.get("Content-Type", ""), \
        "HTTP re-frames files/get as the legacy raw download, got %r" % r.headers.get("Content-Type")


# ---------------------------------------------------------------------------
# BLE request-size constraint
# ---------------------------------------------------------------------------

def test_a_ble_sized_chunk_fits_the_512_byte_request_and_uploads(scripts):
    """Find the largest raw chunk whose tunnel frame stays <= 512 B, upload with it."""
    name = script_name("blechunk")
    body = bytes((i * 7) % 251 for i in range(1500))    # arbitrary binary-ish payload, 1500 B
    # worst case frame: off up to 5 digits, final true, 2 digit id
    raw = 400
    while len(ble_frame("/api/scripts/files/chunk", {
            "name": name, "off": 99999, "b64": base64.b64encode(b"\0" * raw).decode(), "final": False})) > BLE_REQ_MAX:
        raw -= 3
    assert raw >= 240, "chunk payload shrank to %d B: framing overhead exceeds the BLE buffer" % raw

    scripts.created.append(name)
    off = 0
    while off < len(body):
        piece = body[off:off + raw]
        final = off + len(piece) >= len(body)
        req = {"name": name, "off": off, "b64": base64.b64encode(piece).decode(), "final": final}
        frame = ble_frame("/api/scripts/files/chunk", req)
        assert len(frame) <= BLE_REQ_MAX, "tunnel frame is %d B > %d" % (len(frame), BLE_REQ_MAX)
        r = scripts.post("/api/scripts/files/chunk", json=req)
        assert r.status_code == 200 and r.json()["received"] == off + len(piece), r.text
        off += len(piece)
    assert scripts.get_file(name) == body


@pytest.mark.skip(reason="no host-side BLE central in the repo (no bleak / BLE client in python/ or tests/); "
                         "BLE parity is covered by tests/unit/test_route_parity.py + test_scripts_api_parity.py "
                         "(source guards) and by this file's HTTP tests, which run the same api_core_handle() bodies")
def test_scripts_over_the_real_ble_tunnel():
    """Placeholder that keeps the gap visible in every report."""


# ---------------------------------------------------------------------------
# sub-project 1: GETs the iOS app reads over the BLE tunnel (same api_core bodies)
# ---------------------------------------------------------------------------

def _json(scripts, path, timeout=20):
    r = scripts.get(path, timeout=timeout)
    assert r.status_code == 200, "GET %s -> HTTP %d: %s" % (path, r.status_code, r.text[:200])
    return r.json()


def test_selftest_supplies_has_the_camelcase_firmware_shape(scripts):
    j = _json(scripts, "/api/selftest/supplies", timeout=30)
    for k in ("valid", "suppliesOk", "avddHiV", "dvccV", "avccV", "avssV", "tempC"):
        assert k in j, "selftest/supplies missing %r (iOS decodes these): %r" % (k, j)
    assert isinstance(j["valid"], bool) and isinstance(j["suppliesOk"], bool), j
    for k in ("avddHiV", "dvccV", "avccV", "avssV", "tempC"):
        assert isinstance(j[k], (int, float)), (k, j)
    assert not any(k in j for k in ("avdd_hi_v", "supplies_ok", "dvcc_v", "temp_c")), \
        "snake_case keys must not appear (IOS-23 camelCase rule): %r" % j
    if j["valid"]:
        assert -40.0 < j["tempC"] < 125.0, j


def test_selftest_supplies_cached_has_three_named_rails(scripts):
    j = _json(scripts, "/api/selftest/supplies/cached")
    assert isinstance(j["available"], bool) and "timestampMs" in j, j
    assert [r["name"] for r in j["rails"]] == ["VADJ1", "VADJ2", "VLOGIC"], j
    assert all(isinstance(r["voltageV"], (int, float)) for r in j["rails"]), j


def test_wifi_and_update_status_gets(scripts):
    w = _json(scripts, "/api/wifi")
    assert isinstance(w, dict) and w, w
    u = _json(scripts, "/api/update/status")
    assert isinstance(u, dict) and u, u


@pytest.mark.slow
def test_wifi_scan_returns_a_networks_list(scripts):
    j = _json(scripts, "/api/wifi/scan", timeout=30)
    assert isinstance(j.get("networks"), list), j
    for n in j["networks"][:5]:
        assert {"ssid", "rssi", "auth"} <= set(n), n


@pytest.mark.requires_daq
@pytest.mark.requires_daq_http
@pytest.mark.destructive
def test_vdut_setpoint_with_current_limit_roundtrips_and_is_restored(scripts, battsim_guard):
    before = _json(scripts, "/api/daq/vdut/status")
    if not before.get("present"):
        pytest.skip("no DAQ HAT present")
    try:
        r = scripts.post("/api/daq/vdut/setpoint", json={"voltageV": 4.0, "currentLimitMa": 150.0})
        assert r.status_code == 200, r.text
        j = _json(scripts, "/api/daq/vdut/status")
        assert abs(j["voltageSetpointV"] - 4.0) < 0.3, j
        assert abs(j["currentLimitMa"] - 150.0) < 30.0, "currentLimitMa not applied: %r" % j
        # a missing limit is rejected, never silently defaulted (iOS always sends both)
        r = scripts.post("/api/daq/vdut/setpoint", json={"voltageV": 4.0})
        assert r.status_code == 400, "setpoint without currentLimitMa must be 400, got %d" % r.status_code
    finally:
        scripts.post("/api/daq/vdut/setpoint", json={
            "voltageV": before["voltageSetpointV"], "currentLimitMa": before["currentLimitMa"]})
        scripts.post("/api/daq/vdut/enable", json={"enabled": bool(before["enabled"])})
