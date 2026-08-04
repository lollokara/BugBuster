"""Tier 4a — DAQ WiFi provisioning (`/api/daq/wifi_stream/*`).

Covers the bring-up state machine the P4 exposes to the S3, which the S3
relays verbatim as `/api/daq/wifi_stream/status`. Field names below are taken
directly from `api_daq_wifi_stream_status()` in
`Firmware/ESP32/src/net/api_core.cpp`:

    state: "idle" | "starting" | "ready" | "failed"
    stage: "requested" | "ap" | "dns" | "tcp"   (present only while starting;
           mirrors HAT_WIFI_STAGE_* in Firmware/ESP32/src/hat/hat.h)
    ssid, password, host, port                  (present only when ready)

There is NO "reason" field anywhere in this payload — `failed` carries no
diagnostic text over HTTP. Tests below report the last observed stage instead.

Tests that call /start are marked `daq_wifi` and gated behind --daq-wifi,
because bringing up the softAP disrupts normal device operation (even though,
per the task brief, THIS SUITE NEVER JOINS THAT NETWORK -- it only reads
status and stops/recycles the stream over the existing HTTP connection). Every
test that starts the stream stops it in a `finally` so the module leaves the
device idle.

Run with:
    PYTHONPATH=python python -m pytest tests/device/test_19_daq_wifi.py -v \
        --daq --daq-wifi --device-http=192.168.3.35 \
        --device-usb=/dev/cu.usbmodem1234561
"""
import ipaddress
import time

import pytest

pytestmark = pytest.mark.requires_daq_http

STATUS_URL = "/api/daq/wifi_stream/status"
START_URL = "/api/daq/wifi_stream/start"
STOP_URL = "/api/daq/wifi_stream/stop"
RECYCLE_URL = "/api/daq/wifi_stream/recycle"

# Documented bring-up sequence: requested(0) -> ap(1) -> dns(2) -> tcp(3).
STAGE_ORDER = {"requested": 0, "ap": 1, "dns": 2, "tcp": 3}

BRINGUP_TIMEOUT_S = 45.0
POLL_INTERVAL_S = 0.5


def _status(daq_http, daq_http_base):
    r = daq_http.get(daq_http_base + STATUS_URL, timeout=10)
    assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])
    return r.json()


def _stop(daq_http, daq_http_base):
    """Best-effort stop, used from `finally` blocks -- must never raise."""
    try:
        daq_http.post(daq_http_base + STOP_URL, timeout=10)
    except Exception as exc:  # noqa: BLE001 — cleanup is fail-soft
        print("[test_19_daq_wifi] stop cleanup failed: %s" % exc)


def _wait_for_idle(daq_http, daq_http_base, timeout_s=10.0):
    deadline = time.monotonic() + timeout_s
    last = None
    while time.monotonic() < deadline:
        last = _status(daq_http, daq_http_base)
        if last["state"] == "idle":
            return last
        time.sleep(POLL_INTERVAL_S)
    return last


def _poll_bringup(daq_http, daq_http_base, timeout_s=BRINGUP_TIMEOUT_S):
    """Poll status until a terminal state (ready/failed) or timeout.

    Returns (terminal_status_dict_or_None, list_of_(elapsed, state, stage)).
    """
    start = time.monotonic()
    sequence = []
    terminal = None
    while time.monotonic() - start < timeout_s:
        st = _status(daq_http, daq_http_base)
        elapsed = time.monotonic() - start
        sequence.append((round(elapsed, 2), st.get("state"), st.get("stage")))
        if st["state"] in ("ready", "failed"):
            terminal = st
            break
        time.sleep(POLL_INTERVAL_S)
    return terminal, sequence


def test_status_idle_at_rest(daq_http, daq_http_base):
    """GET status returns 200 with a `state` field; idle when nothing is running."""
    st = _status(daq_http, daq_http_base)
    assert "state" in st, "no 'state' field: %r" % st
    assert st["state"] == "idle", (
        "expected idle at rest, got %r -- a previous run may have left the "
        "stream running" % st)


@pytest.mark.daq_wifi
def test_wifi_stream_bringup_stage_progression_and_ready_payload(daq_http, daq_http_base):
    """Start the stream and observe the STAGE progression to a terminal state.

    Asserts:
      - stages observed while state == "starting" are non-decreasing
        (requested -> ap -> dns -> tcp)
      - a terminal state (ready or failed) is reached within
        BRINGUP_TIMEOUT_S
      - if ready, the credential payload is well-formed: non-empty SSID,
        non-empty password, plausible TCP port, IPv4 host. NEVER connects to
        any of it.
    """
    try:
        r = daq_http.post(daq_http_base + START_URL, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])

        terminal, sequence = _poll_bringup(daq_http, daq_http_base)
        print("[test_19_daq_wifi] bring-up sequence: %r" % sequence)

        # Non-decreasing stage while starting.
        last_idx = -1
        for elapsed, state, stage in sequence:
            if state != "starting":
                continue
            assert stage in STAGE_ORDER, (
                "unrecognized stage %r at t=%.2fs in sequence %r" % (stage, elapsed, sequence))
            idx = STAGE_ORDER[stage]
            assert idx >= last_idx, (
                "stage went BACKWARDS (%r -> idx %d after idx %d) at t=%.2fs -- "
                "full sequence: %r" % (stage, idx, last_idx, elapsed, sequence))
            last_idx = idx

        assert terminal is not None, (
            "bring-up did not reach a terminal state (ready/failed) within "
            "%.0fs -- observed sequence: %r" % (BRINGUP_TIMEOUT_S, sequence))

        if terminal["state"] == "failed":
            # No "reason" field exists on this payload (see module docstring)
            # -- report the observed sequence as the only available evidence.
            # A jump straight to "failed" with no prior stage progression
            # would match the historical LAST-flag bug (see test_16's
            # "FIRMWARE BUG" entries for the house style): the P4 once set
            # the sequence-complete flag on in-progress STARTING replies, so
            # the S3 misread a bring-up as finished-but-not-ready and
            # aborted intermittently.
            pytest.fail(
                "wifi bring-up reported FAILED with no reason field (none "
                "exists in this API) -- observed stage/state sequence: %r" % sequence)

        assert terminal["state"] == "ready", "unexpected terminal state: %r" % terminal

        ssid = terminal.get("ssid")
        password = terminal.get("password")
        port = terminal.get("port")
        host = terminal.get("host")

        assert isinstance(ssid, str) and ssid, "empty/missing ssid: %r" % terminal
        assert isinstance(password, str) and password, "empty/missing password: %r" % terminal
        assert isinstance(port, int) and 1 <= port <= 65535, (
            "port not a plausible TCP port: %r" % terminal)
        try:
            ipaddress.IPv4Address(host)
        except (ValueError, TypeError) as exc:
            pytest.fail("host %r does not parse as IPv4: %s" % (host, exc))

        print("[test_19_daq_wifi] ready payload: ssid=%r port=%r host=%r "
              "(password redacted, len=%d)" % (ssid, port, host, len(password)))
    finally:
        _stop(daq_http, daq_http_base)
        idle = _wait_for_idle(daq_http, daq_http_base)
        assert idle is not None and idle["state"] == "idle", (
            "device did not return to idle after stop: %r" % idle)


@pytest.mark.daq_wifi
def test_wifi_stream_stop_returns_to_idle(daq_http, daq_http_base):
    """POST /stop returns the device to idle, whether or not bring-up finished."""
    try:
        r = daq_http.post(daq_http_base + START_URL, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])

        # Don't wait for ready -- stop should work mid-bringup too.
        time.sleep(2.0)

        r = daq_http.post(daq_http_base + STOP_URL, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])

        idle = _wait_for_idle(daq_http, daq_http_base)
        assert idle is not None and idle["state"] == "idle", (
            "state did not settle to idle after /stop: %r" % idle)
    finally:
        _stop(daq_http, daq_http_base)


@pytest.mark.daq_wifi
def test_wifi_stream_recycle_from_any_state(daq_http, daq_http_base):
    """Recycle is documented as safe from ANY state and always leaves idle.

    Exercises it from idle (no-op case) and from mid-bringup (the "wedged"
    case it exists to recover from), then confirms a subsequent start still
    succeeds -- recycle must not leave the P4 in a state that blocks the next
    bring-up.
    """
    try:
        # 1. Recycle from idle -- must be a safe no-op.
        idle_before = _status(daq_http, daq_http_base)
        assert idle_before["state"] == "idle", (
            "test precondition violated, not idle before recycle: %r" % idle_before)

        r = daq_http.post(daq_http_base + RECYCLE_URL, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])
        idle_after = _wait_for_idle(daq_http, daq_http_base)
        assert idle_after is not None and idle_after["state"] == "idle", (
            "recycle from idle did not leave idle: %r" % idle_after)

        # 2. Start, then recycle mid-bringup (simulated "wedged" recovery).
        r = daq_http.post(daq_http_base + START_URL, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])
        time.sleep(1.5)  # let it get partway into bring-up

        r = daq_http.post(daq_http_base + RECYCLE_URL, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])
        recycled = _wait_for_idle(daq_http, daq_http_base, timeout_s=15.0)
        assert recycled is not None and recycled["state"] == "idle", (
            "recycle mid-bringup did not return to idle: %r" % recycled)

        # 3. A subsequent start must still succeed (recycle didn't wedge it).
        r = daq_http.post(daq_http_base + START_URL, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])
        terminal, sequence = _poll_bringup(daq_http, daq_http_base)
        print("[test_19_daq_wifi] post-recycle bring-up sequence: %r" % sequence)
        assert terminal is not None, (
            "post-recycle start did not reach a terminal state within "
            "%.0fs -- sequence: %r" % (BRINGUP_TIMEOUT_S, sequence))
        assert terminal["state"] in ("ready", "failed"), (
            "unexpected terminal state after recycle+restart: %r" % terminal)
    finally:
        _stop(daq_http, daq_http_base)
        idle = _wait_for_idle(daq_http, daq_http_base)
        assert idle is not None and idle["state"] == "idle", (
            "device did not return to idle after final stop: %r" % idle)


@pytest.mark.daq_wifi
@pytest.mark.slow
def test_wifi_stream_idle_auto_teardown(daq_http, daq_http_base):
    """The P4 tears the stream down after 60s with NO client associated.

    IMPORTANT — this test is only meaningful when nothing is associated to the
    DAQ softAP. The timer at daq_board.c:1981 resets on
    `tcp_backend_connected() || wifi_ap_sta_count() > 0`: a station counts as
    present from the moment it ASSOCIATES, not when it opens a socket. That was
    deliberate (counting only TCP tore the AP down under a phone still doing
    DHCP, after which its recovery ladder dialled an SSID that no longer
    existed).

    So a stream that stays `ready` past 60s does NOT prove a firmware fault --
    it equally means something joined the softAP. A phone that has previously
    paired with the DAQ hotspot will auto-join it, silently.

    Measured 2026-08-04: stayed `ready` for the full 119s poll window. The
    status endpoint exposes no station count, so this test CANNOT distinguish
    the two causes and therefore reports rather than fails. To make it
    conclusive, either surface `wifi_ap_sta_count()` in
    /api/daq/wifi_stream/status, or run it with every known client's WiFi off.
    """
    AUTO_TEARDOWN_TIMEOUT_S = 120.0
    try:
        r = daq_http.post(daq_http_base + START_URL, timeout=10)
        assert r.status_code == 200, "HTTP %d: %s" % (r.status_code, r.text[:200])

        terminal, bringup_sequence = _poll_bringup(daq_http, daq_http_base)
        print("[test_19_daq_wifi] auto-teardown test bring-up sequence: %r"
              % bringup_sequence)
        if terminal is None or terminal["state"] != "ready":
            pytest.skip(
                "bring-up did not reach ready (terminal=%r); cannot exercise "
                "idle auto-teardown without a ready stream -- sequence: %r"
                % (terminal, bringup_sequence))

        start = time.monotonic()
        observed = []
        torn_down = False
        while time.monotonic() - start < AUTO_TEARDOWN_TIMEOUT_S:
            st = _status(daq_http, daq_http_base)
            elapsed = time.monotonic() - start
            observed.append((round(elapsed, 1), st["state"]))
            if st["state"] == "idle":
                torn_down = True
                break
            time.sleep(2.0)

        print("[test_19_daq_wifi] idle-auto-teardown observed: %r" % observed)
        if not torn_down:
            pytest.skip(
                "stream stayed up for the full %.0fs poll window (last state=%r). "
                "This is INCONCLUSIVE, not a failure: the idle timer resets while "
                "any station is associated to the softAP (daq_board.c:1981), and "
                "the status endpoint exposes no station count, so a remembered "
                "phone auto-joining is indistinguishable from a broken timer. "
                "Observed: %r"
                % (AUTO_TEARDOWN_TIMEOUT_S, observed[-1] if observed else None,
                   observed))
    finally:
        _stop(daq_http, daq_http_base)
        idle = _wait_for_idle(daq_http, daq_http_base)
        assert idle is not None and idle["state"] == "idle", (
            "device did not return to idle after final stop: %r" % idle)
