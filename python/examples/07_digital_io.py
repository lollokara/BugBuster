"""
Example 07 — Digital IO (ESP32 GPIO)
=====================================

Demonstrates direct control of the 12 ESP32 GPIO-based digital IOs.

These IOs are the raw ESP32 GPIO pins that, on the final PCB, connect
through the ADGS2414D MUX matrix and TXS0108E level shifters to the
physical terminal blocks.  In breadboard mode they are directly accessible
on the ESP32 dev-board headers.

IO numbering and GPIO mapping
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Each IO_Block has 3 IOs.  The third IO in each block is also the one
that can be routed to the AD74416H analog channel via the MUX.

    +-----------+-----+------------------+-----------------------------------+
    | IO_Block  | IO  | GPIO (breadboard)| GPIO (PCB)                        |
    +-----------+-----+------------------+-----------------------------------+
    | 1 (VADJ1) |  1  | 15               |  4                                |
    |           |  2  |  3               |  2                                |
    |           |  3  |  2               |  1     (analog-capable, Ch A)     |
    +-----------+-----+------------------+-----------------------------------+
    | 2 (VADJ1) |  4  | 36               |  7                                |
    |           |  5  | 35               |  6                                |
    |           |  6  | 21               |  5     (analog-capable, Ch B)     |
    +-----------+-----+------------------+-----------------------------------+
    | 3 (VADJ2) |  7  | 39               |  8                                |
    |           |  8  | 38               | 12                                |
    |           |  9  | 37               | 10     (analog-capable, Ch D)     |
    +-----------+-----+------------------+-----------------------------------+
    | 4 (VADJ2) | 10  | 48               | 11                                |
    |           | 11  | 47               | 12                                |
    |           | 12  | 40               | 13     (analog-capable, Ch C)     |
    +-----------+-----+------------------+-----------------------------------+

IO modes
~~~~~~~~
    +-------+----------+-----------------------------------------------------+
    | Value | Name     | Description                                         |
    +-------+----------+-----------------------------------------------------+
    |   0   | disabled | High-impedance, pin not driven or read (default)    |
    |   1   | input    | Digital input — read logic level with dio_read()    |
    |   2   | output   | Digital output — drive with dio_write()             |
    +-------+----------+-----------------------------------------------------+

Works with both USB and HTTP transports.

NOTE: On the final PCB, these GPIOs pass through the MUX.  You must
      also configure the correct MUX switch (via mux_set_all / HAL configure)
      for the signal to actually reach the physical terminal pin.
      In breadboard mode the GPIOs are directly accessible.
"""

import time
import sys
sys.path.insert(0, "..")

import bugbuster as bb

# ---------------------------------------------------------------------------
# Connection — choose USB or HTTP
# ---------------------------------------------------------------------------

# USB (binary protocol, lowest latency)
# Adjust for your OS:  macOS: /dev/cu.usbmodem*   Linux: /dev/ttyACM0
USB_PORT = "/dev/cu.usbmodem1234561"

# HTTP (WiFi REST API — get the IP from bb.wifi_get_status() over USB,
#       or check your router's DHCP lease table)
# HTTP_HOST = "192.168.3.102"

# Pick one:
dev = bb.connect_usb(USB_PORT)
# dev = bb.connect_http(HTTP_HOST)

with dev:
    print("Connected:", dev.ping())

    # =====================================================================
    # 1. READ ALL IO STATES
    # =====================================================================
    # dio_get_all() returns a list of 12 dicts, one per IO.
    # Each dict has: io, gpio, mode, output, input

    print("\n--- All 12 IOs ---")
    ios = dev.dio_get_all()
    for io in ios:
        mode_name = ["disabled", "input", "output"][io["mode"]]
        print(f"  IO {io['io']:2d}  GPIO {io['gpio']:2d}  {mode_name:8s}  "
              f"out={io['output']}  in={io['input']}")


    # =====================================================================
    # 2. CONFIGURE AN IO AS OUTPUT
    # =====================================================================
    # dio_configure(io, mode)
    #   io:   1–12
    #   mode: 0=disabled, 1=input, 2=output

    print("\n--- Configure IO 1 as output ---")
    dev.dio_configure(1, 2)     # IO 1 → output mode


    # =====================================================================
    # 3. WRITE OUTPUT LEVEL
    # =====================================================================
    # dio_write(io, value)
    #   io:    1–12 (must be mode=2)
    #   value: True=HIGH, False=LOW

    print("--- Toggle IO 1 ---")
    for state in [True, False, True, False]:
        dev.dio_write(1, state)
        time.sleep(0.1)
        # Read back to confirm
        r = dev.dio_read(1)
        print(f"  IO 1 = {'HIGH' if r['value'] else 'LOW'}  (mode={r['mode']})")


    # =====================================================================
    # 4. CONFIGURE AN IO AS INPUT
    # =====================================================================

    print("\n--- Configure IO 2 as input ---")
    dev.dio_configure(2, 1)     # IO 2 → input mode


    # =====================================================================
    # 5. READ INPUT LEVEL
    # =====================================================================
    # dio_read(io) returns {"io": N, "mode": M, "value": True/False}
    # For inputs, the value is read live from the pin.
    # For outputs, the value is the last written level.

    print("--- Read IO 2 ---")
    for i in range(5):
        r = dev.dio_read(2)
        print(f"  Read {i+1}: IO 2 = {'HIGH' if r['value'] else 'LOW'}")
        time.sleep(0.2)


    # =====================================================================
    # 6. MULTIPLE IOS — BLINK PATTERN
    # =====================================================================
    # Configure several IOs as output and create a walking pattern.

    print("\n--- Walking pattern on IO 1–3 ---")
    for io in [1, 2, 3]:
        dev.dio_configure(io, 2)    # output

    for _ in range(3):
        for io in [1, 2, 3]:
            dev.dio_write(io, True)
            time.sleep(0.15)
            dev.dio_write(io, False)
    print("  Pattern complete.")


    # =====================================================================
    # 7. BULK READ AFTER CHANGES
    # =====================================================================

    print("\n--- Final state ---")
    ios = dev.dio_get_all()
    for io in ios:
        if io["mode"] != 0:
            mode_name = ["disabled", "input", "output"][io["mode"]]
            print(f"  IO {io['io']:2d}: {mode_name}  out={io['output']}  in={io['input']}")


    # =====================================================================
    # 8. CLEANUP — DISABLE ALL
    # =====================================================================
    # Always disable IOs when done to avoid floating outputs.

    print("\n--- Cleanup ---")
    for io in [1, 2, 3]:
        dev.dio_configure(io, 0)    # disabled
    print("  IOs 1–3 disabled.")
