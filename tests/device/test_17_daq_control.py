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
