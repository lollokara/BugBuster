# BugBuster Test Suite

Test framework for the BugBuster industrial I/O board.  Covers the Python API,
the HTTP REST endpoints, and - via both - the ESP32 and RP2040 firmware.

The suite has five layers:

| Layer | Dir | Needs hardware? | What it validates |
|---|---|---|---|
| **Unit** | `tests/unit/` | no | Pure-Python logic: parsers, HAL routing, HAT guards, rail-lock enforcement, auth flow - **plus the source-parity guards below** |
| **Simulator** | `tests/simulator/` | no | End-to-end BBP + HTTP round-trips against `SimulatedDevice` (140 BBP handlers, `/api/*` surface) |
| **Mock** | `tests/mock/` | no | `SimulatedDevice`, `SimulatedUSBTransport`, `SimulatedHTTPTransport` - shared fixtures used by the simulator and device layers |
| **Synthetic** | `tests/synthetic/` | no | Regression tests for LA USB bulk/streaming protocol, generated stimuli, timing edge-cases |
| **Device** | `tests/device/` | yes (or `--sim`) | The same tests, driven against real hardware over USB / HTTP, or against the simulator with `--sim` |

Current posture (2026-08-07): **1144 passing**, 163 skipped, 2 xpassed across
`unit + synthetic + simulator + device --sim`, plus 6 passing and 17 skipped in
`integration + http_api` (`--sim-full`). Skips are hardware-only paths
(HAT / SWD / LA / DAQ).

```bash
# The full hardware-free run, as CI executes it
PYTHONPATH=python pytest tests/unit tests/synthetic tests/simulator tests/device --sim -q
PYTHONPATH=python pytest tests/integration tests/http_api --sim-full -q
```

## Source-parity guards (the tests that read the firmware)

A large part of `tests/unit/` does not exercise Python at all - it **parses the
firmware C/C++ sources** and asserts the host, the simulator and the firmware
still agree. This exists because the expensive bugs on this project have all
been drift bugs, not logic bugs: a constant retyped in three places, or a
simulator written against a broken client parser instead of against the
firmware (which kept the whole suite green while the device returned garbage).

The rule: **derive the constant from the firmware source in the test, never
retype it.**

| Guard | Reads | Catches |
|---|---|---|
| `test_bbp_command_parity.py` | `bbp.h`, `constants.py`, `client.py` | opcodes defined in firmware but unreachable from the client; duplicate opcodes; `AdcRate` codes the simulator accepts but the device rejects |
| `test_api_route_parity.py` | `api_core.cpp`, `webserver.cpp` | an HTTP route implemented but never registered |
| `test_idac_wire_format.py` | `cmd_idac.cpp` | per-channel record layout drift between firmware and client |
| `test_mux_device_parity.py` | `tasks.cpp` + host/desktop/web sources | the logical→MUX-device C/D swap disagreeing across surfaces |
| `test_memory_telemetry.py` | `tasks.h`, `bbp.h`, `bbp.cpp`, `cmd_registry.cpp` | task-stack sizing, internal-DRAM regressions (see below) |
| `test_direct_daq_ota.py` | `tasks.h`, `main.cpp`, `ble_service.cpp` | task stacks shrunk below their measured peak; OTA workers moved to PSRAM stacks |

These fail on a **firmware** edit, which is the point - change
`ad74416h_regs.h`'s `AdcRate` enum and the Python suite goes red until the
simulator is updated to match.

### Memory-pressure guards

`tests/unit/test_memory_telemetry.py` additionally pins the ESP32-S3's internal
SRAM posture, which is the resource this board actually runs out of:

- the BBP command registry must stay a **pointer index** into the flash-resident
  `static const` descriptor blocks, never a `memcpy`'d copy (that copy cost
  8 KB of internal `.bss`);
- `CMD_REGISTRY_MAX` must still cover every registered descriptor, so a new
  subsystem block cannot be silently dropped at boot;
- seven named scratch buffers must stay **file-scope** `static EXT_RAM_BSS_ATTR`
 - the attribute is silently ignored on function-scope statics, so a `static`
  array declared inside a function lands in internal DRAM no matter what;
- no function-scope `static` arrays may reappear in `ws_stream.cpp`,
  `repl_ws.cpp` or `hat.cpp`, where that trap has already bitten;
- `wavegen_stop_and_reset()` must keep its `wavegen.active` early-return, which
  is what stops every USB disconnect from enqueueing a deep handler onto
  `cmdProc`'s stack.

Runtime memory has its own tooling - see
[`Docs/memory-testing.md`](../Docs/memory-testing.md) and
`tests/tools/mem_watch.py`.

## Setup

```bash
# From repo root - works on macOS and Linux
bash tests/setup.sh
source .venv/bin/activate
```

`setup.sh` auto-detects Python 3.11+, creates `.venv/`, installs the `bugbuster`
library in editable mode (with the `mcp` extra), installs test dependencies, and
optionally builds the ESP32 web bundle if `pnpm` is available.

Manual steps (if you prefer):

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e 'python/[mcp]'          # zsh: quotes prevent glob expansion
pip install -r tests/requirements-test.txt
```

> **Why a venv?** macOS ships a system Python managed by the OS. A virtual
> environment keeps everything isolated and reproducible.

## Environment variables

| Variable | Purpose | Example |
|---|---|---|
| `BUGBUSTER_PORT` | Serial port for USB transport | `/dev/cu.usbmodem1234` |
| `BUGBUSTER_HTTP_HOST` | Host/IP for HTTP transport | `192.168.4.1` |
| `BUGBUSTER_ADMIN_TOKEN` | Admin token for destructive HTTP endpoints | `abc123` |

These are read by both `run_tests.py` and the notebooks. Set them in your shell
or `.env` file before running tests:

```bash
export BUGBUSTER_PORT=/dev/cu.usbmodem1234
export BUGBUSTER_HTTP_HOST=192.168.4.1
export BUGBUSTER_ADMIN_TOKEN=abc123
```

## Running tests

### Quick start

```bash
source .venv/bin/activate

# USB-connected device
python tests/run_tests.py --usb /dev/cu.usbmodem1234

# WiFi / HTTP device
python tests/run_tests.py --http 192.168.4.1

# Both transports + HAT expansion board
python tests/run_tests.py --usb /dev/cu.usbmodem1234 --http 192.168.4.1 --hat
```

### Common options

| Flag | Description |
|------|-------------|
| `--usb <port>` | Serial port for USB connection |
| `--http <ip>` | IP address for HTTP (WiFi) connection |
| `--hat` | Enable HAT expansion board tests |
| `--skip-destructive` | Skip tests that modify device state (reset, cal save) |
| `--transport usb\|http\|both` | Limit which transports to test (default: both) |
| `--category <name>` | Run only one category (see list below) |
| `--html-report` | Generate `tests/report.html` |
| `-x` | Stop on first failure |
| `--timeout <sec>` | Per-test timeout in seconds (default: 30) |

### Test categories

| Name | Tests | Description |
|------|-------|-------------|
| `core` | 7 | Ping, firmware version, device info, status, reset |
| `channels` | 18 | All AD74416H channel functions + ADC/DAC |
| `gpio` | 8 | GPIO pins A–F |
| `mux` | 8 | MUX switch matrix (32 switches) |
| `power` | 10 | IDAC, PCA9535, e-fuse, fault log |
| `usbpd` | 6 | USB Power Delivery (HUSB238) |
| `wavegen` | 8 | Waveform generator (SINE/SQUARE/TRIANGLE/SAWTOOTH) |
| `wifi` | 7 | WiFi status, scan |
| `selftest` | 7 | Boot test, supply measurement, e-fuse currents, auto-cal |
| `streaming` | 8 | ADC/scope streaming - USB only |
| `hat` | 15 | HAT connector, LA, SWD - requires `--hat` |
| `faults` | 10 | Alert clearing, fault log, alert masks |
| `http` | 14 | Direct HTTP REST endpoint contract tests |

### Run a single category

```bash
python tests/run_tests.py --usb /dev/cu.usbmodem1234 --category channels
```

### Run directly with pytest

```bash
pytest tests/ --device-usb=/dev/cu.usbmodem1234 -v
pytest tests/ --device-http=192.168.4.1 -k "not usb_only" -v
pytest tests/device/test_02_channels.py --device-usb=/dev/cu.usbmodem1234 -v
```

### Hardware-free (simulator + synthetic)

All unit tests and the full `device/` suite can run without a board. The
`mock/` layer provides `SimulatedDevice`, `SimulatedUSBTransport`, and
`SimulatedHTTPTransport` used as shared fixtures. The `synthetic/` layer runs
LA streaming regression tests with generated stimuli (no hardware needed).

```bash
# From repo root
PYTHONPATH=python:tests pytest tests/unit -q                    # pure-Python unit tests
PYTHONPATH=python:tests pytest tests/simulator -q               # BBP + HTTP sim round-trips
PYTHONPATH=python:tests pytest tests/synthetic -q               # LA USB synthetic/regression
PYTHONPATH=python:tests pytest tests/device --sim -q            # device suite via simulator
```

The simulator implements every BBP CmdId handler (see
`tests/simulator/test_sim_completeness.py`) and mirrors the firmware's `/api`
schema, including the `macAddress` field on `/api/device/info` and the
admin-token pairing flow (injected automatically by `SimulatedHTTPTransport`).

> **Write simulator handlers against the FIRMWARE, not against the client.**
> `tests/mock/handlers/idac.py` was once written to match a broken client
> parser - 26 bytes/channel for 4 channels, where the firmware wrote 44 bytes
> for 3. The suite was green and `idac_get_status()` returned
> `target_v = 726302457856.0 V` on real hardware. When adding a handler, read
> the `bbp_put_*` sequence in the firmware and mirror it, then pin it with a
> parity guard that parses the layout out of the `.cpp`.

### Memory watch (hardware)

`tests/tools/mem_watch.py` is a live internal-SRAM dashboard driven by the
`MEM_STATUS` BBP command / `GET /api/system/memory`. It doubles as a CI gate:

```bash
# One-shot reading
PYTHONPATH=python python tests/tools/mem_watch.py --device-usb COM6 --once --no-clear

# Under load, exporting a series to compare before/after a change
PYTHONPATH=python python tests/tools/mem_watch.py --device-usb COM6 --stress \
    --duration 60 --interval 1 --json mem-after.json --csv mem-after.csv

# Pass/fail thresholds (non-zero exit when breached)
PYTHONPATH=python python tests/tools/mem_watch.py --device-usb COM6 --stress \
    --duration 30 --fail-under-kb 24 --fail-largest-under-kb 8 --fail-task-pct 80
```

`--stress` starts the ADC and scope streams while sampling. **Idle numbers lie**
 - always compare under load. Stack high-water marks also grow late, so exercise
the deep paths (connect/disconnect cycles, HTTP, the CLI `tui`, an OTA query)
before trusting a reading.

## Test markers

| Marker | Meaning |
|--------|---------|
| `usb_only` | Requires USB transport (streaming, HAT power, register access) |
| `http_only` | Requires HTTP transport |
| `requires_hat` | Requires HAT expansion board - enable with `--hat` |
| `destructive` | Modifies persistent device state (skipped with `--skip-destructive`) |
| `slow` | Takes > 5 seconds |
| `streaming` | Uses ADC/scope streaming (USB only) |

## Device state safety

- All channels are reset to `HIGH_IMP` at the end of the test session.
- Destructive tests (e.g. `idac_cal_save`, `reset`) are skipped by default when `--skip-destructive` is passed.
- Streaming tests always call `stop_*_stream()` in teardown even if the test fails.

## Adding new tests

1. Create a new file in `tests/device/` named `test_NN_feature.py`
2. Add `pytestmark` at the top for any applicable markers
3. Use the `device` fixture (parametrized USB+HTTP) or `usb_device` / `http_device` for transport-specific tests
4. Use the `asserter` fixture for tolerance-based assertions on analog values
5. Register the category in `run_tests.py`'s `CATEGORIES` list

```python
import pytest
pytestmark = [pytest.mark.timeout(10)]

def test_my_feature(device, asserter):
    result = device.get_adc_value(0)
    asserter.assert_near(result.value, 5.0, tol_pct=10.0, msg="ADC readback")
```
