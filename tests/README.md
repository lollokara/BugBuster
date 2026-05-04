# BugBuster Test Suite

Test framework for the BugBuster industrial I/O board.  Covers the Python API,
the HTTP REST endpoints, and &mdash; via both &mdash; the ESP32 and RP2040 firmware.

The suite has five layers:

| Layer | Dir | Needs hardware? | What it validates |
|---|---|---|---|
| **Unit** | `tests/unit/` | no | Pure-Python logic: parsers, HAL routing, HAT guards, rail-lock enforcement, auth flow |
| **Simulator** | `tests/simulator/` | no | End-to-end BBP + HTTP round-trips against `SimulatedDevice` (102 BBP handlers, `/api/*` surface) |
| **Mock** | `tests/mock/` | no | `SimulatedDevice`, `SimulatedUSBTransport`, `SimulatedHTTPTransport` — shared fixtures used by the simulator and device layers |
| **Synthetic** | `tests/synthetic/` | no | Regression tests for LA USB bulk/streaming protocol, generated stimuli, timing edge-cases |
| **Device** | `tests/device/` | yes (or `--sim`) | The same tests, driven against real hardware over USB / HTTP, or against the simulator with `--sim` |

Current posture: **249 tests** (unit + sim/device passing), 64 skipped (HAT / SWD / LA hardware-only).

## Setup

```bash
# From repo root — works on macOS and Linux
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
| `streaming` | 8 | ADC/scope streaming — USB only |
| `hat` | 15 | HAT connector, LA, SWD — requires `--hat` |
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
schema, including the BBP v4 `macAddress` field on `/api/device/info` and the
admin-token pairing flow (injected automatically by `SimulatedHTTPTransport`).

## Test markers

| Marker | Meaning |
|--------|---------|
| `usb_only` | Requires USB transport (streaming, HAT power, register access) |
| `http_only` | Requires HTTP transport |
| `requires_hat` | Requires HAT expansion board — enable with `--hat` |
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
