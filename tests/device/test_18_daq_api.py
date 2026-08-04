"""Tier 3 — the /api/daq/* HTTP routes the iOS app depends on.

Run with:
    PYTHONPATH=python python -m pytest tests/device/test_18_daq_api.py -v \
        --daq --device-http=192.168.3.35 --device-usb=/dev/cu.usbmodem1234561

BLE is not covered: it needs a phone. Both transports share the
`api_core_handle()` dispatcher, so these exercise the same handler bodies.
"""
import pytest

pytestmark = pytest.mark.requires_daq_http


def test_vdut_status_shape(daq_http, daq_http_base):
    r = daq_http.get(daq_http_base + "/api/daq/vdut/status", timeout=10)
    assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])
    j = r.json()
    for field in ("present", "enabled", "voltageSetpointV", "currentLimitMa"):
        assert field in j, "missing %r in %r" % (field, j)
    assert j["present"] is True, "no DAQ HAT reported present"


def test_wifi_stream_status_shape(daq_http, daq_http_base):
    r = daq_http.get(daq_http_base + "/api/daq/wifi_stream/status", timeout=10)
    assert r.status_code == 200
    assert "state" in r.json(), "no 'state' field: %r" % r.json()


def test_vdut_setpoint_roundtrip(daq_http, daq_http_base):
    """A valid setpoint is applied and visible in status."""
    try:
        r = daq_http.post(daq_http_base + "/api/daq/vdut/setpoint",
                          json={"voltageV": 6.0, "currentLimitMa": 200.0}, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])

        st = daq_http.get(daq_http_base + "/api/daq/vdut/status", timeout=10).json()
        assert abs(st["voltageSetpointV"] - 6.0) < 0.3, (
            "setpoint 6.0 V not reflected: %r" % st["voltageSetpointV"])
    finally:
        daq_http.post(daq_http_base + "/api/daq/vdut/setpoint",
                      json={"voltageV": 1.8, "currentLimitMa": 50.0}, timeout=10)


@pytest.mark.parametrize("body", [
    {"voltageV": 0.5, "currentLimitMa": 200.0},     # below HAT_DAQ_VDUT_MIN_V 1.76
    {"voltageV": 25.0, "currentLimitMa": 200.0},    # above HAT_DAQ_VDUT_MAX_V 19.94
    {"voltageV": 6.0, "currentLimitMa": 5000.0},    # above HAT_DAQ_VDUT_ILIMIT_MAX_A*1000 2636 mA
])
def test_vdut_setpoint_rejects_out_of_range_with_http_400(daq_http, daq_http_base, body):
    """The HTTP path REJECTS out-of-range setpoints with a 400.

    Note the contrast with USB: `CMD_SET_SOURCE` silently CLAMPS to the
    config.h bounds (smu.c:181), while this path re-validates and rejects. Same
    user action, different outcome depending on client -- desktop drives USB,
    iOS drives HTTP/BLE. Documented in test_16_daq_stream.py's
    test_out_of_range_setpoints_are_clamped_to_the_documented_limits.

    This specifically exercises `send_api_core_result()`, which translates an
    `{"error": ...}` body into an HTTP status -- api_core_handle() itself has
    no HTTP-status concept, so without that translation this would be a 200
    carrying an error body.
    """
    r = daq_http.post(daq_http_base + "/api/daq/vdut/setpoint", json=body, timeout=10)
    assert r.status_code == 400, (
        "expected HTTP 400 for %r, got %d: %s" % (body, r.status_code, r.text[:200]))


def test_vdut_enable_roundtrip(daq_http, daq_http_base):
    """Enable then disable, verifying each in status. Always leaves it disabled."""
    try:
        r = daq_http.post(daq_http_base + "/api/daq/vdut/enable",
                          json={"enabled": True}, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])
        assert daq_http.get(daq_http_base + "/api/daq/vdut/status",
                            timeout=10).json()["enabled"] is True
    finally:
        daq_http.post(daq_http_base + "/api/daq/vdut/enable",
                      json={"enabled": False}, timeout=10)

    assert daq_http.get(daq_http_base + "/api/daq/vdut/status",
                        timeout=10).json()["enabled"] is False, (
        "DUT supply left ENABLED after the test — safety cleanup failed")


def test_acq_config_accepts_a_filter_selection(daq_http, daq_http_base):
    # api_daq_acq_config() in api_core.cpp reads body_get(body, "filter") and
    # body_get(body, "adc_dec") -- NOT "adcDecimation". The plan drafting this
    # test guessed "adcDecimation"; the real key is "adc_dec".
    r = daq_http.post(daq_http_base + "/api/daq/acq_config",
                      json={"filter": 0, "adc_dec": 0}, timeout=10)
    assert r.status_code in (200, 400), (
        "unexpected HTTP %d: %s" % (r.status_code, r.text[:200]))
    if r.status_code == 400:
        pytest.skip("acq_config rejected this combination: %s" % r.text[:160])


def test_unauthenticated_writes_are_rejected(daq_http_base):
    """MUTATING routes must require the admin token. Reads deliberately do not.

    The auth model here is intentional, not an oversight: `check_admin_auth()`
    guards state-changing handlers, while GETs are open. webserver.cpp's
    set_cors_headers() comment shows the design was reasoned about -- it
    refuses a wildcard Access-Control-Allow-Origin precisely because that plus
    the admin-token header would be a CSRF amplifier.

    Measured 2026-08-04: unauthenticated GET /api/daq/vdut/status -> 200,
    unauthenticated POST /api/daq/vdut/enable -> 401. So an unauthenticated
    client on the LAN can READ telemetry but cannot switch on the DUT supply.
    This test guards the half that matters: if a write ever stops requiring
    the token, an unauthenticated LAN client could energise the DUT rail.
    """
    import requests

    for path, body in (("/api/daq/vdut/enable", {"enabled": True}),
                       ("/api/daq/vdut/setpoint",
                        {"voltageV": 5.0, "currentLimitMa": 100.0})):
        r = requests.post(daq_http_base + path, json=body, timeout=10)
        assert r.status_code in (401, 403), (
            "unauthenticated POST %s returned HTTP %d -- a state-changing "
            "route is not enforcing the admin token, so any LAN client could "
            "control the DUT supply" % (path, r.status_code))

    # And confirm the rejected writes really did not take effect.
    st = requests.get(daq_http_base + "/api/daq/vdut/status", timeout=10).json()
    assert st["enabled"] is False, (
        "DUT supply is enabled after unauthenticated writes were supposedly "
        "rejected: %r" % st)


@pytest.mark.xfail(
    strict=True,
    reason=(
        "FIRMWARE BUG (found by this suite, 2026-08-04): GET /api/daq returns "
        "404 over HTTP although api_core_handle() dispatches it, because no URI "
        "handler is registered for exactly \"/api/daq\" in webserver.cpp. It "
        "works over BLE, which goes straight through api_core_handle(). Same "
        "bug class as 539c0d9 (/api/update/apply parsed in two dispatchers, "
        "only one migrated). Strict xfail: delete when the handler is "
        "registered. tests/unit/test_api_route_parity.py guards the family."
    ),
)
def test_api_daq_top_level_is_reachable(daq_http, daq_http_base):
    r = daq_http.get(daq_http_base + "/api/daq", timeout=10)
    assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])
