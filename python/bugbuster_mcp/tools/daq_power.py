"""
BugBuster MCP - DAQ HAT power-consumption analysis tools.

These drive the Power Profiler Pro HAT (ESP32-P4) end to end for a power
profiling run:

  control plane (BBP via the ESP32-S3)  - DUT supply, current range, sample
                                          rate, trigger/flag IO roles, arming
  data plane   (P4's own USB-HS bulk)   - the fused I/V sample stream

Captures are held in this process and analysed here, not on the P4: the HAT has
to keep the converters fed, so integrals, state segmentation and periodicity
detection all run host-side (see bugbuster.power_analysis).

Typical flow:
  1. daq_power_setup(voltage_mv=3300, current_limit_ma=500)
  2. daq_set_io_role(io=5, role="flag")            # from tools/daq.py
  3. daq_power_capture_start(duration_s=10)
  4. daq_power_capture_status(job_id) -> daq_power_capture_result(job_id)
  5. daq_power_report(capture_id, battery_capacity_mah=225)

Tools: daq_power_stream_status, daq_power_setup, daq_set_current_range,
       daq_set_sample_rate, daq_power_capture, daq_power_capture_start,
       daq_power_capture_status, daq_power_capture_result, daq_power_report,
       daq_power_window, daq_power_compare, daq_power_export,
       daq_power_list_captures, daq_power_drop_capture
"""

from __future__ import annotations

import csv
import threading
import time
import uuid
from typing import Any, Dict

from .. import session
from ..safety import require_hat

# DUT supply limits (mirror daq_config_registry schema bounds).
_VDUT_MIN_MV, _VDUT_MAX_MV = 1800, 20000
_ILIMIT_MIN_MA, _ILIMIT_MAX_MA = 100, 2500

# Registry RANGE_IDX options are ("A", "mA", "uA") - NOT the DaqRange enum order.
_RANGE_NAMES = {"a": 0, "amp": 0, "high": 0, "coarse": 0,
                "ma": 1, "milliamp": 1, "mid": 1,
                "ua": 2, "microamp": 2, "fine": 2, "low": 2}
_RANGE_LABELS = {0: "a (50 mohm shunt, ~50 mA - 3 A)",
                 1: "ma (2 ohm shunt, ~2 - 50 mA)",
                 2: "ua (51 ohm shunt, nA - ~2 mA)"}
_SAMPLE_RATES_SPS = (10_000, 50_000, 100_000, 250_000, 1_000_000)

# Bound a single capture so one bad duration cannot exhaust host memory.
# Samples are Python floats in lists, so budget ~64 B per sample across the
# current + voltage + meta arrays.
MAX_CAPTURE_DURATION_S = 900.0
MAX_CAPTURE_SAMPLES = 4_000_000
MAX_STORED_CAPTURES = 8

# ---------------------------------------------------------------------------
# Capture + job stores (in-process, MCP server lifetime)
# ---------------------------------------------------------------------------
_captures: Dict[str, Any] = {}
_jobs: Dict[str, Dict[str, Any]] = {}
_lock = threading.Lock()


def _store_capture(cap) -> str:
    capture_id = f"cap-{uuid.uuid4().hex[:8]}"
    with _lock:
        _captures[capture_id] = cap
        while len(_captures) > MAX_STORED_CAPTURES:
            _captures.pop(next(iter(_captures)))
    return capture_id


def _get_capture(capture_id: str):
    with _lock:
        cap = _captures.get(capture_id)
    if cap is None:
        raise ValueError(
            f"Unknown capture_id {capture_id!r}. It may have been evicted "
            f"(only the last {MAX_STORED_CAPTURES} captures are kept). "
            f"Use daq_power_list_captures.")
    return cap


def _open_stream():
    from bugbuster.daq_stream import DaqStream
    return DaqStream().open()


def _do_capture(duration_s: float, sample_limit: int, wait_for_trigger: bool,
                trigger_timeout_s: float) -> Dict[str, Any]:
    stream = _open_stream()
    try:
        cap = stream.capture(
            duration_s=duration_s,
            max_samples=sample_limit,
            wait_for_trigger=wait_for_trigger,
            trigger_timeout_s=trigger_timeout_s,
        )
    finally:
        stream.close()
    capture_id = _store_capture(cap)
    return {
        "capture_id": capture_id,
        "sample_count": cap.sample_count,
        "sample_rate_sps": cap.sample_rate,
        "duration_s": cap.duration_s,
        "dropped_samples": cap.dropped_samples,
        "markers": len(cap.markers),
        "next": f"Call daq_power_report(capture_id='{capture_id}') for the "
                f"energy/state analysis.",
    }


def register(mcp) -> None:

    # ---- data plane health ------------------------------------------------
    @mcp.tool()
    def daq_power_stream_status() -> dict:
        """
        Check the DAQ HAT measurement data plane (the ESP32-P4's own USB-HS
        port) before attempting a capture.

        Reports whether pyusb is installed, whether the P4 interface is
        enumerated, and - if reachable - the live device STATUS frame: active
        sample rate, current range, source enable, ADC health, stream drop
        counters and board temperatures.

        Returns: available, reason (when unavailable), device_status.
        """
        from bugbuster.daq_stream import DaqStream, daq_stream_present

        try:
            import usb.core  # noqa: F401
        except ImportError:
            return {"available": False,
                    "reason": "pyusb is not installed (pip install pyusb)."}
        if not daq_stream_present():
            return {"available": False,
                    "reason": "P4 data-plane interface (VID 0x303A PID 0x4001) "
                              "is not enumerated. Cable the HAT's own USB port "
                              "to this PC; the mainboard CDC port carries only "
                              "control traffic."}
        stream = DaqStream()
        try:
            stream.open()
            stream.start()
            status: dict = {}
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline and not status:
                for rec in stream.read_records(timeout_ms=200):
                    raw = getattr(rec, "raw", None)
                    if isinstance(raw, dict) and "sample_rate" in raw:
                        status = raw
                        break
            stream.stop()
            return {"available": True, "device_status": status}
        finally:
            stream.close()

    # ---- control plane: supply, range, rate -------------------------------
    @mcp.tool()
    def daq_power_setup(
        voltage_mv: int | None = None,
        current_limit_ma: int | None = None,
        enable: bool | None = None,
        autorange: bool | None = None,
        sample_rate_sps: int | None = None,
        reset_accumulators: bool = False,
    ) -> dict:
        """
        Prepare the DAQ HAT for a power-consumption run in one call.

        Sets the programmable DUT supply (SMU), the current-ranging mode and the
        acquisition rate. Any argument left as None is unchanged.

        Parameters:
        - voltage_mv: DUT supply voltage in millivolts (1800..20000).
        - current_limit_ma: supply current limit in milliamps (100..2500).
        - enable: True powers the DUT from the HAT; False turns the output off.
          Leave the supply off and use the HAT as an inline ammeter if the DUT
          is powered externally.
        - autorange: True lets the analog loop pick the shunt seamlessly
          (normal). False holds whatever range daq_set_current_range last set,
          which removes range-transition artefacts from a sensitive capture.
        - sample_rate_sps: 10000, 50000, 100000, 250000 or 1000000.
        - reset_accumulators: zero the device energy and charge counters so the
          run starts from 0.

        Returns: the settings that were applied.
        """
        from bugbuster.daq_config import DaqAction, DaqKey

        bb = session.get_client()
        require_hat(bb)
        applied: Dict[str, Any] = {}

        if voltage_mv is not None:
            if not (_VDUT_MIN_MV <= int(voltage_mv) <= _VDUT_MAX_MV):
                raise ValueError(f"voltage_mv must be {_VDUT_MIN_MV}..{_VDUT_MAX_MV}")
            bb.daq.set(DaqKey.DUT_VOLTAGE_MV, int(voltage_mv))
            applied["voltage_mv"] = int(voltage_mv)
        if current_limit_ma is not None:
            if not (_ILIMIT_MIN_MA <= int(current_limit_ma) <= _ILIMIT_MAX_MA):
                raise ValueError(
                    f"current_limit_ma must be {_ILIMIT_MIN_MA}..{_ILIMIT_MAX_MA}")
            bb.daq.set(DaqKey.DUT_ILIMIT_MA, int(current_limit_ma))
            applied["current_limit_ma"] = int(current_limit_ma)
        if autorange is not None:
            bb.daq.set(DaqKey.AUTORANGING, bool(autorange))
            applied["autorange"] = bool(autorange)
        if sample_rate_sps is not None:
            if int(sample_rate_sps) not in _SAMPLE_RATES_SPS:
                raise ValueError(
                    f"sample_rate_sps must be one of {list(_SAMPLE_RATES_SPS)}")
            bb.daq.set(DaqKey.SAMPLE_RATE_IDX,
                       _SAMPLE_RATES_SPS.index(int(sample_rate_sps)))
            applied["sample_rate_sps"] = int(sample_rate_sps)
        if enable is not None:
            bb.daq.set(DaqKey.SOURCE_ENABLE, bool(enable))
            applied["source_enabled"] = bool(enable)
        if reset_accumulators:
            bb.daq.action(DaqAction.ENERGY_RESET)
            bb.daq.action(DaqAction.CHARGE_RESET)
            applied["accumulators_reset"] = True

        if not applied:
            raise ValueError("Specify at least one setting to change.")
        return applied

    @mcp.tool()
    def daq_set_current_range(range_name: str = "ma",
                              autorange: bool = False) -> dict:
        """
        Pin the measurement front-end to one current range, or hand control
        back to the hardware autoranger.

        Ranges (shunt / usable span):
        - "ua"  51 ohm   nA .. ~2 mA     (finest resolution)
        - "ma"  2 ohm    ~2 .. 50 mA
        - "a"   50 mohm  ~50 mA .. 3 A

        Locking a range removes the settling artefacts that appear around a
        range transition, at the cost of clipping (the capture's meta flags will
        report saturated samples). Prefer autorange=True for a first look, then
        lock once the DUT's span is known.

        Parameters:
        - range_name: "ua" | "ma" | "a" (ignored when autorange=True).
        - autorange: True re-enables the seamless hardware autoranger.

        Returns: the resulting range and autorange state.
        """
        from bugbuster.daq_config import DaqKey

        bb = session.get_client()
        require_hat(bb)
        if autorange:
            bb.daq.set(DaqKey.AUTORANGING, True)
            return {"autorange": True,
                    "message": "Hardware autoranging enabled."}
        idx = _RANGE_NAMES.get(range_name.strip().lower())
        if idx is None:
            raise ValueError(
                f"Unknown range {range_name!r}. Use 'ua', 'ma' or 'a'.")
        bb.daq.set(DaqKey.AUTORANGING, False)
        bb.daq.set(DaqKey.RANGE_IDX, idx)
        return {"autorange": False, "range": _RANGE_LABELS[idx]}

    @mcp.tool()
    def daq_set_sample_rate(sample_rate_sps: int = 100000,
                            stream_decimation: int = 1,
                            i_understand_aliasing: bool = False) -> dict:
        """
        Set the acquisition rate.

        Pick the rate from the shortest feature that matters, not the run
        length: a 50 us radio ramp needs 250 ksps or better. The device reports
        the ODR it ACTUALLY applied, which may differ from the request - the
        driver clamps filter/decimation combinations the part cannot hit. Read
        the applied rate back from a capture, never assume the setpoint.

        Parameters:
        - sample_rate_sps: 10000, 50000, 100000, 250000 or 1000000. This is the
          ADC's own ODR and IS anti-alias filtered - the correct way to trade
          bandwidth for a longer or smaller capture.
        - stream_decimation: keep 1 of every N streamed samples. This is a naive
          drop with NO anti-alias filter: everything above the new Nyquist folds
          back into the band as spurious signal. Use sample_rate_sps instead.
          Values >1 require i_understand_aliasing=True.
        - i_understand_aliasing: required to set stream_decimation above 1.

        Returns: the applied rate, decimation and effective streamed rate.
        """
        from bugbuster.daq_config import DaqKey

        bb = session.get_client()
        require_hat(bb)
        if int(sample_rate_sps) not in _SAMPLE_RATES_SPS:
            raise ValueError(
                f"sample_rate_sps must be one of {list(_SAMPLE_RATES_SPS)}")
        if not (1 <= int(stream_decimation) <= 1000):
            raise ValueError("stream_decimation must be 1..1000")
        if int(stream_decimation) > 1 and not i_understand_aliasing:
            raise ValueError(
                "stream_decimation > 1 drops samples with no anti-alias filter, "
                "folding out-of-band noise and narrow current spikes back into "
                "the measurement. Lower sample_rate_sps instead, or pass "
                "i_understand_aliasing=True if you specifically want raw "
                "sub-sampling.")
        bb.daq.set(DaqKey.SAMPLE_RATE_IDX,
                   _SAMPLE_RATES_SPS.index(int(sample_rate_sps)))
        bb.daq.set(DaqKey.USB_DECIMATION, int(stream_decimation))
        return {
            "sample_rate_sps": int(sample_rate_sps),
            "stream_decimation": int(stream_decimation),
            "effective_stream_sps": int(sample_rate_sps) / int(stream_decimation),
            "note": "The device reports the ODR it actually applied in every "
                    "capture; check capture.sample_rate_sps.",
        }

    @mcp.tool()
    def daq_set_range_dwell(dwell_us: int | None = None) -> dict:
        """
        Read or set the autorange down-range dwell - the anti-thrash hold time.

        The minimum time the front-end stays in a coarser range before it may
        drop back to a finer one. The firmware's confirm counter is only a
        glitch filter, so without this a periodically bursting DUT down-ranges
        in every gap between bursts and up-ranges on the next one. Each of
        those transitions costs a settle window, and the samples inside it are
        flagged as settling and are not trustworthy.

        Set it LONGER than the DUT's burst repetition period. If a capture
        reports a high acquisition_quality.settling_share and a large
        range_changes count, this is the knob for it.

        The cost is resolution, not accuracy: while parked in a coarser range
        the low-current tail after a burst is measured on a bigger shunt. Lower
        it (or set 0 to disable) when a sub-uA sleep current matters more than
        transition artefacts.

        The value is persisted to NVS on the HAT and survives a power cycle.

        Parameters:
        - dwell_us: 0..1000000 microseconds. Omit to read the current value.

        Returns: dwell_us, and dwell_ms for convenience.
        """
        from bugbuster.daq_config import DaqKey

        bb = session.get_client()
        require_hat(bb)
        if dwell_us is not None:
            if not (0 <= int(dwell_us) <= 1_000_000):
                raise ValueError("dwell_us must be 0..1000000 (0 disables)")
            bb.daq.set(DaqKey.RANGE_DWELL_US, int(dwell_us))
        value = int(bb.daq.get(DaqKey.RANGE_DWELL_US))
        return {"dwell_us": value, "dwell_ms": value / 1000.0,
                "enabled": value > 0, "persisted": True}

    @mcp.tool()
    def daq_range_stability(
        dwell_us: int | None = None,
        settle_us: int | None = None,
        anti_flap: bool | None = None,
    ) -> dict:
        """
        Tune the three autorange stability controls. Omit all to just read.

        Use this when a capture reports a high
        acquisition_quality.settling_share or a large range_changes count.
        The two problems are independent:

        - dwell_us: minimum hold in a coarser range before dropping back. This
          controls HOW OFTEN transitions happen. Set it longer than the DUT's
          burst repetition period. 0 disables.
        - settle_us: how long after a switch samples are flagged as settling.
          This controls HOW MUCH each transition costs. It is a wall-clock
          figure because the transient is analog; it is floored internally at
          the converter's own filter settle (8 output samples), so it can never
          be set shorter than the ADC needs.
        - anti_flap: adaptive backoff. When an up-range arrives while the dwell
          is still running - the signature of oscillating across a boundary -
          the effective dwell doubles, up to 32x, and relaxes once stable.

        All three persist to NVS on the HAT.

        Returns: the resulting dwell_us, settle_us and anti_flap.
        """
        from bugbuster.daq_config import DaqKey

        bb = session.get_client()
        require_hat(bb)
        if dwell_us is not None:
            if not (0 <= int(dwell_us) <= 1_000_000):
                raise ValueError("dwell_us must be 0..1000000")
            bb.daq.set(DaqKey.RANGE_DWELL_US, int(dwell_us))
        if settle_us is not None:
            if not (0 <= int(settle_us) <= 65535):
                raise ValueError("settle_us must be 0..65535")
            bb.daq.set(DaqKey.RANGE_LOCK_US, int(settle_us))
        if anti_flap is not None:
            bb.daq.set(DaqKey.RANGE_FLAP, bool(anti_flap))
        return {
            "dwell_us": int(bb.daq.get(DaqKey.RANGE_DWELL_US)),
            "settle_us": int(bb.daq.get(DaqKey.RANGE_LOCK_US)),
            "anti_flap": bool(bb.daq.get(DaqKey.RANGE_FLAP)),
            "persisted": True,
        }

    # ---- capture ----------------------------------------------------------
    @mcp.tool()
    def daq_power_capture(
        duration_s: float = 2.0,
        wait_for_trigger: bool = False,
        trigger_timeout_s: float = 10.0,
    ) -> dict:
        """
        Record a block of DUT current and voltage from the DAQ HAT data plane.

        Blocks for the whole duration, so keep it short (<= 10 s); use
        daq_power_capture_start for longer runs. The samples stay in the MCP
        server - only a capture_id comes back, which daq_power_report,
        daq_power_window and daq_power_export then work on.

        With wait_for_trigger=True the capture discards everything before the
        TRIGGER marker, so t=0 is the DUT event. Configure the trigger IO with
        daq_set_io_role(role="trigger") and arm it with daq_arm() first.

        Parameters:
        - duration_s: capture length in seconds (0.001..120).
        - wait_for_trigger: start the window at the trigger marker.
        - trigger_timeout_s: give up if the trigger never fires.

        Returns: capture_id, sample_count, sample_rate_sps, duration_s,
        dropped_samples, marker count.
        """
        if not (0.001 <= duration_s <= MAX_CAPTURE_DURATION_S):
            raise ValueError(f"duration_s must be 0.001..{MAX_CAPTURE_DURATION_S}")
        if duration_s > 10.0:
            raise ValueError(
                "Captures longer than 10 s block the tool call - use "
                "daq_power_capture_start / _status / _result instead.")
        return _do_capture(duration_s, MAX_CAPTURE_SAMPLES,
                           wait_for_trigger, trigger_timeout_s)

    @mcp.tool()
    def daq_power_capture_start(
        duration_s: float = 30.0,
        wait_for_trigger: bool = False,
        trigger_timeout_s: float = 60.0,
    ) -> dict:
        """
        Start a long power capture in the background and return immediately.

        Use this for anything over ~10 s, and for trigger-armed captures where
        the DUT event may be minutes away. Poll daq_power_capture_status, then
        fetch with daq_power_capture_result.

        Parameters as daq_power_capture. Returns: job_id, status.

        A long run at a high ODR will hit the sample cap before the duration
        elapses (4,000,000 samples: ~30 s at 128 ksps, ~6 min at 10 ksps).
        Lower the ODR with daq_set_sample_rate for a long profile - do NOT
        reach for stream decimation, which aliases.
        """
        if not (0.001 <= duration_s <= MAX_CAPTURE_DURATION_S):
            raise ValueError(f"duration_s must be 0.001..{MAX_CAPTURE_DURATION_S}")
        job_id = str(uuid.uuid4())
        with _lock:
            _jobs[job_id] = {"status": "running", "result": None, "error": None,
                             "started": time.monotonic(),
                             "duration_s": duration_s}

        def _worker():
            try:
                res = _do_capture(duration_s, MAX_CAPTURE_SAMPLES,
                                  wait_for_trigger, trigger_timeout_s)
                with _lock:
                    _jobs[job_id].update(status="done", result=res)
            except Exception as exc:
                with _lock:
                    _jobs[job_id].update(status="error", error=str(exc))

        threading.Thread(target=_worker, daemon=True).start()
        return {"job_id": job_id, "status": "running",
                "expected_duration_s": duration_s}

    @mcp.tool()
    def daq_power_capture_status(job_id: str) -> dict:
        """
        Poll a background capture started by daq_power_capture_start.

        Returns: status ("running" | "done" | "error"), elapsed_s and, when
        finished, whether the result is ready to fetch.
        """
        with _lock:
            job = _jobs.get(job_id)
        if job is None:
            raise ValueError(f"Unknown job_id {job_id!r}.")
        out = {"job_id": job_id, "status": job["status"],
               "elapsed_s": time.monotonic() - job["started"],
               "expected_duration_s": job["duration_s"]}
        if job["status"] == "error":
            out["error"] = job["error"]
        return out

    @mcp.tool()
    def daq_power_capture_result(job_id: str) -> dict:
        """
        Fetch the result of a finished background capture. Returns the same
        fields as daq_power_capture. The job is consumed by this call.
        """
        with _lock:
            job = _jobs.get(job_id)
        if job is None:
            raise ValueError(f"Unknown job_id {job_id!r}.")
        if job["status"] == "error":
            raise RuntimeError(f"Capture failed: {job['error']}")
        if job["status"] != "done":
            raise RuntimeError(
                f"Capture {job_id!r} is still running. Poll "
                f"daq_power_capture_status first.")
        with _lock:
            _jobs.pop(job_id, None)
        return job["result"]

    # ---- analysis ---------------------------------------------------------
    @mcp.tool()
    def daq_power_report(
        capture_id: str,
        battery_capacity_mah: float | None = None,
        max_states: int = 6,
        min_state_duration_s: float = 0.0005,
        preview_points: int = 200,
        max_segments: int = 50,
    ) -> dict:
        """
        Full power-consumption analysis of a stored capture.

        Computes, host-side:
        - totals: energy (J / mWh / uWh) and charge (C / mAh / uAh) as a
          trapezoidal integral of v*i, plus mean/RMS/min/max/std current, mean
          and peak power, crest factor, and the measured DUT voltage. If any
          active shunt range lacks factory calibration, totals will include
          "uncalibrated": true and "uncalibrated_ranges": ["hi"|"mid"|"lo"]
          to flag that energy/charge/current values carry systematic offsets.
        - states: the DUT's discrete power modes, found by clustering
          log10(current) - sleep / idle / active / peak - each with mean and
          peak current, occurrence count, total and mean duration, and the
          share of run time and energy it owns.
        - transitions between states, and the duty cycle.
        - periodicity: interval, frequency and jitter of repeating activity
          bursts (advertising intervals, sensor wake-ups).
        - acquisition_quality: which shunt ranges were used, and the share of
          saturated or still-settling samples - read the warnings before
          trusting a number.
        - marker_windows: energy and charge between consecutive digital
          markers, when flag IOs were configured (see daq_set_io_role).
        - preview: a bounded min/max/mean decimation of the waveform.

        Parameters:
        - battery_capacity_mah: if given, adds a projected runtime.
        - max_states: cap on how many power modes to separate (2..10).
        - min_state_duration_s: segments shorter than this are absorbed into
          their neighbour. Raise it on a noisy DUT to stop transition ringing
          being reported as states.
        - preview_points / max_segments: output size limits.

        Returns: the report dict described above.
        """
        from bugbuster.power_analysis import analyze

        if not (2 <= max_states <= 10):
            raise ValueError("max_states must be 2..10")
        cap = _get_capture(capture_id)
        report = analyze(
            cap,
            max_states=max_states,
            min_state_duration_s=min_state_duration_s,
            battery_capacity_mah=battery_capacity_mah,
            preview_points=max(0, min(int(preview_points), 1000)),
            max_segments=max(1, min(int(max_segments), 500)),
        )
        report["capture_id"] = capture_id
        return report

    @mcp.tool()
    def daq_power_window(
        capture_id: str,
        start_s: float = 0.0,
        end_s: float | None = None,
        preview_points: int = 200,
    ) -> dict:
        """
        Zoom into one time window of a stored capture and analyse only that
        slice - the way to inspect a single burst after daq_power_report has
        pointed at it.

        Parameters:
        - start_s / end_s: window bounds in seconds from the capture start.
          end_s=None runs to the end.
        - preview_points: waveform preview resolution for the window.

        Returns: window bounds plus the same totals / states / preview
        structure as daq_power_report, computed over the slice only.
        """
        from bugbuster.daq_stream import PowerCapture
        from bugbuster.power_analysis import analyze

        cap = _get_capture(capture_id)
        if cap.sample_rate <= 0:
            raise ValueError("Capture has no valid sample rate.")
        s = max(0, int(start_s * cap.sample_rate))
        e = cap.sample_count if end_s is None else min(
            cap.sample_count, int(end_s * cap.sample_rate))
        if e <= s:
            raise ValueError(
                f"Empty window: start_s={start_s} end_s={end_s} against a "
                f"{cap.duration_s:.6f} s capture.")

        base = cap.start_index or 0
        sub = PowerCapture(
            sample_rate=cap.sample_rate,
            current=cap.current[s:e],
            voltage=cap.voltage[s:e],
            meta=bytearray(cap.meta[s:e]),
            markers=[m for m in cap.markers
                     if s <= (m.sample_index - base) < e],
            start_index=base + s,
            start_timestamp_us=cap.start_timestamp_us,
        )
        report = analyze(sub, preview_points=max(0, min(int(preview_points), 1000)))
        report["capture_id"] = capture_id
        report["window"] = {"start_s": s / cap.sample_rate,
                            "end_s": e / cap.sample_rate,
                            "samples": e - s}
        return report

    @mcp.tool()
    def daq_power_compare(capture_id_a: str, capture_id_b: str) -> dict:
        """
        Compare two captures - the before/after check for a firmware power
        optimisation.

        Normalises by duration, so runs of different lengths are still
        comparable: it reports average power, average current and energy per
        second for each, with absolute and percentage deltas.

        Returns: a, b, and delta (negative = B consumes less than A).
        """
        from bugbuster.power_analysis import integrate

        ca = _get_capture(capture_id_a)
        cb = _get_capture(capture_id_b)
        ra = integrate(ca.current, ca.voltage, ca.sample_rate)
        rb = integrate(cb.current, cb.voltage, cb.sample_rate)

        def _norm(r):
            d = r.get("duration_s") or 0.0
            return {
                "duration_s": d,
                "energy_j": r.get("energy_j", 0.0),
                "mean_power_w": r.get("power_mean_w", 0.0),
                "mean_current_a": r.get("current_mean_a", 0.0),
                "peak_current_a": r.get("current_max_a", 0.0),
                "energy_per_second_j": (r.get("energy_j", 0.0) / d) if d else 0.0,
            }

        a, b = _norm(ra), _norm(rb)
        delta: Dict[str, Any] = {}
        for k in ("mean_power_w", "mean_current_a", "peak_current_a",
                  "energy_per_second_j"):
            delta[k] = b[k] - a[k]
            delta[f"{k}_pct"] = ((b[k] - a[k]) / a[k] * 100.0) if a[k] else None
        return {"a": {"capture_id": capture_id_a, **a},
                "b": {"capture_id": capture_id_b, **b},
                "delta": delta}

    @mcp.tool()
    def daq_power_export(capture_id: str, path: str,
                         max_rows: int = 2_000_000) -> dict:
        """
        Write a stored capture to CSV on the host running the MCP server, for
        analysis in an external tool.

        Columns: t_s, current_a, voltage_v, power_w, range, source, saturated,
        settling.

        Parameters:
        - path: destination .csv file path.
        - max_rows: safety cap on rows written.

        Returns: path, rows written, whether the file was truncated.
        """
        from bugbuster.daq_stream import (META_RANGE_MASK, META_SATURATED,
                                          META_SETTLING, META_SOURCE_MASK,
                                          META_SOURCE_SHIFT, RANGE_NAMES,
                                          SOURCE_NAMES)

        cap = _get_capture(capture_id)
        if cap.sample_rate <= 0:
            raise ValueError("Capture has no valid sample rate.")
        n = min(cap.sample_count, int(max_rows))
        dt = 1.0 / cap.sample_rate
        with open(path, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["t_s", "current_a", "voltage_v", "power_w", "range",
                        "source", "saturated", "settling"])
            for k in range(n):
                i = cap.current[k]
                v = cap.voltage[k] if k < len(cap.voltage) else ""
                m = cap.meta[k] if k < len(cap.meta) else 0
                p = i * v if isinstance(v, float) and i == i else ""
                w.writerow([
                    f"{k * dt:.9f}", i, v, p,
                    RANGE_NAMES.get(m & META_RANGE_MASK, "unknown"),
                    SOURCE_NAMES.get((m >> META_SOURCE_SHIFT) & META_SOURCE_MASK, "?"),
                    int(bool(m & META_SATURATED)),
                    int(bool(m & META_SETTLING)),
                ])
        return {"path": path, "rows": n,
                "truncated": n < cap.sample_count}

    @mcp.tool()
    def daq_power_markers(capture_id: str, channel: int | None = None) -> dict:
        """
        List every digital event marker in a capture, with the energy consumed
        between consecutive markers.

        Markers come from the ESP32-S3 IOs tagged with daq_set_io_role(...,
        role="flag"), timestamped in the epoch shared with the P4, so each one
        maps to an exact sample index. A firmware GPIO toggle around a code
        region therefore brackets that region's energy precisely.

        Parameters:
        - channel: restrict to one mainboard IO (1..12). None = all.

        Returns: markers (channel, edge, kind, t_s, sample_offset), windows
        (energy/charge/duration between consecutive markers), and per-channel
        edge counts.
        """
        from bugbuster.power_analysis import segment_by_markers

        cap = _get_capture(capture_id)
        marks = cap.marker_dicts()
        if channel is not None:
            marks = [m for m in marks if m["channel"] == channel]
        counts: Dict[int, Dict[str, int]] = {}
        for m in marks:
            c = counts.setdefault(m["channel"], {"rising": 0, "falling": 0,
                                                 "trigger": 0})
            c[m["edge"]] += 1
            if m["kind"] == "trigger":
                c["trigger"] += 1
        return {
            "capture_id": capture_id,
            "marker_count": len(marks),
            "markers": marks[:500],
            "markers_truncated": len(marks) > 500,
            "per_channel": counts,
            "windows": segment_by_markers(cap, channel=channel)[:200],
        }

    @mcp.tool()
    def daq_power_list_captures() -> dict:
        """
        List the captures currently held in the MCP server, oldest first.
        Only the most recent few are kept; older ones are evicted.

        Returns: captures (capture_id, sample_count, duration_s, markers).
        """
        with _lock:
            items = list(_captures.items())
        return {
            "captures": [
                {"capture_id": cid, "sample_count": c.sample_count,
                 "sample_rate_sps": c.sample_rate, "duration_s": c.duration_s,
                 "markers": len(c.markers), "dropped_samples": c.dropped_samples}
                for cid, c in items
            ],
            "max_stored": MAX_STORED_CAPTURES,
        }

    @mcp.tool()
    def daq_power_drop_capture(capture_id: str) -> dict:
        """Free a stored capture's samples from the MCP server. Returns: success."""
        with _lock:
            existed = _captures.pop(capture_id, None) is not None
        return {"success": existed, "capture_id": capture_id}
