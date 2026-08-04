"""Tier 2 — the DAQ HAT's BBP control plane, via the S3 mainboard.

Run with:
    PYTHONPATH=python python -m pytest tests/device/test_17_daq_control.py -v \
        --daq --device-usb=/dev/cu.usbmodem1234561

Needs BOTH links: the S3's CDC0 for control, and the P4's USB-HS for the
stream the control plane is supposed to be describing.
"""
import time

import pytest

from bugbuster.daq_config import DaqAction, DaqKey, DaqTrigEdge, DaqTrigLogic, DaqTrigRole

pytestmark = pytest.mark.requires_daq_bbp


def test_daq_measure_returns_a_plausible_snapshot(daq_bbp):
    m = daq_bbp.daq.measure()
    for field in ("current_a", "voltage_v", "power_w", "range",
                  "streaming", "source_enabled"):
        assert field in m, "DAQ_MEASURE response missing %r: %r" % (field, m)

    assert -5.0 < m["current_a"] < 5.0, "implausible current %r" % m["current_a"]
    assert -1.0 < m["voltage_v"] < 25.0, "implausible voltage %r" % m["voltage_v"]
    assert int(m["range"]) in (0, 1, 2), "range out of 0..2: %r" % m["range"]


def test_daq_measure_power_is_consistent_with_v_times_i(daq_bbp):
    """p == v * i within tolerance, from a single snapshot.

    The three come back in one response but are computed separately, so this
    catches a units or scaling error in any one of them.
    """
    m = daq_bbp.daq.measure()
    expected = m["voltage_v"] * m["current_a"]
    tol = max(abs(expected) * 0.05, 1e-6)
    assert abs(m["power_w"] - expected) <= tol, (
        "power %.9g W != v*i %.9g W (v=%.6g, i=%.9g)"
        % (m["power_w"], expected, m["voltage_v"], m["current_a"]))


def test_daq_measure_reflects_the_streaming_state(daq_bbp, daq_link):
    """The S3's view of `streaming` must track what the P4 is actually doing."""
    daq_link.stop()
    time.sleep(0.5)
    assert daq_bbp.daq.measure()["streaming"] is False, (
        "S3 reports streaming while the P4 is stopped")

    daq_link.drain()
    daq_link.start()
    time.sleep(0.8)
    try:
        assert daq_bbp.daq.measure()["streaming"] is True, (
            "S3 reports not-streaming while the P4 is streaming")
    finally:
        daq_link.stop()


@pytest.mark.xfail(
    strict=True,
    reason=(
        "FIRMWARE BUG: bb.daq.get_all() deterministically fails with DeviceError "
        "0x11 (BBP_ERR_TIMEOUT), while single-key daq.get() and daq.measure() "
        "succeed over the same transport (verified live: AUTORANGING, "
        "DUT_VOLTAGE_MV, FFT_ENABLE, and measure() all returned correctly; "
        "get_all() failed every time with 'DeviceError 0x11'). Root cause is "
        "not a slow bulk read or a client timeout -- it's a buffer-size "
        "mismatch on the S3 side of the HAT UART bridge:\n"
        "  - Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.c handle_config_get_all() "
        "packs as many TLVs as fit into a HATP_MAX_PAYLOAD=240-byte page "
        "(s3_link.h) before returning, so a GET_ALL response is routinely "
        "tens to ~240 bytes.\n"
        "  - Firmware/ESP32/src/bbp/cmds/cmd_daq.cpp handler_daq_config() calls "
        "hat_request(..., rsp, &rsp_len, timeout_ms=300, sizeof(rsp)=240), "
        "implying it expects to receive up to 240 bytes back.\n"
        "  - But Firmware/ESP32/src/hat/hat.cpp hat_command_internal() reads the "
        "response into `uint8_t local_payload[HAT_FRAME_MAX_LEN]`, and "
        "HAT_FRAME_MAX_LEN is #defined to 32 in hat.h ('narrow' frame cap used "
        "by every command except OTA, which has its own wide 240-byte sender/"
        "receiver pair per the comment above hat_send_frame_wide()). "
        "hat_recv_frame() checks `if (len > HAT_FRAME_MAX_LEN) return 0` "
        "(hat.cpp ~line 244) BEFORE reading the payload -- so any P4 response "
        "over 32 bytes is silently rejected as if the HAT never answered, "
        "hat_command() retries once and still fails, and handler_daq_config() "
        "maps the resulting rsp_code==0 to -CMD_ERR_TIMEOUT (0x11). "
        "Single-key GET/SET/MEASURE responses are all well under 32 bytes, so "
        "they never hit this ceiling. Fix belongs on the S3 side: either use "
        "the existing hat_send_frame_wide()/HAT_OTA_WIDE_MAX-style wide path "
        "for BBP_CMD_DAQ_CONFIG's GET_ALL response, or have the P4 paginate "
        "GET_ALL replies to <=32 bytes per page (the wire protocol already "
        "supports pagination via next_idx; s3_link.c's page-fill loop just "
        "needs to stop filling at 32 bytes instead of 240). Strict xfail so "
        "it flips the moment either side is fixed."
    ),
)
def test_daq_config_get_all_returns_a_dict(daq_bbp):
    cfg = daq_bbp.daq.get_all()
    assert isinstance(cfg, dict) and cfg, "get_all returned %r" % (cfg,)


# ---------------------------------------------------------------------------
# Cross-layer: the S3's view vs the P4's own stream
# ---------------------------------------------------------------------------

def test_measure_agrees_with_the_p4_stream(daq_bbp, daq_safe):
    """DAQ_MEASURE and the USB stream must report the same physical reality.

    DAQ_MEASURE goes S3 -> HAT UART -> P4 and back; the stream comes straight
    off the P4's USB-HS endpoint. They are independent paths to the same
    quantity, so disagreement means a units error, a stale cache, or a broken
    bridge -- and nothing else in this repo checks it.

    Tolerance is deliberately loose: the two are not sampled at the same
    instant, so this is a sanity check on scale and sign, not on precision.
    A units error (mA vs A, mV vs V) is a 1000x discrepancy and is caught
    easily; genuine sampling skew is a few percent and is not flagged.
    """
    from tests.device.test_16_daq_stream import capture, mean_current

    cap = capture(daq_safe, 2.0, settle=0.5)
    stream_i = mean_current(cap)
    volts = cap.all_voltage()
    stream_v = sum(volts) / len(volts) if volts else None
    assert stream_v is not None, "no WAVE_V samples to compare against"

    m = daq_bbp.daq.measure()

    # Voltage: absolute floor of 50 mV absorbs offset on a near-zero rail.
    v_tol = max(abs(stream_v) * 0.20, 0.05)
    assert abs(m["voltage_v"] - stream_v) <= v_tol, (
        "voltage disagrees across layers: DAQ_MEASURE %.6g V vs stream %.6g V "
        "-- a ~1000x gap here means a mV/V units error"
        % (m["voltage_v"], stream_v))

    # Current: absolute floor of 1 mA, since an unloaded bench sits near zero
    # and a relative comparison there is meaningless.
    i_tol = max(abs(stream_i) * 0.20, 1e-3)
    assert abs(m["current_a"] - stream_i) <= i_tol, (
        "current disagrees across layers: DAQ_MEASURE %.9g A vs stream %.9g A "
        "-- a ~1000x gap here means a mA/A units error"
        % (m["current_a"], stream_i))


def test_measure_range_agrees_with_the_stream_meta(daq_bbp, daq_safe):
    """The range the S3 reports must match the range the samples were taken in."""
    from tests.device.test_16_daq_stream import capture
    from tests.lib.daq_records import meta_range

    cap = capture(daq_safe, 1.5, settle=0.5)
    meta = cap.all_meta()
    assert meta, "no meta bytes captured"
    stream_ranges = {meta_range(b) for b in meta}

    m = daq_bbp.daq.measure()
    assert int(m["range"]) in stream_ranges, (
        "DAQ_MEASURE reports range %d but the stream's samples were taken in "
        "range(s) %r" % (int(m["range"]), sorted(stream_ranges)))
