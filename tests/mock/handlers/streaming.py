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
"""

import math
import struct
import threading

from bugbuster.constants import CmdId


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

        handler = device._transport._event_handlers.get(int(CmdId.ADC_DATA_EVT))
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

        handler = device._transport._event_handlers.get(int(CmdId.SCOPE_DATA_EVT))
        if handler:
            try:
                handler(buf)
            except Exception:
                pass

        stop_event.wait(interval_s)
        t += interval_s
        seq += 1


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

def register(device) -> None:
    # Separate stop event and thread for scope (can run alongside ADC)
    _scope_stop = threading.Event()
    _scope_thread_holder = [None]

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
        return b''

    device.register_handler(CmdId.START_ADC_STREAM,   handle_start_adc)
    device.register_handler(CmdId.STOP_ADC_STREAM,    handle_stop_adc)
    device.register_handler(CmdId.START_SCOPE_STREAM, handle_start_scope)
    device.register_handler(CmdId.STOP_SCOPE_STREAM,  handle_stop_scope)
