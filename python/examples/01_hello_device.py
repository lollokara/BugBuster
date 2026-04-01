"""
Example 01 — Hello Device
=========================

Connect to the BugBuster and read basic device information.
Works with both USB and HTTP transports.

Demonstrates:
  - USB connection via binary protocol
  - HTTP connection via WiFi REST API
  - Reading firmware version, silicon ID, and die temperature
  - Device reset to a known-good state
"""

import sys
sys.path.insert(0, "..")   # allow running from the examples/ directory

import bugbuster as bb

# ── USB connection ──────────────────────────────────────────────────────────
# Adjust the port for your OS:
#   Linux/Mac:  /dev/ttyACM0  or  /dev/tty.usbmodem*
#   Windows:    COM3  (check Device Manager)

# Adjust for your OS:  macOS: /dev/cu.usbmodem*   Linux: /dev/ttyACM0   Windows: COM3
USB_PORT = "/dev/cu.usbmodem1234561"

print("=== Connecting via USB ===")
with bb.connect_usb(USB_PORT) as dev:

    # Firmware version comes from the binary handshake response — no extra command needed
    major, minor, patch = dev.get_firmware_version()
    print(f"Firmware version: {major}.{minor}.{patch}")

    # Read silicon identification (read-only, set at factory)
    info = dev.get_device_info()
    print(f"Silicon rev: {info.silicon_rev:#04x}")
    print(f"Silicon ID:  {info.silicon_id0:#06x} / {info.silicon_id1:#06x}")
    print(f"SPI healthy: {info.spi_ok}")

    # Ping — measures round-trip latency over USB
    result = dev.ping(token=0x12345678)
    print(f"Ping OK — uptime: {result.uptime_ms} ms")

    # Full status snapshot — die temperature from the AD74416H internal sensor
    status = dev.get_status()
    print(f"Die temperature: {status['die_temp_c']:.1f} °C")

    # Print per-channel function codes
    for ch in status["channels"]:
        func_name = bb.ChannelFunction(ch["function"]).name
        print(f"  Channel {ch['id']}: {func_name}  ADC={ch['adc_value']:.4f}")

    # Reset all channels to HIGH_IMP — safe starting point before reconfiguring
    dev.reset()
    print("Device reset to HIGH_IMP on all channels.")


# ── HTTP connection ─────────────────────────────────────────────────────────
# Connect when the device is on WiFi.  The default AP address is 192.168.4.1.

HTTP_HOST = "192.168.4.1"

print("\n=== Connecting via HTTP ===")
with bb.connect_http(HTTP_HOST) as dev:

    major, minor, patch = dev.get_firmware_version()
    print(f"Firmware version: {major}.{minor}.{patch}")

    status = dev.get_status()
    print(f"Die temperature: {status['die_temp_c']:.1f} °C")

    info = dev.get_device_info()
    print(f"Silicon rev: {info.silicon_rev:#04x}")
