# BugBuster — Real-world scenarios

Concrete examples of what AI assistants do with BugBuster once it's wired up
via the MCP server.  Every scenario here is a plausible first-session task an
engineer might hand off verbatim.

---

## 1. Debugging a sensor that reports incorrect values

> **You:** "The temperature sensor on IO 1 reads 150 °C but the room is 22 °C. Figure out why."

> **AI:** Configures IO 1 as analog input. Reads voltage (0.48 V — consistent with
> 150 °C for a PT100). Switches to RTD mode with 4-wire excitation. Measures
> 108.7 Ω.  Notes that a PT100 at 22 °C should read ~108.9 Ω. Concludes: the
> sensor is correct; the target's ADC reference voltage is wrong. Measures the
> target's 3.3 V rail on IO 4 — reads 2.87 V.  Reports:
>
> *"Your 3.3 V reference is sagging to 2.87 V, which causes the firmware's
> voltage-to-temperature conversion to over-read by 128 °C."*

What happened under the hood:

1. `configure_io(1, "ANALOG_IN")` → `read_voltage(1)`
2. `configure_io(1, "RTD_4WIRE")` → `read_resistance(1)`
3. Internal reasoning about PT100 R-T curve
4. `configure_io(4, "ANALOG_IN")` → `read_voltage(4)`
5. `check_faults()` between each measurement to guard against overcurrent

---

## 2. Automated production test

> **You:** "Test the board on connectors A and B. Check the 3.3 V rail, verify I²C pull-ups, and confirm the status LED blinks at 1 Hz."

> **AI:** Powers connector A at 3.3 V.  Reads supply on IO 1 (3.31 V, within
> spec). Configures IO 2 as digital input, checks pull-up (reads HIGH,
> confirmed). Configures IO 3 as digital input, captures 5 seconds of data,
> detects toggling at 0.98 Hz.  Reports:
>
> *"All three checks pass. 3.3 V rail nominal, I²C pull-ups present, LED
> blinks at 0.98 Hz (within 1 Hz ±5% tolerance)."*

This exercises the capture pipeline and logic-analyzer engine on the HAT
alongside analog measurements — the kind of multi-modal sequencing that is
tedious to script by hand but trivial over MCP.

---

## 3. Reverse-engineering an unknown board

> **You:** "I found this PCB in a drawer. Figure out what it does."

> **AI:** Runs `selftest` to confirm BugBuster is healthy. Configures all 4
> analog inputs. Sweeps all 12 IOs to detect pull-ups, pull-downs, and driven
> signals.  Finds 3.3 V on IO 1, a steady DC level on IO 4, and toggling
> activity on IOs 7-8.  Sets up UART bridge on IOs 2-3, detects 115200 baud
> traffic.  Captures logic-analyzer data on IOs 7-8 at 1 MHz, decodes I²C,
> identifies device address `0x48`.  Reports:
>
> *"This is a temperature-monitoring board with an I²C sensor (likely TMP117
> at `0x48`), serial debug output at 115200 baud, and a 3.3 V enable line.
> The sensor is currently reading 23.5 °C."*

This one uses essentially every BugBuster subsystem at least once: ADC sweep,
digital IO probing, UART bridge, logic analyzer with protocol decoder, and the
MUX matrix to route signals through the correct pins.

---

## See also

- [`../python/bugbuster_mcp/README.md`](../python/bugbuster_mcp/README.md) — tool
  catalog, safety model, and how these workflows map to individual MCP calls.
- [`LogicAnalyzer.md`](LogicAnalyzer.md) — vendor-bulk streaming architecture
  used by scenario 3.
- [`Hardware.md`](Hardware.md) — IC list and power topology for the board the
  scenarios drive.
