# BugBuster ESP32 HTTP API - Full Reference Guide

This document is the exhaustive technical reference for the BugBuster ESP32 HTTP API. Every endpoint registered in the firmware is documented here.

---

## 1. Fundamentals

### 1.1 Connection
- **Port**: 80 (HTTP)
- **Content-Type**: `application/json` (for all POST requests)

### 1.2 Authentication
- **Header**: `X-BugBuster-Admin-Token`
- **Format**: 64-character lowercase hex string.
- **Scope**: Required for all POST/DELETE/PUT and sensitive GET endpoints.

---

## 2. Authentication Flow & Dynamics

The BugBuster HTTP API employs a stateless, token-based authentication mechanism designed for high-security environments while maintaining low latency.

### 2.1 The Admin Token
All administrative and control operations require a **64-character lowercase hex token** (generated from 32 random bytes). This token is stored in the device's Non-Volatile Storage (NVS) and persists across reboots.

- **Storage**: NVS namespace `auth`, key `admin_token`.
- **Transmission**: The token MUST be sent in every authenticated request using the `X-BugBuster-Admin-Token` header.
- **Security**: Token verification uses **constant-time comparison** to prevent timing attacks.

### 2.2 Token Lifecycle & Rotation
Tokens are dynamic and can be managed through the following lifecycle:

1.  **Initial Generation**: On the first boot (or after a factory reset), the device automatically generates a unique random token and stores it in NVS.
2.  **Pairing (Fingerprinting)**: To allow clients to identify the device without exposing the full token, the API provides a **Fingerprint**.
    - **Endpoint**: `GET /api/pairing/info`
    - **Logic**: The fingerprint is the first 8 bytes of `SHA-256(admin_token)`, rendered as 16 hex characters.
    - **Usage**: Clients can use the fingerprint to verify they are talking to the expected device before sending sensitive data.
3.  **Rotation**: For security compliance or to revoke access from all current clients, the token can be rotated.
    - **Endpoint**: `POST /api/pairing/rotate` (Requires current valid token).
    - **Effect**: Generates a new 64-char token, commits it to NVS, and **immediately invalidates** the old token. Subsequent requests with the old token will return `401 Unauthorized`.
4.  **Verification**: A lightweight endpoint allows clients to test a token without performing an action.
    - **Endpoint**: `POST /api/pairing/verify`.

### 2.3 WebSocket Authentication
For real-time streams (REPL and Scope Data), authentication occurs during the handshake phase:
- **Handshake**: The connection is initially established via an unauthenticated `GET` upgrade request.
- **Activation**: The device **requires the full admin token as the first text frame** sent over the socket.
- **Timeout**: If the token is not received within a short timeout (typically 2-5 seconds), the device forcibly closes the WebSocket.

### 2.4 Security Recommendations
- **Transport**: While the device operates on HTTP, it is recommended to use it within a protected local network or via a secure tunnel.
- **Client Caching**: Apps should securely store the token in the system's keychain or encrypted storage, keyed by the device's MAC address and Token Fingerprint.

---

## 3. Pairing & System Identity (6 URIs)

| Endpoint | Method | Description | Auth |
| :--- | :--- | :--- | :--- |
| `/api/pairing/info` | GET | Returns MAC, token fingerprint, and transport type. | No |
| `/api/pairing/verify` | POST | Validates the current admin token. | Yes |
| `/api/pairing/rotate` | POST | Generates and returns a fresh admin token. | Yes |
| `/api/device/info` | GET | Returns silicon revision, IDs, and MAC. | No |
| `/api/device/version` | GET | Returns FW version, build marker, and partition info. | No |
| `/api/device/reset` | POST | Clears alerts and resets all channels to HIGH_IMP. | Yes |

---

## 3. Status & Global Monitoring (8 URIs)

| Endpoint | Method | Description | Auth |
| :--- | :--- | :--- | :--- |
| `/api/status` | GET | Full snapshot of system, channels, and telemetry. | No |
| `/api/overview` | GET | Lightweight dashboard summary. | No |
| `/api/faults` | GET | Current system and channel alert statuses. | No |
| `/api/faults/clear` | POST | Clear all system-wide alerts. | Yes |
| `/api/faults/mask` | POST | Set `alertMask` and `supplyMask`. | Yes |
| `/api/debug` | GET | Internal I2C bus and device driver debug metrics. | Yes |
| `/api/board` | GET | List available board profiles and get active one. | No |
| `/api/board/select` | POST | Activate a profile by `boardId`. | Yes |

---

## 4. Channel Control (0-3) (20+ URIs via Dispatcher)

Pattern: `/api/channel/{0-3}/{suffix}` or `/api/channel/` (suffixes also mapped via dispatcher).

| Suffix | Method | Request Body / Query Params |
| :--- | :--- | :--- |
| `function` | POST | `{"function": 0-11}` (HIGH_IMP, VOUT, IOUT, VIN, etc.) |
| `dac` | POST | `{"code": 0-65535}` OR `{"voltage": F, "bipolar": B}` OR `{"current_mA": F}` |
| `dac/readback` | GET | Returns `{ "channel": N, "code": N }` |
| `adc` | GET | Returns current measurement and config. |
| `adc/config` | POST | `{"mux": N, "range": N, "rate": N}` |
| `din/config` | POST | `{"thresh": N, "debounce": N, "sink": N, ...}` |
| `do/config` | POST | `{"mode": N, "srcSelGpio": B, "t1": N, "t2": N}` |
| `do/set` | POST | `{"on": bool}` |
| `vout/range` | POST | `{"bipolar": bool}` |
| `ilimit` | POST | `{"limit8mA": bool}` |
| `avdd` | POST | `{"select": 0-3}` |
| `rtd/config` | POST | `{"current": 0-1, "excitation_ua": 500/1000}` |

---

## 5. Digital IO (1-12) (28 URIs via Wildcards/Dispatcher)

| Endpoint | Method | Description |
| :--- | :--- | :--- |
| `/api/dio` | GET | Get status of all 12 IO terminals. |
| `/api/dio/{1-12}` | GET | Get status of a single IO. |
| `/api/dio/{1-12}/config` | POST | `{"mode": 0-2}` (0:Disabled, 1:Input, 2:Output). |
| `/api/dio/{1-12}/set` | POST | `{"value": bool}`. |

**Internal GPIOs (0-11):**
- `GET /api/gpio`: List all internal GPIOs.
- `POST /api/gpio/{0-11}/config`: `{"mode": 0-4, "pulldown": bool}`.
- `POST /api/gpio/{0-11}/set`: `{"value": bool}`.

---

## 6. MUX & Signal Routing (5 URIs)

| Endpoint | Method | Body |
| :--- | :--- | :--- |
| `/api/mux` | GET | Returns all switch states as an array. |
| `/api/mux/switch` | POST | `{"device": 0-3, "switch": 0-7, "closed": bool}` |
| `/api/mux/all` | POST | `{"states": [u8, u8, u8, u8]}` |
| `/api/adgs/routes` | GET | Summary of closed routes for main devices. |
| `/api/lshift/oe` | POST | `{"on": bool}` (Enable Level Shifters). |

---

## 7. HAT Expansion Board (18 URIs)

| Endpoint | Method | Purpose |
| :--- | :--- | :--- |
| `/api/hat` | GET | Basic HAT detection and pin status. |
| `/api/hat/power` | GET/POST | `{"connector": 0-1, "enable": bool}`. |
| `/api/hat/config` | POST | Set HAT pin mapping functions. |
| `/api/hat/detect` | POST | Force hardware redetection. |
| `/api/hat/reset` | POST | Hardware reset of HAT RP2040. |
| `/api/hat/v2/caps` | GET | Returns capability bitmask. |
| `/api/hat/v2/rails` | GET | Returns all rail V/I status. |
| `/api/hat/v2/rail/enable` | POST | `{"railId": 0-2, "enable": bool}`. |
| `/api/hat/v2/rail/voltage` | POST | `{"railId": 0-2, "voltageMv": N}`. |
| `/api/hat/v2/led` | POST | `{"ledId": N, "colorCode": N}`. |
| `/api/hat/v2/la/route` | POST | `{"route": N}` (Mux LA signals). |
| `/api/hat/v2/la/log/enable`| POST | `{"enable": bool}` (Relay RP2040 logs). |
| `/api/hat/v2/la/log` | GET | Drain buffered LA events. |
| `/api/hat/v2/swd/setup` | POST | `{"connector": 0-1, "targetVoltageMv": N}`. |
| `/api/hat/v2/io_bank` | POST | `{"dirs": mask, "ups": mask, "dns": mask}`. |
| `/api/hat/v2/io_voltage` | POST | `{"voltageMv": N}`. |

---

## 8. Scripting & Automation (12 URIs)

| Endpoint | Method | Body / Query |
| :--- | :--- | :--- |
| `/api/scripts/eval` | POST | Body: Raw Python code. Query: `?persist=true`. |
| `/api/scripts/run-file` | POST | Query: `?name=myscript.py`. |
| `/api/scripts/status` | GET | Memory usage, script ID, and VM state. |
| `/api/scripts/stop` | POST | Terminates running script. |
| `/api/scripts/reset` | POST | Reboots MicroPython environment. |
| `/api/scripts/logs` | GET | Query: `?since=N`. Returns text and `X-BugBuster-Log-Next`. |
| `/api/scripts/files` | GET/POST | List or upload scripts. |
| `/api/scripts/files/get` | GET | Query: `?name=...`. Download script. |
| `/api/scripts/storage` | GET | SPIFFS filesystem usage. |
| `/api/scripts/autorun/` | GET/POST | Enable/Disable boot script. |

---

## 9. Connectivity & Bus (10 URIs)

| Endpoint | Method | Description |
| :--- | :--- | :--- |
| `/api/bus/status` | GET | Active configuration for I2C and SPI buses. |
| `/api/bus/i2c/setup` | POST | `{"sdaGpio": N, "sclGpio": N, "frequencyHz": N}`. |
| `/api/bus/i2c/scan` | POST | Scans range `startAddr` to `stopAddr`. |
| `/api/bus/i2c/write` | POST | `{"address": N, "data": [bytes]}`. |
| `/api/bus/i2c/read` | POST | `{"address": N, "length": N}`. |
| `/api/bus/i2c/write_read`| POST | `{"address": N, "writeData": [...], "readLength": N}`. |
| `/api/bus/spi/setup` | POST | `{"sck": N, "mosi": N, "miso": N, "cs": N, "hz": N}`. |
| `/api/bus/spi/transfer`| POST | `{"data": [...]}` (Full-duplex transfer). |

---

## 10. WiFi & Maintenance (15 URIs)

| Endpoint | Method | Purpose |
| :--- | :--- | :--- |
| `/api/wifi` | GET | Returns IP, RSSI, and Connection status. |
| `/api/wifi/connect` | POST | `{"ssid": "...", "password": "..."}`. |
| `/api/wifi/scan` | GET | Start background scan and return results. |
| `/api/wifi/hostname` | GET/POST | `{"hostname": "..."}`. |
| `/api/wifi/ap_password`| POST | Change device's own AP WPA2 key. |
| `/api/ota/upload` | POST | Query: `?sha256=...`. Body: ESP32 bin. |
| `/api/ota/upload_rp2040`| POST | Body: RP2040 bin (saved to scripts). |
| `/api/ota/uploadfs` | POST | Body: Web UI SPIFFS image. |
| `/api/ota/info` | GET | Partition slots and rollback status. |
| `/api/ota/rollback` | POST | Revert to previous firmware slot. |
| `/api/update/check` | GET | Query remote server for new firmware. |
| `/api/update/apply` | POST | `{"rp2040": bool, "esp32": bool}`. |
| `/api/update/status` | GET | Progress of background update task. |

---

## 11. Peripherals (IDAC, USB PD, Expander) (12 URIs)

| Endpoint | Method | Purpose |
| :--- | :--- | :--- |
| `/api/idac` | GET | DS4424 current codes and targets. |
| `/api/idac/set` | POST | Set output current. |
| `/api/idac/cal/point` | POST | `{"ch": 0-2, "code": N, "measuredV": F}`. |
| `/api/idac/cal/save` | POST | Save cal to NVS. |
| `/api/usbpd` | GET | Negotiation status and source caps. |
| `/api/usbpd/select` | POST | `{"index": N}` (Request PD profile). |
| `/api/ioexp` | GET | Read PCA9535 inputs. |
| `/api/ioexp/control` | POST | Set PCA9535 outputs. |

---

## 12. Real-time Streams

| Endpoint | Method | Description |
| :--- | :--- | :--- |
| `/api/ws/stream` | WS | Binary scope/ADC stream (Auth required). |
| `/api/scripts/repl/ws`| WS | Interactive MicroPython REPL (Auth required). |
| `/api/scope/stream` | SSE | Downsampled scope data (Server-Sent Events). |
| `/api/scope` | GET | One-shot scope buffer capture. |
| `/api/wavegen/start` | POST | `{"channel": N, "waveform": N, "freq_hz": F, ...}`. |
| `/api/wavegen/stop` | POST | Stop wavegen. |
