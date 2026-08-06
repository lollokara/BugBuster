"""
Streaming handlers for SimulatedDevice.

Handles: START_ADC_STREAM, STOP_ADC_STREAM, START_SCOPE_STREAM, STOP_SCOPE_STREAM.

ADC_DATA_EVT payload layout (matching client.py _handler):
  [0]      channel_mask  (B)
  [1:5]    base_ts_ms    (I, LE)
  [5:7]    count         (H, LE)
  [7:]     N × 3-byte LE u24 ADC samples (one per active channel per group)

SCOPE_DATA_EVT payload layout (matching client.py _handler):
  [0:4]    seq           (I, LE)
  [4:8]    timestamp_ms  (I, LE)
  [8:10]   count         (H, LE)
  [10:]    4 × (avg f, min f, max f) LE floats

ADC_DSP_EVT payload layout (matching client.py _parse_adc_dsp_evt):
  [0]      channel       (B)
  [1:5]    timestamp_us  (I, LE)
  [5:7]    n_samples     (H, LE)
  [7:23]   min/max/mean/rms (4 × f, LE)
  [23]     n_fft_peaks   (B), then n × (bin B, magnitude f)
  [..]     n_spikes      (B), then n × (offset_us I, value f)
"""

import math
import struct
import threading

from bugbuster.constants import CmdId
from bugbuster.transport.usb import DeviceError, ErrorCode

# AD74416H CONV_RATE codes — mirrors AdcRate in
# Firmware/ESP32/src/hal/ad74416h_regs.h (note the gaps: 2, 5, 7, 10, 11 are
# not valid rates).
_VALID_ADC_RATES = frozenset({0, 1, 3, 4, 6, 8, 9, 12, 13})


# ---------------------------------------------------------------------------
# ADC stream loop
# ---------------------------------------------------------------------------

def _adc_stream_loop(device, stop_event, interval_s, channel_mask):
    """Push ADC_DATA_EVT frames at the configured interval."""
    n_active = bin(channel_mask).count('1')
    if n_active == 0:
        return

    t = 0.0
    base_ts = 0

    while not stop_event.is_set():
        # Synthetic sine-wave sample for each active channel
        # Simulate a mid-scale ±12 V range signal
        samples = []
        for ch in range(4):
            if not (channel_mask & (1 << ch)):
                continue
            # Each channel gets a slightly different frequency
            voltage = 5.0 * math.sin(2 * math.pi * (ch + 1) * t)
            # Map ±12 V → 0..0xFFFFFF (24-bit unsigned)
            raw_code = int((voltage + 12.0) / 24.0 * 0xFFFFFF) & 0xFFFFFF
            samples.append(raw_code)

        # Build payload: mask(B) + base_ts(I) + count(H) + samples(u24×N)
        count = 1  # one group of samples
        buf = struct.pack('<BIH', channel_mask & 0xFF, base_ts, count)
        for code in samples:
            buf += code.to_bytes(3, 'little')

        transport = device._transport
        handler = (
            transport._event_handlers.get(int(CmdId.ADC_DATA_EVT))
            if transport is not None else None
        )
        if handler:
            try:
                handler(buf)
            except Exception:
                pass

        stop_event.wait(interval_s)
        t += interval_s
        base_ts = int(t * 1000) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Scope stream loop
# ---------------------------------------------------------------------------

def _scope_stream_loop(device, stop_event, interval_s):
    """Push SCOPE_DATA_EVT frames at the configured interval."""
    seq = 0
    t = 0.0

    while not stop_event.is_set():
        ts_ms = int(t * 1000) & 0xFFFFFFFF
        count = 1

        # Build 4-channel scope payload
        # Each channel: avg, min, max (float LE)
        buf = struct.pack('<IIH', seq, ts_ms, count)
        for ch in range(4):
            voltage = 3.3 * math.sin(2 * math.pi * (ch + 1) * t)
            noise = 0.05
            avg = voltage
            mn = voltage - noise
            mx = voltage + noise
            buf += struct.pack('<fff', avg, mn, mx)

        transport = device._transport
        handler = (
            transport._event_handlers.get(int(CmdId.SCOPE_DATA_EVT))
            if transport is not None else None
        )
        if handler:
            try:
                handler(buf)
            except Exception:
                pass

        stop_event.wait(interval_s)
        t += interval_s
        seq += 1


# ---------------------------------------------------------------------------
# ADC DSP stream loop
# ---------------------------------------------------------------------------

def _dsp_stream_loop(device, stop_event, interval_s, channel,
                     window_samples, spike_threshold, n_fft_peaks):
    """Push ADC_DSP_EVT windows describing a synthetic 1 kHz tone."""
    ts_us = 0
    period_us = int(interval_s * 1_000_000)

    while not stop_event.is_set():
        # Window statistics of a 1 V-amplitude sine about a 2.5 V offset. The
        # host asserts on the min/max/mean/rms relationship, so these must be
        # mutually consistent rather than arbitrary.
        offset, amp = 2.5, 1.0
        min_v = offset - amp
        max_v = offset + amp
        mean_v = offset
        rms_v = math.sqrt(offset * offset + (amp * amp) / 2.0)

        buf = bytearray()
        buf += struct.pack('<BIH', channel & 0xFF, ts_us & 0xFFFFFFFF,
                           window_samples & 0xFFFF)
        buf += struct.pack('<ffff', min_v, max_v, mean_v, rms_v)

        peaks = min(n_fft_peaks, 8)
        buf.append(peaks)
        for i in range(peaks):
            # Fundamental carries the most energy, harmonics decay.
            buf += struct.pack('<Bf', (i + 1) * 4, amp / (i + 1))

        # One spike per window only once it clears the caller's threshold, so
        # a high threshold really does suppress spikes.
        spikes = 1 if amp >= spike_threshold else 0
        buf.append(spikes)
        for _ in range(spikes):
            buf += struct.pack('<If', period_us // 2, max_v)

        transport = getattr(device, '_transport', None)
        handler = (
            transport._event_handlers.get(int(CmdId.ADC_DSP_EVT))
            if transport is not None else None
        )
        if handler:
            try:
                handler(bytes(buf))
            except Exception:
                pass

        stop_event.wait(interval_s)
        ts_us += period_us


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

def register(device) -> None:
    # Separate stop event and thread for scope (can run alongside ADC)
    _scope_stop = threading.Event()
    _scope_thread_holder = [None]
    _dsp_stop = threading.Event()
    _dsp_thread_holder = [None]

    def handle_start_adc(payload: bytes) -> bytes:
        channel_mask = payload[0] if len(payload) >= 1 else 0x0F
        # divider = payload[1] if len(payload) >= 2 else 1  # unused in sim
        device._stream_stop.clear()
        t = threading.Thread(
            target=_adc_stream_loop,
            args=(device, device._stream_stop, 0.05, channel_mask),
            daemon=True,
        )
        t.start()
        device._stream_thread = t
        return b''

    def handle_stop_adc(payload: bytes) -> bytes:
        device._stream_stop.set()
        if device._stream_thread and device._stream_thread.is_alive():
            device._stream_thread.join(timeout=2.0)
        device._stream_thread = None
        return b''

    def handle_start_scope(payload: bytes) -> bytes:
        ch_mask = payload[0] if len(payload) >= 1 else 0x0F
        if ch_mask == 0:
            ch_mask = 0x0F
        device.scope_ch_mask = ch_mask
        device.adc_diag_paused = True
        _scope_stop.clear()
        t = threading.Thread(
            target=_scope_stream_loop,
            args=(device, _scope_stop, 0.01),  # 100 Hz → ~10 ms buckets
            daemon=True,
        )
        t.start()
        _scope_thread_holder[0] = t
        return b''

    def handle_stop_scope(payload: bytes) -> bytes:
        _scope_stop.set()
        th = _scope_thread_holder[0]
        if th and th.is_alive():
            th.join(timeout=2.0)
        _scope_thread_holder[0] = None
        device.scope_ch_mask = 0x0F
        device.adc_diag_paused = False
        return b''

    def handle_start_dsp(payload: bytes) -> bytes:
        channel, rate, window, threshold, peaks = struct.unpack('<BBHfB', payload) \
            if len(payload) >= struct.calcsize('<BBHfB') else (0, 0, 256, 0.1, 8)
        # Mirrors bbpStartAdcDspStream() in Firmware/ESP32/src/bbp/bbp.cpp.
        if channel >= 4 or rate not in _VALID_ADC_RATES:
            raise DeviceError(ErrorCode.INVALID_CHANNEL, 0)
        # adcPoll never samples a HIGH_IMP channel, so the firmware refuses
        # rather than streaming stale zeros forever.
        if device.channels[channel]["function"] == 0:
            raise DeviceError(ErrorCode.INVALID_STATE, 0)
        device.dsp_stream = {
            "channel": channel,
            "window_samples": window,
            "spike_threshold": threshold,
            "n_fft_peaks": peaks,
        }
        # The firmware puts the channel at the requested rate for the duration
        # of the stream and restores it on stop.
        device.channels[channel]["adc_rate_before_dsp"] = device.channels[channel]["adc_rate"]
        device.channels[channel]["adc_rate"] = rate
        _dsp_stop.clear()
        t = threading.Thread(
            target=_dsp_stream_loop,
            args=(device, _dsp_stop, 0.03, channel, window, threshold, peaks),
            daemon=True,
        )
        t.start()
        _dsp_thread_holder[0] = t
        return b''

    def handle_stop_dsp(payload: bytes) -> bytes:
        _dsp_stop.set()
        th = _dsp_thread_holder[0]
        if th and th.is_alive():
            th.join(timeout=2.0)
        _dsp_thread_holder[0] = None
        if device.dsp_stream is not None:
            ch = device.dsp_stream["channel"]
            prev = device.channels[ch].pop("adc_rate_before_dsp", None)
            if prev is not None:
                device.channels[ch]["adc_rate"] = prev
        device.dsp_stream = None
        return b''

    device.register_handler(CmdId.START_ADC_STREAM,     handle_start_adc)
    device.register_handler(CmdId.STOP_ADC_STREAM,      handle_stop_adc)
    device.register_handler(CmdId.START_SCOPE_STREAM,   handle_start_scope)
    device.register_handler(CmdId.STOP_SCOPE_STREAM,    handle_stop_scope)
    device.register_handler(CmdId.START_ADC_DSP_STREAM, handle_start_dsp)
    device.register_handler(CmdId.STOP_ADC_DSP_STREAM,  handle_stop_dsp)

    # Scope and DSP threads are owned by closures here, so hand the device a
    # way to stop them on disconnect.
    device._stream_stoppers.append(lambda: handle_stop_scope(b''))
    device._stream_stoppers.append(lambda: handle_stop_dsp(b''))
