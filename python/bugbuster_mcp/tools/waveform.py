"""
BugBuster MCP — Waveform generation and signal capture tools.

Tools: start_waveform, stop_waveform, capture_adc_snapshot, capture_logic_analyzer
"""

from __future__ import annotations
import time
import math
import logging
from .. import session
from ..safety import (
    require_analog_io, require_io_mode,
    require_la_ready, validate_la_config, check_faults_post,
)
from ..config import (
    MAX_SNAPSHOT_DURATION_S, DEFAULT_SNAPSHOT_DURATION_S,
    SNAPSHOT_POLL_INTERVAL_S, MAX_SNAPSHOT_SAMPLES,
    WAVEFORM_PREVIEW_POINTS,
    LA_DEFAULT_RATE_HZ, LA_DEFAULT_DEPTH, LA_DEFAULT_CHANNELS,
    LA_CAPTURE_TIMEOUT_S,
)

log = logging.getLogger(__name__)

_WAVEFORM_TYPES = {"sine": 0, "square": 1, "triangle": 2, "sawtooth": 3}


def register(mcp) -> None:
    mcp.tool()(start_waveform)
    mcp.tool()(stop_waveform)
    mcp.tool()(capture_adc_snapshot)
    mcp.tool()(capture_logic_analyzer)


def start_waveform(
    io:        int,
    waveform:  str,
    freq_hz:   float,
    amplitude: float,
    offset:    float = 0.0,
) -> dict:
    """
    Start a periodic waveform on an ANALOG_OUT IO port.

    The IO must be configured as ANALOG_OUT with configure_io first.
    Only one waveform can run at a time. The firmware continuously updates
    the DAC at the requested frequency (0.01-100 Hz).

    Parameters:
    - io: IO number — must be 1, 4, 7, or 10.
    - waveform: Shape — "sine", "square", "triangle", or "sawtooth".
    - freq_hz: Frequency in Hz (0.01 to 100.0).
    - amplitude: Peak amplitude in volts. The waveform spans
                 [offset - amplitude, offset + amplitude].
    - offset: DC offset in volts (default 0.0).

    Example — 1 Hz sine wave from 0 V to 5 V: amplitude=2.5, offset=2.5.

    Returns: io, waveform, freq_hz, amplitude, offset, success.
    """
    require_analog_io(io, "start_waveform")
    wtype = waveform.lower()
    if wtype not in _WAVEFORM_TYPES:
        raise ValueError(
            f"Unknown waveform {waveform!r}. Use: sine, square, triangle, sawtooth."
        )
    if not (0.01 <= freq_hz <= 100.0):
        raise ValueError(
            f"Frequency {freq_hz} Hz out of range. Supported: 0.01-100 Hz."
        )
    if amplitude < 0:
        raise ValueError("Amplitude must be non-negative.")
    if amplitude + abs(offset) > 12.0:
        raise ValueError(
            f"Peak value {abs(offset) + amplitude:.2f} V exceeds DAC maximum of 12.0 V."
        )

    # Verify IO is configured as ANALOG_OUT — the HAL must have already set
    # the MUX to route this IO to the AD74416H DAC channel. Only one path
    # can be active at a time; configure_io enforces this.
    hal = session.get_hal()
    from bugbuster.hal import PortMode
    require_io_mode(hal, io, PortMode.ANALOG_OUT, "start_waveform")

    bb = session.get_client()
    # Analog IOs map to AD74416H channels: IO1→ch0, IO4→ch1, IO7→ch2, IO10→ch3
    _IO_TO_CHANNEL = {1: 0, 4: 1, 7: 2, 10: 3}
    ch = _IO_TO_CHANNEL[io]

    from bugbuster.constants import WaveformType, OutputMode
    bb.start_waveform(
        channel=ch,
        waveform=WaveformType(_WAVEFORM_TYPES[wtype]),
        freq_hz=freq_hz,
        amplitude=amplitude,
        offset=offset,
        mode=OutputMode.VOLTAGE,
    )
    warnings = check_faults_post(bb)
    res = {
        "io":        io,
        "channel":   ch,
        "waveform":  wtype,
        "freq_hz":   freq_hz,
        "amplitude": amplitude,
        "offset":    offset,
        "success":   True,
    }
    if warnings:
        res["warnings"] = warnings
    return res


def stop_waveform() -> dict:
    """
    Stop the waveform generator.

    The DAC output holds at the last generated value after stopping.
    Call write_voltage to set a specific DC level afterwards.

    Returns: success.
    """
    bb = session.get_client()
    bb.stop_waveform()
    warnings = check_faults_post(bb)
    res = {"success": True, "message": "Waveform stopped. DAC holds last value."}
    if warnings:
        res["warnings"] = warnings
    return res


def capture_adc_snapshot(
    io:          int,
    duration_s:  float = DEFAULT_SNAPSHOT_DURATION_S,
    n_samples:   int   = 0,
) -> dict:
    """
    Capture multiple ADC readings from an ANALOG_IN IO and return statistics.

    The IO must be configured as ANALOG_IN with configure_io first.
    Polls the ADC repeatedly over the specified duration and computes
    statistical summary and a downsampled waveform preview.

    Parameters:
    - io: IO number — must be 1, 4, 7, or 10.
    - duration_s: Capture duration in seconds (default 1.0, max 10.0).
    - n_samples: Override sample count (0 = determine from duration).

    Returns: io, n_samples, duration_s, min_v, max_v, mean_v, stddev_v,
             peak_to_peak_v, frequency_hz (estimated), waveform_preview (list).
    """
    require_analog_io(io, "capture_adc_snapshot")
    from bugbuster.hal import PortMode
    hal = session.get_hal()

    # Verify mode
    current_mode = hal._io_mode.get(io)
    if current_mode != PortMode.ANALOG_IN:
        raise ValueError(
            f"IO {io} is in mode {current_mode.name if current_mode else 'UNCONFIGURED'}. "
            f"Configure as ANALOG_IN first."
        )

    duration_s = min(float(duration_s), MAX_SNAPSHOT_DURATION_S)
    if duration_s <= 0:
        raise ValueError("duration_s must be positive.")

    # Collect samples
    samples = []
    t_start = time.monotonic()
    t_end   = t_start + duration_s
    max_s   = n_samples if n_samples > 0 else MAX_SNAPSHOT_SAMPLES

    while time.monotonic() < t_end and len(samples) < max_s:
        try:
            v = hal.read_voltage(io)
            samples.append(v)
        except Exception as e:
            log.warning("ADC read error: %s", e)
            break
        time.sleep(SNAPSHOT_POLL_INTERVAL_S)

    if not samples:
        raise RuntimeError(f"No samples captured from IO {io}.")

    actual_duration = time.monotonic() - t_start
    n = len(samples)
    mean_v      = sum(samples) / n
    min_v       = min(samples)
    max_v       = max(samples)
    variance    = sum((s - mean_v) ** 2 for s in samples) / n
    stddev_v    = math.sqrt(variance)
    peak_to_peak = max_v - min_v

    # Rough frequency estimate: count zero-crossings of AC component
    freq_hz = None
    if n >= 10 and peak_to_peak > 0.01:
        crossings = sum(
            1 for i in range(1, n)
            if (samples[i - 1] - mean_v) * (samples[i] - mean_v) < 0
        )
        if crossings >= 2:
            sample_rate = n / actual_duration
            freq_hz = round((crossings / 2) * sample_rate / n, 2)

    # Downsample to preview
    step = max(1, n // WAVEFORM_PREVIEW_POINTS)
    preview = [round(samples[i], 5) for i in range(0, n, step)][:WAVEFORM_PREVIEW_POINTS]

    return {
        "io":             io,
        "n_samples":      n,
        "duration_s":     round(actual_duration, 3),
        "sample_rate_hz": round(n / actual_duration, 1) if actual_duration > 0 else 0,
        "min_v":          round(min_v, 6),
        "max_v":          round(max_v, 6),
        "mean_v":         round(mean_v, 6),
        "stddev_v":       round(stddev_v, 6),
        "peak_to_peak_v": round(peak_to_peak, 6),
        "frequency_hz":   freq_hz,
        "waveform_preview": preview,
    }


def capture_logic_analyzer(
    channels:     int   = LA_DEFAULT_CHANNELS,
    rate_hz:      int   = LA_DEFAULT_RATE_HZ,
    depth:        int   = LA_DEFAULT_DEPTH,
    trigger_type: str   = "none",
    trigger_ch:   int   = 0,
) -> dict:
    """
    Capture digital waveforms using the HAT logic analyzer (RP2040 PIO).

    Requires the HAT expansion board. Captures 1-4 channels at up to
    100 MHz. Uses the PIO state machine for accurate timing.

    Parameters:
    - channels: Number of channels to capture (1-4).
    - rate_hz: Sample rate in Hz (default 1 MHz, max 100 MHz).
    - depth: Number of samples to capture (default 100,000, max 622,592).
    - trigger_type: Trigger condition — "none", "rising", "falling",
                    "both", "high", or "low" (default "none").
    - trigger_ch: Channel to use for trigger (0-3, default 0).

    Returns: channels, rate_hz, depth, duration_ms, edges (per channel),
             frequency_hz (per channel), protocol_hints, timing_diagram.
    """
    bb = session.get_client()
    require_la_ready(bb)
    validate_la_config(channels, rate_hz, depth)

    _TRIGGER_MAP = {"none": 0, "rising": 1, "falling": 2, "both": 3, "high": 4, "low": 5}
    trig = trigger_type.lower()
    if trig not in _TRIGGER_MAP:
        raise ValueError(
            f"Unknown trigger_type {trigger_type!r}. "
            f"Use: none, rising, falling, both, high, low."
        )

    # Configure
    bb.hat_la_configure(channels=channels, rate_hz=rate_hz, depth=depth)
    if trig != "none":
        bb.hat_la_set_trigger(_TRIGGER_MAP[trig], trigger_ch)

    # Arm and trigger
    bb.hat_la_arm()

    if trig == "none":
        # Force-trigger immediately
        bb.hat_la_force()

    # Wait for capture completion
    t_start = time.monotonic()
    actual_rate = rate_hz
    while time.monotonic() - t_start < LA_CAPTURE_TIMEOUT_S:
        st = bb.hat_la_get_status()
        # Phase 0 compatibility: handles "DONE", "done", state=3, and "actualRateHz"
        is_done = (st.get("state") == 3 or 
                   st.get("stateName") == "DONE" or 
                   st.get("state_name") == "done" or
                   st.get("done"))
        
        if is_done:
            actual_rate = st.get("actual_rate_hz") or st.get("actualRateHz") or st.get("clockHz") or rate_hz
            break
        time.sleep(0.05)
    else:
        bb.hat_la_stop()
        raise RuntimeError(
            f"Logic analyzer capture timed out after {LA_CAPTURE_TIMEOUT_S} s. "
            f"No trigger event detected. Try trigger_type='none' for an immediate capture."
        )

    # Read and decode
    raw = bb.hat_la_read_all()
    if not raw:
        raise RuntimeError("Logic analyzer returned empty capture.")

    ch_data = bb.hat_la_decode(raw, channels=channels)

    # Build per-channel edge summary
    duration_ms = (depth / actual_rate) * 1000
    ch_edges = []
    ch_freq  = []
    for chi, edges in enumerate(ch_data):
        if chi >= channels:
            break
        # edges is list of (timestamp_s, level) tuples from decode
        transitions = len(edges) if edges else 0
        ch_edges.append({
            "channel":     chi,
            "transitions": transitions,
            "edges":       edges[:100] if edges else [],  # cap at 100 for readability
        })
        # Estimate frequency from transition rate
        if transitions >= 2 and duration_ms > 0:
            freq = transitions / 2 / (duration_ms / 1000)
            ch_freq.append(round(freq, 2))
        else:
            ch_freq.append(0)

    # Protocol hints based on timing
    hints = _protocol_hints(ch_freq, actual_rate)

    # Compact timing diagram (text, first 80 samples per channel)
    diagram = _timing_diagram(ch_data, channels, min(depth, 80))

    # Check for faults after capture just in case
    warnings = check_faults_post(bb)

    res = {
        "channels":       channels,
        "rate_hz":        actual_rate,
        "depth":          depth,
        "duration_ms":    round(duration_ms, 3),
        "channel_edges":  ch_edges,
        "frequency_hz":   ch_freq,
        "protocol_hints": hints,
        "timing_diagram": diagram,
    }
    if warnings:
        res["warnings"] = warnings
    return res


def _protocol_hints(freq_hz_list: list, rate_hz: int) -> list[str]:
    hints = []
    for i, freq in enumerate(freq_hz_list):
        if freq <= 0:
            continue
        # Standard UART baud rates
        for baud in [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]:
            if 0.9 * baud <= freq <= 1.1 * baud:
                hints.append(f"CH{i}: frequency ~{freq:.0f} Hz matches UART baud {baud}")
                break
        # SPI / I2C clock ranges
        if 90_000 <= freq <= 110_000:
            hints.append(f"CH{i}: frequency ~{freq/1000:.0f} kHz matches I2C standard (100 kHz)")
        elif 380_000 <= freq <= 420_000:
            hints.append(f"CH{i}: frequency ~{freq/1000:.0f} kHz matches I2C fast (400 kHz)")
        elif 900_000 <= freq <= 1_100_000:
            hints.append(f"CH{i}: frequency ~{freq/1000:.0f} kHz matches SPI/I2C fast+ (1 MHz)")
    return hints if hints else ["No standard protocol detected at these frequencies."]


def _timing_diagram(ch_data: list, n_channels: int, n_points: int) -> str:
    if not ch_data or n_points <= 0:
        return "No data"
    lines = []
    for chi in range(n_channels):
        if chi >= len(ch_data) or not ch_data[chi]:
            lines.append(f"CH{chi}: {'_' * n_points}")
            continue
        # Build a binary string from first n_points samples
        # ch_data[chi] is a flat list of 0/1 bits (one per sample)
        row: list[str] = []
        from typing import Any
        samples: list[Any] = ch_data[chi]
        for t in range(n_points):
            if t < len(samples):
                val = samples[t]
                row.append("‾" if val else "_")
            else:
                row.append("_")
        lines.append(f"CH{chi}: {''.join(row)}")
    return "\n".join(lines)
