"""
Example 05 — Power Management
==============================

The BugBuster has a multi-rail power system:
  - USB PD sink (HUSB238): negotiate up to 20 V / 100 W from a USB-C PD source
  - Adjustable supplies (DS4424 + LTM8063/LTM8078): software-controlled voltage
    on V_ADJ1, V_ADJ2, and the level-shifter rail
  - E-fuse protection on 4 output ports (P1–P4)
  - PCA9535 I/O expander for enable/fault monitoring

Demonstrates:
  - Reading USB PD contract (voltage, current, available PDOs)
  - Enabling adjustable supplies and setting their voltage
  - Monitoring power-good flags and e-fuse faults
  - Safe power-on sequence for the device outputs
"""

import time
import sys
sys.path.insert(0, "..")

import bugbuster as bb
from bugbuster import PowerControl

USB_PORT = "/dev/ttyACM0"

with bb.connect_usb(USB_PORT) as dev:

    # ── 1. USB Power Delivery status ───────────────────────────────────────
    print("--- USB PD Status ---")

    pd = dev.usbpd_get_status()
    if pd["attached"]:
        print(f"USB PD negotiated: {pd['voltage_v']:.1f} V / {pd['current_a']:.2f} A  ({pd['power_w']:.1f} W)")
        print("Available PDOs:")
        _V_CODES = {1: 5, 2: 9, 3: 12, 4: 15, 5: 18, 6: 20}
        for i, pdo in enumerate(pd["pdos"]):
            if pdo["detected"]:
                v = _V_CODES.get(i + 1, "?")
                print(f"  PDO {i+1}: {v} V, max {pdo['max_current'] * 10} mA")
    else:
        print("No USB PD source attached (running from 5 V VBUS or bench supply).")

    # Request 20 V if available (check pdos first — don't request what isn't offered)
    if any(p["detected"] for p in pd["pdos"][5:6]):   # index 5 = 20 V PDO
        print("Requesting 20 V PDO...")
        dev.usbpd_select_voltage(20)
        time.sleep(0.5)
        pd2 = dev.usbpd_get_status()
        print(f"New contract: {pd2['voltage_v']:.1f} V")


    # ── 2. PCA9535 power management status ─────────────────────────────────
    print("\n--- Power Rail Status ---")

    pwr = dev.power_get_status()
    print(f"PCA9535 present:  {pwr['present']}")
    print(f"Logic power-good: {pwr['logic_pg']}")
    print(f"V_ADJ1 PG:        {pwr['vadj1_pg']}")
    print(f"V_ADJ2 PG:        {pwr['vadj2_pg']}")
    print(f"E-fuse faults:    {pwr['efuse_faults']}  (True = fault, False = OK)")


    # ── 3. Safe power-on sequence ───────────────────────────────────────────
    # Recommended order:
    #   a. Enable the ±15 V analog supply (needed by AD74416H AVDD rails)
    #   b. Enable V_ADJ1 / V_ADJ2 regulators
    #   c. Set target voltages via IDAC
    #   d. Enable e-fuses for output ports P1–P4

    print("\n--- Powering up supplies ---")

    # a. Enable ±15 V analog supply
    dev.power_set(PowerControl.V15A, on=True)
    print("±15 V supply enabled.")
    time.sleep(0.1)   # allow regulator to settle

    # b. Enable V_ADJ1 (feeds ports P1 and P2)
    dev.power_set(PowerControl.VADJ1, on=True)
    time.sleep(0.05)

    # c. Set V_ADJ1 to 5.0 V via IDAC channel 1
    idac_status = dev.idac_get_status()
    if idac_status["present"]:
        print("Setting V_ADJ1 = 5.0 V via IDAC...")
        dev.idac_set_voltage(1, 5.0)    # IDAC ch 1 = V_ADJ1
        time.sleep(0.2)

        # Read back what voltage was achieved
        idac_status = dev.idac_get_status()
        ch1 = idac_status["channels"][1]
        print(f"  IDAC ch1: code={ch1.code}  target={ch1.target_v:.3f} V  actual={ch1.actual_v:.3f} V")

    # d. Enable e-fuse 1 (protects port P1)
    dev.power_set(PowerControl.EFUSE1, on=True)
    print("E-fuse 1 enabled.")
    time.sleep(0.05)

    # Verify no e-fuse faults
    pwr = dev.power_get_status()
    if not any(pwr["efuse_faults"]):
        print("All e-fuses healthy — no faults detected.")
    else:
        for i, fault in enumerate(pwr["efuse_faults"]):
            if fault:
                print(f"  WARNING: E-fuse {i+1} FAULT — check for overcurrent or short circuit!")


    # ── 4. IDAC calibration workflow ────────────────────────────────────────
    # Use this when factory calibration is insufficient or after hardware changes.
    # You need an external DMM to measure the actual output voltage.

    print("\n--- IDAC Calibration (interactive example — skipped in automated run) ---")
    CALIBRATE = False   # set to True and provide measured voltages to calibrate

    if CALIBRATE:
        import builtins

        ch = 1   # calibrate V_ADJ1

        print(f"Calibrating IDAC channel {ch}  (V_ADJ1)")
        print("You will need an external DMM on the output.")

        # Add two calibration points: one low, one high
        for code in [-80, 80]:
            dev.idac_set_code(ch, code)
            time.sleep(0.3)
            measured_str = builtins.input(f"  Measure voltage for code={code:+4d}: ")
            measured_v   = float(measured_str)
            dev.idac_cal_add_point(ch, code, measured_v)
            print(f"  Point added: code={code:+4d} → {measured_v:.4f} V")

        # Save calibration to NVS so it survives power cycles
        dev.idac_cal_save()
        print("Calibration saved to NVS.")


    # ── 5. Safe power-down sequence ─────────────────────────────────────────
    print("\n--- Powering down ---")

    # Disable output e-fuses first
    for ctrl in [PowerControl.EFUSE4, PowerControl.EFUSE3,
                 PowerControl.EFUSE2, PowerControl.EFUSE1]:
        dev.power_set(ctrl, on=False)

    # Disable adjustable regulators
    dev.power_set(PowerControl.VADJ2, on=False)
    dev.power_set(PowerControl.VADJ1, on=False)
    time.sleep(0.05)

    # Disable ±15 V supply last (AVDD needs to stay up while channels are active)
    dev.power_set(PowerControl.V15A, on=False)
    print("All supplies disabled.")
