"""
Example 03 — ADC Streaming  (USB only)
=======================================

The BugBuster can stream raw 24-bit ADC data from up to 4 channels
simultaneously at up to 9.6 kSPS per channel (~115 KB/s total).

Data arrives as unsolicited EVT frames pushed by the device.
The library dispatches them to a callback on a background thread.

Additionally, the device generates pre-processed oscilloscope "scope"
buckets (min / avg / max per 10 ms window) that are useful for real-time
display without processing every raw sample.

Demonstrates:
  - start_adc_stream / stop_adc_stream
  - ADC data callback with sample decoding
  - Scope bucket callback (on_scope_data)
  - Converting raw 24-bit codes to voltages manually
"""

import time
import sys
import threading
sys.path.insert(0, "..")

import bugbuster as bb
from bugbuster import ChannelFunction, AdcRange, AdcRate, AdcMux

# Adjust for your OS:  macOS: /dev/cu.usbmodem*   Linux: /dev/ttyACM0   Windows: COM3
USB_PORT = "/dev/cu.usbmodem1234561"

# ── ADC range parameters needed to convert raw codes to volts ──────────────
# V = v_offset + (raw / 2^24) * v_span
RANGE_PARAMS = {
    AdcRange.V_0_12:         (0.000,   12.000),
    AdcRange.V_NEG12_12:     (-12.000, 24.000),
    AdcRange.V_NEG312_312MV: (-0.3125,  0.6250),
    AdcRange.V_NEG2_5_2_5:   (-2.500,   5.000),
}

def raw_to_volts(raw: int, range_code: AdcRange) -> float:
    v_offset, v_span = RANGE_PARAMS.get(AdcRange(range_code), (0.0, 12.0))
    return v_offset + (raw / 16_777_216.0) * v_span


with bb.connect_usb(USB_PORT) as dev:

    # Configure channels 0 and 1 as voltage inputs at maximum sample rate
    for ch in range(2):
        dev.set_channel_function(ch, ChannelFunction.VIN)
        dev.set_adc_config(
            ch,
            mux=AdcMux.LF_TO_AGND,
            range_=AdcRange.V_0_12,
            rate=AdcRate.SPS_9600,   # 9.6 kSPS per channel
        )

    # ── Sample counter shared between callback and main thread ─────────────
    sample_count   = 0
    sample_lock    = threading.Lock()
    last_print_time = time.time()

    def on_adc_data(channel_mask: int, raw_samples: list[int]):
        """
        Called from the background reader thread for every ADC data batch.

        *channel_mask* — bitmask of active channels in this batch.
                         bit 0 = channel 0, bit 1 = channel 1, …
        *raw_samples*  — flat list of raw 24-bit codes.
                         Layout: [ch0_s0, ch1_s0, ch0_s1, ch1_s1, …]
                         (channels interleaved per sample group).
        """
        global sample_count, last_print_time

        n_active = bin(channel_mask).count('1')
        n_groups = len(raw_samples) // n_active if n_active else 0

        with sample_lock:
            sample_count += n_groups * n_active

        # Print the first sample of each batch for demonstration
        if n_groups > 0:
            now = time.time()
            if now - last_print_time >= 0.5:   # throttle to 2 Hz console output
                last_print_time = now
                ch_idx = 0
                for bit in range(4):
                    if channel_mask & (1 << bit):
                        v = raw_to_volts(raw_samples[ch_idx], AdcRange.V_0_12)
                        print(f"  Ch{bit}: {v:.4f} V  (raw={raw_samples[ch_idx]:#08x})")
                        ch_idx += 1
                print(f"  [batch: {n_groups} groups, total samples so far: {sample_count}]")

    # ── Scope bucket callback — min / avg / max per 10 ms ─────────────────
    scope_buckets = []

    def on_scope(bucket: dict):
        """
        Called every 10 ms with pre-processed min/avg/max for all 4 channels.

        *bucket* — dict with keys:
            ``seq``           — monotonic sequence number
            ``timestamp_ms``  — device uptime when bucket was closed
            ``count``         — number of ADC samples in this bucket
            ``channels``      — list of 4 dicts {'avg', 'min', 'max'}
        """
        scope_buckets.append(bucket)
        # Only keep the latest 100 buckets in memory
        if len(scope_buckets) > 100:
            scope_buckets.pop(0)

    dev.on_scope_data(on_scope)   # register scope callback before streaming

    # ── Start streaming channels 0 and 1 at full rate ─────────────────────
    print("Starting ADC stream on channels 0 and 1 for 3 seconds...")
    dev.start_adc_stream(
        channels=[0, 1],
        divider=1,           # divider=1 → full 9.6 kSPS, divider=2 → 4.8 kSPS, …
        callback=on_adc_data,
    )

    time.sleep(3.0)

    dev.stop_adc_stream()
    print(f"\nStream stopped.  Total samples received: {sample_count}")

    # ── Show scope statistics ──────────────────────────────────────────────
    if scope_buckets:
        last = scope_buckets[-1]
        print(f"\nLast scope bucket (seq={last['seq']}, t={last['timestamp_ms']} ms):")
        for i, ch in enumerate(last["channels"]):
            print(f"  Ch{i}: avg={ch['avg']:.4f} V  min={ch['min']:.4f} V  max={ch['max']:.4f} V")

        # Calculate throughput
        elapsed_ms = scope_buckets[-1]["timestamp_ms"] - scope_buckets[0]["timestamp_ms"]
        total_in_scope = sum(b["count"] for b in scope_buckets)
        if elapsed_ms > 0:
            sps = total_in_scope / (elapsed_ms / 1000.0) / 2  # 2 channels
            print(f"\nMeasured aggregate rate: {total_in_scope} samples over {elapsed_ms} ms")
            print(f"~{sps:.0f} SPS per channel")

    # Reset channels to safe state
    for ch in range(2):
        dev.set_channel_function(ch, ChannelFunction.HIGH_IMP)
