"""
Example 04 — Waveform Generator & MUX Switch Matrix
====================================================

Demonstrates:
  - Software waveform generator (sine, square, triangle, sawtooth)
  - Routing signals through the 32-switch analog MUX matrix
  - Combining both: generate a signal, route it to a measurement channel

Hardware setup assumed:
  - Channel 0 used as voltage output (waveform source)
  - Channel 1 used as voltage input  (measurement)
  - MUX device 0, switch 0 connects channel 0 output to channel 1 input
    (replace with your actual board routing)
"""

import time
import sys
sys.path.insert(0, "..")

import bugbuster as bb
from bugbuster import ChannelFunction, WaveformType, OutputMode, AdcRate, AdcMux, AdcRange

USB_PORT = "/dev/ttyACM0"

with bb.connect_usb(USB_PORT) as dev:

    dev.reset()  # start from a known-good state

    # ── 1. Waveform generator ───────────────────────────────────────────────
    print("--- Waveform Generator ---")

    # Configure channel 0 as voltage output (required before starting wavegen)
    dev.set_channel_function(0, ChannelFunction.VOUT)

    # Generate a 0.5 Hz sine wave, amplitude 2 V peak, centred at 3 V
    # → output swings between 1 V and 5 V
    dev.start_waveform(
        channel=0,
        waveform=WaveformType.SINE,
        freq_hz=0.5,
        amplitude=2.0,   # ±2 V swing around the offset
        offset=3.0,      # DC offset (output centre = 3 V)
        mode=OutputMode.VOLTAGE,
    )
    print("Sine wave started: 0.5 Hz, 1–5 V on channel 0")
    time.sleep(2.0)   # let it run for two full cycles

    # Switch to a 2 Hz square wave between 0 V and 5 V
    dev.start_waveform(
        channel=0,
        waveform=WaveformType.SQUARE,
        freq_hz=2.0,
        amplitude=2.5,   # half of 5 V span
        offset=2.5,      # centre → 0 V to 5 V
        mode=OutputMode.VOLTAGE,
    )
    print("Square wave started: 2 Hz, 0–5 V on channel 0")
    time.sleep(1.5)

    # Triangle wave
    dev.start_waveform(
        channel=0,
        waveform=WaveformType.TRIANGLE,
        freq_hz=1.0,
        amplitude=4.0,
        offset=4.0,
        mode=OutputMode.VOLTAGE,
    )
    print("Triangle wave started: 1 Hz, 0–8 V on channel 0")
    time.sleep(2.0)

    # Stop — DAC holds the last output value
    dev.stop_waveform()
    print("Waveform stopped.")

    # Example: current output waveform (simulated 4–20 mA sine)
    dev.set_channel_function(0, ChannelFunction.IOUT)
    dev.start_waveform(
        channel=0,
        waveform=WaveformType.SINE,
        freq_hz=0.2,
        amplitude=8.0,   # ±8 mA swing
        offset=12.0,     # centre at 12 mA → sweeps 4–20 mA
        mode=OutputMode.CURRENT,
    )
    print("Current waveform started: 0.2 Hz, 4–20 mA sine on channel 0")
    time.sleep(2.0)
    dev.stop_waveform()

    dev.set_channel_function(0, ChannelFunction.HIGH_IMP)


    # ── 2. MUX switch matrix ───────────────────────────────────────────────
    print("\n--- MUX Switch Matrix ---")

    # Read current state of all 32 switches
    states = dev.mux_get()
    print(f"Current MUX states: {[f'{s:#04x}' for s in states]}")

    # Open all switches first (safe state)
    dev.mux_set_all([0x00, 0x00, 0x00, 0x00])
    print("All switches opened.")

    # Close switch 0 on device 0 (routes signal path A)
    dev.mux_set_switch(device=0, switch=0, closed=True)
    print("Closed: device 0, switch 0")

    # Close switches 0 and 2 on device 1
    # Using mux_set_all for atomic multi-switch update:
    #   device 1 byte = 0b00000101 = 0x05  (switches 0 and 2 closed)
    dev.mux_set_all([0x01, 0x05, 0x00, 0x00])
    print("Set: device 0 sw0=closed, device 1 sw0+sw2=closed")

    time.sleep(0.2)  # device enforces 100 ms dead time between group changes

    # Read back
    states = dev.mux_get()
    print(f"MUX states after: {[f'{s:#04x}' for s in states]}")

    # ── 3. Route waveform through MUX and measure it ───────────────────────
    print("\n--- Signal routing: source → MUX → measurement ─")

    # Source: channel 0 generates a 1 Hz sine
    dev.set_channel_function(0, ChannelFunction.VOUT)
    dev.start_waveform(
        channel=0,
        waveform=WaveformType.SINE,
        freq_hz=1.0,
        amplitude=3.0,
        offset=3.0,
        mode=OutputMode.VOLTAGE,
    )

    # Route: close the switch that connects ch0 output to ch1 input
    # (adjust device/switch indices to match your board layout)
    dev.mux_set_switch(device=0, switch=0, closed=True)

    # Measure: configure channel 1 as voltage input at 200 SPS
    dev.set_channel_function(1, ChannelFunction.VIN)
    dev.set_adc_config(1, mux=AdcMux.LF_TO_AGND, range_=AdcRange.V_0_12, rate=AdcRate.SPS_200_H)

    print("Sampling channel 1 for 2 seconds while sine plays on channel 0...")
    start = time.time()
    samples = []
    while time.time() - start < 2.0:
        r = dev.get_adc_value(1)
        samples.append(r.value)
        time.sleep(0.05)  # ~20 Hz polling

    if samples:
        print(f"  Samples: {len(samples)}")
        print(f"  Min: {min(samples):.3f} V  Max: {max(samples):.3f} V  Avg: {sum(samples)/len(samples):.3f} V")

    # Clean up
    dev.stop_waveform()
    dev.mux_set_all([0x00, 0x00, 0x00, 0x00])  # open all switches
    dev.reset()
    print("\nDone.")
