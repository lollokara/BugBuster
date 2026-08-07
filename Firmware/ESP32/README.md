# ESP32-S3 mainboard firmware

The controller for the BugBuster mainboard. It drives the AD74416H quad-channel
analog front end, the MUX matrix and the power subsystem, terminates the BBP
control protocol over USB, serves the HTTP API over WiFi, and bridges to
whichever HAT is attached.

Firmware `5.1.0` · BBP wire protocol `v10`.

## Build

```bash
cd Firmware/ESP32
pio run -e esp32s3                # build
pio run -e esp32s3 -t upload      # flash
pio run -e esp32s3 -t uploadfs    # web UI to SPIFFS
```

USB flashing is only needed for bring-up or recovery - normal updates go over
the air (see [OTA](#ota)).

## Architecture

FreeRTOS, four pinned tasks: `adcPoll`, `faultMon`, `cmdProc`, `wavegen`.

```
src/
  bbp/          BBP dispatcher - COBS framing, CRC-16, command registry
    cmds/       One file per command family (adc, dac, hat, ota, wifi, …)
  hal/          Chip drivers - ad74416h, adgs2414d, ds4424, husb238, pca9535
  hat/          HAT detection and UART bridge
  net/          WiFi, mDNS, BLE, USB CDC, UART bridge, api_core
  web/          HTTP server, auth, quick-setup, WebSocket streaming
  bus/          External I²C/SPI engine and bus planner
  mp/           MicroPython runtime, bindings, script storage
  dio/          Digital IO and the DAQ trigger engine
  dsp/          ADC DSP (FFT, statistics)
  power/        USB-PD manager and the VADJ guard
  update/       OTA orchestration for all four MCUs
  diag/         Self-test, board profiles, status LEDs
  cli/          Serial CLI and TUI
```

### Transports

| Transport | Framing | Notes |
|---|---|---|
| **BBP over USB CDC #0** | `0xBB 0x42 0x55 0x47` handshake, COBS frames, CRC-16/CCITT | Single-client lock. The v10 handshake is 14 bytes: magic[4] + proto[1] + fw[3] + mac[6]. |
| **HTTP REST over WiFi** | JSON | 155 distinct `/api/*` paths. Mutating routes need `X-BugBuster-Admin-Token`, issued by the USB `GET_ADMIN_TOKEN` (0x74) command. `/api/device/info` reports `macAddress`, which pairing keys on. |
| **BLE GATT tunnel** | chunked APIREQ/APIRESP | Same JSON dispatch as HTTP, via `net/api_core.cpp`. |
| **HAT UART** | `0xAA` sync, CRC-8, ≤32-byte payload, 200 ms timeout | 921600 8N1 to the RP2040 or DAQ HAT. |

HTTP and BLE both delegate to `api_core_handle(method, path, body)`, so a route
added there is available on both.

Wire formats: [../bbp-protocol.md](../bbp-protocol.md) ·
[../hat-uart-protocol.md](../hat-uart-protocol.md)

### SPI bus sharing

The AD74416H and the ADGS2414D MUX chain share `SPI2_HOST`, arbitrated by the
`g_spi_bus_mutex` FreeRTOS mutex. The ADC poll task releases the mutex between
cycles so MUX operations and ADC reconfiguration can get in.

## Discovery

Once on a WiFi network the firmware advertises two mDNS services on port 80:

- `_http._tcp` - for Bonjour browsers and `curl`
- `_bugbuster._tcp` - so BugBuster-aware tooling can filter cleanly

Both carry TXT keys `version`, `mac`, `proto`, `model`. Default hostname is
`bugbuster-<last3macbytes>.local`.

```python
import bugbuster as bb
for d in bb.discover(timeout=2.0):
    print(d.hostname, d.ip, d.firmware, d.mac)
```

Override the hostname (lowercase letters, digits and `-`; ≤31 chars; no leading
or trailing `-`). Posting an empty string reverts to the default:

```bash
curl -X POST -H "X-BugBuster-Admin-Token: $TOKEN" \
     -d '{"hostname":"benchA"}' http://<ip>/api/wifi/hostname
```

## OTA

```bash
curl -H "X-BugBuster-Admin-Token: $TOKEN" \
     --data-binary @.pio/build/esp32s3/firmware.bin \
     "http://<ip>/api/ota/upload?sha256=$(shasum -a 256 firmware.bin | awk '{print $1}')"
```

The `sha256` query parameter is verified before the boot partition is switched,
so a corrupted upload is discarded with the boot target unchanged.

| Endpoint | Purpose |
|---|---|
| `GET /api/ota/info` | Running and next slot, `ota_state`, `canRollback` |
| `POST /api/ota/upload` | Application image |
| `POST /api/ota/uploadfs` | SPIFFS web-UI partition |
| `POST /api/ota/rollback` | Manual rollback; 409 when there is no target |
| `POST /api/update/apply` | Pull a GitHub release and update all four MCUs |

From Python:

```python
from bugbuster.transport import HTTPTransport
from bugbuster.ota import OTAClient

ota = OTAClient(HTTPTransport("bugbuster-a1b2c3.local", admin_token=tok))
ota.upload_firmware("firmware.bin", on_progress=lambda d, t: print(f"{d}/{t}"))
if ota.get_info().can_rollback:
    ota.rollback()
```

The ESP32 also orchestrates the other three MCUs, applying images in a
firmware-enforced **RP2040 → C6 → P4 → S3** order.

## MicroPython

An embedded MicroPython interpreter, reachable over USB (BBP `0xF5–0xFD`) and
HTTP (`/api/scripts/eval`, `/logs`, `/status`, `/stop`, `/storage`, all
admin-gated).

Two modes: **ephemeral** (the VM is torn down after each eval) and
**persistent** (the VM survives, keeping globals). The built-in `bugbuster`
module exposes `Channel`, `I2C` and `SPI`, so a script can drive hardware with
no host attached - including on boot, via autorun.

Examples: [../../Docs/MicroPython Examples/](../../Docs/MicroPython%20Examples/) ·
Troubleshooting: [../../Docs/micropython-troubleshooting.md](../../Docs/micropython-troubleshooting.md)

## External buses

An external **I²C** bus (`I2C_NUM_1`, BBP `0xB8–0xBC`) and **SPI** bus
(`SPI3_HOST`, BBP `0xBD–0xBE`) let you talk to arbitrary off-board peripherals
without custom firmware. Long transfers can be offloaded as deferred jobs
(`0x75 EXT_JOB_SUBMIT` / `0x76 EXT_JOB_GET`) and polled for completion.

Wiring, timing limits and examples:
[../../Docs/external-bus.md](../../Docs/external-bus.md)

## Board profiles

NVS-backed profiles record the hardware variant, terminal labelling, and safe
operating limits for the connected DUT. Read and written via
`GET`/`POST /api/board`; they affect channel defaults, calibration scope, and UI
labelling. Schema: [../../Docs/board-profiles.md](../../Docs/board-profiles.md)

## Testing without hardware

`tests/mock/` implements every BBP handler plus the `/api` surface, so the
Python client and MCP server can be exercised end to end with no board
attached. See [../../tests/README.md](../../tests/README.md).

## Known limitations

- **HAT polling is on-demand**, not periodic - periodic polling collided with
  the HAT command UART.
- **No system watchdog.** Should be added before any unattended deployment.
- **Default AP password is hardcoded** (`bugbuster123` in `config.h`). Change it
  with `wifi_set_ap_password`.
- **HTTP is unencrypted.** Acceptable on a local bench network, not beyond it.
