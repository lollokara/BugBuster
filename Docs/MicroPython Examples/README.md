# BugBuster On-Device MicroPython Examples

This folder contains runnable MicroPython examples for the BugBuster on-device scripting engine. Each example is a standalone script that executes on the device itself (not on the host), using only the `bugbuster` module and Python built-ins.

## Overview

BugBuster runs **MicroPython 1.24.1** bytecode (no native compilation) in a hermetic FreeRTOS task with 1 MB PSRAM heap. Each script execution:
- Initializes the MicroPython interpreter
- Runs your code with full access to hardware (analog channels, I2C, SPI)
- Raises `KeyboardInterrupt` at the next `bugbuster.sleep()` call when you call `script_stop`
- De-initializes the interpreter (no persistent globals across runs)

Scripts can be:
- **Evaluated inline** via HTTP/USB (`eval` transport)
- **Uploaded to SPIFFS** and run by name (`run-file` transport)
- **Set as autorun** to run at every boot (with three safety gates)

---

## Quick Start

### Option 1: Inline eval (instant feedback)

Use curl or the Python client to send code directly:

```bash
# Get admin token over USB
TOKEN=$(python -c "from bugbuster import connect_usb; b=connect_usb('/dev/cu.usbmodem...'); print(b.get_admin_token())")

# Send a script
curl -X POST http://device.local/api/scripts/eval \
  -H "X-BugBuster-Admin-Token: $TOKEN" \
  --data-binary 'print("hello from bugbuster")'

# Fetch logs
curl http://device.local/api/scripts/logs -H "X-BugBuster-Admin-Token: $TOKEN"
```

Or use the Python client over USB:

```python
from bugbuster import connect_usb
bb = connect_usb('/dev/cu.usbmodem...')
bb.script_eval('print("hello")')
import time; time.sleep(0.1)
print(bb.script_logs())
```

### Option 2: Upload and run by name

Use HTTP to upload a script file, then run it:

```bash
# Upload
curl -X POST http://device.local/api/scripts/files?name=test.py \
  -H "X-BugBuster-Admin-Token: $TOKEN" \
  --data-binary @test.py

# Run it
curl -X POST http://device.local/api/scripts/run-file?name=test.py \
  -H "X-BugBuster-Admin-Token: $TOKEN"

# List files
curl http://device.local/api/scripts/files -H "X-BugBuster-Admin-Token: $TOKEN"

# Get logs
curl http://device.local/api/scripts/logs -H "X-BugBuster-Admin-Token: $TOKEN"
```

### Option 3: Autorun (boot-time execution)

Enable a stored script to run automatically at every boot (with safety gates):

```bash
# Upload the script first
curl -X POST http://device.local/api/scripts/files?name=boot_blink.py \
  -H "X-BugBuster-Admin-Token: $TOKEN" \
  --data-binary @boot_blink.py

# Enable autorun
curl -X POST http://device.local/api/scripts/autorun/enable?name=boot_blink.py \
  -H "X-BugBuster-Admin-Token: $TOKEN"

# Check status
curl http://device.local/api/scripts/autorun/status \
  -H "X-BugBuster-Admin-Token: $TOKEN"

# To disable (keeps the script file)
curl -X POST http://device.local/api/scripts/autorun/disable \
  -H "X-BugBuster-Admin-Token: $TOKEN"
```

---

## The `bugbuster` Module

### Cooperative sleep

```python
bugbuster.sleep(ms)
```

Blocks for `ms` milliseconds. Unlike `time.sleep()` (which is unavailable), `bugbuster.sleep()` cooperates with the script-stop mechanism: calling `script_stop` injects a `KeyboardInterrupt` at the next `bugbuster.sleep()` call.

**Required:** Always use `bugbuster.sleep()` in your scripts, never `time.sleep()`.

### Analog Channels (0–3)

```python
ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VOUT)  # Set function
ch.set_voltage(2.5)                   # Output voltage (V)
v = ch.read_voltage()                 # Read ADC (V), returns float
ch.set_do(True)                       # Set digital output (bool)
```

**Channel functions** — choose one per channel:

| Function | Code | Use | Output | Input |
|---|---|---|---|---|
| `FUNC_HIGH_IMP` | 0 | High impedance (default) | — | — |
| `FUNC_VOUT` | 1 | Voltage output | 0–5 V DAC | — |
| `FUNC_IOUT` | 2 | Current output | 0–20 mA DAC | — |
| `FUNC_VIN` | 3 | Voltage input | — | 0–5 V ADC |
| `FUNC_IIN_EXT_PWR` | 4 | Current input (external power) | — | 0–20 mA ADC |
| `FUNC_IIN_LOOP_PWR` | 5 | Current input (loop power) | — | 0–20 mA ADC |
| `FUNC_RES_MEAS` | 6 | Resistance measurement | — | 0–100 Ω ADC |
| `FUNC_DIN_LOGIC` | 7 | Digital input (3.3 V logic) | — | 0 = LOW, 1 = HIGH |
| `FUNC_DIN_LOOP` | 8 | Digital input (loop power) | — | 0 = LOW, 1 = HIGH |
| `FUNC_IOUT_HART` | 9 | Current output + HART modem | 0–20 mA + HART | — |
| `FUNC_IIN_EXT_PWR_HART` | 10 | Current input + HART (external) | — | 0–20 mA ADC + HART |
| `FUNC_IIN_LOOP_PWR_HART` | 11 | Current input + HART (loop) | — | 0–20 mA ADC + HART |

**Methods:**
- `set_function(func)` — select a channel function (must be called first)
- `set_voltage(v, bipolar=False)` — output voltage in volts (0–5 V); ignore for non-DAC functions
- `read_voltage()` — read ADC as voltage in volts; returns `float`
- `set_do(bool)` — set digital output (does not apply to input functions)

**Limits:** `supply` (for external loads) ≤ 5.0 V (enforced at I2C/SPI setup time).

### I2C Bus

```python
i2c = bugbuster.I2C(sda_io=2, scl_io=3, freq=400_000, pullups='external', 
                     supply=3.3, vlogic=3.3)
addrs = i2c.scan()                                           # [0x48, 0x68, ...]
i2c.writeto(0x48, b'\x01\x80')                              # Write bytes
data = i2c.readfrom(0x48, 2)                                # Read N bytes
data = i2c.writeto_then_readfrom(0x48, b'\x00', 2)          # Write, then read
```

**Constructor arguments:**
- `sda_io`, `scl_io` — **IO terminal numbers 1–12** (never raw GPIO)
- `freq` — I2C bus frequency in Hz; default 400,000
- `pullups` — `'external'` (recommended, default), `'internal'` (weak), or `'off'`
- `supply` — external power supply voltage (3.3–5.0 V); default 3.3 V
- `vlogic` — logic-level voltage (3.3 or 5.0 V); default 3.3 V

**Methods:**
- `scan(start=0x08, stop=0x77, skip_reserved=True, timeout_ms=50)` — find all responding 7-bit addresses; returns `list[int]`
- `writeto(addr, buf, timeout_ms=50)` — write bytes to address
- `readfrom(addr, n, timeout_ms=50)` — read N bytes from address
- `writeto_then_readfrom(addr, wr_buf, rd_n, timeout_ms=50)` — atomic write+read (common for register access)

**Wiring note:** The firmware applies routing automatically — MUX, level-shifter, VADJ, e-fuse, all handled transparently on the device.

**Limits:** Single transaction ≤ 255 bytes.

### SPI Bus

```python
spi = bugbuster.SPI(sck_io=4, mosi_io=5, miso_io=6, cs_io=7, 
                     freq=1_000_000, mode=0, supply=3.3, vlogic=3.3)
rx = spi.transfer(b'\x9F\x00\x00\x00')                       # bytes
```

**Constructor arguments:**
- `sck_io` — **IO terminal 1–12** (required)
- `mosi_io`, `miso_io`, `cs_io` — **IO terminal 1–12 or `None`** (optional)
- `freq` — SPI clock in Hz; default 1,000,000
- `mode` — SPI mode 0–3 (CPOL/CPHA); default 0
- `supply`, `vlogic` — same as I2C

**Methods:**
- `transfer(buf)` — full-duplex SPI transfer; returns `bytes` (same length as input)

**Limits:** Single transfer ≤ 512 bytes.

---

## Channel Example Walkthrough

See **`02_channel_voltage_sweep.py`**. It sets channel 0 to `FUNC_VOUT` and sweeps 0 to 5 V in 1 V steps, reading back the ADC each time. Each step sleeps 100 ms (cooperative) so you can interrupt with `script_stop`.

The readback should track within ±50 mV of the DAC setting. If you don't have the bench hardware, you'll see near-zero ADC reads (expected).

---

## I2C / SPI Example Walkthrough

**I2C examples:**
- **`04_i2c_scan.py`** — scan IO2/IO3 for devices (prints addresses in hex)
- **`05_i2c_register_read.py`** — read two bytes from address 0x48 (temperature sensor pattern)

**SPI example:**
- **`06_spi_flash_jedec_id.py`** — read JEDEC manufacturer ID from SPI flash; decodes well-known vendor bytes

All three demonstrate:
1. IO terminal numbers (not raw GPIO)
2. Error handling with try/except
3. Bus routing happening automatically in firmware
4. Cooperative sleep for cancellation

---

## Files and Autorun

### Upload flow

1. Create a `.py` script (see examples below)
2. `POST /api/scripts/files?name=your_script.py` with the file body (max 32 KB source)
3. `GET /api/scripts/files` to list uploaded scripts
4. `POST /api/scripts/run-file?name=your_script.py` to execute it by name
5. `GET /api/scripts/logs` to fetch output
6. `DELETE /api/scripts/files?name=your_script.py` to remove

### Autorun three gates

To run a script automatically at every boot, **all three gates must pass:**

1. **Sentinel file** — `/spiffs/.autorun_enabled` must exist (created by `POST /api/scripts/autorun/enable?name=...`)
2. **5-second grace window** — if any HTTP/BBP/CLI activity occurs in the first 5 seconds after boot, autorun is cancelled (so you can always recover via host connection)
3. **IO12 must read HIGH** — on boot, if IO12 is LOW (pulled to GND), autorun is disabled. Default (no external wiring, internal pull-up) reads HIGH and **autorun runs**. To prevent autorun, wire a 10 kΩ pull-down to GND or a button to GND on IO12.

**Recovery:** if autorun crashes the device, the OTA bootloader will roll back to the previous firmware within 30 seconds (no manual recovery needed). To suppress autorun while developing:
- Pull IO12 LOW at boot (simplest: temporary jumper to GND)
- Or delete the sentinel via `POST /api/scripts/autorun/disable`
- Or trigger the 5-second grace window by connecting over HTTP/BBP/CLI within 5 seconds of power-on

---

## Cooperative Cancellation

When you call `POST /api/scripts/stop` (or `bb.script_stop()` in Python), the device sets a flag. At the **next** `bugbuster.sleep()` call, the VM injects a `KeyboardInterrupt` that you can catch:

```python
# 07_cooperative_sleep.py example
try:
    for i in range(50):
        bugbuster.sleep(100)
        print('iter', i)
except KeyboardInterrupt:
    print('stopped')
```

This allows you to:
- Cancel long-running scripts gracefully
- Wrap cleanup code in `try/finally`
- Avoid forcefully killing the device task

**Limitation:** the VM does not raise `KeyboardInterrupt` if your code is in a tight C computation (e.g., inside `spi.transfer()`). Prefer to structure your scripts with frequent `bugbuster.sleep()` calls.

---

## Limitations

1. **No `import` from SPIFFS** — `MICROPY_VFS` is disabled (deferred to V2). Scripts cannot `import` user modules from `/spiffs/`. Multi-file projects must inline everything into one script.

2. **No persistent globals** — each eval/run hermetically initializes and tears down the interpreter. Globals do not survive across `script_eval` calls. Persistent state is V2-A.

3. **No REPL on-device** — scripts only (V1). Interactive REPL is V2-B.

4. **Size limits:**
   - Script source ≤ 32 KB
   - I2C transaction ≤ 255 bytes
   - SPI transfer ≤ 512 bytes
   - Log ring 4 KB (drained by `GET /api/scripts/logs`)

5. **Voltage cap** — `supply` parameter ≤ 5.0 V. Exceeding this raises `ValueError` at setup time.

6. **HTTP routes require auth** — all `/api/scripts/*` endpoints need `X-BugBuster-Admin-Token` header (BBP/USB does not require auth — cable = trust).

7. **Available modules** — only `bugbuster` and Python built-ins (`print`, `range`, `len`, `hex`, `bytes`, `int`, `float`, `list`, `dict`, `str`, exception types, etc.). No `time`, `os`, `random`, `_thread`, `socket`, network, or file I/O.

---

## Troubleshooting

### `OSError(MP_EIO)` from I2C or SPI

**Cause:** bus setup failed, likely the route could not be applied. Check:
- All IO terminal numbers are in range 1–12
- No duplicate IO terminals
- `supply` ≤ 5.0 V
- Hardware is properly wired

**Solution:** enable firmware debug logs and inspect `bus_planner_apply_*` output in the device console (available via USB serial).

### Long sleep won't stop

**Cause:** you called `time.sleep()` instead of `bugbuster.sleep()`. The `time` module is not available.

**Solution:** use `bugbuster.sleep()` everywhere.

### Script source size > 32 KB

**Cause:** your script is too large for the queue buffer.

**Solution:** split into multiple stored files and call them sequentially via `run-file`, or move repeated helper logic into one-liners.

### "queue_full" response

**Cause:** a previous script is still running; the queue (depth 4) is exhausted.

**Solution:** call `POST /api/scripts/stop` first, wait a moment, then re-submit.

---

## Where to Look in the Source

| Component | File | Purpose |
|---|---|---|
| Module globals & constants | `Firmware/ESP32/src/modbugbuster.c` | `bugbuster.FUNC_*`, `sleep`, type registration |
| Channel binding | `Firmware/ESP32/src/modbugbuster_channel.c` | `Channel` class, voltage/function/digital methods |
| I2C binding | `Firmware/ESP32/src/modbugbuster_i2c.c` | `I2C` class, scan/read/write methods |
| SPI binding | `Firmware/ESP32/src/modbugbuster_spi.c` | `SPI` class, transfer method |
| VM lifecycle | `Firmware/ESP32/src/scripting.cpp` | `mp_init`, `mp_deinit`, stop flag, log ring |
| Bus routing | `Firmware/ESP32/src/bus_planner.cpp` | MUX, level-shifter, e-fuse, VADJ (transparent to scripts) |
| Storage layer | `Firmware/ESP32/src/script_storage.cpp` | `/spiffs/scripts/` file I/O for upload/list/delete |
| Autorun engine | `Firmware/ESP32/src/autorun.cpp` | three-gate logic, IO12 sensing, OTA marking |
| HTTP routes | `Firmware/ESP32/src/webserver.cpp:3600+` | `/api/scripts/*` endpoint implementations |
| CLI commands | `Firmware/ESP32/src/cmds/cmd_script.cpp` | BBP handlers for script operations |

---

## Further Reading

- **`Docs/scripting-plan.md`** — full v1 + v2 + v3 roadmap (persistent state, REPL, sandboxing, fleet)
- **`Docs/scripting-progress.md`** — current implementation status and bench notes
- **`Firmware/BugBusterProtocol.md` § 6.23** — wire-format details for script opcodes
- **`python/bugbuster/client.py`** — Python API for remote script execution (USB/HTTP)
