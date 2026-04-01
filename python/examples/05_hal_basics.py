"""
Example 05 — Hardware Abstraction Layer (HAL)
=============================================

The HAL provides an Arduino-style ``configure() / read() / write()`` API
that hides all the low-level details (AD74416H registers, MUX switch matrix,
power rail sequencing, level shifters) behind simple port numbers.

Physical layout
~~~~~~~~~~~~~~~
12 IOs organised in 2 Blocks x 2 IO_Blocks x 3 IOs:

    +---------------------------------------------------------------------------+
    | BLOCK 1  (VADJ1, rail 1, 3-15 V)                                         |
    |                                                                           |
    |   IO_Block 1 (EFUSE1)            IO_Block 2 (EFUSE2)                     |
    |   +---------------------+        +---------------------+                 |
    |   | IO 1  - analog/HAT  |        | IO 4  - analog/HAT  |                 |
    |   | IO 2  - digital     |        | IO 5  - digital     |                 |
    |   | IO 3  - digital     |        | IO 6  - digital     |                 |
    |   | VCC   GND           |        | VCC   GND           |                 |
    |   +---------------------+        +---------------------+                 |
    +---------------------------------------------------------------------------+
    | BLOCK 2  (VADJ2, rail 2, 3-15 V)                                         |
    |                                                                           |
    |   IO_Block 3 (EFUSE3)            IO_Block 4 (EFUSE4)                     |
    |   +---------------------+        +---------------------+                 |
    |   | IO 7  - analog/HAT  |        | IO 10 - analog/HAT  |                 |
    |   | IO 8  - digital     |        | IO 11 - digital     |                 |
    |   | IO 9  - digital     |        | IO 12 - digital     |                 |
    |   | VCC   GND           |        | VCC   GND           |                 |
    |   +---------------------+        +---------------------+                 |
    +---------------------------------------------------------------------------+

IO capabilities (each IO can only be ONE mode at a time via MUX):
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
+--------------------+-------------------------------+----------+-----------+
| Mode               | Description                   | IO 1,4,  | IO 2,3,   |
|                    |                               | 7,10     | 5..12     |
+--------------------+-------------------------------+----------+-----------+
| DISABLED           | Safe default / disconnected    | yes      | yes       |
| ANALOG_IN          | ADC voltage input  (0-12 V)   | yes      | no        |
| ANALOG_OUT         | DAC voltage output (0-12 V)   | yes      | no        |
| CURRENT_IN         | 4-20 mA input (ext. powered)  | yes      | no        |
| CURRENT_OUT        | 4-20 mA current source        | yes      | no        |
| DIGITAL_IN         | GPIO input  - high drive       | yes      | yes       |
| DIGITAL_OUT        | GPIO output - high drive       | yes      | yes       |
| DIGITAL_IN_LOW     | GPIO input  - low drive        | yes      | yes       |
| DIGITAL_OUT_LOW    | GPIO output - low drive        | yes      | yes       |
| RTD                | Resistance / PT100 measurement | yes      | no        |
| HART               | HART modem overlay (4-20 mA)  | yes      | no        |
| HAT                | Passthrough to HAT expansion   | yes      | no        |
+--------------------+-------------------------------+----------+-----------+

Supply rails:
~~~~~~~~~~~~~
+-------------+-----------+-----------------------------------------------+
| Rail        | Range     | Controls                                       |
+-------------+-----------+-----------------------------------------------+
| VADJ1       | 3-15 V    | VCC pin on IO_Block 1 & 2 (IO 1-6)            |
| VADJ2       | 3-15 V    | VCC pin on IO_Block 3 & 4 (IO 7-12)           |
| VLOGIC      | 1.8-5.0 V | Logic level for all digital IOs (level shifted)|
+-------------+-----------+-----------------------------------------------+

MUX switch matrix:
~~~~~~~~~~~~~~~~~~
Each IO_Block is served by one ADGS2414D octal switch.  The switches are
MUX-exclusive: closing one switch in a group opens all others in that group.

    +-------+----------------+----------------------------------------------+
    | Group | Switches       | IO / function                                |
    +-------+----------------+----------------------------------------------+
    | A     | S1-S4 (0x0F)   | Analog IO (pos. 1): ADC, ESP high/low, HAT  |
    | B     | S5-S6 (0x30)   | Digital IO (pos. 2): ESP high/low drive      |
    | C     | S7-S8 (0xC0)   | Digital IO (pos. 3): ESP high/low drive      |
    +-------+----------------+----------------------------------------------+
"""

import time
import sys
sys.path.insert(0, "..")

import bugbuster as bb
from bugbuster import PortMode

# ---------------------------------------------------------------------------
# Port auto-detection helper (works on macOS, Linux, and Windows)
# ---------------------------------------------------------------------------

def find_bugbuster_port() -> str:
    """Try common BugBuster serial port names and return the first that exists."""
    import glob
    candidates = (
        glob.glob("/dev/cu.usbmodem*")       # macOS
        + glob.glob("/dev/ttyACM*")           # Linux
        + [f"COM{i}" for i in range(3, 20)]   # Windows
    )
    for port in candidates:
        try:
            import serial
            s = serial.Serial(port, 921600, timeout=0.5)
            s.close()
            return port
        except Exception:
            continue
    raise RuntimeError(
        "No BugBuster USB device found. Check the cable and try again."
    )


USB_PORT = find_bugbuster_port()
print(f"Using port: {USB_PORT}\n")


# ===========================================================================
# Main example
# ===========================================================================

with bb.connect_usb(USB_PORT) as dev:

    # Access the HAL through the client's .hal property.
    # The HAL is created lazily on first access.
    hal = dev.hal

    # =====================================================================
    # 1. POWER UP
    # =====================================================================
    # begin() performs the full power-on sequence:
    #   - Enable +/-15 V analog supply (AD74416H AVDD)
    #   - Enable MUX switch matrix + level shifters
    #   - Set VLOGIC to the requested voltage
    #   - Open all MUX switches (safe state)
    #   - Reset all AD74416H channels to HIGH_IMP
    #
    # supply_voltage: default VADJ when IO_Blocks are first enabled.
    # vlogic:         logic level for all digital IOs (1.8-5.0 V).

    print("--- Power Up ---")
    hal.begin(supply_voltage=12.0, vlogic=3.3)
    print("  HAL ready.\n")


    # =====================================================================
    # 2. SUPPLY VOLTAGE CONTROL
    # =====================================================================
    # Two independent adjustable supplies:
    #   rail 1 (VADJ1) -> IO 1-6   (IO_Block 1 & 2)
    #   rail 2 (VADJ2) -> IO 7-12  (IO_Block 3 & 4)
    #
    # set_voltage(rail, voltage) sets the DCDC converter output.
    # The voltage appears on the VCC pin of the IO_Block's terminal.
    #
    # set_vlogic(voltage) sets the logic level for ALL digital IOs.
    # All digital signals pass through TXS0108E level shifters to this level.

    print("--- Supply Voltages ---")
    hal.set_voltage(rail=1, voltage=12.0)   # IO 1-6 VCC -> 12 V
    hal.set_voltage(rail=2, voltage=5.0)    # IO 7-12 VCC -> 5 V
    hal.set_vlogic(3.3)                      # digital logic -> 3.3 V
    print("  VADJ1=12V, VADJ2=5V, VLOGIC=3.3V\n")


    # =====================================================================
    # 3. ANALOG OUTPUT (IO 1, 4, 7, 10 only)
    # =====================================================================
    # configure() is the equivalent of Arduino's pinMode().
    # It handles: power sequencing, MUX routing, AD74416H channel setup.
    #
    # write_voltage() sets the DAC output in volts.

    print("--- Analog Output (IO 1) ---")
    hal.configure(1, PortMode.ANALOG_OUT)           # 0-12 V unipolar
    hal.write_voltage(1, 5.0)
    print("  IO 1 -> 5.0 V output")

    # Bipolar mode: configure() with bipolar=True enables -12 to +12 V range
    # hal.configure(1, PortMode.ANALOG_OUT, bipolar=True)
    # hal.write_voltage(1, -3.0, bipolar=True)

    time.sleep(0.1)


    # =====================================================================
    # 4. ANALOG INPUT (IO 1, 4, 7, 10 only)
    # =====================================================================
    # read_voltage() returns the measured voltage from the AD74416H ADC.
    #
    # IMPORTANT: After configuring channels, wait >= 1 second before reading.
    # The AD74416H has a single shared ADC that sequences across all active
    # channels.  The first reading after a configuration change may be stale.

    print("\n--- Analog Input (IO 4) ---")
    hal.configure(4, PortMode.ANALOG_IN)
    time.sleep(1.0)   # ADC settle time

    readings = [hal.read_voltage(4) for _ in range(5)]
    avg = sum(readings) / len(readings)
    print(f"  IO 4 average over 5 reads: {avg:.4f} V")


    # =====================================================================
    # 5. CURRENT OUTPUT / INPUT (IO 1, 4, 7, 10 only)
    # =====================================================================
    # 4-20 mA process loop support:
    #   CURRENT_OUT: sources current (0-25 mA, typically 4-20 mA)
    #   CURRENT_IN:  measures current through internal 12 ohm RSENSE
    #
    # write_current() sets output in milliamps.
    # read_current()  returns measured current in milliamps.

    print("\n--- Current Output (IO 7) ---")
    hal.configure(7, PortMode.CURRENT_OUT)

    for pct in [0, 25, 50, 75, 100]:
        ma = 4.0 + (pct / 100.0) * 16.0   # 4-20 mA from 0-100%
        hal.write_current(7, ma)
        time.sleep(0.05)
        print(f"  {pct:3d}% -> {ma:.2f} mA")


    # =====================================================================
    # 6. RTD / RESISTANCE MEASUREMENT (IO 1, 4, 7, 10 only)
    # =====================================================================
    # Measures resistance using the AD74416H's built-in excitation current.
    #
    # Default: 1 mA excitation -> max ~625 ohm (good for PT100 up to ~260 C)
    # Optional: 500 uA excitation -> max ~1250 ohm (for PT1000 or high-R sensors)
    #   hal.configure(10, PortMode.RTD, rtd_ma_1=False)  # 500 uA
    #
    # read_resistance()         -> ohms
    # read_temperature_pt100()  -> degrees C (linear Callendar-Van Dusen)
    # read_temperature_pt1000() -> degrees C

    print("\n--- RTD Measurement (IO 10) ---")
    hal.configure(10, PortMode.RTD)
    time.sleep(1.0)   # ADC settle

    ohms = hal.read_resistance(10)
    temp = hal.read_temperature_pt100(10)
    print(f"  Resistance: {ohms:.2f} ohm")
    print(f"  PT100 temp: {temp:.1f} C")


    # =====================================================================
    # 7. DIGITAL OUTPUT (all 12 IOs)
    # =====================================================================
    # Two drive strength options:
    #   DIGITAL_OUT     -> high drive (through main DO driver or ESP GPIO)
    #   DIGITAL_OUT_LOW -> low drive  (through low-current ESP GPIO path)
    #
    # write_digital(io, True/False) drives the pin high or low.

    print("\n--- Digital Output (IO 2, high drive) ---")
    hal.configure(2, PortMode.DIGITAL_OUT)
    hal.write_digital(2, True)
    print("  IO 2 -> HIGH")
    time.sleep(0.5)
    hal.write_digital(2, False)
    print("  IO 2 -> LOW")

    print("\n--- Digital Output (IO 3, low drive) ---")
    hal.configure(3, PortMode.DIGITAL_OUT_LOW)
    # hal.write_digital(3, True)   # low-drive ESP GPIO write (pending firmware)


    # =====================================================================
    # 8. DIGITAL INPUT (all 12 IOs)
    # =====================================================================
    # read_digital() returns True (high) or False (low).
    #
    # For analog-capable IOs in DIGITAL_IN mode, the AD74416H's DIN_LOGIC
    # comparator is used.  For digital-only IOs or low-drive mode, the
    # signal is read through the ESP GPIO via the MUX.

    print("\n--- Digital Input (IO 5) ---")
    hal.configure(5, PortMode.DIGITAL_IN)
    state = hal.read_digital(5)
    print(f"  IO 5 = {'HIGH' if state else 'LOW'}")


    # =====================================================================
    # 9. RECONFIGURE AN IO
    # =====================================================================
    # You can freely switch any IO between its supported modes.
    # configure() always transitions through a safe state (HIGH_IMP / MUX open)
    # before applying the new mode.

    print("\n--- Reconfigure IO 1: analog out -> digital out ---")
    hal.configure(1, PortMode.DIGITAL_OUT)
    hal.write_digital(1, True)
    print("  IO 1 -> DIGITAL_OUT -> HIGH")


    # =====================================================================
    # 10. SERIAL BRIDGE
    # =====================================================================
    # Route the ESP32's secondary UART to any two IOs (TX + RX).
    # The bridge connects to an external serial port managed by other programs.
    # BugBuster only controls the routing, not the serial data.
    #
    #   hal.set_serial(tx=3, rx=6)                       # 115200 baud default
    #   hal.set_serial(tx=3, rx=6, baudrate=9600)        # custom baud
    #   hal.set_serial(tx=3, rx=6, bridge=1)             # use second bridge
    #
    # NOTE: Requires the MUX routing table to be configured for the specific
    #       hardware.  On the breadboard setup this is not yet connected.

    print("\n--- Serial Bridge ---")
    print("  (skipped: requires MUX routing table for target hardware)")
    # hal.set_serial(tx=3, rx=6)


    # =====================================================================
    # 11. DISABLE IOs
    # =====================================================================
    # configure(io, PortMode.DISABLED) returns an IO to safe state:
    #   - AD74416H channel -> HIGH_IMP
    #   - MUX switches -> all open
    #   - No power state change (supply stays on for other IOs in the block)

    print("\n--- Disable IOs ---")
    for io in [1, 2, 3, 4, 5, 7, 10]:
        hal.configure(io, PortMode.DISABLED)
    print("  IOs disabled.")


    # =====================================================================
    # 12. SHUTDOWN
    # =====================================================================
    # shutdown() performs a safe power-down in reverse order:
    #   1. Reset all AD74416H channels to HIGH_IMP
    #   2. Open all MUX switches
    #   3. Disable all e-fuses
    #   4. Disable VADJ1/VADJ2 regulators
    #   5. Disable +/-15 V analog supply
    #   6. Disable level shifters + MUX power

    print("\n--- Shutdown ---")
    hal.shutdown()
    print("  HAL shutdown complete.")
