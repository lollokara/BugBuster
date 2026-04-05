"""
BugBuster MCP — Prompt templates for common hardware debugging workflows.
"""

from __future__ import annotations
from mcp.types import PromptMessage, TextContent


def register(mcp) -> None:

    @mcp.prompt()
    def debug_unknown_device() -> list[PromptMessage]:
        """
        Non-invasive characterization workflow for an unknown connected device.
        Use when a device is connected and you need to identify and characterize it.
        """
        return [PromptMessage(
            role="user",
            content=TextContent(type="text", text="""
You are connected to a BugBuster hardware debugging platform.
A device under test (DUT) is connected. Your task is to identify and
characterize it safely, starting with non-invasive measurements.

Follow this workflow:

1. ORIENT — Call device_status to understand the current BugBuster state.
   Call selftest to verify all internal supplies are healthy.

2. NON-INVASIVE SCAN — Do not drive outputs yet.
   a. Configure IO 1 as ANALOG_IN, call read_voltage.
      - If > 0.5 V: the DUT has power. Note the voltage.
      - If ~0 V: the DUT may be unpowered or the IO is wrong.
   b. Configure IO 4 as ANALOG_IN, read voltage.
   c. Try other analog IOs (7, 10) similarly.
   d. Configure several digital IOs as DIGITAL_IN, read_digital.

3. IDENTIFY POWER RAILS — If the DUT needs power:
   a. Call set_supply_voltage(rail=1, voltage=3.3) for 3.3 V.
   b. Enable e-fuse: power_control("efuse1", True).
   c. Configure IO 1 as ANALOG_OUT, write_voltage(1, 3.3).
   d. Monitor with read_voltage on an adjacent IO.

4. PROBE UART — Look for serial output:
   a. setup_serial_bridge(tx_io=3, rx_io=6, baudrate=115200).
   b. Watch USB CDC #1 for output after DUT power-on/reset.
   c. Try common bauds: 9600, 115200, 921600.

5. TRY SWD — If ARM Cortex-M suspected (check_faults, any SWD activity):
   a. setup_swd(target_voltage_mv=3300, connector=0).
   b. Use OpenOCD/pyOCD externally to connect.

6. REPORT — Summarize: detected voltages, communication interfaces, any faults.
""".strip()),
        )]

    @mcp.prompt()
    def measure_signal() -> list[PromptMessage]:
        """
        Structured signal measurement workflow for a specific IO port.
        Use when you need to characterize an electrical signal.
        """
        return [PromptMessage(
            role="user",
            content=TextContent(type="text", text="""
You need to measure an electrical signal on a BugBuster IO port.

Workflow:

1. CHECK STATE — Call device_status to see the current IO configuration.
   Call check_faults to clear any existing fault state.

2. CONFIGURE IO:
   - For DC voltage: configure_io(io=N, mode="ANALOG_IN")
   - For 4-20 mA: configure_io(io=N, mode="CURRENT_IN")
   - For resistance/temperature: configure_io(io=N, mode="RTD")
   - For digital logic: configure_io(io=N, mode="DIGITAL_IN")

3. SINGLE READING — Take one reading to verify the IO is working:
   - read_voltage / read_current / read_resistance / read_digital

4. STATISTICAL CAPTURE — For signal characterization:
   - capture_adc_snapshot(io=N, duration_s=2.0)
   - This returns min, max, mean, stddev, estimated frequency, and a waveform preview.

5. INTERPRET RESULTS:
   - DC signal: report mean ± stddev
   - AC signal: report frequency_hz, peak_to_peak_v, amplitude
   - If frequency_hz is None: likely DC or too slow to detect
   - If stddev > 0.01 V on a supposed DC signal: noise or ripple present

6. REPORT — Include: IO number, mode, mean value, units, signal type (DC/AC),
   frequency (if AC), and any anomalies.
""".strip()),
        )]

    @mcp.prompt()
    def program_target() -> list[PromptMessage]:
        """
        Firmware programming workflow via SWD debug probe (requires HAT).
        Use when you need to flash firmware to an ARM Cortex-M target.
        """
        return [PromptMessage(
            role="user",
            content=TextContent(type="text", text="""
You need to program a target MCU via SWD through the BugBuster HAT.

Prerequisites: BugBuster HAT expansion board is physically connected.

Workflow:

1. VERIFY HAT — Call device_status and check hat.detected = true.
   If not detected, the HAT is not connected or not powered.

2. CONFIGURE POWER — Set the target's VCC voltage:
   a. set_supply_voltage(rail=1, voltage=3.3) for the target supply.
   b. power_control("efuse1", True) to enable e-fuse protection.
   c. Connect VCC to the target via the IO_Block VCC pin.

3. SETUP SWD — setup_swd(target_voltage_mv=3300, connector=0).
   This configures SWCLK/SWDIO and enables the CMSIS-DAP interface.

4. CONNECT WITH PROGRAMMER — Use an external tool:
   - OpenOCD: `openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg`
   - pyOCD: `pyocd flash --target=cortex_m firmware.bin`
   The RP2040 HAT appears as a CMSIS-DAP USB device.

5. VERIFY — After programming:
   a. Reset the target: toggle its NRST pin or power cycle via IO.
   b. Read voltage on power pins to confirm target is running.
   c. setup_serial_bridge to capture UART boot messages.

6. REPORT — Success/failure, firmware hash if available, boot output.
""".strip()),
        )]

    @mcp.prompt()
    def power_cycle_test() -> list[PromptMessage]:
        """
        Power cycle testing workflow for reliability testing.
        Use to verify a device behaves correctly across power cycles.
        """
        return [PromptMessage(
            role="user",
            content=TextContent(type="text", text="""
You need to perform power cycle testing on a device under test (DUT).

Workflow:

1. BASELINE — Before the first power cycle:
   a. device_status — record initial state.
   b. read_voltage on all active IO ports — record baseline voltages.
   c. Note any UART output from the DUT (if serial bridge is set up).

2. POWER DOWN — Disable the DUT's power:
   a. write_voltage(io=N, voltage=0.0) — reduce supply voltage first.
   b. power_control("efuse1", False) — disable e-fuse (cuts power).
   c. Note timestamp and any fault events.

3. WAIT — Allow time for capacitors to discharge.
   (At least 500 ms for most devices.)

4. POWER UP — Re-enable power:
   a. power_control("efuse1", True).
   b. write_voltage(io=N, voltage=3.3) — ramp back to operating voltage.
   c. Note timestamp.

5. MONITOR BOOT:
   a. read_voltage during boot ramp — should reach nominal within 50 ms.
   b. read_digital on status/LED IO — watch for boot indication.
   c. Monitor UART output for boot messages.

6. COMPARE — Check against baseline:
   a. Voltages match baseline within tolerance (±2%).
   b. Digital outputs match expected state.
   c. No new faults in check_faults.

7. REPEAT — Perform N cycles (typically 10-100) and report pass/fail summary.
""".strip()),
        )]
