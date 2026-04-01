"""
Example 05 — Hardware Abstraction Layer (HAL) Basics
====================================================

Demonstrates the Arduino-style port API for all 12 IOs.

Physical layout
~~~~~~~~~~~~~~~
The BugBuster has 12 IOs arranged in 2 Blocks × 2 IO_Blocks × 3 IOs:

    ┌─────────────────────────────────────────────────────────────────────────┐
    │ BLOCK 1 (VADJ1 → IO 1–6)                                              │
    │   IO_Block 1 (EFUSE1)          IO_Block 2 (EFUSE2)                    │
    │   IO 1  — analog/HAT           IO 4  — analog/HAT                     │
    │   IO 2  — digital              IO 5  — digital                        │
    │   IO 3  — digital              IO 6  — digital                        │
    ├─────────────────────────────────────────────────────────────────────────┤
    │ BLOCK 2 (VADJ2 → IO 7–12)                                             │
    │   IO_Block 3 (EFUSE3)          IO_Block 4 (EFUSE4)                    │
    │   IO 7  — analog/HAT           IO 10 — analog/HAT                     │
    │   IO 8  — digital              IO 11 — digital                        │
    │   IO 9  — digital              IO 12 — digital                        │
    └─────────────────────────────────────────────────────────────────────────┘

IO capabilities
~~~~~~~~~~~~~~~
┌──────────────────────┬───────────────────────────────┬──────────┬──────────┐
│ Mode                 │ Description                   │ IO 1,4,  │ IO 2,3,  │
│                      │                               │ 7,10     │ 5,6…12   │
├──────────────────────┼───────────────────────────────┼──────────┼──────────┤
│ DISABLED             │ Safe default / disconnected    │ ✓        │ ✓        │
│ ANALOG_IN            │ ADC voltage input  (0–12 V)   │ ✓        │ ✗        │
│ ANALOG_OUT           │ DAC voltage output (0–12 V)   │ ✓        │ ✗        │
│ CURRENT_IN           │ 4–20 mA input (ext. powered)  │ ✓        │ ✗        │
│ CURRENT_OUT          │ 4–20 mA current source        │ ✓        │ ✗        │
│ DIGITAL_IN           │ GPIO input — high drive        │ ✓        │ ✓        │
│ DIGITAL_OUT          │ GPIO output — high drive       │ ✓        │ ✓        │
│ DIGITAL_IN_LOW       │ GPIO input — low drive         │ ✓        │ ✓        │
│ DIGITAL_OUT_LOW      │ GPIO output — low drive        │ ✓        │ ✓        │
│ RTD                  │ Resistance / PT100 measurement │ ✓        │ ✗        │
│ HART                 │ HART modem overlay (4–20 mA)   │ ✓        │ ✗        │
│ HAT                  │ Passthrough to HAT expansion   │ ✓        │ ✗        │
└──────────────────────┴───────────────────────────────┴──────────┴──────────┘

Supply rails
~~~~~~~~~~~~
┌─────────────┬───────────┬──────────────────────────────────────┐
│ Rail        │ Range     │ Controls                              │
├─────────────┼───────────┼──────────────────────────────────────┤
│ VADJ1       │ 3–15 V    │ VCC pin on IO_Block 1 & 2 (IO 1–6)  │
│ VADJ2       │ 3–15 V    │ VCC pin on IO_Block 3 & 4 (IO 7–12) │
│ VLOGIC      │ 1.8–5.0 V │ Logic level for all digital IOs      │
└─────────────┴───────────┴──────────────────────────────────────┘
"""

import time
import sys
sys.path.insert(0, "..")

import bugbuster as bb
from bugbuster import PortMode

USB_PORT = "/dev/ttyACM0"


with bb.connect_usb(USB_PORT) as dev:

    hal = dev.hal

    # ── Power up ──────────────────────────────────────────────────────────
    hal.begin(supply_voltage=12.0, vlogic=3.3)

    # ── Set supply voltages ──────────────────────────────────────────────
    print("--- Supply Voltages ---")
    hal.set_voltage(rail=1, voltage=12.0)   # VADJ1 → 12 V (IO 1–6)
    hal.set_voltage(rail=2, voltage=5.0)    # VADJ2 → 5 V  (IO 7–12)
    hal.set_vlogic(3.3)                      # logic level → 3.3 V
    print("  VADJ1 = 12 V, VADJ2 = 5 V, VLOGIC = 3.3 V")


    # ── Analog output on IO 1 ────────────────────────────────────────────
    print("\n--- Analog Output (IO 1) ---")
    hal.configure(1, PortMode.ANALOG_OUT)
    hal.write_voltage(1, 5.0)
    print("  IO 1 → 5.0 V")
    time.sleep(0.1)


    # ── Analog input on IO 4 ─────────────────────────────────────────────
    print("\n--- Analog Input (IO 4) ---")
    hal.configure(4, PortMode.ANALOG_IN)
    time.sleep(1.0)   # ADC needs settle time after channel change

    readings = [hal.read_voltage(4) for _ in range(5)]
    avg = sum(readings) / len(readings)
    print(f"  IO 4 average: {avg:.4f} V")


    # ── Current output on IO 7 ───────────────────────────────────────────
    print("\n--- Current Output 4–20 mA (IO 7) ---")
    hal.configure(7, PortMode.CURRENT_OUT)

    for pct in [0, 25, 50, 75, 100]:
        ma = 4.0 + (pct / 100.0) * 16.0
        hal.write_current(7, ma)
        time.sleep(0.05)
        print(f"  {pct:3d}% → {ma:.2f} mA")


    # ── RTD / resistance on IO 10 ────────────────────────────────────────
    print("\n--- RTD Measurement (IO 10) ---")
    hal.configure(10, PortMode.RTD)
    time.sleep(1.0)

    ohms = hal.read_resistance(10)
    temp = hal.read_temperature_pt100(10)
    print(f"  Resistance: {ohms:.2f} Ω")
    print(f"  PT100 temp: {temp:.1f} °C")


    # ── Digital output on IO 2 ───────────────────────────────────────────
    print("\n--- Digital Output (IO 2) ---")
    hal.configure(2, PortMode.DIGITAL_OUT)

    hal.write_digital(2, True)
    print("  IO 2 → HIGH")
    time.sleep(0.5)

    hal.write_digital(2, False)
    print("  IO 2 → LOW")


    # ── Digital output low-drive on IO 3 ─────────────────────────────────
    print("\n--- Digital Output Low-Drive (IO 3) ---")
    hal.configure(3, PortMode.DIGITAL_OUT_LOW)
    # NOTE: low-drive GPIO write not yet implemented in firmware path
    print("  (low-drive digital write pending firmware support)")


    # ── Serial bridge ────────────────────────────────────────────────────
    print("\n--- Serial Bridge ---")
    # Route UART TX to IO 5, RX to IO 6
    # (requires MUX routing table to be configured)
    # hal.set_serial(tx=5, rx=6)
    print("  Serial bridge routing requires MUX table — skipped")


    # ── Reconfigure: switch IO 1 from analog to digital ──────────────────
    print("\n--- Reconfigure IO 1: analog → digital ---")
    hal.configure(1, PortMode.DIGITAL_OUT)
    hal.write_digital(1, True)
    print("  IO 1 → DIGITAL_OUT → HIGH")


    # ── Disable unused IOs ───────────────────────────────────────────────
    print("\n--- Disable IOs ---")
    for io in [1, 2, 3, 4, 7, 10]:
        hal.configure(io, PortMode.DISABLED)
    print("  IOs disabled.")


    # ── Clean shutdown ───────────────────────────────────────────────────
    hal.shutdown()
    print("\nHAL shutdown complete.")
