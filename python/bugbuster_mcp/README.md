# BugBuster MCP Server

An [MCP (Model Context Protocol)](https://modelcontextprotocol.io) server that exposes the BugBuster hardware debugging platform to AI models. Once configured, an AI assistant (Claude, etc.) can autonomously measure signals, control outputs, manage power, capture waveforms, and program/debug target devices — all through natural language.

## Prerequisites

- **BugBuster hardware** connected via USB or WiFi
- **macOS** (these instructions use Homebrew; adapt paths for Linux/Windows)
- **Python 3.11+** (installed via Homebrew)
- **uv** package manager (installed via Homebrew)

---

## Installation

### 1. Install uv

```bash
brew install uv
```

### 2. Create a virtual environment

From the `python/` directory:

```bash
cd /path/to/BugBuster/python

uv venv --python 3.11 .venv
```

### 3. Install the package with MCP dependencies

```bash
uv pip install --python .venv/bin/python -e ".[mcp]"
```

This installs:
- `bugbuster` — the core Python library (USB and HTTP transports)
- `mcp` — Anthropic's MCP Python SDK
- `pydantic` — input validation

---

## Finding your USB port

Before running the server, find the BugBuster's serial port.

**macOS:**
```bash
ls /dev/cu.usbmodem*
```
BugBuster exposes two CDC interfaces. Use the **first** one (lower number), which is the command/protocol port (CDC #0).

**Linux:**
```bash
ls /dev/ttyACM*
```

**Windows:**
Check Device Manager → Ports (COM & LPT). Use the lower-numbered COM port.

---

## Running the server

### USB (recommended — full functionality)

```bash
.venv/bin/python -m bugbuster_mcp \
    --transport usb \
    --port /dev/cu.usbmodemXXXXXX \
    --vlogic 3.3
```

### WiFi / HTTP

```bash
.venv/bin/python -m bugbuster_mcp \
    --transport http \
    --host 192.168.4.1 \
    --vlogic 3.3
```

> The default BugBuster WiFi AP is `BugBuster` / `bugbuster123`, IP `192.168.4.1`.

### All options

| Option | Default | Description |
|--------|---------|-------------|
| `--transport` | `usb` | `usb` (binary BBP) or `http` (WiFi REST) |
| `--port` | — | USB serial port path (required for USB transport) |
| `--host` | `192.168.4.1` | BugBuster hostname or IP (HTTP transport) |
| `--vlogic` | `3.3` | Logic level for digital IOs in volts (1.8–5.0 V) |
| `--log-level` | `WARNING` | Logging verbosity: DEBUG, INFO, WARNING, ERROR |

### Transport comparison

| Feature | USB | HTTP |
|---------|-----|------|
| Latency | < 1 ms | ~10 ms |
| ADC streaming | Yes | No |
| Logic analyzer | Yes | No |
| SWD / HAT | Yes | No |
| Register access | Yes | No |
| Remote access | No | Yes |
| Cable required | Yes | No |

---

## Integrating with Claude Code

Add the server to your Claude Code MCP configuration.

### Option A — Direct path (simplest)

Edit `~/.claude/settings.json` and add to `mcpServers`:

```json
{
  "mcpServers": {
    "bugbuster": {
      "command": "/absolute/path/to/BugBuster/python/.venv/bin/python",
      "args": [
        "-m", "bugbuster_mcp",
        "--transport", "usb",
        "--port", "/dev/cu.usbmodemXXXXXX",
        "--vlogic", "3.3"
      ]
    }
  }
}
```

Replace `/absolute/path/to/BugBuster/python` and the port with your actual values.

### Option B — Via uv run

```json
{
  "mcpServers": {
    "bugbuster": {
      "command": "uv",
      "args": [
        "run",
        "--project", "/absolute/path/to/BugBuster/python",
        "--python", "3.11",
        "python", "-m", "bugbuster_mcp",
        "--transport", "usb",
        "--port", "/dev/cu.usbmodemXXXXXX",
        "--vlogic", "3.3"
      ]
    }
  }
}
```

### Applying the configuration

In Claude Code, run `/mcp` to reload MCP servers, or restart Claude Code. The `bugbuster` server should appear as connected with a tool count.

---

## VLOGIC — important setup decision

**VLOGIC** is the logic-level voltage applied to all 12 digital IOs through the TXS0108E level shifters. It must match the target device's IO voltage.

| Target device | `--vlogic` |
|---------------|------------|
| 5 V AVR/Arduino | `5.0` |
| 3.3 V ARM / ESP32 / RP2040 | `3.3` (default) |
| 1.8 V low-power MCU | `1.8` |

**VLOGIC is fixed at server startup and cannot be changed by AI tools.** This is intentional — changing the logic level while a target is connected could damage it. If you need a different voltage, restart the server with the correct `--vlogic` value.

---

## Available tools (28 total)

### Discovery & status
| Tool | Description |
|------|-------------|
| `device_status` | Full device snapshot (channels, power, HAT, faults). Call first to orient. |
| `device_info` | Silicon ID, firmware version, transport type. |
| `check_faults` | Active hardware faults with remediation hints. |
| `selftest` | Internal self-test: supply voltages, e-fuse currents, boot status. |

### IO configuration
| Tool | Description |
|------|-------------|
| `configure_io` | Set IO mode (analog in/out, digital, current, RTD, HAT). Must be called before read/write. |
| `set_supply_voltage` | Set VADJ1 or VADJ2 (3–15 V). Cannot set VLOGIC (user-controlled). |
| `reset_device` | Safe reset: all outputs off, MUX open, HAL re-initialized. |

### Analog measurement
| Tool | Description |
|------|-------------|
| `read_voltage` | Read voltage on an ANALOG_IN IO (24-bit ADC, 0–12 V). |
| `read_current` | Read 4–20 mA loop current on a CURRENT_IN IO. |
| `read_resistance` | Read resistance in Ω, or temperature in °C for PT100/PT1000. |

### Analog output
| Tool | Description |
|------|-------------|
| `write_voltage` | Set DAC voltage on an ANALOG_OUT IO (0–12 V unipolar, ±12 V bipolar). |
| `write_current` | Set current source on a CURRENT_OUT IO (0–8 mA safe default, up to 25 mA). |

### Digital IO
| Tool | Description |
|------|-------------|
| `read_digital` | Read logic level on a DIGITAL_IN IO. |
| `write_digital` | Set logic level on a DIGITAL_OUT IO. |

### Waveform & capture
| Tool | Description |
|------|-------------|
| `start_waveform` | Generate sine/square/triangle/sawtooth on an ANALOG_OUT IO (0.01–100 Hz). |
| `stop_waveform` | Stop waveform generator (DAC holds last value). |
| `capture_adc_snapshot` | Capture N samples over T seconds; returns min/max/mean/stddev/frequency/preview. |
| `capture_logic_analyzer` | Capture 1–4 digital channels at up to 10 MHz (HAT required). |

### UART & debug
| Tool | Description |
|------|-------------|
| `setup_serial_bridge` | Route UART bridge to two IOs (TX/RX) at a specified baud rate. |
| `setup_swd` | Configure SWD debug probe for ARM Cortex-M targets (HAT required). |
| `uart_config` | Read or update UART bridge settings. |

### Power management
| Tool | Description |
|------|-------------|
| `usb_pd_status` | Read USB-C PD contract: negotiated voltage, current, available PDOs. |
| `usb_pd_select` | Request a USB PD voltage: 5/9/12/15/18/20 V. |
| `power_control` | Enable/disable power rails or e-fuses manually. |
| `wifi_status` | Read WiFi connection status, SSID, IP address. |

### Advanced (low-level)
| Tool | Description |
|------|-------------|
| `mux_control` | Direct ADGS2414D switch matrix control. Requires `i_understand_the_risk=True`. |
| `register_access` | Raw AD74416H SPI register read/write. USB only. Requires risk acknowledgment. |
| `idac_control` | Direct DS4424 current DAC control (power supply fine-tuning). |

---

## Resources

Resources provide read-only state that the AI can query for context:

| URI | Description |
|-----|-------------|
| `bugbuster://status` | Full device state JSON |
| `bugbuster://power` | Supply voltages, USB PD, e-fuse status |
| `bugbuster://faults` | Active faults with remediation hints |
| `bugbuster://hat` | HAT detection, pin config, logic analyzer state |
| `bugbuster://capabilities` | Static limits: IO modes, voltage ranges, feature availability |

---

## Prompt templates

Four guided workflows are included:

| Prompt | Use case |
|--------|----------|
| `debug_unknown_device` | Non-invasive characterization of an unknown connected device |
| `measure_signal` | Structured single-channel signal measurement with statistics |
| `program_target` | Firmware flashing via SWD (requires HAT) |
| `power_cycle_test` | Automated power cycle reliability testing |

To use a prompt in Claude Code, type `/` and select the workflow.

---

## Safety model

The server enforces hardware protection at the tool layer:

- **MUX mutual exclusivity** — `configure_io` sets exactly one signal path per IO. Each IO can be analog OR digital, not both. This is enforced by the HAL and cannot be bypassed through normal tools.
- **E-fuse auto-enable** — Configuring an IO as an output automatically enables the e-fuse (overcurrent protection) for that IO_Block.
- **Conservative current default** — `write_current` defaults to max 8 mA. Pass `allow_full_range=True` to use the full 0–25 mA range.
- **Voltage confirmation** — `set_supply_voltage` requires `confirm=True` for voltages above 12 V.
- **VLOGIC locked** — Cannot be changed by AI tools; only settable via `--vlogic` at startup.
- **Risk gates** — `mux_control` and `register_access` require `i_understand_the_risk=True` to prevent accidental low-level operations.
- **Post-action fault check** — After every output-driving tool call, e-fuse and power-good states are checked; warnings are included in the response.

### IO capability reference

```
IOs 1, 4, 7, 10  — analog-capable
  Modes: ANALOG_IN, ANALOG_OUT, CURRENT_IN, CURRENT_OUT, RTD, HART, HAT
  Plus all digital modes below

IOs 2,3,5,6,8,9,11,12  — digital only
  Modes: DIGITAL_IN, DIGITAL_OUT, DIGITAL_IN_LOW, DIGITAL_OUT_LOW, DISABLED

All IOs: DISABLED (safe default, high-impedance)
```

### Power topology

```
VADJ1 (rail 1) → IOs 1–6  (IO_Blocks 1 & 2, E-fuses 1 & 2)
VADJ2 (rail 2) → IOs 7–12 (IO_Blocks 3 & 4, E-fuses 3 & 4)
VLOGIC         → all 12 IOs (level shifters, fixed at startup)
```

---

## Troubleshooting

**Server doesn't connect to the device**
- Verify the port: `ls /dev/cu.usbmodem*` (macOS) or `ls /dev/ttyACM*` (Linux)
- Use CDC #0 (the lower-numbered port of the two BugBuster CDC ports)
- Check USB connection and that the BugBuster firmware is running

**`mcp` module not found**
```bash
uv pip install --python .venv/bin/python "mcp>=1.0"
```

**Claude Code doesn't show the server**
- Run `/mcp` in Claude Code to reload
- Check that the path in `settings.json` is absolute, not relative
- Run the command manually in a terminal to see any startup errors:
  ```bash
  .venv/bin/python -m bugbuster_mcp --transport usb --port /dev/cu.usbmodemXXXX
  ```
  (It will hang waiting for stdio — that means it started correctly. Press Ctrl+C.)

**HAT tools fail**
- SWD, logic analyzer, and `capture_logic_analyzer` require the RP2040 HAT expansion board
- Check `device_status` → `hat.detected` is `true`
- HAT commands are USB-only; HTTP transport does not support them

**Logic analyzer captures empty data**
- Use `trigger_type="none"` for an immediate (force-triggered) capture
- Verify the signal is connected to EXP_EXT pins 1–4 on the HAT connector
- The RP2040 HAT logic analyzer uses GPIO14–17 internally

**`set_supply_voltage` rejected for VLOGIC**
- VLOGIC is fixed at startup. Restart the server with `--vlogic <voltage>`.

---

## Example session

Once configured in Claude Code, you can ask the AI:

> "I have a device connected to BugBuster. Can you identify what it is and check if it's working?"

The AI will call `device_status` → `selftest` → configure IOs as `ANALOG_IN` → `read_voltage` → try `setup_serial_bridge` → report findings.

> "Measure the signal on IO 1 for 2 seconds and tell me the frequency."

The AI will call `configure_io(1, "ANALOG_IN")` → `capture_adc_snapshot(1, duration_s=2)` → report frequency and waveform statistics.

> "Set IO 4 to output 3.3 V."

The AI will call `set_supply_voltage(rail=1, voltage=3.3)` → `configure_io(4, "ANALOG_OUT")` → `write_voltage(4, 3.3)` → check for faults.

---

## Board profiles

A **board profile** is a JSON file describing the DUT's pin map, rail locks,
SWD target, and UART baudrate.  When a profile is active, the MCP safety layer
refuses to change any rail marked `locked: true`, so an AI cannot accidentally
drive VLOGIC to 5 V on a 3.3 V board.

Schema (nested — see [`Docs/board_profiles.md`](../../Docs/board_profiles.md)):

```json
{
  "name": "stm32f4_discovery",
  "description": "STM32F407 Discovery reference board",
  "vlogic": { "value": 3.3, "locked": true },
  "vadj1":  { "value": 3.3, "locked": true },
  "vadj2":  { "value": 5.0, "locked": false },
  "pins": {
    "1": { "name": "PA0_BTN", "type": "GPIO",    "direction": "IN" },
    "3": { "name": "USART2_TX", "type": "UART_TX", "direction": "OUT" },
    "4": { "name": "USART2_RX", "type": "UART_RX", "direction": "IN" },
    "8": { "name": "SWDIO",   "type": "SWD",    "direction": "INOUT" },
    "9": { "name": "SWCLK",   "type": "SWD",    "direction": "OUT" }
  },
  "swd":  { "target": "stm32f4x" },
  "uart": { "baudrate": 115200 }
}
```

Profiles live in `python/bugbuster_mcp/board_profiles/*.json` and are loaded
via three tools and one resource:

| Surface | What it does |
|---|---|
| `list_boards()` | Enumerate available profile names |
| `set_board(name)` | Activate a profile; subsequent tool calls consult it |
| `bugbuster://board` | Resource exposing the active profile (or `null`) |
| `safety.validate_vadj_voltage` / `validate_vlogic` | Reject any change that violates a locked rail |

The desktop app's **Board** tab writes profiles to the same directory, so an
exported profile is immediately visible to the MCP server.

---

## Hardware-free testing

All MCP tools can be exercised end-to-end against the in-process simulator
in `tests/mock/`, without a board:

```bash
PYTHONPATH=python:tests pytest tests/unit tests/simulator -q
PYTHONPATH=python:tests pytest tests/device --sim -q
```

The simulator implements every BBP CmdId and mirrors the firmware `/api`
schema, including the BBP v4 `macAddress` field on `/api/device/info` (required
by the desktop pairing flow).  See [`../../tests/README.md`](../../tests/README.md)
for the layered test taxonomy.
