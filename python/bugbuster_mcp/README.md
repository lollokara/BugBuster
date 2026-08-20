# BugBuster MCP server

An [MCP](https://modelcontextprotocol.io) server that gives an AI model direct
control of BugBuster hardware. Once it is registered, the model can measure
signals, drive outputs, manage power rails, capture traces, scan buses, and
debug a target over SWD - on its own, without a human relaying readings.

**119 tools in 18 groups, 6 resources, 4 prompt workflows.**

## Install

Requires Python 3.11+ and BugBuster hardware on USB or WiFi.

```bash
cd python
pip install -e ".[mcp]"
```

This installs `bugbuster` (the control library), `mcp` (the Python SDK), and
`pydantic`, and puts a `bugbuster-mcp` executable on your PATH.

## Run

Find the serial port first - `ls /dev/cu.usbmodem*` (macOS),
`ls /dev/ttyACM*` (Linux), or Device Manager → Ports (Windows). BugBuster
exposes two CDC interfaces; use the **lower-numbered** one (CDC #0, the command
port).

```bash
bugbuster-mcp --transport usb --port /dev/cu.usbmodem1234561 --vlogic 3.3
```

Over WiFi instead (default AP `BugBuster` / `bugbuster123` at `192.168.4.1`):

```bash
bugbuster-mcp --transport http --host 192.168.4.1 --vlogic 3.3
```

Started correctly, the process looks like it has hung - it is waiting on stdio.
That is what an MCP server does.

| Option | Default | Description |
|---|---|---|
| `--transport {auto,usb,http}` | `auto` | `auto` picks USB when a board is attached and falls back to HTTP. Binary BBP over USB CDC, or JSON REST over WiFi |
| `--port PATH` | auto-detect | Serial port. Omit it: the server ranks ports by USB descriptor and confirms with the BBP handshake |
| `--host IP` | `192.168.4.1` | Device hostname or IP (HTTP transport) |
| `--vlogic FLOAT` | `3.3` | Digital IO logic level in volts, 1.8–5.0 |
| `--admin-token TOKEN` | - | HTTP admin auth token |
| `--log-level LEVEL` | `WARNING` | `DEBUG` / `INFO` / `WARNING` / `ERROR` |

### USB or HTTP?

USB is the full-capability transport. HTTP trades capability for remote access.

| | USB | HTTP |
|---|---|---|
| Latency | < 1 ms | ~10 ms |
| ADC streaming, logic analyzer | yes | no |
| SWD, HAT, register access | yes | no |
| Remote access | no | yes |

CDC #0 holds a single-client lock: the MCP server, the desktop app, and a serial
monitor cannot use it at the same time.

## Register with an MCP client

Claude Desktop (`claude_desktop_config.json`) or Claude Code (`~/.claude.json`):

```json
{
  "mcpServers": {
    "bugbuster": {
      "command": "bugbuster-mcp",
      "args": ["--transport", "usb",
               "--port", "/dev/cu.usbmodem1234561",
               "--vlogic", "3.3"]
    }
  }
}
```

If `bugbuster-mcp` is not on the client's PATH, point at the interpreter
instead:

```json
{
  "command": "/absolute/path/to/BugBuster/python/.venv/bin/python",
  "args": ["-m", "bugbuster_mcp", "--transport", "usb", "--port", "/dev/cu.usbmodem1234561"]
}
```

Run `/mcp` in Claude Code to reload. The server should report as connected with
a tool count.

## Set VLOGIC before connecting a target

`--vlogic` is the level-shifter voltage applied to all 12 digital IOs. It must
match the target's IO voltage.

| Target | `--vlogic` |
|---|---|
| 5 V AVR / Arduino | `5.0` |
| 3.3 V ARM / ESP32 / RP2040 | `3.3` (default) |
| 1.8 V low-power MCU | `1.8` |

**VLOGIC cannot be changed by any tool.** Raising it while a target is attached
could destroy the target, so it is fixed at startup. To change it, restart the
server.

## Tools

The authoritative list is whatever the running server advertises - `/mcp` in
Claude Code, or your client's tool inspector. The groups:

| Group | Count | Covers |
|---|---:|---|
| `discovery` | 10 | `device_status` (call this first), `device_info`, `check_faults`, `selftest`, `device_memory`, board profiles, device discovery, `link_status` / `reset_link` for control-link health and recovery |
| `io_config` | 3 | `configure_io` (required before any read/write), `set_supply_voltage`, `reset_device` |
| `analog` | 5 | `read_voltage`, `read_current`, `read_resistance`, `write_voltage`, `write_current` |
| `digital` | 2 | `read_digital`, `write_digital` |
| `waveform` | 10 | Waveform generation, ADC snapshots, logic-analyzer capture - each with a blocking form and an async `_start` / `_status` / `_result` form |
| `bus` | 9 | I²C and SPI: `plan_*` dry runs, `scan_i2c_bus`, `spi_transfer`, `spi_jedec_id`, deferred queued transactions |
| `debug` | 3 | `setup_serial_bridge`, `setup_swd`, `uart_config` |
| `target` | 3 | `target_power_up`, `enter_bootloader`, `release_bootloader` |
| `power` | 5 | USB-PD status and selection, rail/e-fuse control, WiFi status and AP password |
| `hat` | 13 | Logic Analyzer HAT: capabilities, rail status and control, calibration, LED state, LA routing, IO bank, level shifters |
| `daq` | 11 | Power Profiler Pro HAT: settings, source control, measurement, energy/charge reset, triggers |
| `daq_power` | 17 | Power-consumption profiling over the P4's own USB-HS data plane: capture (blocking and async), energy/state/periodicity report, window zoom, A/B compare, marker windows, CSV export, supply/range/rate/stability control |
| `daq_cal` | 4 | Power Profiler Pro HAT calibration flow |
| `ota` | 9 | Firmware and SPIFFS upload, release check and apply, rollback, status |
| `io_owner` | 4 | Cooperative IO leases - `io_claim`, `io_release`, `io_owner_status`, `io_force_release` |
| `advanced` | 3 | `mux_control`, `register_access`, `idac_control` - risk-gated |
| `scripting` | 1 | `run_device_script` - evaluate Python on the on-device MicroPython engine |

Two rules the model has to follow, and the tools enforce:

1. Call `device_status` first, to orient.
2. Call `configure_io` before reading or writing any IO. An IO is in analog
   **or** digital mode, never both.

### Resources

Read-only state the model can pull for context:

| URI | Contents |
|---|---|
| `bugbuster://status` | Full device state |
| `bugbuster://power` | Supply voltages, USB-PD contract, e-fuse status |
| `bugbuster://faults` | Active faults with remediation hints |
| `bugbuster://hat` | HAT detection, pin config, logic-analyzer state |
| `bugbuster://capabilities` | Static limits - IO modes, voltage ranges, feature availability |
| `bugbuster://board` | Active board profile, or `null` |

### Prompt workflows

Type `/` in Claude Code to pick one.

| Prompt | Use case |
|---|---|
| `debug_unknown_device` | Non-invasive characterisation of an unknown board |
| `measure_signal` | Structured single-channel measurement with statistics |
| `program_target` | Firmware flashing over SWD (Logic Analyzer HAT required) |
| `power_cycle_test` | Automated power-cycle reliability testing |

## Safety model

Enforced in the tool layer, below the prompt - the model cannot argue its way
past these. Constants live in [config.py](config.py); the checks are in
[safety.py](safety.py).

| Rule | Effect |
|---|---|
| MUX exclusivity | `configure_io` sets exactly one signal path per IO. Analog or digital, never both. Enforced in the HAL. |
| E-fuse auto-arm | Configuring an IO as an output enables overcurrent protection for its IO block. |
| Current ceiling | `write_current` caps at 8 mA. `allow_full_range=True` unlocks the full 25 mA. |
| Voltage confirmation | `set_supply_voltage` above 12 V requires `confirm=True`. Hard maximum 15 V. |
| VLOGIC lock | Not settable by any tool. `--vlogic` at startup only. |
| Risk gates | `mux_control` and `register_access` require `i_understand_the_risk=True`. |
| Rail lock | An active board profile can mark VLOGIC / VADJ1 / VADJ2 `locked`; changes are then rejected. |
| Post-action fault check | After every output-driving call, e-fuse and power-good state is read back and warnings are attached to the response. |

### IO capabilities

```
IOs 3, 6, 9, 12          analog-capable
                         ANALOG_IN, ANALOG_OUT, CURRENT_IN, CURRENT_OUT,
                         RTD, HART, HAT - plus every digital mode

IOs 1,2,4,5,7,8,10,11    digital only
                         DIGITAL_IN, DIGITAL_OUT, DIGITAL_IN_LOW,
                         DIGITAL_OUT_LOW, DISABLED

all IOs                  DISABLED - safe default, high impedance
```

### Power topology

```
VADJ1  →  IOs 1–6   (IO blocks 1 & 2, e-fuses 1 & 2)
VADJ2  →  IOs 7–12  (IO blocks 3 & 4, e-fuses 3 & 4)
VLOGIC →  all 12 IOs (level shifters, fixed at startup)
```

## Board profiles

A board profile is a JSON file describing a DUT's pin map, rail locks, SWD
target, and UART baud rate. With a profile active, the safety layer refuses to
change any rail marked `locked: true` - so the model cannot drive VLOGIC to 5 V
on a 3.3 V board.

```json
{
  "name": "stm32f4_discovery",
  "description": "STM32F407 Discovery reference board",
  "vlogic": { "value": 3.3, "locked": true },
  "vadj1":  { "value": 3.3, "locked": true },
  "vadj2":  { "value": 5.0, "locked": false },
  "pins": {
    "1": { "name": "PA0_BTN",   "type": "GPIO",    "direction": "IN" },
    "3": { "name": "USART2_TX", "type": "UART_TX", "direction": "OUT" },
    "8": { "name": "SWDIO",     "type": "SWD",     "direction": "INOUT" },
    "9": { "name": "SWCLK",     "type": "SWD",     "direction": "OUT" }
  },
  "swd":  { "target": "stm32f4x" },
  "uart": { "baudrate": 115200 }
}
```

Profiles live in [board_profiles/](board_profiles/). `list_boards()` enumerates
them, `set_board(name)` activates one, and `bugbuster://board` exposes the
active profile. The desktop app's **Board** tab writes to the same directory, so
an exported profile is immediately visible here.

Full schema: [Docs/board-profiles.md](../../Docs/board-profiles.md)

## Testing without hardware

Every tool can be exercised end to end against the in-process simulator:

```bash
PYTHONPATH=python:tests pytest tests/unit tests/simulator -q
PYTHONPATH=python:tests pytest tests/device --sim -q
```

The simulator implements every BBP command ID and mirrors the firmware `/api`
schema. See [tests/README.md](../../tests/README.md).

## Troubleshooting

**Server won't connect.** Confirm the port, and that it is CDC #0 (the
lower-numbered of the two). Close anything else holding it - the desktop app or
a serial monitor will block the port.

**Client doesn't show the server.** Run `/mcp` to reload. Use absolute paths in
the config. Run the command manually in a terminal to see startup errors.

**HAT tools fail.** SWD and logic-analyzer tools need the Logic Analyzer HAT.
Check `device_status` → `hat.detected`. HAT commands are USB-only.

**Logic analyzer returns nothing.** Use `trigger_type="none"` for an immediate
capture, and confirm the signal reaches the HAT LA channels - see
[Docs/logic-analyzer.md](../../Docs/logic-analyzer.md) for the two routing
options.

**`set_supply_voltage` rejected for VLOGIC.** By design. Restart with
`--vlogic <voltage>`.
