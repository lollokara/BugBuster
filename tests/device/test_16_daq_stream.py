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


# ---------------------------------------------------------------------------
# Rate, decimation and record integrity
# ---------------------------------------------------------------------------

ADAQ_BASE_SPS = 8_192_000

# End-to-end floor at the stress point (odr 64 + dspdecim 1). Measured 127,258
# Sa/s after the DSP-tail float conversion in 8504122 (up from 73,514). The
# threshold sits well below the measured ceiling so run-to-run variance cannot
# flake the gate, while a real regression -- which was a 40% drop -- still fails.
STRESS_MIN_SPS = 120_000


def test_wave_i_header_count_matches_payload_length(daq_safe):
    """A decoded WaveI proves count, the f32 array and the meta array agree.

    _decode_wave returns None on any mismatch, and Capture only appends
    non-None records, so a truncated frame would show as a decoded-frame count
    below the raw frame count.
    """
    cap = capture(daq_safe, 1.0)
    assert cap.wave_i, "no WAVE_I records decoded"
    assert len(cap.wave_i) == cap.frames["WAVE_I"], (
        "%d of %d WAVE_I frames failed to decode — header count disagrees with "
        "payload length" % (cap.frames["WAVE_I"] - len(cap.wave_i),
                            cap.frames["WAVE_I"]))
    for w in cap.wave_i:
        assert len(w.samples) == w.count
        assert len(w.meta) == w.count


def test_wave_v_has_no_meta_array_and_native_decimation(daq_safe):
    cap = capture(daq_safe, 1.0)
    assert cap.wave_v, "no WAVE_V records decoded"
    assert len(cap.wave_v) == cap.frames["WAVE_V"]
    for w in cap.wave_v:
        assert w.decimation == 1, (
            "WAVE_V must stream at native ODR, got decimation=%d" % w.decimation)
        assert len(w.samples) == w.count


def test_wave_i_sample_index_is_contiguous(daq_safe):
    """Consecutive WAVE_I blocks must tile the sample index with no hole."""
    cap = capture(daq_safe, 2.0)
    assert len(cap.wave_i) >= 2, "need at least 2 WAVE_I blocks to check tiling"
    assert cap.contiguous_wave_i, (
        "sample-index hole between WAVE_I blocks — samples were dropped "
        "between capture and transmit")


def test_meta_bytes_decode_to_plausible_values(daq_safe):
    from tests.lib.daq_records import (meta_range, meta_saturated, meta_settling,
                                       meta_source)

    cap = capture(daq_safe, 1.0)
    meta = cap.all_meta()
    assert meta, "no meta bytes captured"

    ranges = {meta_range(m) for m in meta}
    assert ranges <= {P.RANGE_HI, P.RANGE_MID, P.RANGE_LO}, (
        "meta reported range ids outside 0..2: %r" % sorted(ranges))
    assert {meta_source(m) for m in meta} <= {0, 1, 2, 3}

    # Saturation and settling are legal but should be rare in steady state.
    sat = sum(1 for m in meta if meta_saturated(m))
    assert sat < 0.5 * len(meta), (
        "%d/%d samples flagged saturated — the front end is clipping"
        % (sat, len(meta)))
    settling = sum(1 for m in meta if meta_settling(m))
    assert settling < 0.5 * len(meta), (
        "%d/%d samples flagged settling — the range latch is thrashing"
        % (settling, len(meta)))


def test_wave_headers_report_a_sane_sample_rate(daq_safe):
    cap = capture(daq_safe, 1.0)
    for w in cap.wave_i:
        assert 0 < w.sample_rate <= ADAQ_BASE_SPS, (
            "implausible sample_rate %d in a WAVE_I header" % w.sample_rate)


def test_measured_rate_matches_the_header_rate(daq_safe):
    """The rate the device claims and the rate it actually delivers must agree.

    This is the assertion that catches a pipeline losing samples silently:
    headers keep advertising 64 kSPS while the host receives far fewer.
    """
    cap = capture(daq_safe, 3.0)
    assert cap.wave_i, "no WAVE_I records"
    claimed = cap.wave_i[-1].sample_rate / max(1, cap.wave_i[-1].decimation)
    measured = cap.wave_i_sps
    assert measured == pytest.approx(claimed, rel=0.15), (
        "headers advertise %.0f Sa/s but only %.0f arrived (%.1f%% of claimed)"
        % (claimed, measured, 100.0 * measured / claimed if claimed else 0.0))


@pytest.mark.slow
@pytest.mark.parametrize("ratio", [64, 128, 256])
def test_odr_ratio_produces_the_expected_capture_rate(daq_safe, p4_console, ratio):
    """`odr N` is an oversampling RATIO: per-channel SPS = 8192000 / N.

    Reprogramming goes through the P4 console because it rewrites both ADAQs
    over SPI, which requires the capture task to release the bus first.
    """
    p4_console.set_odr(ratio)
    time.sleep(0.5)

    cap = capture(daq_safe, 2.0, settle=0.5)
    expected_per_ch = ADAQ_BASE_SPS / ratio
    claimed = cap.wave_i[-1].sample_rate if cap.wave_i else 0
    assert claimed == pytest.approx(expected_per_ch, rel=0.1), (
        "odr %d should give %.0f SPS/ch, headers report %d"
        % (ratio, expected_per_ch, claimed))


@pytest.mark.slow
def test_stress_point_throughput_meets_the_regression_floor(daq_safe, p4_console):
    """End-to-end throughput gate at the stress point (odr 64, dspdecim 1).

    This is the number the DSP-tail float conversion moved from 73,514 to
    127,258 Sa/s (8504122). Measured end to end against a real consumer, which
    matters: with no host attached emit_frame_inplace() early-returns and the
    entire back half of the pipeline never runs, so a device-side-only
    measurement here would be measuring a stub.
    """
    p4_console.set_odr(64)
    p4_console.cmd("dspdecim 1", deadline=10.0)
    time.sleep(0.5)

    try:
        cap = capture(daq_safe, 5.0, settle=1.0)
        assert cap.wave_i_sps >= STRESS_MIN_SPS, (
            "throughput regression: %.0f Sa/s at odr 64 + dspdecim 1, floor is "
            "%d (last known good: 127,258 after commit 8504122)"
            % (cap.wave_i_sps, STRESS_MIN_SPS))
        st = cap.last_status
        assert st.get("drop_fine", 0) == 0, (
            "drop_fine=%d — ADC pairing is losing samples under load"
            % st.get("drop_fine"))
    finally:
        p4_console.cmd("dspdecim 8", deadline=10.0)


# ---------------------------------------------------------------------------
# DSP correctness
# ---------------------------------------------------------------------------

@pytest.mark.slow
def test_rms_squared_equals_std_squared_plus_mean_squared(daq_safe):
    """The variance identity, checked against independent accumulators.

    rms^2 == std^2 + mean^2 holds for any signal. It is a strong check here
    because the P4 computes rms, std and mean through SEPARATE accumulation
    paths -- so agreement means all three survived the blocked (two-level)
    summation introduced in 8504122, where float accumulators fold into double
    totals every PDSP_FOLD_N=512 samples.

    The P4 FPU is SINGLE precision: every double op on a per-sample path is
    software-emulated. A regression here typically means someone reintroduced
    a double into the hot path, or broke the fold boundary.
    """
    cap = capture(daq_safe, 5.0, settle=1.0)
    assert cap.stats, "no STATS records in 5s"
    s = cap.stats[-1]

    for name, blk in (("i", s.i), ("v", s.v), ("p", s.p)):
        if blk.count == 0:
            continue
        lhs = blk.rms ** 2
        rhs = blk.std ** 2 + blk.mean ** 2
        scale = max(abs(lhs), abs(rhs), 1e-12)
        assert abs(lhs - rhs) / scale < 1e-4, (
            "%s: rms^2 (%.9g) != std^2 + mean^2 (%.9g), rel err %.3g — the "
            "statistics accumulators disagree"
            % (name, lhs, rhs, abs(lhs - rhs) / scale))


def test_stats_min_le_mean_le_max(daq_safe):
    cap = capture(daq_safe, 2.0)
    assert cap.stats, "no STATS records"
    s = cap.stats[-1]
    for name, blk in (("i", s.i), ("v", s.v), ("p", s.p)):
        if blk.count == 0:
            continue
        assert blk.min <= blk.mean <= blk.max, (
            "%s: min=%.6g mean=%.6g max=%.6g is not ordered"
            % (name, blk.min, blk.mean, blk.max))
        assert blk.std >= 0.0, "%s: negative std %.6g" % (name, blk.std)


@pytest.mark.slow
def test_charge_integral_equals_mean_current_times_elapsed(daq_safe):
    """d(charge) == mean_i * d(elapsed), checked across two ENERGY records.

    Charge is accumulated as a per-sample trapezoid; mean_i comes from the
    statistics path. They are independent computations of the same physical
    quantity, so agreement validates the energy/charge trapezoids AND the
    dt-factoring introduced when dt moved out of the per-sample work and got
    applied once per 512-sample block.

    Both accumulators are reset first so they cover the SAME interval. STATS
    reports a cumulative mean over every sample since the last reset, which can
    be millions of samples old; differencing ENERGY over a 6 s window and
    comparing against that all-time mean only agrees while the current happens
    to be constant. Without the reset this test passes on a steady bench and
    fails the moment anything moves V_DUT -- measured 17% disagreement that way,
    which is a property of the test, not of the firmware.
    """
    daq_safe.reset_stats()
    daq_safe.reset_energy()
    time.sleep(0.3)

    cap = capture(daq_safe, 6.0, settle=1.0)
    assert len(cap.energy) >= 2, (
        "need >= 2 ENERGY records to difference, got %d" % len(cap.energy))
    assert cap.stats, "no STATS records to source mean current from"

    first, last = cap.energy[0], cap.energy[-1]
    d_charge = last.charge_c - first.charge_c
    d_elapsed = last.elapsed_s - first.elapsed_s
    assert d_elapsed > 0.5, "elapsed barely advanced (%.3fs)" % d_elapsed

    mean_i = cap.stats[-1].i.mean
    expected = mean_i * d_elapsed

    # Absolute floor guards the near-zero-current case, where a relative
    # comparison is meaningless and would flake on an unloaded bench.
    tol = max(abs(expected) * 0.05, 1e-6)
    assert abs(d_charge - expected) <= tol, (
        "charge integral %.9g C disagrees with mean_i * elapsed %.9g C "
        "(mean_i=%.9g A, elapsed=%.3f s)"
        % (d_charge, expected, mean_i, d_elapsed))


@pytest.mark.slow
def test_elapsed_tracks_wall_clock(daq_safe):
    """The device's own elapsed accumulator must track real time.

    elapsed cannot be a period-count * dt, because dt changes with rate and
    dspdecim -- so this catches the class of bug where a rate change silently
    corrupts the time base.
    """
    daq_safe.drain()
    daq_safe.start()
    time.sleep(0.5)
    daq_safe.drain()

    t0 = time.monotonic()
    cap = daq_safe.collect(6.0)
    wall = time.monotonic() - t0
    daq_safe.stop()

    assert len(cap.energy) >= 2, "need >= 2 ENERGY records"
    d_elapsed = cap.energy[-1].elapsed_s - cap.energy[0].elapsed_s
    assert d_elapsed == pytest.approx(wall, rel=0.10, abs=0.5), (
        "device elapsed advanced %.3fs over %.3fs of wall clock" % (d_elapsed, wall))


def test_reset_energy_zeroes_the_accumulators(daq_safe):
    daq_safe.drain()
    daq_safe.start()
    time.sleep(1.5)                    # let energy/charge accumulate
    before = daq_safe.collect(1.0)
    assert before.energy, "no ENERGY records before reset"
    assert before.energy[-1].elapsed_s > 0

    daq_safe.reset_energy()
    time.sleep(0.3)
    daq_safe.drain()
    after = daq_safe.collect(1.5)
    daq_safe.stop()

    assert after.energy, "no ENERGY records after reset"
    e = after.energy[0]
    assert e.elapsed_s < before.energy[-1].elapsed_s, (
        "elapsed did not restart after CMD_RESET_ENERGY (%.3f -> %.3f)"
        % (before.energy[-1].elapsed_s, e.elapsed_s))
    assert abs(e.charge_c) < abs(before.energy[-1].charge_c) + 1e-9


def test_reset_stats_restarts_the_sample_count(daq_safe):
    daq_safe.drain()
    daq_safe.start()
    time.sleep(1.5)
    before = daq_safe.collect(1.0)
    assert before.stats, "no STATS records before reset"

    daq_safe.reset_stats()
    time.sleep(0.2)
    daq_safe.drain()
    after = daq_safe.collect(1.0)
    daq_safe.stop()

    assert after.stats, "no STATS records after reset"
    assert after.stats[0].i.count < before.stats[-1].i.count, (
        "stats sample count did not restart after CMD_RESET_STATS "
        "(%d -> %d)" % (before.stats[-1].i.count, after.stats[0].i.count))


# ---------------------------------------------------------------------------
# FFT
# ---------------------------------------------------------------------------

def test_fft_config_produces_records_with_the_requested_bin_count(daq_safe):
    daq_safe.set_fft(nbins=256, source=0, window=1, enabled=True)
    try:
        cap = capture(daq_safe, 3.0, settle=0.5)
        assert cap.fft, "no FFT records after CMD_FFT_CONFIG enabled=1"
        for f in cap.fft:
            assert f.nbins == 256, "requested 256 bins, got %d" % f.nbins
            assert len(f.bins) == f.nbins
    finally:
        daq_safe.set_fft(nbins=256, source=0, window=1, enabled=False)


@pytest.mark.xfail(
    strict=True,
    reason=(
        "FIRMWARE BUG (found by this suite, 2026-08-04): disabling the FFT stops "
        "COMPUTATION but not TRANSMISSION. spectrum_push() correctly gates on "
        "s->enabled (dsp/spectrum.c:134), so no new FFTs are computed -- but the "
        "emit path in board/daq_board.c:836 only checks `nb > 0` and keeps "
        "shipping the last computed magnitude buffer forever. Measured: 30 FFT "
        "records in 3s while disabled, of which 1/30 were distinct (i.e. all "
        "stale); enabled, 20/20 were distinct. Costs stream bandwidth and feeds "
        "a host stale spectra it explicitly asked to stop receiving. Fix: gate "
        "the send on spectrum enabled state. This xfail is strict, so it will "
        "FAIL once the firmware is fixed -- delete the marker then."
    ),
)
def test_fft_can_be_disabled(daq_safe):
    daq_safe.set_fft(nbins=256, source=0, window=1, enabled=False)
    cap = capture(daq_safe, 2.0, settle=0.5)
    assert not cap.fft, "%d FFT records arrived while disabled" % len(cap.fft)


@pytest.mark.xfail(
    strict=True,
    reason=(
        "FIRMWARE BUG (found by this suite, 2026-08-04): the FFT header's window "
        "byte is hardcoded to SPEC_WIN_HANN in board/daq_board.c:843 rather than "
        "reporting the configured window. Measured: requesting window=0 (RECT) "
        "or window=2 (BLACKMAN_HARRIS) both report 1 (HANN). The window is "
        "correctly APPLIED to the computation (spectrum_configure stores it and "
        "build_window uses it) -- only the reported id is wrong, so a host cannot "
        "tell which window produced a spectrum. Strict xfail: delete when fixed."
    ),
)
def test_fft_header_reports_the_configured_window(daq_safe):
    for window in (0, 2):          # RECT and BLACKMAN_HARRIS; neither is HANN(1)
        daq_safe.set_fft(nbins=256, source=0, window=window, enabled=True)
        cap = capture(daq_safe, 1.5, settle=0.5)
        assert cap.fft, "no FFT records for window=%d" % window
        reported = {f.window for f in cap.fft}
        assert reported == {window}, (
            "requested window=%d but the FFT header reports %r" % (window, reported))


def test_changing_the_window_changes_the_spectrum(daq_safe):
    """Different window functions must produce different magnitudes.

    If the window id is ignored, both captures return identical spectra.
    """
    spectra = {}
    try:
        for window in (0, 1):
            daq_safe.set_fft(nbins=256, source=0, window=window, enabled=True)
            cap = capture(daq_safe, 2.0, settle=0.5)
            assert cap.fft, "no FFT records for window=%d" % window
            spectra[window] = cap.fft[-1].bins
    finally:
        daq_safe.set_fft(nbins=256, source=0, window=1, enabled=False)

    assert spectra[0] != spectra[1], (
        "window 0 and window 1 produced byte-identical spectra — the window "
        "selection is being ignored")


@pytest.mark.slow
def test_spectrum_keeps_updating_over_thousands_of_ffts(daq_safe):
    """Regression guard for the Welch spectrum freeze fixed in 7cdef43.

    The spectrum silently stopped updating after a few thousand FFTs. A short
    capture cannot see it -- the failure needs sustained running, which is
    precisely why it survived manual bench checks. This test runs long enough
    to cross that threshold and asserts the spectrum is still changing at the
    END of the window, not just that FFT records are still arriving (they kept
    arriving during the bug; they were just stale).
    """
    daq_safe.set_fft(nbins=256, source=0, window=1, enabled=True)
    try:
        daq_safe.drain()
        daq_safe.start()
        time.sleep(0.5)
        daq_safe.drain()

        cap = daq_safe.collect(20.0)
        daq_safe.stop()

        assert len(cap.fft) >= 100, (
            "only %d FFT records in 20s — too few to exercise the freeze"
            % len(cap.fft))

        # Compare consecutive spectra in the LAST 20% of the capture. On live
        # analogue input no two consecutive spectra are ever bit-identical, so
        # any identical adjacent pair late in the run means the buffer froze.
        tail = cap.fft[int(len(cap.fft) * 0.8):]
        assert len(tail) >= 5
        changed = sum(1 for a, b in zip(tail, tail[1:]) if a.bins != b.bins)
        assert changed >= len(tail) // 2, (
            "spectrum stopped updating: only %d of %d consecutive pairs "
            "differed in the final fifth of a %d-FFT run"
            % (changed, len(tail) - 1, len(cap.fft)))
    finally:
        daq_safe.set_fft(nbins=256, source=0, window=1, enabled=False)


# ---------------------------------------------------------------------------
# Range control and SMU source
# ---------------------------------------------------------------------------

def mean_current(cap) -> float:
    """Mean fused current over a capture, from raw samples."""
    vals = cap.all_current()
    return sum(vals) / len(vals) if vals else 0.0


@pytest.mark.parametrize("range_id", [P.RANGE_HI, P.RANGE_MID, P.RANGE_LO])
def test_range_lock_is_reflected_in_meta_and_status(daq_safe, range_id):
    daq_safe.set_range_lock(range_id)
    time.sleep(0.3)
    cap = capture(daq_safe, 1.5, settle=0.5)

    from tests.lib.daq_records import meta_range
    meta = cap.all_meta()
    assert meta, "no meta bytes captured while locked to %s" % P.RANGE_NAMES[range_id]

    observed = {meta_range(m) for m in meta}
    assert observed == {range_id}, (
        "locked to %s but meta reported ranges %r"
        % (P.RANGE_NAMES[range_id], sorted(P.RANGE_NAMES.get(r, r) for r in observed)))

    st = cap.last_status
    assert st.get("range_locked") == 1, "STATUS does not report range_locked"
    assert st.get("range") == range_id


def test_range_unlock_restores_autorange(daq_safe):
    daq_safe.set_range_lock(P.RANGE_MID)
    time.sleep(0.3)
    daq_safe.set_range_lock(P.RANGE_AUTO)
    time.sleep(0.3)

    cap = capture(daq_safe, 1.5, settle=0.5)
    assert cap.last_status.get("range_locked") == 0, (
        "range still locked after CMD_RANGE_LOCK 0xFF")


# Below this current at ~19 V the DUT terminals are effectively open: 1e-5 A at
# 19 V is ~2 MOhm, far beyond any real bench load. Measured with NO load
# attached: ~1e-7 A. Note the detection MUST be made with the supply ON --
# with it OFF the front end reports a ~650 uA offset artifact that is larger
# than the real signal and would invert the comparison.
LOAD_DETECT_A = 1e-5


@pytest.fixture(scope="session")
def daq_load(daq_link):
    """(measured_current, load_present) -- probed once per session.

    Several tests here can only mean anything with a load wired across the DUT
    terminals. Rather than let each guess from noise, probe once at high
    V_DUT and let them skip explicitly with a real number in the message.
    """
    daq_link.set_source(19.0, 2.0, True)
    time.sleep(1.0)
    cap = capture(daq_link, 1.5, settle=0.5)
    i = abs(mean_current(cap))
    daq_link.set_source(0.0, SMU_ILIMIT_MIN_A, False)
    time.sleep(0.3)
    return i, i >= LOAD_DETECT_A


def _require_load(daq_load):
    i, present = daq_load
    if not present:
        pytest.skip(
            "no load attached to the DUT terminals (|I| = %.3g A at 19 V, "
            "below the %.0e A detection threshold). This test measures how "
            "current RESPONDS to voltage, which an open circuit cannot show. "
            "Wire a resistor across the DUT terminals to enable it -- a few "
            "hundred ohms also unlocks the autorange-transition test."
            % (i, LOAD_DETECT_A))


def test_source_enable_produces_current_and_disable_removes_it(daq_safe, daq_load):
    """Relational, not absolute: current with the supply on must exceed
    current with it off. The load value is unknown and irrelevant.

    No-load guard: with an open circuit at the DUT terminals, current is ~0
    regardless of the supply state and the comparison would be decided by
    noise rather than by anything real. Skip explicitly, reporting both
    measured values, rather than pass or fail vacuously.
    """
    _require_load(daq_load)

    daq_safe.set_source(0.0, SMU_ILIMIT_MIN_A, False)
    time.sleep(0.8)
    off = capture(daq_safe, 1.5, settle=0.5)
    i_off = abs(mean_current(off))

    daq_safe.set_source(10.0, 0.5, True)
    time.sleep(0.8)
    on = capture(daq_safe, 1.5, settle=0.5)
    i_on = abs(mean_current(on))

    daq_safe.set_source(0.0, 0.05, False)

    if abs(i_on - i_off) < 1e-6:
        pytest.skip(
            "no measurable current change with the supply on vs off "
            "(off=%.9g A, on=%.9g A) -- no load is attached to the DUT "
            "terminals" % (i_off, i_on))

    assert i_on > i_off, (
        "enabling the DUT supply did not increase current "
        "(off=%.9g A, on=%.9g A) — is anything wired to the DUT terminals?"
        % (i_off, i_on))


def test_source_disabled_settles_near_zero(daq_safe):
    daq_safe.set_source(0.0, 0.05, False)
    time.sleep(1.0)
    cap = capture(daq_safe, 2.0, settle=0.5)

    st = cap.last_status
    assert st.get("source_enabled") == 0, "STATUS still reports source_enabled"

    # 1 mA is generous: it is far below anything a real load draws at the
    # voltages this suite uses, while leaving room for leakage and offset.
    assert abs(mean_current(cap)) < 1e-3, (
        "%.9g A flowing with the DUT supply disabled — leakage or a stuck "
        "range offset" % mean_current(cap))


def test_vdut_setpoint_is_reflected_in_status(daq_safe):
    daq_safe.set_source(12.0, 0.5, True)
    time.sleep(0.8)
    cap = capture(daq_safe, 1.5, settle=0.5)
    daq_safe.set_source(0.0, 0.05, False)

    st = cap.last_status
    assert st.get("vdut_set") == pytest.approx(12.0, rel=0.05), (
        "programmed V_DUT 12.0 V, STATUS reports %.3f" % st.get("vdut_set", -1))
    assert st.get("ilimit_set") == pytest.approx(0.5, rel=0.05)


def test_measured_voltage_tracks_the_setpoint(daq_safe):
    """WAVE_V must follow the programmed V_DUT.

    Relational: we compare two setpoints against each other, never against an
    absolute reference.
    """
    readings = {}
    for vdut in (5.0, 15.0):
        daq_safe.set_source(vdut, 0.5, True)
        time.sleep(1.0)
        cap = capture(daq_safe, 1.5, settle=0.5)
        vals = cap.all_voltage()
        assert vals, "no WAVE_V samples at V_DUT=%.1f" % vdut
        readings[vdut] = sum(vals) / len(vals)

    daq_safe.set_source(0.0, 0.05, False)

    assert readings[15.0] > readings[5.0], (
        "measured voltage did not increase with the setpoint "
        "(5V -> %.4f, 15V -> %.4f)" % (readings[5.0], readings[15.0]))
    ratio = readings[15.0] / readings[5.0] if readings[5.0] else 0.0
    assert 2.0 < ratio < 4.5, (
        "measured voltage ratio %.2f is implausible for a 3x setpoint change "
        "(5V -> %.4f, 15V -> %.4f)" % (ratio, readings[5.0], readings[15.0]))


# SMU limits from Firmware/DAQ_HAT/ESP32P4/include/config.h.
SMU_VDUT_MIN = 1.76
SMU_VDUT_MAX = 19.94
SMU_ILIMIT_MIN_A = 0.05
SMU_ILIMIT_FULLSCALE_A = 2.636


@pytest.mark.parametrize("vdut,ilimit,exp_vdut,exp_ilimit", [
    (0.5, 0.5, SMU_VDUT_MIN, 0.5),                     # below SMU_VDUT_MIN
    (25.0, 0.5, SMU_VDUT_MAX, 0.5),                    # above SMU_VDUT_MAX
    (10.0, 0.001, 10.0, SMU_ILIMIT_MIN_A),             # below SMU_ILIMIT_MIN_A
    (10.0, 5.0, 10.0, SMU_ILIMIT_FULLSCALE_A),         # above FULLSCALE
])
def test_out_of_range_setpoints_are_clamped_to_the_documented_limits(
        daq_safe, vdut, ilimit, exp_vdut, exp_ilimit):
    """Over USB, out-of-range setpoints are CLAMPED to the config.h bounds.

    This pins the clamp bounds against the firmware constants, so a change to
    SMU_VDUT_MIN/MAX or SMU_ILIMIT_MIN_A/FULLSCALE_A that is not intended to be
    user-visible shows up here.

    INCONSISTENCY WORTH KNOWING (found by this suite, 2026-08-04): the two
    control paths disagree about out-of-range handling.

      * USB CMD_SET_SOURCE (this path) CLAMPS silently --
        smu.c:181 `s->ilimit_set = clampf(amps, MIN, FULLSCALE)`, and the
        voltage path likewise resolves to a boundary code.
      * The HAT/HTTP/BLE VDUT path (HAT_CMD_DAQ_VDUT_SETPOINT ->
        api_daq_vdut_setpoint) explicitly RE-VALIDATES and REJECTS with an
        error, per the daq-hat manifest.

    So the same user action produces different results depending on the client:
    the desktop app drives USB and gets a silent clamp; iOS drives HTTP/BLE and
    gets a 400. Measured clamps: 0.5 V -> 1.760, 25.0 V -> 19.940,
    0.001 A -> 0.050, 5.0 A -> 2.636 -- every one exactly on a documented limit.
    """
    daq_safe.set_source(10.0, 0.5, True)
    time.sleep(0.8)
    capture(daq_safe, 1.0, settle=0.3)

    daq_safe.set_source(vdut, ilimit, True)
    time.sleep(0.8)
    after = capture(daq_safe, 1.0, settle=0.3).last_status

    daq_safe.set_source(0.0, SMU_ILIMIT_MIN_A, False)

    assert after.get("vdut_set") == pytest.approx(exp_vdut, rel=0.02), (
        "requested %.3f V, expected a clamp to %.3f, STATUS reports %.3f"
        % (vdut, exp_vdut, after.get("vdut_set", -1)))
    assert after.get("ilimit_set") == pytest.approx(exp_ilimit, rel=0.02), (
        "requested %.4f A, expected a clamp to %.4f, STATUS reports %.4f"
        % (ilimit, exp_ilimit, after.get("ilimit_set", -1)))


@pytest.mark.slow
def test_current_is_linear_in_vdut(daq_safe, daq_load_ohms, daq_load):
    """Sweep V_DUT and fit I = V/R. Assert R^2 > 0.99.

    This is the suite's substitute for absolute-accuracy verification. It
    exercises SMU setpoint programming, both ADAQ chains, current fusion and
    the DSP tail in one assertion, and it needs NO knowledge of the load: a
    resistive load makes I proportional to V whatever R happens to be. It fails
    loudly on a nonlinearity, a stuck ADC code, or a range-transition
    discontinuity.
    """
    _require_load(daq_load)

    points = []
    try:
        for vdut in (2.0, 5.0, 8.0, 11.0, 14.0, 17.0, 19.0):
            daq_safe.set_source(vdut, 2.0, True)
            time.sleep(0.6)
            cap = capture(daq_safe, 1.0, settle=0.3)
            vals = cap.all_voltage()
            v_meas = sum(vals) / len(vals) if vals else vdut
            points.append((v_meas, mean_current(cap)))
    finally:
        daq_safe.set_source(0.0, 0.05, False)

    assert len(points) == 7

    # Least-squares fit I = a*V + b, then coefficient of determination.
    n = len(points)
    sx = sum(v for v, _ in points)
    sy = sum(i for _, i in points)
    sxx = sum(v * v for v, _ in points)
    sxy = sum(v * i for v, i in points)
    denom = n * sxx - sx * sx
    assert denom != 0, "degenerate voltage sweep — all setpoints read the same"
    a = (n * sxy - sx * sy) / denom
    b = (sy - a * sx) / n

    mean_i = sy / n
    ss_tot = sum((i - mean_i) ** 2 for _, i in points)
    ss_res = sum((i - (a * v + b)) ** 2 for v, i in points)

    if ss_tot < 1e-18:
        pytest.skip(
            "current did not vary measurably across the V_DUT sweep — is a "
            "load wired to the DUT terminals? (--daq-load-ohms=%s)"
            % daq_load_ohms)

    r2 = 1.0 - ss_res / ss_tot
    assert r2 > 0.99, (
        "I vs V_DUT is not linear (R^2=%.5f, slope=%.6g A/V, implied R=%.1f ohm). "
        "Points: %r" % (r2, a, (1.0 / a) if a else float("inf"), points))

    assert a > 0, "current decreases as V_DUT rises (slope %.6g A/V)" % a


@pytest.mark.slow
def test_current_is_monotonic_in_vdut(daq_safe, daq_load):
    """Each voltage step must not decrease the current.

    No-load guard: with an open circuit, all readings sit near the noise
    floor and the 5% monotonicity tolerance would be decided by noise rather
    than a real inversion. Skip explicitly, reporting the observed span,
    before running the comparison loop.
    """
    _require_load(daq_load)

    readings = []
    try:
        for vdut in (3.0, 7.0, 11.0, 15.0, 19.0):
            daq_safe.set_source(vdut, 2.0, True)
            time.sleep(0.6)
            readings.append((vdut, abs(mean_current(capture(daq_safe, 1.0, settle=0.3)))))
    finally:
        daq_safe.set_source(0.0, 0.05, False)

    readings_current = [i for _, i in readings]
    span = max(readings_current) - min(readings_current)
    if span < 1e-6:
        pytest.skip(
            "current stayed within %.9g A across the whole V_DUT sweep -- no "
            "load is attached to the DUT terminals. Readings: %r"
            % (span, readings))

    for (v0, i0), (v1, i1) in zip(readings, readings[1:]):
        # 5% tolerance absorbs noise at a range boundary without permitting a
        # real inversion.
        assert i1 >= i0 * 0.95, (
            "current fell from %.9g A at %.1f V to %.9g A at %.1f V — full "
            "sweep: %r" % (i0, v0, i1, v1, readings))


@pytest.mark.slow
def test_autorange_transitions_during_a_sweep(daq_safe, daq_load_ohms, daq_load):
    """Sweep V_DUT under autorange and require at least one range change.

    Skips explicitly (never passes vacuously) if the wired load keeps current
    inside a single range for the whole 1.76-19.94 V span. Boundaries are
    800 uA (HI->MID) and 37 mA (MID->LO).
    """
    from tests.lib.daq_records import meta_range

    _require_load(daq_load)

    daq_safe.set_range_lock(P.RANGE_AUTO)
    seen = set()
    currents = []
    try:
        for vdut in (2.0, 6.0, 10.0, 14.0, 19.0):
            daq_safe.set_source(vdut, 2.0, True)
            time.sleep(0.6)
            cap = capture(daq_safe, 1.0, settle=0.3)
            seen |= {meta_range(m) for m in cap.all_meta()}
            currents.append(abs(mean_current(cap)))
    finally:
        daq_safe.set_source(0.0, 0.05, False)

    if len(seen) < 2:
        pytest.skip(
            "load stayed within a single range (%s) across the whole sweep; "
            "current spanned %.6g..%.6g A. Autorange transitions need a load "
            "that crosses 800 uA or 37 mA — roughly a few hundred ohms. "
            "(--daq-load-ohms=%s)"
            % (", ".join(P.RANGE_NAMES.get(r, str(r)) for r in sorted(seen)),
               min(currents), max(currents), daq_load_ohms))

    assert seen <= {P.RANGE_HI, P.RANGE_MID, P.RANGE_LO}
