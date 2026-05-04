# BugBuster — ESP32-S3 Firmware

Main controller firmware for BugBuster, running on ESP32-S3 (DFRobot FireBeetle 2).
Controls the AD74416H quad-channel analog I/O chip via SPI, provides USB and WiFi connectivity.

**Current version:** `3.0.0` (BBP wire protocol `v4`)

## Architecture

- **FreeRTOS** with 4 main tasks: ADC poll, fault monitor, command processor, main loop
- **Dual USB CDC**: CLI interface + UART bridge
- **BBP Protocol**: COBS-encoded binary protocol over USB for high-throughput streaming.
  BBP v4 handshake is **14 bytes** (magic[4] + proto[1] + fw[3] + mac[6]); older 8-byte responses are accepted as a legacy fallback.
- **HTTP REST API**: 24+ endpoints under `/api/*` for WiFi clients.  Mutating routes
  require an admin token (`X-BugBuster-Admin-Token` header) issued via the USB
  `GET_ADMIN_TOKEN` (0x74) opcode.  `/api/device/info` now emits `macAddress`
  so pairing can key on the same identifier as the USB handshake.
- **HAT expansion**: UART interface to RP2040 HAT board (921600 baud)
- **Simulated device**: a hardware-free simulator in `tests/mock/` implements
  every BBP handler plus the `/api` surface, so the Python client + MCP server
  can be exercised end-to-end without a board.

### Key Components

| File | Purpose |
|------|---------|
| `ad74416h.cpp/h` | AD74416H HAL — channel functions, DAC, ADC, diagnostics |
| `ad74416h_spi.cpp/h` | SPI driver — 40-bit frames with CRC-8 |
| `tasks.cpp/h` | FreeRTOS task management, ADC polling, command dispatch |
| `bbp.cpp/h` | Binary protocol — COBS framing, SPSC ring buffer, streaming |
| `webserver.cpp/h` | HTTP REST API server |
| `hat.cpp/h` | HAT board detection and UART command forwarding |
| `adgs2414d.cpp/h` | ADGS2414D octal SPST switch driver (4x daisy-chain MUX) |
| `cli.cpp/h` | Serial CLI menu interface |
| `wifi_manager.cpp/h` | WiFi AP+STA management |
| `uart_bridge.cpp/h` | UART-to-USB transparent bridge |

### SPI Bus Sharing

The AD74416H and ADGS2414D MUX share the same SPI bus (SPI2_HOST). Access is arbitrated
via a FreeRTOS mutex (`g_spi_bus_mutex`). The ADC poll task yields the mutex between
polling cycles so MUX operations and ADC config changes can proceed.

### Communication Protocols

- **BBP (USB)**: `0xBB 0x42 0x55 0x47` handshake, COBS-encoded frames, CRC-16/CCITT
- **HTTP (WiFi)**: REST API at `/api/*` endpoints, JSON payloads
- **HAT (UART)**: `0xAA` sync byte, CRC-8, max 32-byte payload, 200ms command timeout

## Build

### PlatformIO (recommended)
```bash
pio run -e esp32s3
pio run -e esp32s3 -t upload
```

### ESP-IDF
```bash
idf.py set-target esp32s3
idf.py build
idf.py flash
```

## Known Limitations

1. **HAT polling disabled** — Conflicts with HAT command UART; ESP32 polls on-demand instead
2. **No system watchdog** — Should be added for production deployment
3. **WiFi password hardcoded** — Default AP password "bugbuster123" in config.h
4. **No HTTPS** — HTTP traffic is unencrypted (acceptable for local network)
5. **ADC stream uses lock-free SPSC** — Uses atomic load/store for proper memory ordering

## MicroPython Scripting

BugBuster embeds a MicroPython interpreter accessible over USB (BBP commands `0xF5–0xFD`) and HTTP
(admin-auth endpoints `/api/scripts/eval`, `/api/scripts/logs`, `/api/scripts/status`,
`/api/scripts/stop`, `/api/scripts/storage`).  Scripts run in one of two modes: **ephemeral**
(VM torn down after each eval) or **persistent** (VM lives across evals, retaining globals).
The built-in `bugbuster` module exposes `Channel`, `I2C`, and `SPI` helpers so scripts can
drive hardware directly without a USB tether.  See [`Docs/MicroPython Examples/`](../Docs/MicroPython%20Examples/)
for annotated examples.

## Board Profiles

BugBuster supports NVS-backed **board profiles** that record the hardware variant, terminal
labeling, and safe operating limits for the connected device under test.  The active profile
is read and written via `GET /POST /api/board` and affects channel defaults, calibration
scope, and UI labeling.  See [`Docs/board_profiles.md`](../Docs/board_profiles.md) for the
profile schema and built-in profile list.

## External Bus

BugBuster exposes an **external I2C** bus (`I2C_NUM_1`, BBP commands `0xB8–0xBC`) and an
**external SPI** bus (`SPI3_HOST`, BBP commands `0xBD–0xBE`) for talking to arbitrary
off-board peripherals without custom firmware.  Long-running transfers can be offloaded as
**deferred jobs** (`0x75 EXT_JOB_SUBMIT` / `0x76 EXT_JOB_GET`) that run in the background
and are polled for completion.  See [`Docs/ExternalBus.md`](../Docs/ExternalBus.md) for
wiring, timing limits, and Python examples.
