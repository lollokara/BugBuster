"""Tier 1 — the DAQ HAT's direct USB-HS surface.

Run with:
    PYTHONPATH=python python -m pytest tests/device/test_16_daq_stream.py -v --daq

These tests assert RELATIONAL properties only. The DUT load resistance is
unknown to the suite and must never appear in an assertion -- see
docs/superpowers/specs/2026-08-03-daq-test-suite-design.md, "Bench Setup".
"""
import struct
import time

import pytest

from tests.lib import daq_proto as P
from tests.lib.daq_capture import Capture

pytestmark = pytest.mark.requires_daq


# ---------------------------------------------------------------------------
# Shared helpers (used by tasks 9-12 as well)
# ---------------------------------------------------------------------------

def capture(link, seconds: float, settle: float = 0.2) -> Capture:
    """Start the stream, discard `settle` seconds, capture, stop.

    The settle window matters: right after CMD_START the device is still
    filling its ring and the first frames can carry a partial block, which
    would show up as a spurious sample-index discontinuity.
    """
    link.drain()
    link.start()
    if settle:
        time.sleep(settle)
        link.drain()
    cap = link.collect(seconds)
    link.stop()
    return cap


def wait_for(pred, timeout: float = 3.0, poll: float = 0.05) -> bool:
    """Poll `pred` until true or the timeout expires."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(poll)
    return False


# ---------------------------------------------------------------------------
# Link and protocol
# ---------------------------------------------------------------------------

def test_device_enumerates_and_streams_frames(daq_safe):
    cap = capture(daq_safe, 1.0)
    assert cap.bytes_rx > 0, "no bytes arrived from the P4"
    assert cap.frames, "no frames decoded"


def test_status_heartbeat_arrives(daq_safe):
    cap = capture(daq_safe, 2.0)
    assert cap.frames.get("STATUS", 0) >= 1, (
        "no STATUS heartbeat in 2s; frames seen: %r" % cap.frames)
    assert cap.last_status.get("sample_rate", 0) > 0


def test_every_frame_carries_the_expected_protocol_version(daq_safe):
    # The parser only accepts headers whose version == PROTO_VERSION, so any
    # frame at all decoding proves the version matches. A zero-frame capture
    # here means a version bump landed in firmware without one here.
    cap = capture(daq_safe, 1.0)
    total = sum(cap.frames.values())
    assert total > 0, (
        "no frames decoded at all — firmware may have bumped USB_PROTO_VERSION "
        "past the %d this suite understands" % P.PROTO_VERSION)


def test_no_resyncs_on_a_healthy_link(daq_safe):
    # A resync means the parser lost frame alignment. On USB bulk that should
    # never happen: it is the signature of the partial-send desync bug class.
    cap = capture(daq_safe, 2.0)
    assert cap.resyncs == 0, "%d resyncs — stream framing is desynchronising" % cap.resyncs


def test_sequence_is_contiguous_across_frames(daq_safe):
    cap = capture(daq_safe, 2.0)
    assert cap.seq_gaps == 0, (
        "%d seq gaps, %d frames lost (%.2f%%) — the device dropped frames it "
        "had already decided to send"
        % (cap.seq_gaps, cap.seq_lost, cap.frame_loss_pct))


def test_device_rejects_a_control_frame_with_a_bad_crc(daq_safe):
    """PC->device control frames carry a real CRC and are verified on receipt.

    Corrupt the CRC of a STOP sent while streaming: if the device honoured it,
    the stream would stop. It must keep streaming instead.
    """
    daq_safe.drain()
    daq_safe.start()
    time.sleep(0.3)

    frame = bytearray(P.build_control_frame(P.CMD_STOP))
    frame[-1] ^= 0xFF          # corrupt the CRC, leave everything else valid
    daq_safe.io.write(bytes(frame))

    time.sleep(0.3)
    cap = daq_safe.collect(1.0)
    daq_safe.stop()
    assert cap.wave_i_samples > 0, (
        "device acted on a control frame with an invalid CRC")


def test_device_honours_a_valid_stop(daq_safe):
    """Control against the previous test: a correctly-CRCed STOP must work."""
    daq_safe.drain()
    daq_safe.start()
    time.sleep(0.3)
    daq_safe.stop()
    time.sleep(0.3)
    daq_safe.drain()

    cap = daq_safe.collect(1.0)
    assert cap.wave_i_samples == 0, (
        "stream still emitting %d samples after a valid STOP" % cap.wave_i_samples)


# ---------------------------------------------------------------------------
# Stream lifecycle
# ---------------------------------------------------------------------------

def test_start_produces_waveform_data(daq_safe):
    cap = capture(daq_safe, 1.0)
    assert cap.wave_i_samples > 0, "no WAVE_I samples after CMD_START"
    assert cap.wave_v_samples > 0, "no WAVE_V samples after CMD_START"


@pytest.mark.slow
def test_three_start_stop_cycles_all_stream(daq_safe):
    """Regression guard for the teardown use-after-free fixed in 7cdef43.

    Fixed 20/150 ms teardown waits raced a consumer that could park 500 ms in
    the TCP retry, freeing the rings underneath it. The bug only manifests on a
    REPEATED cycle, which is exactly what a single manual bench check misses --
    the same shape as the OTA stack-leak bug (971714e), where update #2 in a
    boot always failed.
    """
    for cycle in range(3):
        cap = capture(daq_safe, 1.0)
        assert cap.wave_i_samples > 0, (
            "cycle %d produced no samples — teardown left the pipeline dead"
            % cycle)
        assert cap.resyncs == 0, "cycle %d desynced" % cycle
        time.sleep(0.3)


@pytest.mark.slow
def test_stop_during_active_read_does_not_wedge_the_device(daq_safe):
    """Stop mid-flight, then prove the device still starts cleanly."""
    daq_safe.drain()
    daq_safe.start()
    daq_safe.read(65536, 200)       # a read is in flight / just completed
    daq_safe.stop()                 # stop without draining first
    time.sleep(0.5)

    cap = capture(daq_safe, 1.0)
    assert cap.wave_i_samples > 0, "device wedged after a mid-read STOP"


def test_redundant_start_is_harmless(daq_safe):
    daq_safe.drain()
    daq_safe.start()
    daq_safe.start()
    time.sleep(0.3)
    cap = daq_safe.collect(1.0)
    daq_safe.stop()
    assert cap.wave_i_samples > 0
    assert cap.resyncs == 0


def test_redundant_stop_is_harmless(daq_safe):
    daq_safe.stop()
    daq_safe.stop()
    cap = capture(daq_safe, 1.0)
    assert cap.wave_i_samples > 0, "device did not recover from a double STOP"
