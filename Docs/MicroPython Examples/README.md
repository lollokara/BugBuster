# BugBuster On-Device MicroPython Examples

This folder contains runnable MicroPython examples for the BugBuster on-device scripting engine. Each example is a standalone script that executes on the device itself (not on the host), using the `bugbuster` module, frozen helper modules, and Python built-ins.

## Overview

BugBuster runs **MicroPython 1.24.1** bytecode (no native compilation) in a hermetic FreeRTOS task with 1 MB PSRAM heap. Scripts execute under two modes:

- **Ephemeral mode** (default) - interpreter initializes, runs your code, tears down. No state persists across calls. Lightweight; use for one-off tasks.
- **Persistent mode** (`persist=true`) - interpreter stays alive between calls. Globals and imports survive across `script_eval` calls. Auto-resets on heap watermark (≥80 % soft, ≥95 % hard) or idle timeout (10 minutes default). Use for multi-step workflows.

All scripts have full access to hardware (analog channels, I2C, SPI, network, logging). Cancellation via `script_stop` injects `KeyboardInterrupt` at the next `bugbuster.sleep()` call, allowing graceful shutdown.

---

## Running scripts: 5 ways

1. **Inline eval (ephemeral)** - `curl -X POST /api/scripts/eval` with code in the body. Simplest for testing snippets. See "Quick Start" below.

2. **Persistent session** - `POST /api/scripts/eval?persist=true` keeps the VM alive. Call `POST /api/scripts/reset` to tear it down. Use the host-side `with bb.script_session()` context manager (Python) for ergonomic multi-statement workflows.

3. **Upload + run by name** - `POST /api/scripts/files?name=...` uploads a script to SPIFFS; `POST /api/scripts/run-file?name=...` executes it by name. Max 32 KB source. Persistent across device reboots (stored on disk).

4. **Autorun at boot** - Enable a stored script to run automatically at boot via `POST /api/scripts/autorun/enable?name=...`. Guarded by three safety gates: sentinel file, 5-second grace window, and IO12 HIGH. See "Files and Autorun" section and the script management guide (`Docs/MicroPython Examples/script-management.md`).

5. **WebSocket REPL** - `GET /api/scripts/repl/ws` opens an interactive terminal. Supports command history, syntax highlighting, and persistent session state. See `repl.md` for protocol details.

---

## Quick Start

### Option 1: Inline eval (instant feedback)

Use curl or the Python client to send code directly:

```bash
# Get admin token over USB
TOKEN=$(python -c "from bugbuster import connect_usb; b=connect_usb('/dev/cu.usbmodem...'); print(b.get_admin_token())")

# Send a script (ephemeral)
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

### Option 2: Persistent session (multi-statement)

Keep the VM alive across calls:

```python
from bugbuster import connect_usb
bb = connect_usb('/dev/cu.usbmodem...')

with bb.script_session() as s:
    s.eval("x = 5")
    s.eval("y = 10")
    logs = s.eval("print(x + y)")
    print(logs)  # "15"
    print("Globals:", s.status().globalsCount, "bytes:", s.status().globalsBytes)
```

### Option 3: Upload and run by name

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

### Option 4: Autorun (boot-time execution)

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

### Option 5: WebSocket REPL

Open an interactive terminal via WebSocket:

```bash
# See repl.md for full protocol and auth details
wscat -c 'ws://device.local/api/scripts/repl/ws'
# Type auth token on first frame, then enter Python commands interactively
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

**Channel functions** - choose one per channel:

| Function | Code | Use | Output | Input |
|---|---|---|---|---|
| `FUNC_HIGH_IMP` | 0 | High impedance (default) | - | - |
| `FUNC_VOUT` | 1 | Voltage output | 0–5 V DAC | - |
| `FUNC_IOUT` | 2 | Current output | 0–20 mA DAC | - |
| `FUNC_VIN` | 3 | Voltage input | - | 0–5 V ADC |
| `FUNC_IIN_EXT_PWR` | 4 | Current input (external power) | - | 0–20 mA ADC |
| `FUNC_IIN_LOOP_PWR` | 5 | Current input (loop power) | - | 0–20 mA ADC |
| `FUNC_RES_MEAS` | 6 | Resistance measurement | - | 0–100 Ω ADC |
| `FUNC_DIN_LOGIC` | 7 | Digital input (3.3 V logic) | - | 0 = LOW, 1 = HIGH |
| `FUNC_DIN_LOOP` | 8 | Digital input (loop power) | - | 0 = LOW, 1 = HIGH |
| `FUNC_IOUT_HART` | 9 | Current output + HART modem | 0–20 mA + HART | - |
| `FUNC_IIN_EXT_PWR_HART` | 10 | Current input + HART (external) | - | 0–20 mA ADC + HART |
| `FUNC_IIN_LOOP_PWR_HART` | 11 | Current input + HART (loop) | - | 0–20 mA ADC + HART |

**Methods:**
- `set_function(func)` - select a channel function (must be called first)
- `set_voltage(v, bipolar=False)` - output voltage in volts (0–5 V); ignore for non-DAC functions
- `read_voltage()` - read ADC as voltage in volts; returns `float`
- `set_do(bool)` - set digital output (does not apply to input functions)

**Limits:** `supply` (for external loads) ≤ 5.0 V (enforced at I2C/SPI setup time).

### I2C Bus

```python
i2c = bugbuster.I2C(sda_io=2, scl_io=3, freq=400_000, pullups='external', 
                     supply=3.3, vlogic=3.3)
addrs = i2c.scan()                                           # [0x48, 0x68, ...]
i2c.writeto(0x48, b'\x01\x80')                              # Write bytes
data = i2c.readfrom(0x48, 2)                                # Read N bytes
data = i2c.writeto_then_readfrom(0x48, b'\x00', 2)          # Write, then read
i2c.close()                                                 # Release the shared controller
```

**Constructor arguments:**
- `sda_io`, `scl_io` - **IO terminal numbers 1–12** (never raw GPIO)
- `freq` - I2C bus frequency in Hz; default 400,000
- `pullups` - `'external'` (recommended, default), `'internal'` (weak), or `'off'`
- `supply` - external power supply voltage (3.3–5.0 V); default 3.3 V
- `vlogic` - logic-level voltage (3.3 or 5.0 V); default 3.3 V

**Methods:**
- `close()` - release the shared external I2C controller so another wire pair can be configured
- `scan(start=0x08, stop=0x77, skip_reserved=True, timeout_ms=50)` - find all responding 7-bit addresses; returns `list[int]`
- `writeto(addr, buf, timeout_ms=50)` - write bytes to address
- `readfrom(addr, n, timeout_ms=50)` - read N bytes from address
- `writeto_then_readfrom(addr, wr_buf, rd_n, timeout_ms=50)` - atomic write+read (common for register access)

**Wiring note:** The firmware applies routing automatically - MUX, level-shifter, VADJ, e-fuse, all handled transparently on the device.

**Limits:** Single transaction ≤ 255 bytes.

### SPI Bus

```python
spi = bugbuster.SPI(sck_io=4, mosi_io=5, miso_io=6, cs_io=7, 
                     freq=1_000_000, mode=0, supply=3.3, vlogic=3.3)
rx = spi.transfer(b'\x9F\x00\x00\x00')                       # bytes
spi.close()                                                  # Release the shared controller
```

**Constructor arguments:**
- `sck_io` - **IO terminal 1–12** (required)
- `mosi_io`, `miso_io`, `cs_io` - **IO terminal 1–12 or `None`** (optional)
- `freq` - SPI clock in Hz; default 1,000,000
- `mode` - SPI mode 0–3 (CPOL/CPHA); default 0
- `supply`, `vlogic` - same as I2C

**Methods:**
- `transfer(buf)` - full-duplex SPI transfer; returns `bytes` (same length as input)
- `close()` - release the shared external SPI controller so another wire group can be configured

**Limits:** Single transfer ≤ 512 bytes.

### HAT v2 (rails, LEDs, shifted IO, SWD)

HAT support is exposed as flat functions on the `bugbuster` module. These calls
use the ESP32-to-RP2040 HAT UART bridge, so they raise `OSError(EIO)` if no HAT
is connected or the command times out.

```python
import bugbuster

status = bugbuster.hat_status()
print('HAT:', status['detected'], status['connected'])

if status['connected']:
    caps = bugbuster.hat_caps()
    print('rails:', caps['rail_count'], 'leds:', caps['led_count'])

    for rail in bugbuster.hat_rails():
        print('rail', rail['rail_id'], rail['voltage_mv'], 'mV', 'enabled=', rail['enabled'])

    bugbuster.hat_led(1, bugbuster.HAT_LED_GREEN)
    print('cal:', bugbuster.hat_calibrate_status())
```

**Functions:**
- `hat_status()` - ESP32 HAT snapshot with presence, UART health, firmware version, pin config, SWD flags, and cached telemetry
- `hat_caps()` - HAT capability metadata (`rail_count`, `led_count`, `shifted_io_count`, firmware version, etc.)
- `hat_rails()` - list of rail dictionaries (`rail_id`, `enabled`, `voltage_mv`, `current_ma`, `status`)
- `hat_set_rail_enable(rail_id, enable)` - enable/disable a HAT rail
- `hat_set_rail_voltage(rail_id, voltage_mv)` - set adjustable rail target in mV
- `hat_led(led_id, color_code)` - set status LED 1..8 using `HAT_LED_*` constants
- `hat_io_bank(dirs, ups=0, dns=0, vals=0)` - configure shifted IO direction, pulls, and output values as 8-bit masks
- `hat_level_shift(oe, dir)` - override HAT level-shifter OE/DIR and return the applied state
- `hat_calibrate_start(rail_id)`, `hat_calibrate_status()`, `hat_calibrate_import(rail_id, points)` - HAT rail calibration controls
- `hat_setup_swd(target_voltage_mv=3300, connector=0)` - set up the dedicated HAT CMSIS-DAP/SWD connector

**Constants:**
- Rails: `HAT_RAIL_3V3_ADJ`, `HAT_RAIL_VADJ3`, `HAT_RAIL_VADJ4`
- LED colors: `HAT_LED_OFF`, `HAT_LED_RED`, `HAT_LED_GREEN`, `HAT_LED_BLUE`, `HAT_LED_YELLOW`, `HAT_LED_CYAN`, `HAT_LED_MAGENTA`, `HAT_LED_WHITE`

### Frozen Modules

Three helper modules are pre-compiled and importable without VFS:

#### `bb_helpers`

```python
import bb_helpers

# Settle time (alias for bugbuster.sleep)
bb_helpers.settle(100)

# Ramp DAC output with automatic readback
# Sets FUNC_VOUT, steps from lo to hi V, reads ADC at each step, restores FUNC_HIGH_IMP.
results = bb_helpers.dac_ramp(channel=0, lo=0.0, hi=5.0, step=1.0, settle_ms=50)
for v, readback in results:
    print('set=%.2f  read=%.5f' % (v, readback))

# Sweep through an explicit voltage list
# Same FUNC_VOUT / FUNC_HIGH_IMP lifecycle as dac_ramp.
results = bb_helpers.channel_sweep(channel=0, voltages=[0.0, 1.0, 2.5, 5.0], settle_ms=100)
for v, readback in results:
    print('set=%.2f  read=%.5f' % (v, readback))

# Sweep without readback (faster, returns target voltages only)
bb_helpers.channel_sweep(0, [0.0, 2.5, 5.0], readback=False)
```

**`bb_helpers` API:**

| Function | Signature | Returns |
|---|---|---|
| `settle` | `settle(ms)` | None |
| `dac_ramp` | `dac_ramp(channel, lo, hi, step, settle_ms=50)` | `list[(v, readback)]` |
| `channel_sweep` | `channel_sweep(channel, voltages, settle_ms=100, readback=True)` | `list[(v, readback)]` or `list[v]` |

#### `bb_devices`

```python
import bb_devices
import bugbuster

# OneWire / DS18B20 temperature sensor.
# Wire the data line to an ESP32 GPIO and add a 4.7 kOhm pull-up to 3.3 V.
sensor = bb_devices.DS18B20(2)
print(sensor.read_all())

# I2C devices all share the same bus object.
i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)

rtc = bb_devices.DS3231(i2c)
print(rtc.read_datetime(), '%.2f C' % rtc.temperature_c())

light = bb_devices.BH1750(i2c)
print('Lux: %.1f' % light.read_lux())

env = bb_devices.BME280(i2c)
temp_c, pressure_pa, humidity = env.read()
print('T=%.2f C  P=%.0f Pa  RH=%.1f%%' % (temp_c, pressure_pa, humidity))

display = bb_devices.SSD1306(128, 64, i2c)
display.fill(0)
display.text('BugBuster', 0, 0, 1)
display.text('Scripts tab', 0, 12, 1)
display.show()
```

`bb_devices` bundles small, educational drivers for the most common bench parts:

| Device | Notes |
|---|---|
| `DS18B20` | OneWire sensor. Requires `machine.Pin`, `onewire`, `ds18x20`, and a 4.7 kOhm pull-up. |
| `DS3231` | RTC plus temperature readback. |
| `BH1750`, `AHT20`, `SHT31`, `BME280` | Ambient light, humidity, and environmental sensors. |
| `INA219`, `ADS1115` | Current/power monitor and precision ADC. |
| `MCP23017`, `PCF8574`, `PCA9685` | GPIO expanders and PWM driver. `MCP23017` includes per-pin `pin_mode()`, `set_dir()`, and `set_pullup()` helpers. |
| `TCS34725`, `MPU6050` | Color sensor and IMU. |
| `MCP4725` | 12-bit I2C DAC. |
| `SSD1306` | I2C OLED wrapper over the upstream MicroPython driver. |

I2C/SPI setup rule of thumb:

- Use one `I2C(...)` or `SPI(...)` object for every device on the same pin group.
- If you wire a second bus on a different pin group while the first one is active, the constructor fails cleanly instead of stealing the controller.
- If the planner cannot route the bus or power domain, the constructor raises an error instead of silently half-configuring the target.
- If you need to move the shared controller to a different pin group, call `i2c.close()` or `spi.close()` first, then create a new bus object on the new pins.
- If the external controller is already in use on a different pin group, the constructor fails cleanly instead of stealing the active bus.

New device examples live beside the existing scripts:

- `15_mcp4725_dac.py` - MCP4725 DAC sweep
- `17_ds18b20_1wire.py` - OneWire temperature readout
- `18_ds3231_rtc.py` - RTC and temperature readback
- `19_bh1750_lux.py` - Ambient light readout
- `20_aht20_humidity.py` - Humidity and temperature
- `21_sht31_humidity.py` - Humidity and temperature
- `22_bme280_environment.py` - Temperature, pressure, humidity
- `23_ina219_power.py` - Bus voltage, shunt voltage, current, power
- `24_ads1115_adc.py` - Precision ADC voltage read
- `25_mcp23017_gpio.py` - 16-bit GPIO expander
- `26_pcf8574_gpio.py` - 8-bit GPIO expander
- `27_pca9685_pwm.py` - PWM output demo
- `28_tcs34725_color.py` - RGB color readout
- `29_mpu6050_imu.py` - Accelerometer and gyro readout
- `30_ssd1306_display.py` - OLED text demo

#### `bb_logging`

```python
import bb_logging

bb_logging.info('Starting measurement')
bb_logging.warn('Voltage above threshold')
bb_logging.error('Sensor not responding')
# Output: [     12345] INFO  Starting measurement
```

### Network (HTTP + MQTT)

Network bindings are exposed on the `bugbuster` module (not on a `Channel` instance):

```python
# HTTP GET with TLS (Mozilla CA bundle built-in)
try:
    response = bugbuster.http_get('https://api.example.com/status', 
                                   timeout_ms=10000)
    print('Status:', response.status)
    print('Body:', response.body)
except OSError as e:
    print('Request failed:', e)

# HTTP POST
response = bugbuster.http_post('https://api.example.com/log', 
                                body=b'{"temp": 25.3}',
                                headers={'Content-Type': 'application/json'},
                                timeout_ms=5000)

# MQTT publish
bugbuster.mqtt_publish(topic='sensors/temp', payload=b'25.3',
                        host='mqtt.example.com', port=1883,
                        username='user', password='pass')
```

**Signatures:**
- `bugbuster.http_get(url, headers=None, timeout_ms=10000)` - returns attrtuple `(status, body)` where `status` is int, `body` is bytes
- `bugbuster.http_post(url, body=b"", headers=None, timeout_ms=10000)` - same return
- `bugbuster.mqtt_publish(topic, payload, host, port=1883, username=None, password=None)` - void; raises `OSError` on failure

**TLS:** all HTTPS calls use `esp_crt_bundle_attach` with the Mozilla CA certificate bundle; no manual certificate handling needed.

**Cancellation:** timeout is the only cancellation mechanism. `KeyboardInterrupt` is raised after `http_get()` / `http_post()` / `mqtt_publish()` return if `script_stop` was called during the operation.

### Web UI

The device's web UI includes a dedicated **Scripts tab** (`Firmware/ESP32/web/src/tabs/scripts/`) that provides:
- CodeMirror editor with syntax highlighting
- File browser for uploaded scripts
- Log viewer with real-time output
- Script execution controls
- Embedded WebSocket REPL
- Autorun status and controls

Access via `http://device.local/` after connecting to the device.

### Host-side Helpers (Python)

The `python/bugbuster/script.py` module provides ergonomic wrappers for the host:

#### Context manager for persistent sessions

```python
from bugbuster import connect_usb
bb = connect_usb('/dev/cu.usbmodem...')

with bb.script_session() as s:
    s.eval("x = 5")
    s.eval("y = 10")
    result = s.eval("print(x + y)")
    print(result)  # "15"
    status = s.status()  # ScriptStatusResult with mode, globalsBytes, etc.
```

#### Decorator for running functions on-device

```python
from bugbuster import connect_usb
from bugbuster.script import on_device

bb = connect_usb('/dev/cu.usbmodem...')

@on_device
def read_sensor():
    import bb_devices, bugbuster
    i2c = bugbuster.I2C(sda_io=2, scl_io=3, supply=3.3, vlogic=3.3)
    sensor = bb_devices.TMP102(i2c)
    temp = sensor.read_celsius()
    print(repr(temp))  # Return via print(repr(...))

temp = read_sensor()  # Decorator parses the last log line via ast.literal_eval
print('Device returned:', temp, type(temp))
```

The decorator captures the function source via `inspect.getsource()` and sends it as an eval. The function must be defined in a real source file (not REPL or lambda). Return values are communicated by the device function printing `repr(value)` as the last statement; the decorator parses the last log line to reconstruct the Python object on the host.

#### Command-line interface

```bash
# Run a script file with optional persistent mode
python -m bugbuster.script run path/to/script.py [--persist]

# Tail logs from the device (real-time)
python -m bugbuster.script logs [--tail]

# Set autorun
python -m bugbuster.script autorun-set script_name

# Disable autorun
python -m bugbuster.script autorun-disable

# Reset the persistent VM
python -m bugbuster.script reset

# Show script engine status
python -m bugbuster.script status

# All commands accept --host IP and --token TOKEN (or --usb /dev/...)
python -m bugbuster.script status --usb /dev/cu.usbmodem...
```

---

## V2 Example Walkthrough

See the examples folder for V2-specific scripts:

- **`10_persistent_session.py`** - demonstrates persistent mode globals and multi-statement workflows
- **`11_frozen_helpers.py`** - uses `bb_helpers.dac_ramp()` and `bb_devices.TMP102()`
- **`12_network_post.py`** - HTTP POST to a remote endpoint with HTTPS
- **`13_on_device_decorator.py`** - shows the host-side `@on_device` decorator usage pattern
- **`15_mcp4725_dac.py`** - MCP4725 DAC control through `bb_devices.MCP4725()`
- **`17_ds18b20_1wire.py`** - OneWire temperature readout with `bb_devices.DS18B20()`
- **`30_ssd1306_display.py`** - OLED text output using `bb_devices.SSD1306()`

Classic examples (still valid):
- **`02_channel_voltage_sweep.py`** - sweep 0–5 V on channel 0 with ADC readback
- **`04_i2c_scan.py`** - scan IO2/IO3 for I2C devices
- **`05_i2c_register_read.py`** - read temperature sensor register
- **`06_spi_flash_jedec_id.py`** - read JEDEC ID from SPI flash
- **`07_cooperative_sleep.py`** - demonstrate graceful cancellation with `KeyboardInterrupt`
- **`14_vfs_import.py`** - demonstrates importing user-uploaded modules from SPIFFS

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

1. **Sentinel file** - `/spiffs/.autorun_enabled` must exist (created by `POST /api/scripts/autorun/enable?name=...`)
2. **5-second grace window** - if any HTTP/BBP/CLI activity occurs in the first 5 seconds after boot, autorun is cancelled (so you can always recover via host connection)
3. **IO12 must read HIGH** - on boot, if IO12 is LOW (pulled to GND), autorun is disabled. Default (no external wiring, internal pull-up) reads HIGH and **autorun runs**. To prevent autorun, wire a 10 kΩ pull-down to GND or a button to GND on IO12.

**Recovery:** if autorun crashes the device, the OTA bootloader will roll back to the previous firmware within 30 seconds (no manual recovery needed). To suppress autorun while developing:
- Pull IO12 LOW at boot (simplest: temporary jumper to GND)
- Or delete the sentinel via `POST /api/scripts/autorun/disable`
- Or trigger the 5-second grace window by connecting over HTTP/BBP/CLI within 5 seconds of power-on

---

## Persistent Mode Status

When using persistent mode, call `script_status()` to inspect the VM state:

```python
from bugbuster import connect_usb
bb = connect_usb('/dev/cu.usbmodem...')

with bb.script_session() as s:
    s.eval("x = [1, 2, 3]")
    status = s.status()
    print('Mode:', status.mode)              # 'persistent'
    print('Globals count:', status.globalsCount)
    print('Globals bytes:', status.globalsBytes)
    print('Soft watermark hit:', status.watermarkSoftHit)
    print('Idle for (ms):', status.idleForMs)
```

The VM auto-resets when:
- Heap soft watermark (≥ 80 %) is crossed
- Heap hard watermark (≥ 95 %) is crossed
- Idle timeout (default 10 minutes) expires

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

1. **VFS enabled** - `MICROPY_VFS` is active. Scripts can `import` user modules from `/spiffs/scripts/`. Upload your `.py` files via the web UI or API, and they are immediately importable by other scripts.

2. **No threading** - `MICROPY_PY_THREAD` is disabled. The `_thread` module is not available.

3. **No native emitter** - `@micropython.native` and `@viper` decorators will crash the device (deferred to V3). Use pure Python.

4. **Size limits:**
   - Script source ≤ 32 KB
   - I2C transaction ≤ 255 bytes
   - SPI transfer ≤ 512 bytes
   - Log ring 4 KB (drained by `GET /api/scripts/logs`)

5. **Voltage cap** - `supply` parameter ≤ 5.0 V. Exceeding this raises `ValueError` at setup time.

6. **HTTP routes require auth** - all `/api/scripts/*` endpoints need `X-BugBuster-Admin-Token` header (BBP/USB does not require auth - cable = trust).

7. **Available modules** - `bugbuster`, `bb_helpers`, `bb_devices`, `bb_logging`, and Python built-ins. VFS allows importing any user file from `/spiffs/scripts/`. No `time`, `os`, `random`, `socket` (use `bugbuster` equivalents).

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

### "Soft/hard watermark hit" auto-reset in persistent mode

**Cause:** your script allocated too much state. Globals and imports are consuming heap.

**Solution:** clear large variables with `del`, reset the VM with `script_reset()`, or split the task into multiple smaller evals.

---

## Where to Look in the Source

| Component | File | Purpose |
|---|---|---|
| Module globals & constants | `Firmware/ESP32/src/mp/modbugbuster.c` | `bugbuster.FUNC_*`, `sleep`, type registration |
| Channel binding | `Firmware/ESP32/src/mp/modbugbuster_channel.c` | `Channel` class, voltage/function/digital methods |
| I2C binding | `Firmware/ESP32/src/mp/modbugbuster_i2c.c` | `I2C` class, scan/read/write methods |
| SPI binding | `Firmware/ESP32/src/mp/modbugbuster_spi.c` | `SPI` class, transfer method |
| HAT binding | `Firmware/ESP32/src/mp/modbugbuster_hat.c` | HAT rails, LEDs, shifted IO, calibration, SWD setup |
| Network HTTP/MQTT | `Firmware/ESP32/src/mp/modbugbuster_net.c` | `http_get`, `http_post`, `mqtt_publish` |
| VM lifecycle & persistence | `Firmware/ESP32/src/mp/scripting.cpp` | `mp_init`, `mp_deinit`, stop flag, persistent mode, watermark, auto-reset |
| Persistent VM header | `Firmware/ESP32/src/mp/scripting.h` | status struct definitions |
| WebSocket REPL | `Firmware/ESP32/src/mp/repl_ws.cpp` | `/api/scripts/repl/ws` endpoint, auth, single-session lock |
| Network bridge | `Firmware/ESP32/src/mp/net_bridge.cpp` | HTTP/MQTT transport layer |
| Bus routing | `Firmware/ESP32/src/bus/bus_planner.cpp` | MUX, level-shifter, e-fuse, VADJ (transparent to scripts) |
| Storage layer | `Firmware/ESP32/src/mp/script_storage.cpp` | `/spiffs/scripts/` file I/O for upload/list/delete |
| Autorun engine | `Firmware/ESP32/src/mp/autorun.cpp` | three-gate logic, IO12 sensing, OTA marking |
| HTTP routes | `Firmware/ESP32/src/bbp/cmds/cmd_script.cpp` | `/api/scripts/*` endpoint implementations |
| Web tab (UI) | `Firmware/ESP32/web/src/tabs/scripts/` | CodeMirror editor, file browser, log viewer, REPL |
| Frozen modules | `python/firmware_modules/` | `bb_helpers.py`, `bb_devices.py`, `bb_logging.py` |
| Host-side helpers | `python/bugbuster/script.py` | `ScriptSession`, `@on_device`, CLI entry point |

---

## Further Reading

- **`Docs/scripting-plan-v2.md`** - full V2 feature roadmap and implementation notes
- **`Docs/MicroPython Examples/repl.md`** - WebSocket REPL protocol and usage
- **`Docs/MicroPython Examples/script-management.md`** - detailed autorun, script storage, and persistence management guide
- **`Firmware/bbp-protocol.md` § 6.23** - wire-format details for script opcodes
- **`python/bugbuster/client.py`** - Python API for remote script execution (USB/HTTP)
