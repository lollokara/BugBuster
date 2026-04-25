# External I2C/SPI Bus Engine

BugBuster can turn any valid subset of the 12 front-panel IO terminals into a
target-facing I2C or SPI bus. The host asks for physical IO numbers; the Python
planner resolves the real ESP32 GPIOs, power domain, e-fuse, level shifter, and
MUX switches, then the ESP32 binds a separate external peripheral for bus
traffic.

This feature is intended for testing external devices without hand-building
adapter firmware.

---

## Status

Implemented:

- Python route planner and route applier: `BugBuster.bus`
- I2C setup, scan, write, read, write-read
- SPI setup, full-duplex transfer, JEDEC ID helper
- MCP tools for dry-run planning, I2C scan, SPI transfer, JEDEC ID, and deferred jobs
- ESP32 direct BBP commands for I2C/SPI operations
- ESP32 direct HTTP endpoints under `/api/bus/*`
- ESP32 USB deferred job queue for timing-tolerant host workflows

Pending:

- Real-device bench validation across representative I2C/SPI targets
- Desktop and web UI
- USB BBP bus-status command and durable session lease model
- Optional HTTP deferred-job endpoints
- Simulator/device-test coverage for the new BBP opcodes

---

## Hardware Model

The 12 IOs are not ESP32 GPIO numbers. They are physical terminals routed
through the MUX matrix, level shifters, e-fuses, and two adjustable power
domains.

```
Physical IO request
  -> Python planner resolves IO -> ESP32 GPIO + MUX switch + supply/e-fuse
  -> VLOGIC is set for digital logic level
  -> VADJ rail is set and enabled for the IO block
  -> e-fuse for that IO block is enabled
  -> level-shifter OE is enabled
  -> MUX switches close the digital paths
  -> ESP32 external I2C/SPI peripheral is configured on resolved GPIOs
```

Power domains:

| Physical IOs | Supply | E-fuses |
|---|---|---|
| IO1-IO6 | VADJ1 | EFUSE1 for IO1-IO3, EFUSE2 for IO4-IO6 |
| IO7-IO12 | VADJ2 | EFUSE3 for IO7-IO9, EFUSE4 for IO10-IO12 |

The planner rejects routes spanning VADJ1 and VADJ2 unless
`allow_split_supplies=True` is passed. A split bus can work electrically only
when the target and wiring really expect two powered domains.

Known GPIO mapping detail:

| Physical IO | ESP32 GPIO |
|---|---:|
| IO1 | 47 |
| IO2 | 2 |
| IO3 | 4 |
| IO4 | 5 |
| IO5 | 6 |
| IO6 | 7 |
| IO7 | 15 |
| IO8 | 16 |
| IO9 | 17 |
| IO10 | 18 |
| IO11 | 21 |
| IO12 | 38 |

IO3 maps to ESP32 GPIO4. Do not assume physical IO number equals GPIO number.

---

## Safety Rules

- Always plan first when wiring a new target.
- Keep `io_voltage` equal to the target logic level.
- Keep `supply_voltage` equal to the rail you want BugBuster to provide to the IO block.
- Use external I2C pull-ups for 400 kHz and above.
- Do not enable `allow_split_supplies` unless the target wiring is intentional.
- The MUX matrix gives each IO one active path; using a bus route will replace
  conflicting paths for the selected IO group.
- HTTP direct bus operations exist, but deferred jobs are currently USB BBP only.

---

## Python Usage

### Dry-Run Plan

```python
import bugbuster as bb

with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    plan = dev.bus.plan_i2c(
        sda=2,
        scl=3,
        io_voltage=3.3,
        supply_voltage=3.3,
        frequency_hz=400_000,
    )
    print(plan.as_dict())
```

Expected important fields:

```python
{
    "kind": "i2c",
    "pins": {"sda": 2, "scl": 3},
    "esp_gpios": {"sda": 2, "scl": 4},
    "mux_states": [0x50, 0x00, 0x00, 0x00],
    "supplies": ["VADJ1"],
    "efuses": ["EFUSE1"],
}
```

### Configure and Scan I2C

```python
import bugbuster as bb

with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    result = dev.bus.i2c_scan(
        sda=2,
        scl=3,
        io_voltage=3.3,
        supply_voltage=3.3,
        frequency_hz=400_000,
        pullups="external",
    )

    print(result["addresses"])       # ["0x50", "0x68", ...]
    print(result["plan"]["esp_gpios"])
```

`i2c_scan()` with `sda` and `scl` performs setup first. If the bus is already
configured, omit `sda` and `scl` to scan the active session.

### I2C Register Read

```python
with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    dev.bus.setup_i2c(
        sda=2,
        scl=3,
        io_voltage=3.3,
        supply_voltage=3.3,
    )

    whoami = dev.bus.i2c_write_read(0x68, [0x75], 1)
    print(whoami.hex())
```

### SPI JEDEC ID

```python
with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    dev.bus.setup_spi(
        sck=3,
        mosi=2,
        miso=5,
        cs=6,
        io_voltage=3.3,
        supply_voltage=3.3,
        frequency_hz=1_000_000,
        mode=0,
    )

    ident = dev.bus.spi_jedec_id()
    print(ident["jedec_id"])          # e.g. "EF4018"
```

### Raw SPI Transfer

```python
rx = dev.bus.spi_transfer([0x9F, 0x00, 0x00, 0x00])
print(rx.hex())
```

The SPI response length equals the transmitted byte count.

---

## Deferred Jobs

Deferred jobs queue bus operations on the ESP32. Buffers are allocated with
PSRAM-capable heap flags first and fall back to internal heap if needed.

Use this when the host should not be in the timing path between related bus
transactions. The current queue supports up to 16 jobs and up to 512 bytes per
job buffer.

Supported deferred operations:

- I2C read
- I2C write-read
- SPI transfer

Example:

```python
with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    dev.bus.setup_spi(
        sck=3,
        mosi=2,
        miso=5,
        cs=6,
        io_voltage=3.3,
        supply_voltage=3.3,
    )

    job_id = dev.bus.defer_spi_transfer([0x9F, 0x00, 0x00, 0x00])

    while True:
        result = dev.bus.deferred_result(job_id)
        if result["status_name"] in {"done", "error"}:
            break

    print(result)
```

Result shape:

```python
{
    "job_id": 1,
    "status": 3,
    "status_name": "done",
    "kind": 3,
    "kind_name": "spi_transfer",
    "data": [0, 239, 64, 24],
    "data_hex": "00EF4018",
}
```

Deferred job status codes:

| Code | Name |
|---:|---|
| 0 | empty |
| 1 | queued |
| 2 | running |
| 3 | done |
| 4 | error |

Deferred job kind codes:

| Code | Name |
|---:|---|
| 1 | i2c_read |
| 2 | i2c_write_read |
| 3 | spi_transfer |

---

## MCP Tools

Planning tools are dry-run and do not touch hardware:

- `plan_i2c_bus`
- `plan_spi_bus`

Hardware tools configure or use the active external bus:

- `scan_i2c_bus`
- `spi_transfer`
- `spi_jedec_id`
- `defer_i2c_read`
- `defer_i2c_write_read`
- `defer_spi_transfer`
- `get_deferred_bus_result`

Recommended smoke flow for an AI agent:

1. Call `plan_i2c_bus` or `plan_spi_bus` and inspect supplies, e-fuses, GPIOs, and warnings.
2. Confirm the physical wiring matches the plan.
3. Use `scan_i2c_bus` for I2C targets or `spi_jedec_id` for SPI flash-like targets.
4. Use direct transfer/read-write helpers for interactive debug.
5. Use deferred jobs when host jitter would make a polling loop unreliable.

---

## BBP Commands

Protocol version remains BBP v4. These opcodes add capabilities without changing
the frame format.

Direct operations:

| Command | ID | Payload | Response |
|---|---:|---|---|
| `EXT_I2C_SETUP` | `0xB8` | `sda_gpio:u8, scl_gpio:u8, frequency_hz:u32, internal_pullups:u8` | same fields |
| `EXT_I2C_SCAN` | `0xB9` | `start_addr:u8, stop_addr:u8, skip_reserved:u8, timeout_ms:u16` | `count:u8, addrs[count]:u8` |
| `EXT_I2C_WRITE` | `0xBA` | `addr:u8, timeout_ms:u16, len:u8, data[len]` | `written:u8` |
| `EXT_I2C_READ` | `0xBB` | `addr:u8, timeout_ms:u16, len:u8` | `len:u8, data[len]` |
| `EXT_I2C_WRITE_READ` | `0xBC` | `addr:u8, timeout_ms:u16, wr_len:u8, rd_len:u8, wr_data[wr_len]` | `len:u8, data[len]` |
| `EXT_SPI_SETUP` | `0xBD` | `sck:u8, mosi:u8, miso:u8, cs:u8, frequency_hz:u32, mode:u8` | same fields |
| `EXT_SPI_TRANSFER` | `0xBE` | `timeout_ms:u16, tx_len:u16, tx_data[tx_len]` | `rx_len:u16, rx_data[rx_len]` |

Deferred operations:

| Command | ID | Payload | Response |
|---|---:|---|---|
| `EXT_JOB_SUBMIT` | `0x75` | `kind:u8, timeout_ms:u16, kind-specific fields` | `job_id:u32` |
| `EXT_JOB_GET` | `0x76` | `job_id:u32` | `job_id:u32, status:u8, kind:u8, result_len:u16, result[result_len]` |

`EXT_JOB_SUBMIT` kind-specific payloads:

| Kind | Payload after `kind, timeout_ms` |
|---:|---|
| 1 | `addr:u8, read_len:u8` |
| 2 | `addr:u8, wr_len:u8, read_len:u8, wr_data[wr_len]` |
| 3 | `tx_len:u16, tx_data[tx_len]` |

---

## HTTP Endpoints

Direct HTTP operations mirror the direct bus operations:

| Method | Path |
|---|---|
| `GET` | `/api/bus/status` |
| `POST` | `/api/bus/i2c/setup` |
| `POST` | `/api/bus/i2c/scan` |
| `POST` | `/api/bus/i2c/write` |
| `POST` | `/api/bus/i2c/read` |
| `POST` | `/api/bus/i2c/write_read` |
| `POST` | `/api/bus/spi/setup` |
| `POST` | `/api/bus/spi/transfer` |

Example I2C setup body:

```json
{
  "sdaGpio": 2,
  "sclGpio": 4,
  "frequencyHz": 400000,
  "internalPullups": false
}
```

Example SPI transfer body:

```json
{
  "timeoutMs": 100,
  "data": [159, 0, 0, 0]
}
```

The Python high-level bus planner still runs host-side before HTTP setup. HTTP
receives resolved ESP32 GPIOs, not physical IO numbers.

---

## Firmware Boundaries

The external target bus engine is intentionally separate from BugBuster internal
control buses:

- Internal board I2C remains for DS4424, HUSB238, and PCA9535.
- External target I2C uses `I2C_NUM_1`.
- Internal board SPI remains for AD74416H and ADGS2414D MUX.
- External target SPI uses `SPI3_HOST`.

This separation keeps target-device transactions from colliding with rail,
MUX, or ADC/DAC control traffic.

---

## Bench Smoke Tests

### I2C Target

Use a known I2C peripheral powered at 3.3 V with external pull-ups.

```python
with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    scan = dev.bus.i2c_scan(
        sda=2,
        scl=3,
        io_voltage=3.3,
        supply_voltage=3.3,
    )
    assert scan["count"] >= 1
    print(scan["addresses"])
```

### SPI Flash-Like Target

Use a target that supports JEDEC `0x9F`.

```python
with bb.connect_usb("/dev/cu.usbmodem1234561") as dev:
    dev.bus.setup_spi(
        sck=3,
        mosi=2,
        miso=5,
        cs=6,
        io_voltage=3.3,
        supply_voltage=3.3,
    )
    ident = dev.bus.spi_jedec_id()
    assert len(ident["jedec_id"]) == 6
    print(ident)
```

If these fail, inspect:

- Plan warnings
- `esp_gpios` against physical wiring
- Target voltage and pull-ups
- Whether the selected IOs span multiple supplies
- Whether another client owns CDC0
