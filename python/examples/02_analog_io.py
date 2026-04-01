"""
Example 02 — Analog I/O
=======================

Demonstrates all analog channel operations:
  - Voltage output (VOUT)
  - Current output (IOUT) — 4–20 mA loop
  - Voltage input  (VIN)  — measure an external voltage
  - Current input  (IIN)  — measure a 4–20 mA transmitter
  - Resistance / RTD measurement (RES_MEAS)

Each section includes a table of available options for every parameter.
Works with both USB and HTTP transports; USB is used in the examples below.
"""

import time
import sys
sys.path.insert(0, "..")

import bugbuster as bb
from bugbuster import (
    ChannelFunction,
    AdcRange, AdcRate, AdcMux,
    VoutRange, CurrentLimit,
    RtdCurrent,
)

# ┌─────────────────────────────────────────────────────────────────────────┐
# │  ChannelFunction — channel operating modes                              │
# ├──────────────────────────┬──────────────────────────────────────────────┤
# │ HIGH_IMP                 │ High-impedance (safe default / switching)    │
# │ VOUT                     │ Voltage output  0–12 V or ±12 V             │
# │ IOUT                     │ Current output  0–25 mA                     │
# │ VIN                      │ Voltage input   (ADC measures loop voltage)  │
# │ IIN_EXT_PWR              │ Current input   externally powered 4–20 mA  │
# │ IIN_LOOP_PWR             │ Current input   loop-powered 4–20 mA        │
# │ RES_MEAS                 │ Resistance / RTD (2/3/4-wire, up to 12 kΩ)  │
# │ DIN_LOGIC                │ Digital input   logic level                  │
# │ DIN_LOOP                 │ Digital input   loop-powered                 │
# │ IOUT_HART                │ Current output  with HART overlay            │
# │ IIN_EXT_PWR_HART         │ Current input   ext-powered + HART           │
# │ IIN_LOOP_PWR_HART        │ Current input   loop-powered + HART          │
# └──────────────────────────┴──────────────────────────────────────────────┘

USB_PORT = "/dev/ttyACM0"

with bb.connect_usb(USB_PORT) as dev:

    # ── IMPORTANT: configure all channels before reading ───────────────────
    # The AD74416H has one shared ADC that sequences across all active channels.
    # Set up every channel you need first, then wait ≥1 s before the first read.

    # ── 1. Voltage output on channel 0 ─────────────────────────────────────
    print("--- Voltage Output (channel 0) ---")

    # Always switch through HIGH_IMP when changing function to avoid glitches
    dev.set_channel_function(0, ChannelFunction.HIGH_IMP)
    dev.set_channel_function(0, ChannelFunction.VOUT)

    # Set output range:
    # ┌─────────────────────┬───────────────────────────────────────────────┐
    # │ VoutRange.UNIPOLAR  │ 0 V to +12 V  (default after function set)   │
    # │ VoutRange.BIPOLAR   │ -12 V to +12 V                               │
    # └─────────────────────┴───────────────────────────────────────────────┘
    dev.set_vout_range(0, VoutRange.UNIPOLAR)

    dev.set_dac_voltage(0, 5.0)      # 5 V unipolar
    time.sleep(0.1)
    code = dev.get_dac_readback(0)
    print(f"  DAC 5.0 V → code={code}  ({code / 65535 * 12:.3f} V equivalent)")

    # Switch to bipolar and output a negative voltage
    dev.set_vout_range(0, VoutRange.BIPOLAR)
    dev.set_dac_voltage(0, -3.0, bipolar=True)
    time.sleep(0.1)
    print(f"  DAC -3.0 V (bipolar) → code={dev.get_dac_readback(0)}")

    dev.set_channel_function(0, ChannelFunction.HIGH_IMP)


    # ── 2. Current output on channel 1 ─────────────────────────────────────
    print("\n--- Current Output 4–20 mA (channel 1) ---")

    dev.set_channel_function(1, ChannelFunction.IOUT)

    # Optionally restrict the maximum output current:
    # ┌──────────────────────┬──────────────────────────────────────────────┐
    # │ CurrentLimit.MA_25   │ Full range 0–25 mA (default)                │
    # │ CurrentLimit.MA_8    │ Limited to 0–8 mA (protect sensitive loads) │
    # └──────────────────────┴──────────────────────────────────────────────┘
    dev.set_current_limit(1, CurrentLimit.MA_25)

    def ma_from_percent(pct: float) -> float:
        """Convert 0–100 % process value to 4–20 mA."""
        return 4.0 + (pct / 100.0) * 16.0

    for pct in [0, 25, 50, 75, 100]:
        current = ma_from_percent(pct)
        dev.set_dac_current(1, current)
        time.sleep(0.05)
        print(f"  {pct:3d}% → {current:.2f} mA")

    dev.set_channel_function(1, ChannelFunction.HIGH_IMP)


    # ── 3. Voltage input on channel 2 ──────────────────────────────────────
    print("\n--- Voltage Input (channel 2) ---")

    dev.set_channel_function(2, ChannelFunction.VIN)

    # ADC range options:
    # ┌──────────────────────────┬───────────────────────────────────────────┐
    # │ AdcRange.V_0_12          │ 0 V to +12 V    — widest, most common    │
    # │ AdcRange.V_NEG12_12      │ -12 V to +12 V  — bipolar full-range     │
    # │ AdcRange.V_NEG312_312MV  │ ±312.5 mV       — precision / RSENSE     │
    # │ AdcRange.V_0_625MV       │ 0 to +625 mV    — low-voltage signals    │
    # │ AdcRange.V_NEG104_104MV  │ ±104 mV         — thermocouple           │
    # │ AdcRange.V_NEG2_5_2_5    │ ±2.5 V          — mid-range bipolar      │
    # └──────────────────────────┴───────────────────────────────────────────┘

    # ADC rate options:
    # ┌──────────────────────┬────────────┬───────────────────────────────────┐
    # │ AdcRate.SPS_10_H     │  10 SPS    │ Highest resolution, 50/60Hz notch │
    # │ AdcRate.SPS_20       │  20 SPS    │ Default — good for slow signals   │
    # │ AdcRate.SPS_20_H     │  20 SPS    │ 20 SPS with 50/60Hz rejection     │
    # │ AdcRate.SPS_200_H    │  200 SPS   │ Fast with noise rejection         │
    # │ AdcRate.SPS_1200     │  1.2 kSPS  │ Fast sampling                    │
    # │ AdcRate.SPS_4800     │  4.8 kSPS  │ High-speed acquisition            │
    # │ AdcRate.SPS_9600     │  9.6 kSPS  │ Maximum rate (streaming mode)     │
    # └──────────────────────┴────────────┴───────────────────────────────────┘

    # ADC mux options:
    # ┌────────────────────────────────┬──────────────────────────────────────┐
    # │ AdcMux.LF_TO_AGND              │ Terminal vs AGND (standard voltage)  │
    # │ AdcMux.HF_TO_LF                │ Differential across RSENSE (current) │
    # │ AdcMux.VSENSEN_TO_AGND         │ Kelvin sense- vs AGND                │
    # │ AdcMux.LF_TO_VSENSEN           │ Terminal vs Kelvin sense (3W RTD)    │
    # │ AdcMux.AGND_TO_AGND            │ Self-test / zero offset calibration  │
    # └────────────────────────────────┴──────────────────────────────────────┘

    dev.set_adc_config(
        channel=2,
        mux=AdcMux.LF_TO_AGND,
        range_=AdcRange.V_0_12,
        rate=AdcRate.SPS_200_H,   # 200 SPS with 50/60 Hz rejection
    )
    time.sleep(0.3)

    readings = [dev.get_adc_value(2).value for _ in range(5)]
    avg = sum(readings) / len(readings)
    print(f"  Average over 5 readings: {avg:.4f} V")

    dev.set_channel_function(2, ChannelFunction.HIGH_IMP)


    # ── 4. Current input — externally-powered 4–20 mA (channel 3) ─────────
    print("\n--- Current Input (channel 3) ---")

    dev.set_channel_function(3, ChannelFunction.IIN_EXT_PWR)
    time.sleep(0.3)

    result = dev.get_adc_value(3)
    # Firmware measures voltage across the internal 12 Ω RSENSE
    current_ma = (result.value / 12.0) * 1000.0
    pct = max(0.0, min(100.0, (current_ma - 4.0) / 16.0 * 100.0))
    print(f"  RSENSE: {result.value * 1000:.2f} mV  → {current_ma:.2f} mA  → {pct:.1f}%")

    dev.set_channel_function(3, ChannelFunction.HIGH_IMP)


    # ── 5. RTD resistance measurement on channel 0 ─────────────────────────
    print("\n--- RTD Resistance Measurement (channel 0) ---")

    dev.set_channel_function(0, ChannelFunction.RES_MEAS)

    # RTD excitation current:
    # ┌─────────────────────┬──────────┬────────────────────────────────────┐
    # │ RtdCurrent.MA_1     │ 1 mA     │ Default — PT100 / low-R RTDs      │
    # │ RtdCurrent.UA_500   │ 500 µA   │ High-resistance sensors (> ~600 Ω)│
    # └─────────────────────┴──────────┴────────────────────────────────────┘
    # Note: max measurable resistance ≈ 625 mV / excitation_current
    #   1 mA  → max ~625 Ω (suitable for PT100 up to ~260 °C)
    #   500µA → max ~1250 Ω (suitable for PT1000 up to ~65 °C)
    dev.set_rtd_config(0, current=RtdCurrent.MA_1)

    # ⚠  After reset, set all channels first then wait ≥1 s before reading.
    # The shared ADC needs time to sequence through all configured channels.
    time.sleep(1.0)

    result = dev.get_adc_value(0)
    resistance_ohm = result.value  # firmware returns Ω directly in RES_MEAS mode

    # PT100 temperature (simplified Callendar–Van Dusen, valid -50…+200 °C):
    R0 = 100.0
    temp_c = (resistance_ohm - R0) / (R0 * 3.9083e-3)
    print(f"  Resistance: {resistance_ohm:.2f} Ω")
    print(f"  PT100 temperature ≈ {temp_c:.1f} °C")

    dev.reset()
    print("\nAll analog I/O examples complete.")
