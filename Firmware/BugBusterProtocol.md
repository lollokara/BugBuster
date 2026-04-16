# BugBuster Binary Protocol Specification

**Version:** 1.6
**BBP Protocol Version:** 4
**Transport:** USB CDC (Virtual COM Port) + HTTP REST API (WiFi)
**Target:** ESP32-S3 (TinyUSB, Full-Speed 12 Mbps)
**Status:** Active

---

## 1. Overview & Goals

The BugBuster Binary Protocol (BBP) provides a high-throughput, low-latency interface
between the BugBuster device and a host application over USB CDC. It replaces HTTP
polling with a persistent binary link capable of streaming raw 24-bit ADC data at
the full hardware rate (up to 9.6 kSPS x 4 channels).

**Design goals:**
- Full feature parity with the existing HTTP REST API
- Continuous ADC data streaming at up to ~38.4k samples/sec aggregate
- Sub-millisecond command latency (no HTTP overhead)
- Cross-platform: Mac, Linux, Windows (standard CDC/ACM driver, no custom driver)
- Coexists with the UART bridge on CDC #1 (unchanged)
- Zero-configuration: CLI auto-detects the host app and switches modes

**What stays the same:**
- CDC #1 remains the transparent UART bridge (COM port passthrough)
- The HTTP/WiFi interface remains available in parallel
- The command queue architecture is reused (BBP enqueues the same `Command` structs)

---

## 2. Transport Layer

### 2.1 USB CDC Allocation

| CDC Port | Default Mode | Binary Mode | Purpose |
|----------|-------------|-------------|---------|
| CDC #0 | Text CLI | BBP binary link | Debug CLI / Host app data channel |
| CDC #1 | UART bridge | UART bridge (unchanged) | Transparent serial passthrough |

The ESP32-S3 TinyUSB stack supports exactly 2 CDC interfaces. CDC #0 is shared
between the text CLI and the binary protocol, switching at runtime via handshake.

### 2.2 Throughput Budget

USB Full-Speed CDC theoretical max: ~1 MB/s
Worst-case ADC stream: 4 channels x 9.6 kSPS x 3 bytes/sample = **115.2 KB/s**
With COBS + framing overhead (~1.5%): **~117 KB/s**
Headroom: ~8x margin for commands, responses, and burst traffic.

---

## 3. Mode Switching (CLI <-> Binary)

CDC #0 boots in **text CLI mode** (default). The host application triggers a switch
to binary mode via a handshake sequence. On disconnect, the device returns to CLI.

### 3.1 Handshake: CLI -> Binary

**Host sends** (raw bytes, not COBS-encoded):
```
0xBB 0x42 0x55 0x47  ("BUG" with 0xBB prefix)
```

The 0xBB prefix byte is non-printable ASCII, so it cannot be accidentally triggered
by normal CLI typing. The device scans incoming CLI bytes for this 4-byte sequence.

**Device responds** (raw bytes):
```
0xBB 0x42 0x55 0x47 0x04 0x01 0x06 0x00
```

After sending this response, the device:
1. Stops the CLI parser
2. Flushes any pending CLI output
3. Enters binary mode (all subsequent I/O on CDC #0 uses COBS framing)
4. Sets an internal flag `g_binaryMode = true`

The host must wait for the 8-byte response before sending COBS-framed messages.

### 3.2 Binary -> CLI (Disconnect)

Binary mode ends when any of these occur:
1. **DISCONNECT command** (0xFF) sent by the host (graceful)
2. **USB DTR drop** (host closes the serial port)
3. **Handshake timeout** - if no valid COBS frame is received within 5 seconds
   after handshake, the device reverts to CLI

On exit from binary mode:
1. All active streams are stopped
2. `g_binaryMode = false`
3. CLI parser resumes
4. Device prints `\r\n[CLI Ready]\r\n` to signal text mode is active

### 3.3 Detection Logic (Firmware Side)

The CLI input loop already reads bytes one at a time. The detection is a simple
4-byte shift register:

```
// In CLI byte processing loop:
static uint8_t magic_buf[4];
static uint8_t magic_idx = 0;

if (byte == MAGIC[magic_idx]) {
    magic_idx++;
    if (magic_idx == 4) {
        enter_binary_mode();
        magic_idx = 0;
    }
} else {
    // Not a match - process buffered bytes as CLI input, reset
    magic_idx = 0;
}
```

Normal CLI input is unaffected because 0xBB is not a valid ASCII character and
cannot appear in typed commands.

---

## 4. Framing: COBS (Consistent Overhead Byte Stuffing)

All messages in binary mode are framed using **COBS encoding** with a **0x00
delimiter** byte.

### 4.1 Why COBS

- Guarantees no 0x00 bytes in the encoded payload -> 0x00 is an unambiguous
  frame delimiter
- Fixed, predictable overhead: at most 1 byte per 254 payload bytes (~0.4%)
- Re-synchronization after corruption: skip to next 0x00 and resume
- Simple to implement (< 50 lines of C)
- No escape sequences (unlike SLIP)

### 4.2 Frame Format

```
[COBS-encoded payload] [0x00]
 └─ variable length ─┘  └ delimiter
```

The payload (before COBS encoding) is the raw BBP message described in Section 5.

### 4.3 Maximum Frame Size

- Max payload before encoding: **1024 bytes**
- Max encoded frame: **1026 bytes** (1024 + COBS overhead + delimiter)

This limit accommodates the largest possible ADC stream batch (see Section 7)
while keeping firmware buffer requirements reasonable.

### 4.4 COBS Reference Implementation

**Encoding** (host and device):
```c
size_t cobs_encode(const uint8_t *input, size_t length, uint8_t *output) {
    size_t read_idx  = 0;
    size_t write_idx = 1;
    size_t code_idx  = 0;
    uint8_t code     = 1;

    while (read_idx < length) {
        if (input[read_idx] == 0x00) {
            output[code_idx] = code;
            code_idx = write_idx++;
            code = 1;
        } else {
            output[write_idx++] = input[read_idx];
            code++;
            if (code == 0xFF) {
                output[code_idx] = code;
                code_idx = write_idx++;
                code = 1;
            }
        }
        read_idx++;
    }
    output[code_idx] = code;
    return write_idx;
}
```

**Decoding** (host and device):
```c
size_t cobs_decode(const uint8_t *input, size_t length, uint8_t *output) {
    size_t read_idx  = 0;
    size_t write_idx = 0;

    while (read_idx < length) {
        uint8_t code = input[read_idx++];
        for (uint8_t i = 1; i < code && read_idx < length; i++) {
            output[write_idx++] = input[read_idx++];
        }
        if (code != 0xFF && read_idx < length) {
            output[write_idx++] = 0x00;
        }
    }
    if (write_idx > 0) write_idx--;  // Remove trailing zero
    return write_idx;
}
```

---

## 5. Message Format

All messages (after COBS decoding) share a common header:

```
Byte   Field         Size    Description
─────────────────────────────────────────────────────
0      MSG_TYPE      1       Message type (see 5.1)
1-2    SEQ           2       Sequence number (little-endian)
3      CMD_ID        1       Command/event identifier
4..N-3 PAYLOAD       0..1016 Command-specific payload
N-2    CRC_LO        1       CRC-16 low byte
N-1    CRC_HI        1       CRC-16 high byte
```

**Minimum message size:** 6 bytes (header + empty payload + CRC)
**Maximum message size:** 1024 bytes

### 5.1 Message Types

| Value | Name | Direction | Description |
|-------|------|-----------|-------------|
| 0x01 | CMD | Host -> Device | Command request |
| 0x02 | RSP | Device -> Host | Response to a command |
| 0x03 | EVT | Device -> Host | Unsolicited event (stream data, alerts) |
| 0x04 | ERR | Device -> Host | Error response |

### 5.2 Sequence Numbers

- Host assigns a 16-bit sequence number to each CMD message (monotonically
  increasing, wrapping at 0xFFFF)
- RSP and ERR messages echo the SEQ of the CMD they respond to
- EVT messages use a device-side counter (independent sequence space)
- Sequence numbers enable the host to match responses to commands and detect
  dropped messages

### 5.3 CRC-16/CCITT

- Polynomial: 0x1021
- Initial value: 0xFFFF
- Input reflection: false
- Output reflection: false
- Final XOR: 0x0000
- Computed over bytes 0 through N-3 (everything except the CRC itself)

```c
uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}
```

If CRC verification fails, the device silently discards the frame (the host
detects the missing response via timeout).

---

## 6. Command Reference

### 6.1 Data Type Encoding

All multi-byte integers are **little-endian**.
Floats are **IEEE 754 single-precision (32-bit), little-endian**.
Booleans are **1 byte** (0x00 = false, 0x01 = true).

| Type | Size | Notes |
|------|------|-------|
| u8 | 1 | Unsigned 8-bit |
| u16 | 2 | Unsigned 16-bit LE |
| u24 | 3 | Unsigned 24-bit LE (ADC raw codes) |
| u32 | 4 | Unsigned 32-bit LE |
| i32 | 4 | Signed 32-bit LE |
| f32 | 4 | IEEE 754 float LE |
| bool | 1 | 0x00 or 0x01 |

### 6.2 Status & Information

#### 0x01 GET_STATUS
Full device state snapshot (equivalent to `GET /api/status`).

**Request payload:** (empty)

**Response payload:**
```
Offset  Field               Type    Description
0       spi_ok              bool    SPI communication OK
1       die_temp            f32     Die temperature (C)
5       alert_status        u16     Global alert status register
7       alert_mask          u16     Global alert mask
9       supply_alert_status u16     Supply alert status
11      supply_alert_mask   u16     Supply alert mask
13      live_status         u16     Live status register

Per channel (4x, starting at offset 15, stride = 28 bytes):
+0      channel_id          u8      Channel index (0-3)
+1      function            u8      Channel function code (0-12)
+2      adc_raw             u24     ADC raw code (24-bit)
+5      adc_value           f32     Converted ADC value (V, mA, or Ω for RES_MEAS)
+9      adc_range           u8      ADC range code
+10     adc_rate            u8      ADC rate code
+11     adc_mux             u8      ADC mux code
+12     dac_code            u16     Active DAC code
+14     dac_value           f32     Converted DAC value
+18     din_state           bool    Digital input state
+19     din_counter         u32     DIN event counter
+23     do_state            bool    Digital output state
+24     channel_alert       u16     Per-channel alert bits
+26     rtd_excitation_ua   u16     RTD excitation current in µA (125 or 250; 0 when not in RES_MEAS)

Per diagnostic slot (4x, starting at offset 127, stride = 7 bytes):
+0      source              u8      Diagnostic source code (0-13)
+1      raw_code            u16     Raw diagnostic ADC code
+3      value               f32     Converted value (V or C)

MUX state (4 bytes, starting at offset 155):
+0-3    mux_state           u8[4]   Current state of 4 MUX devices (Bit 0 = S1)

Total response: 15 + (4 x 28) + (4 x 7) + 4 = 159 bytes
```

#### 0x02 GET_DEVICE_INFO
Silicon identification (equivalent to `GET /api/device/info`).

**Request payload:** (empty)

**Response payload:**
```
Offset  Field           Type    Description
0       spi_ok          bool    SPI communication OK
1       silicon_rev     u8      Silicon revision
2       silicon_id0     u16     Silicon ID word 0
4       silicon_id1     u16     Silicon ID word 1
```

#### 0x03 GET_FAULTS
Fault/alert status (equivalent to `GET /api/faults`).

**Request payload:** (empty)

**Response payload:**
```
Offset  Field               Type    Description
0       alert_status        u16     Global ALERT_STATUS
2       alert_mask          u16     Global ALERT_MASK (read-only)
4       supply_alert_status u16     SUPPLY_ALERT_STATUS
6       supply_alert_mask   u16     SUPPLY_ALERT_MASK (read-only)

Per channel (4x, stride = 5):
+0      channel_id          u8      Channel (0-3)
+1      channel_alert       u16     CHANNEL_ALERT_STATUS
+3      channel_alert_mask  u16     CHANNEL_ALERT_MASK

Total: 8 + (4 x 5) = 28 bytes
```

#### 0x04 GET_DIAGNOSTICS
Diagnostic slot readings (equivalent to `GET /api/diagnostics`).

**Request payload:** (empty)

**Response payload:**
```
Per slot (4x, stride = 8):
+0      slot            u8      Slot index (0-3)
+1      source          u8      Diagnostic source code (0-13)
+2      raw_code        u16     Raw diagnostic ADC code
+4      value           f32     Converted value (V or C)

Total: 4 x 8 = 32 bytes
```

---

### 6.3 Channel Configuration

#### 0x10 SET_CHANNEL_FUNC
Set channel function (equivalent to `POST /api/channel/X/function`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       function        u8      Function code (0-12, see Section 6.10)
```

**Response payload:**
```
0       channel         u8      Echoed channel
1       function        u8      Echoed function code
```

#### 0x11 SET_DAC_CODE
Set DAC raw code (equivalent to `POST /api/channel/X/dac` with `code`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       code            u16     DAC code (0-65535)
```

**Response payload:**
```
0       channel         u8
1       code            u16
```

#### 0x12 SET_DAC_VOLTAGE
Set DAC voltage output (equivalent to `POST /api/channel/X/dac` with `voltage`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       voltage         f32     Target voltage
5       bipolar         bool    Range: false=0..12V, true=-12..12V
```

**Response payload:**
```
0       channel         u8
1       voltage         f32
5       bipolar         bool
```

#### 0x13 SET_DAC_CURRENT
Set DAC current output (equivalent to `POST /api/channel/X/dac` with `current_mA`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       current_mA      f32     Target current (mA)
```

**Response payload:**
```
0       channel         u8
1       current_mA      f32
```

#### 0x14 SET_ADC_CONFIG
Configure ADC parameters (equivalent to `POST /api/channel/X/adc/config`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       mux             u8      ADC mux code (0-4)
2       range           u8      ADC range code (0-7)
3       rate            u8      ADC rate code (0-13)
```

**Response payload:**
```
0       channel         u8
1       mux             u8
2       range           u8
3       rate            u8
```

#### 0x15 SET_DIN_CONFIG
Configure digital input (equivalent to `POST /api/channel/X/din/config`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       thresh          u8      Comparator threshold (7-bit)
2       thresh_mode     bool    Fixed (true) or programmable (false)
3       debounce        u8      Debounce time code (5-bit)
4       sink            u8      Current sink code (5-bit)
5       sink_range      bool    Sink range (false=low, true=high)
6       oc_det          bool    Open-circuit detection
7       sc_det          bool    Short-circuit detection
```

**Response payload:** Echoes request.

#### 0x16 SET_DO_CONFIG
Configure digital output (equivalent to `POST /api/channel/X/do/config`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       mode            u8      DO mode (DO_MODE[1:0])
2       src_sel_gpio    bool    Source: false=SPI, true=GPIO
3       t1              u8      T1 timing parameter
4       t2              u8      T2 timing parameter
```

**Response payload:** Echoes request.

#### 0x17 SET_DO_STATE
Set digital output on/off (equivalent to `POST /api/channel/X/do/set`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       on              bool    Output state
```

**Response payload:** Echoes request.

#### 0x18 SET_VOUT_RANGE
Set voltage output range (equivalent to `POST /api/channel/X/vout/range`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       bipolar         bool    false=0..12V, true=-12..12V
```

**Response payload:** Echoes request.

#### 0x19 SET_CURRENT_LIMIT
Set current limit (equivalent to `POST /api/channel/X/ilimit`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       limit_8mA       bool    true=8mA limit, false=25mA full
```

**Response payload:** Echoes request.

#### 0x1A SET_AVDD_SELECT
Set AVDD source (equivalent to `POST /api/channel/X/avdd`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       select          u8      AVDD source (0-3)
```

**Response payload:** Echoes request.

#### 0x1B GET_ADC_VALUE
Read single ADC value (equivalent to `GET /api/channel/X/adc`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
```

**Response payload:**
```
0       channel         u8
1       adc_raw         u24     24-bit raw ADC code
4       adc_value       f32     Converted value
8       adc_range       u8      Current range code
9       adc_rate        u8      Current rate code
10      adc_mux         u8      Current mux code
```

#### 0x1C GET_DAC_READBACK
Read active DAC code (equivalent to `GET /api/channel/X/dac/readback`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
```

**Response payload:**
```
0       channel         u8
1       active_code     u16     DAC_ACTIVE register value
```

#### 0x1D SET_RTD_CONFIG
Configure the RTD excitation current for a channel in RES_MEAS mode.
Writes the RTD_CONFIG register (bit 0 = RTD_CURRENT, bit 3 = RTD_ADC_REF ratiometric reference).
The converted `adc_value` in GET_STATUS will be in Ohms: R = V_adc / I_excitation.

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       current         u8      Excitation current: 0 = 500 µA, 1 = 1000 µA (1 mA)
```

**Response payload:** Echoes request.

| current | RTD_CURRENT bit | I_excitation   | Max R (0–625 mV range) | Max R (0–12 V range) |
|---------|----------------|----------------|------------------------|----------------------|
| 0       | 0              | 500 µA         | 1250 Ω                 | 24 kΩ                |
| 1       | 1              | 1000 µA (1 mA) | 625 Ω                  | 12 kΩ                |

> **Note:** Always set the channel function to RES_MEAS (0x07) before calling SET_RTD_CONFIG.
> Switching away from RES_MEAS clears RTD_CONFIG automatically.

---

### 6.4 Fault Management

#### 0x20 CLEAR_ALL_ALERTS
Clear all alert status bits (equivalent to `POST /api/faults/clear`).

**Request payload:** (empty)
**Response payload:** (empty)

#### 0x21 CLEAR_CHANNEL_ALERT
Clear a single channel's alert (equivalent to `POST /api/faults/clear/X`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
```

**Response payload:** Echoes request.

#### 0x22 SET_ALERT_MASK
Set global alert masks (equivalent to `POST /api/faults/mask`).

**Request payload:**
```
0       alert_mask      u16     ALERT_MASK register value
2       supply_mask     u16     SUPPLY_ALERT_MASK register value
```

**Response payload:** Echoes request.

#### 0x23 SET_CHANNEL_ALERT_MASK
Set per-channel alert mask (equivalent to `POST /api/faults/mask/X`).

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       mask            u16     CHANNEL_ALERT_MASK value
```

**Response payload:** Echoes request.

---

### 6.5 Diagnostics Configuration

#### 0x30 SET_DIAG_CONFIG
Configure a diagnostic slot source (equivalent to `POST /api/diagnostics/config`).

**Request payload:**
```
0       slot            u8      Slot index (0-3)
1       source          u8      Diagnostic source code (0-13)
```

**Response payload:** Echoes request.

---

### 6.6 GPIO Control

#### 0x40 GET_GPIO_STATUS
Read all GPIO states (equivalent to `GET /api/gpio`).

**Request payload:** (empty)

**Response payload:**
```
Per GPIO (6x, stride = 5):
+0      gpio_id         u8      GPIO index (0-5, maps to A-F)
+1      mode            u8      GPIO_SELECT mode (0-4)
+2      output          bool    Current output value
+3      input           bool    Current input value
+4      pulldown        bool    Pull-down enabled

Total: 6 x 5 = 30 bytes
```

#### 0x41 SET_GPIO_CONFIG
Configure GPIO mode (equivalent to `POST /api/gpio/X/config`).

**Request payload:**
```
0       gpio            u8      GPIO index (0-5)
1       mode            u8      GPIO mode (0-4)
2       pulldown        bool    Enable pull-down
```

**Response payload:** Echoes request.

#### 0x42 SET_GPIO_VALUE
Set GPIO output value (equivalent to `POST /api/gpio/X/set`).

**Request payload:**
```
0       gpio            u8      GPIO index (0-5)
1       value           bool    Output value
```

**Response payload:** Echoes request.

---

### 6.5b Self-Test, Calibration & E-fuse Monitoring

Uses U23 (5th ADGS2414D in daisy-chain, device index 4) to route internal
power rails and e-fuse IMON pins to AD74416H Channel D for measurement.

**Safety interlock:** U23 and U17 S2 (IO 10 analog mode) are mutually exclusive.
If U17 S2 is closed, all self-test measurement commands return -1.  If U23 is
active, U17 S2 cannot be closed.

HTTP equivalents:
- `GET /api/selftest` — boot result + calibration status
- `GET /api/selftest/supply/{rail}` — measure supply rail (0=VADJ1, 1=VADJ2, 2=3V3_ADJ)
- `GET /api/selftest/efuse` — all 4 e-fuse currents
- `POST /api/selftest/calibrate` body `{"channel": 1}` — start auto-calibration

#### 0x05 SELFTEST_STATUS
Get boot self-test result and calibration status.

**Request payload:** (empty)

**Response payload:**
```
0       boot_ran        bool    true if boot test was executed
1       boot_passed     bool    true if all supplies OK
2       vadj1_v         f32     measured VADJ1 voltage (or -1)
6       vadj2_v         f32     measured VADJ2 voltage (or -1)
10      vlogic_v        f32     measured 3V3_ADJ voltage (or -1)
14      cal_status      u8      0=idle, 1=running, 2=success, 3=failed
15      cal_channel     u8      IDAC channel being calibrated
16      cal_points      u8      calibration points collected
17      cal_error_mv    f32     final error in mV (if success)
```

#### 0x06 SELFTEST_MEASURE_SUPPLY
Measure a supply rail via U23. Requires U17 S2 to be open.

**Request payload:**
```
0       rail            u8      0=VADJ1, 1=VADJ2, 2=3V3_ADJ
```

**Response payload:**
```
0       rail            u8      echoed rail
1       voltage         f32     measured voltage in volts (corrected for divider), -1 on error
```

VADJ1/VADJ2 are measured through a voltage divider (R_top=34.8k, R_bottom=100k,
ratio=0.7418).  The returned voltage is the actual supply voltage (corrected).
3V3_ADJ is measured directly (no divider).

#### 0x07 SELFTEST_EFUSE_CURRENTS
Get all 4 e-fuse output currents from background monitoring.

**Request payload:** (empty)

**Response payload:**
```
0       available       bool    false if U17 S2 is closed (cannot measure)
1       timestamp_ms    u32     device uptime when last measured
5       efuse1_a        f32     e-fuse 1 current in amps (-1 = unavailable)
9       efuse2_a        f32     e-fuse 2 current in amps
13      efuse3_a        f32     e-fuse 3 current in amps
17      efuse4_a        f32     e-fuse 4 current in amps
```

IMON scaling: V_IMON = I_OUT × G_IMON × R_IOCP.
G_IMON = 50 µA/A (TPS1641x typ), R_IOCP = 11 kΩ → 550 mV per amp.

#### 0x08 SELFTEST_AUTO_CAL
Start automatic IDAC calibration. Sweeps DAC codes, measures output via U23,
builds calibration curve, saves to NVS. Takes several seconds.

**Request payload:**
```
0       idac_channel    u8      1=VADJ1, 2=VADJ2
```

**Response payload:**
```
0       status          u8      0=idle, 1=running, 2=success, 3=failed
1       channel         u8      channel calibrated
2       points          u8      calibration points collected
3       error_mv        f32     verification error in mV
```

Returns ERR_BUSY if calibration already running or interlock violation.

#### 0x09 SELFTEST_INT_SUPPLIES
Measure internal AD74416H supply rails using diagnostic slots.
Works in both breadboard and PCB mode (no U23 required).
Equivalent to `GET /api/selftest/supplies`.

**Request payload:** (empty)

**Response payload:**
```
0       valid           bool    true if measurement completed
1       supplies_ok     bool    true if all supplies within expected range
2       avdd_hi_v       f32     Positive analog supply (breadboard: ~21.5V, PCB: ~15V)
6       dvcc_v          f32     Digital supply (breadboard: ~5V, PCB: ~3.3V)
10      avcc_v          f32     Analog supply AVCC (~5V both modes)
14      avss_v          f32     Negative analog supply (breadboard: ~-16V, PCB: ~-15V)
18      temp_c          f32     Die temperature in °C
```

Expected supply ranges:

| Supply | Breadboard | PCB |
|--------|-----------|-----|
| AVDD_HI | 18–25 V | 13.5–16.5 V |
| DVCC | 4.5–5.5 V | 3.0–3.6 V |
| AVCC | 4.5–5.5 V | 4.5–5.5 V |
| AVSS | -20 to -13 V | -16.5 to -13.5 V |

---

### 6.6b Digital IO (ESP32 GPIO)

The BugBuster exposes 12 logical digital IOs mapped to ESP32 GPIO pins.
On the PCB these GPIOs pass through the ADGS2414D MUX matrix and TXS0108E
level shifters to the physical terminal blocks.  The MUX routing is managed
by the host library (SET_MUX_ALL); the DIO commands only drive the ESP32
GPIO pins themselves.

IO numbering: 1–12 (matches the HAL port numbers).

- IOs 1, 4, 7, 10 — first IO of each IO_Block (also analog-capable)
- IOs 2, 3, 5, 6, 8, 9, 11, 12 — digital-only (positions 2 & 3)

HTTP equivalents:
- `GET /api/dio` — read all 12 IO states
- `GET /api/dio/{n}` — read single IO
- `POST /api/dio/{n}/config` body `{"mode": 1}` — configure direction
- `POST /api/dio/{n}/set` body `{"value": true}` — set output level

#### 0x43 DIO_GET_ALL
Read the state of all 12 digital IOs (equivalent to `GET /api/dio`).

**Request payload:** (empty)

**Response payload:**
```
0       count           u8      Number of IOs (always 12)

Per IO (stride = 5):
+0      io              u8      IO number (1-12)
+1      gpio            i8      ESP32 GPIO pin number
+2      mode            u8      0=disabled, 1=input, 2=output
+3      output          bool    Last written output level
+4      input           bool    Last read input level
```

#### 0x44 DIO_CONFIG
Configure an IO's direction (equivalent to `POST /api/dio/{n}/config`).

**Request payload:**
```
0       io              u8      IO number (1-12)
1       mode            u8      0=disabled, 1=input, 2=output
```

**Response payload:** Echoes request.

#### 0x45 DIO_WRITE
Set a digital output level (equivalent to `POST /api/dio/{n}/set`).

**Request payload:**
```
0       io              u8      IO number (1-12), must be mode=2 (output)
1       value           bool    true=HIGH, false=LOW
```

**Response payload:** Echoes request.

#### 0x46 DIO_READ
Read a single IO (equivalent to `GET /api/dio/{n}`).

**Request payload:**
```
0       io              u8      IO number (1-12)
```

**Response payload:**
```
0       io              u8      IO number (echoed)
1       mode            u8      Current mode (0=disabled, 1=input, 2=output)
2       value           bool    Current level (input reads live, output reads last written)
```

---

### 6.7 UART Bridge Configuration

#### 0x50 GET_UART_CONFIG
Read UART bridge configuration (equivalent to `GET /api/uart/config`).

**Request payload:** (empty)

**Response payload:**
```
0       bridge_count    u8      Number of bridges

Per bridge (stride = 12):
+0      bridge_id       u8      Bridge index
+1      uart_num        u8      UART peripheral (0-2)
+2      tx_pin          u8      TX GPIO pin
+3      rx_pin          u8      RX GPIO pin
+4      baudrate        u32     Baud rate
+8      data_bits       u8      Data bits (5-8)
+9      parity          u8      Parity (0=none, 1=odd, 2=even)
+10     stop_bits       u8      Stop bits (0=1, 1=1.5, 2=2)
+11     enabled         bool    Bridge active
```

#### 0x51 SET_UART_CONFIG
Configure UART bridge (equivalent to `POST /api/uart/X/config`).

**Request payload:**
```
0       bridge_id       u8      Bridge index
1       uart_num        u8      UART peripheral (0-2)
2       tx_pin          u8      TX GPIO pin
3       rx_pin          u8      RX GPIO pin
4       baudrate        u32     Baud rate (300-3000000)
8       data_bits       u8      Data bits (5-8)
9       parity          u8      Parity (0=none, 1=odd, 2=even)
10      stop_bits       u8      Stop bits (0=1, 1=1.5, 2=2)
11      enabled         bool    Bridge active
```

**Response payload:** Echoes request.

#### 0x52 GET_UART_PINS
Get available GPIO pins for UART (equivalent to `GET /api/uart/pins`).

**Request payload:** (empty)

**Response payload:**
```
0       count           u8      Number of available pins
1..N    pins            u8[]    GPIO pin numbers
```

---

### 6.8 Register Access (Low-Level)

#### 0x71 REGISTER_READ
Read a raw AD74416H register (equivalent to CLI `rreg`).

**Request payload:**
```
0       address         u8      Register address (hex)
```

**Response payload:**
```
0       address         u8      Echoed address
1       value           u16     16-bit register value
```

#### 0x72 REGISTER_WRITE
Write a raw AD74416H register (equivalent to CLI `wreg`).

**Request payload:**
```
0       address         u8      Register address
1       value           u16     16-bit register value
```

**Response payload:** Echoes request.

#### 0x73 SET_WATCHDOG
Enable or disable the AD74416H hardware watchdog timer. When enabled,
the device resets all channels to HIGH_IMP if no SPI transaction occurs
within the configured timeout. Disabled by default.

**Request payload:**
```
0       enable          u8      1=enable, 0=disable
1       timeout_code    u8      0=1ms, 1=5ms, 2=10ms, 3=25ms, 4=50ms,
                                5=100ms, 6=250ms, 7=500ms, 8=750ms, 9=1000ms, 10=2000ms
```

**Response payload:** Echoes request.

---

### 6.9 System Commands

#### 0x70 DEVICE_RESET
Reset all channels to HIGH_IMP and clear alerts (equivalent to `POST /api/device/reset`).

**Request payload:** (empty)
**Response payload:** (empty)

#### 0xFE PING
Keepalive / latency measurement. Device echoes immediately.

**Request payload:**
```
0..3    token           u32     Arbitrary token (echoed back)
```

**Response payload:**
```
0..3    token           u32     Echoed token
4..7    uptime_ms       u32     Device uptime in milliseconds
```

#### 0x74 GET_ADMIN_TOKEN
Retrieve the transient admin token for auth-protected REST endpoints.
**USB CDC #0 ONLY.**

**Request payload:** (empty)
**Response payload:**
```
0       len             u8      Token length
1..N    token           char[]  Admin token string
```

#### 0xFF DISCONNECT
Graceful exit from binary mode. Device returns to CLI.

**Request payload:** (empty)
**Response payload:** (empty -- sent before reverting to CLI mode)

---

### 6.10 Enum Reference Tables

**ChannelFunction codes:**

| Code | Name | Description |
|------|------|-------------|
| 0 | HIGH_IMP | High-impedance (safe state) |
| 1 | VOUT | Voltage output |
| 2 | IOUT | Current output |
| 3 | VIN | Voltage input |
| 4 | IIN_EXT_PWR | Current input (external power) |
| 5 | IIN_LOOP_PWR | Current input (loop powered) |
| 7 | RES_MEAS | Resistance measurement (RTD) |
| 8 | DIN_LOGIC | Digital input (logic level) |
| 9 | DIN_LOOP | Digital input (loop powered) |
| 10 | IOUT_HART | Current output with HART |
| 11 | IIN_EXT_HART | Current input HART (ext power) |
| 12 | IIN_LOOP_HART | Current input HART (loop) |

**ADC Range codes:** See FirmwareStructure.md Section 2.
**ADC Rate codes:** See FirmwareStructure.md Section 2.
**ADC Mux codes:** See FirmwareStructure.md Section 2.
**GPIO Mode codes:** 0=HIGH_IMP, 1=OUTPUT, 2=INPUT, 3=DIN_OUT, 4=DO_EXT
**Diagnostic Source codes:** See FirmwareStructure.md Section 12.

### 6.11 DS4424 IDAC (I2C, addr 0x20)

Controls output voltage of LTM8063/LTM8078 regulators via current injection into
feedback networks. 3 active channels: IDAC0=Level Shifter V, IDAC1=V_ADJ1, IDAC2=V_ADJ2.

#### 0xA0 IDAC_GET_STATUS
Get all IDAC channel states.

**Request payload:** (empty)

**Response payload:**
```
0       present         bool    DS4424 found on I2C
Per channel (4x):
+0      ch              u8      Channel index
+1      dac_code        u8      Current DAC code (signed as i8: -127..+127)
+2      target_v        f32     Target output voltage
+6      actual_v        f32     Last measured voltage (from ADC calibration)
+10     midpoint_v      f32     Midpoint voltage (DAC=0)
+14     v_min           f32     Minimum allowed voltage
+18     v_max           f32     Maximum allowed voltage
+22     step_mv         f32     Step size (mV per code)
+26     calibrated      bool    Has valid calibration data
```

#### 0xA1 IDAC_SET_CODE
Set raw DAC code for a channel.

**Request payload:**
```
0       ch              u8      Channel (0-3)
1       code            u8      DAC code (signed i8: -127 sink/raise to +127 source/lower)
```

**Response:** ch(u8) + code(u8) + computed_voltage(f32)

#### 0xA2 IDAC_SET_VOLTAGE
Set target output voltage. Driver computes optimal DAC code.

**Request payload:**
```
0       ch              u8      Channel (0-2, ch3 not connected)
1       voltage         f32     Target voltage (clamped to channel range)
```

**Response:** ch(u8) + code(u8) + target_v(f32)

#### 0xA3 IDAC_CALIBRATE (reserved)
Reserved; calibration is UI-driven via 0xA4-0xA6.

#### 0xA4 IDAC_CAL_ADD_POINT
Add a single calibration point (measured via ADC externally).

**Request payload:**
```
Offset  Field           Type    Description
0       ch              u8      Channel (0-2)
1       code            u8      DAC code (signed i8)
2       measured_v      f32     ADC-measured voltage at this code
```

**Response payload:**
```
Offset  Field           Type    Description
0       ch              u8      Echoed channel
1       count           u8      Total calibration points stored for this channel
2       valid           bool    true if enough points for a valid fit
```

#### 0xA5 IDAC_CAL_CLEAR
Clear all calibration data for a channel, resetting it to uncalibrated state.

**Request payload:**
```
Offset  Field           Type    Description
0       ch              u8      Channel (0-2)
```

**Response payload:**
```
Offset  Field           Type    Description
0       ch              u8      Echoed channel
```

#### 0xA6 IDAC_CAL_SAVE
Save all calibration data to NVS flash. Persists across reboots.

**Request payload:** (empty)

**Response payload:**
```
Offset  Field           Type    Description
0       success         bool    true if NVS write succeeded
```

### 6.12 PCA9535 GPIO Expander (I2C, addr 0x23)

16-bit I/O expander controlling power supply enables, E-Fuse enables, and reading
power-good/fault status signals.

#### 0xB0 PCA_GET_STATUS
Get all PCA9535 port states.

**Response payload:**
```
0       present         bool    PCA9535 found on I2C
1       input0          u8      Port 0 input register
2       input1          u8      Port 1 input register
3       output0         u8      Port 0 output register
4       output1         u8      Port 1 output register
5       logic_pg        bool    Main logic power good
6       vadj1_pg        bool    V_ADJ1 power good
7       vadj2_pg        bool    V_ADJ2 power good
8-11    efuse_flt[4]    bool    E-Fuse fault flags (active = fault)
12      vadj1_en        bool    V_ADJ1 enable state
13      vadj2_en        bool    V_ADJ2 enable state
14      en_15v          bool    ±15V analog supply enable
15      en_mux          bool    MUX power enable
16      en_usb_hub      bool    USB hub enable
17-20   efuse_en[4]     bool    E-Fuse enable states
```

#### 0xB1 PCA_SET_CONTROL
Set a named control output.

**Request payload:**
```
0       control         u8      Control ID (see PcaControl enum)
1       on              bool    true=enable, false=disable
```

**PcaControl enum:**

| Code | Name | Description |
|------|------|-------------|
| 0 | VADJ1_EN | V_ADJ1 regulator enable |
| 1 | VADJ2_EN | V_ADJ2 regulator enable |
| 2 | EN_15V_A | ±15V analog supply enable |
| 3 | EN_MUX | MUX switch power enable |
| 4 | EN_USB_HUB | USB hub enable |
| 5 | EFUSE1_EN | E-Fuse 1 enable (→ P1) |
| 6 | EFUSE2_EN | E-Fuse 2 enable (→ P2) |
| 7 | EFUSE3_EN | E-Fuse 3 enable (→ P3) |
| 8 | EFUSE4_EN | E-Fuse 4 enable (→ P4) |

#### 0xB2 PCA_SET_PORT
Set raw output port value (only output-configured bits are applied).

**Request payload:**
```
0       port            u8      Port number (0 or 1)
1       value           u8      Output value
```

#### 0xB3 PCA_SET_FAULT_CFG
Configure PCA9535 fault handling behavior.

**Request payload:**
```
0       auto_disable    u8      1=auto-disable faulted e-fuse, 0=log only
1       log_events      u8      1=log events to console, 0=silent
```

**Response payload:** Same as request (echo of applied config).

#### 0xB4 PCA_GET_FAULT_LOG
Get recent PCA9535 fault events (ring buffer, last 16).

**Request payload:** Empty.

**Response payload:**
```
0       count           u8      Number of events (0–16)
Per event (count×):
+0      type            u8      0=efuse_trip, 1=efuse_clear, 2=pg_lost, 3=pg_restored
+1      channel         u8      E-fuse index (0-3) or PG source (0=logic, 1=vadj1, 2=vadj2)
+2      timestamp_ms    u32     Milliseconds since boot
```

### 6.13 HAT Expansion Board

HAT (Hardware Attached on Top) boards connect via a dedicated header providing
UART communication (GPIO43/44, 115200 8N1) and ADC-based detection (GPIO47).
BugBuster is the UART master. PCB mode only.

**Detection:** GPIO47 has a 10kΩ pull-up to 3.3V. HAT boards have pull-down
resistors creating a voltage divider. ~3.3V = no HAT, ~1.65V = SWD/GPIO HAT.

**EXP_EXT_1-4:** Four I/O lines independently configurable as:
DISCONNECTED(0), SWDIO(1), SWCLK(2), TRACE1(3), TRACE2(4),
GPIO1(5), GPIO2(6), GPIO3(7), GPIO4(8).

#### 0xC5 HAT_GET_STATUS
Get HAT detection state, connection status, and current pin configuration.

**Response payload:**
```
0       detected        bool    HAT physically present (ADC)
1       connected       bool    UART communication established
2       type            u8      HAT type (0=none, 1=SWD/GPIO)
3       detect_voltage  f32     Raw ADC voltage on detect pin
7       fw_major        u8      HAT firmware version major
8       fw_minor        u8      HAT firmware version minor
9       confirmed       bool    Last config was acknowledged by HAT
10-13   ext_func        u8[4]   EXP_EXT_1 to EXP_EXT_4 functions
14      a_enabled       bool    Connector A power
15      a_current_ma    f32     Connector A current
19      a_fault         bool    Connector A fault
20      b_enabled       bool    Connector B power
21      b_current_ma    f32     Connector B current
25      b_fault         bool    Connector B fault
26      io_voltage_mv   u16     HVPAK IO voltage
28      hvpak_part      u8      HVPAK identity
29      hvpak_ready     bool    HVPAK mailbox ready
30      hvpak_last_err  u8      HVPAK error code
31      dap_connected   bool    CMSIS-DAP host connected
32      target_detect   bool    SWD target detected
33      target_dpidr    u32     Target DPIDR
```

#### 0xC6 HAT_SET_PIN
Set a single EXP_EXT pin function.

**Request payload:**
```
0       pin             u8      Pin index (0-3 = EXP_EXT_1 to EXP_EXT_4)
1       function        u8      HatPinFunction enum value
```

**Response payload:** `[pin, function, confirmed:bool]`

#### 0xC7 HAT_SET_ALL_PINS
Set all 4 EXP_EXT pin functions at once.

**Request payload:**
```
0       ext1_func       u8      EXP_EXT_1 function
1       ext2_func       u8      EXP_EXT_2 function
2       ext3_func       u8      EXP_EXT_3 function
3       ext4_func       u8      EXP_EXT_4 function
```

**Response payload:** `[ext1, ext2, ext3, ext4, confirmed:bool]`

#### 0xC8 HAT_RESET
Reset HAT to default state (all pins disconnected).

**Request payload:** Empty.
**Response payload:** Empty (success) or error.

#### 0xC9 HAT_DETECT
Re-run HAT detection (ADC read + UART connect attempt).

**Response payload:**
```
0       detected        bool    HAT present
1       type            u8      HAT type
2       detect_voltage  f32     ADC voltage
6       connected       bool    UART connected
```

### 6.13b HAT Power Management

Commands for controlling HAT connector power, IO voltage (via HVPAK I2C), and SWD setup.

#### 0xCA HAT_SET_POWER
Enable or disable power to a HAT connector.

**Request payload:**
```
0       connector       u8      0=Connector A, 1=Connector B
1       enable          bool    true=enable power, false=disable
```

**Response payload:** `[connector, enable, confirmed:bool]`

The RP2040 controls power via BB_EN_A_PIN (GPIO4) / BB_EN_B_PIN (GPIO5). Overcurrent faults are monitored on BB_FAULT_A_PIN (GPIO20) / BB_FAULT_B_PIN (GPIO21), active low.

#### 0xCB HAT_GET_POWER
Get power status for both connectors.

**Response payload:**
```
0       a_enabled        bool    Connector A power state
1       a_current_ma     f32     Connector A current (mA, from ADC0)
5       a_fault          bool    Connector A overcurrent fault
6       b_enabled        bool    Connector B power state
7       b_current_ma     f32     Connector B current (mA, from ADC1)
11      b_fault          bool    Connector B overcurrent fault
12      io_voltage_mv    u16     Last applied HVPAK preset voltage
14      hvpak_part       u8      0=unknown, 1=SLG47104, 2=SLG47115-E
15      hvpak_ready      bool    true when identity is resolved and mailbox is usable
16      hvpak_last_error u8      RP2040-side HVPAK error code
```

#### 0xCC HAT_SET_IO_VOLTAGE
Set the HVPAK IO voltage level through the RP2040 ↔ GreenPAK mailbox contract.

**Request payload:**
```
0       voltage_mv      u16     Target preset voltage in millivolts
```

**Supported preset voltages:** `1200`, `1800`, `2500`, `3300`, `5000`

**Response payload:** `[requested_mv:u16, actual_mv:u16, hvpak_part:u8, hvpak_ready:bool, hvpak_last_error:u8]`

**GreenPAK mailbox contract:**
- I2C address: `0x48`
- register `0x48`: read-only identity byte programmed in OTP
- register `0x4C`: writable command byte (virtual input mailbox)
- identity values: `0x04` = `SLG47104`, `0x15` = `SLG47115-E`

The programmed GreenPAK image must match this mailbox contract for the RP2040
driver to report `hvpak_ready=true`.

#### 0xCD HAT_SETUP_SWD
Configure SWD debug probe (set IO voltage, enable connector, configure pins for SWD).

**Request payload:**
```
0       voltage_mv      u16     Target IO voltage (default 3300)
2       connector       u8      0=A, 1=B
```

**Response payload:** `[configured:bool, voltage_mv:u16, connector:u8]`

Automatically sets EXP_EXT_1=SWDIO, EXP_EXT_2=SWCLK, enables the specified connector, and sets IO voltage.

#### 0xCE HAT_GET_HVPAK_INFO
Get detected HVPAK part, readiness, error, and voltage summary.

**Response payload:** `[part:u8, ready:bool, last_error:u8, factory_virgin:bool, service_window_ok:bool, requested_mv:u16, applied_mv:u16, service_f5:u8, service_fd:u8, service_fe:u8]`

`factory_virgin` is a conservative heuristic:
- mailbox identity did not match a provisioned image
- GreenPAK service bytes were still readable
- service bytes `F5`, `FD`, and `FE` were all at their default `0x00` state

This should be read as “factory-virgin / unprovisioned candidate”, not as a
strong proof of pristine OTP contents.

#### 0xDB HAT_GET_HVPAK_CAPS
Get the detected part capability profile.

**Response payload:** `[flags:u32, lut2_count:u8, lut3_count:u8, lut4_count:u8, pwm_count:u8, comparator_count:u8, bridge_count:u8]`

#### 0xDC / 0xDD HAT_GET_HVPAK_LUT / HAT_SET_HVPAK_LUT
Read or write a runtime LUT truth table.

**Request payload:** `[kind:u8, index:u8]` for GET, `[kind:u8, index:u8, truth_table:u16]` for SET

**Response payload:** `[kind:u8, index:u8, width_bits:u8, truth_table:u16]`

#### 0xDE / 0xDF HAT_GET_HVPAK_BRIDGE / HAT_SET_HVPAK_BRIDGE
Read or write the HV bridge configuration.

**Bridge payload:** `[output_mode0:u8, ocp_retry0:u8, output_mode1:u8, ocp_retry1:u8, predriver:bool, full_bridge:bool, control_sel_ph_en:bool, ocp_deglitch:bool, uvlo_enable:bool]`

#### 0xE5 / 0xE6 HAT_GET_HVPAK_ANALOG / HAT_SET_HVPAK_ANALOG
Read or write the HVPAK analog runtime configuration.

**Analog payload:** `[vref_mode:u8, vref_powered:bool, vref_power_from_matrix:bool, vref_sink_12ua:bool, vref_input_sel:u8, current_sense_vref:u8, current_sense_dynamic_from_pwm:bool, current_sense_gain:u8, current_sense_invert:bool, current_sense_enable:bool, acmp0_gain:u8, acmp0_vref:u8, has_acmp1:bool, acmp1_gain:u8, acmp1_vref:u8]`

#### 0xE7 / 0xE8 HAT_GET_HVPAK_PWM / HAT_SET_HVPAK_PWM
Read or write one PWM block.

**GET request payload:** `[index:u8]`

**PWM response payload:** `[index:u8, initial_value:u8, current_value:u8, resolution_7bit:bool, out_plus_inverted:bool, out_minus_inverted:bool, async_powerdown:bool, autostop_mode:bool, boundary_osc_disable:bool, phase_correct:bool, deadband:u8, stop_mode:bool, i2c_trigger:bool, duty_source:u8, period_clock_source:u8, duty_clock_source:u8, last_error:u8]`

**SET request payload:** same shape as response, but `current_value` is ignored on write.

#### 0xE9 / 0xEA HAT_HVPAK_REG_READ / HAT_HVPAK_REG_WRITE_MASKED
Guarded raw register access for advanced/debug use.

**REG_READ request:** `[addr:u8]`
**REG_READ response:** `[addr:u8, value:u8]`

**REG_WRITE_MASKED request:** `[addr:u8, mask:u8, value:u8]`
**REG_WRITE_MASKED response:** `[addr:u8, mask:u8, value:u8, actual:u8]`

**Advanced HVPAK error mapping:**
- `0x0B` `HVPAK_NO_DEVICE`
- `0x0C` `HVPAK_TIMEOUT`
- `0x0D` `HVPAK_UNKNOWN_IDENTITY`
- `0x0E` `HVPAK_UNSUPPORTED_CAP`
- `0x0F` `HVPAK_INVALID_INDEX`
- `0x10` `HVPAK_UNSAFE_REGISTER`

`INVALID_PARAM` remains the top-level error for malformed payloads / invalid arguments.

### 6.13c HAT Logic Analyzer

The RP2040 HAT provides a PIO-based logic analyzer with 1/2/4-channel capture at up to 125 MHz. Data is captured via DMA into a 76 KB SRAM buffer (19,456 × 32-bit words).

**Sample packing (raw mode):**
- 1-channel: 32 samples per 32-bit word
- 2-channel: 16 samples per 32-bit word
- 4-channel: 8 samples per 32-bit word

**GPIO pins:** CH0=GPIO14, CH1=GPIO15, CH2=GPIO16, CH3=GPIO17

#### 0xCF HAT_LA_CONFIG
Configure logic analyzer capture parameters.

**Request payload:**
```
0       channels        u8      Number of channels (1, 2, or 4)
1       sample_rate_hz  u32     Desired sample rate in Hz (LE)
5       depth_samples   u32     Number of samples to capture (LE, 0=max)
9       rle_enabled     bool    Enable RLE compression
```

**Response payload:**
```
0       channels        u8      Configured channels
1       actual_rate_hz  u32     Actual rate achieved (PIO clock divider quantization)
5       max_samples     u32     Maximum samples for this config
```

The actual rate is `sys_clk / round(sys_clk / desired_rate)`. Maximum depth depends on channel count and RLE mode.

#### 0xDA HAT_LA_TRIGGER
Set trigger condition for the next capture.

**Request payload:**
```
0       type            u8      Trigger type (see table below)
1       channel         u8      Trigger channel (0-3)
```

| Type | Name | Description |
|------|------|-------------|
| 0 | NONE | No trigger — capture starts immediately on arm |
| 1 | RISING | Rising edge on specified channel |
| 2 | FALLING | Falling edge on specified channel |
| 3 | BOTH | Any edge on specified channel |
| 4 | HIGH | Level high on specified channel |
| 5 | LOW | Level low on specified channel |

Hardware triggers use a dedicated PIO state machine (SM 1 on PIO 1). The trigger SM fires IRQ 0 which enables the capture SM (SM 0).

**Response payload:** `[type, channel]`

#### 0xD5 HAT_LA_ARM
Arm the logic analyzer. If a trigger is configured, capture begins when the trigger fires. If no trigger, capture starts immediately.

**Request payload:** Empty.
**Response payload:** `[state:u8]` (1=ARMED)

DMA is pre-configured before arming. The capture PIO program is loaded and started on SM 0.

#### 0xD6 HAT_LA_FORCE
Force-trigger the logic analyzer (bypass trigger condition and start capture immediately).

**Request payload:** Empty.
**Response payload:** `[state:u8]` (2=CAPTURING)

#### 0xD7 HAT_LA_STATUS
Get current capture status.

**Response payload:**
```
0       state           u8      LA state (see table)
1       channels        u8      Configured channels
2       samples_captured u32    Samples captured so far (LE)
6       total_samples   u32     Target depth (LE)
10      actual_rate_hz  u32     Actual sample rate (LE)
```

| State | Name | Description |
|-------|------|-------------|
| 0 | IDLE | Not configured or stopped |
| 1 | ARMED | Waiting for trigger |
| 2 | CAPTURING | Trigger fired, DMA active |
| 3 | DONE | Capture complete, data ready |
| 4 | STREAMING | Continuous double-buffered mode |
| 5 | ERROR | Capture failed |

#### 0xD8 HAT_LA_READ
Read a chunk of captured data. Each response contains up to 28 bytes (limited by HAT UART frame size).

**Request payload:**
```
0       offset          u32     Byte offset into capture buffer (LE)
4       length          u16     Bytes to read (max 28)
```

**Response payload:** Raw capture data bytes (up to 28 bytes).

For large captures, the desktop app uses the RP2040 USB CDC endpoint for bulk transfer instead of this chunked UART path. See Section 7.6 (LA_DONE_EVENT) for the recommended high-throughput flow.

#### 0xD9 HAT_LA_STOP
Stop an active capture or disarm a waiting trigger.

**Request payload:** (empty)
**Response payload:** (empty)

#### 0xEB HAT_LA_LOG_ENABLE
Enable or disable verbose debug logging for the logic analyzer subsystem on the HAT UART.

**Request payload:**
```
0       enable          bool    1=enable, 0=disable
```

**Response payload:** (empty)

#### 0xED HAT_LA_USB_RESET
Force a reset of the HAT's USB subsystem. Useful for clearing hung USB states during streaming.

**Request payload:** (empty)
**Response payload:** (empty)

#### 0xEE HAT_LA_STREAM_START
Start continuous streaming mode (double-buffered DMA to USB).
Capture parameters must be pre-configured via `0xCF HAT_LA_CONFIG`.

**Request payload:** (empty)
**Response payload:** (empty)

#### RLE Data Format

When `rle_enabled=true` in HAT_LA_CONFIG, captured data is compressed using run-length encoding before readout.

**RLE word format (32-bit, little-endian):**
```
Bits [31:28] = 4-bit channel values (MSB-first: CH3, CH2, CH1, CH0)
Bits [27:0]  = 28-bit run length (number of consecutive identical samples)
```

Maximum run length per entry: 268,435,455 (0x0FFFFFFF). Runs exceeding this are split across multiple entries. Typical compression ratio for digital signals: 10–100×.

### 6.14 HUSB238 USB PD (I2C, addr 0x08)

USB Power Delivery sink controller. Read-only status of PD contract and source capabilities.

#### 0xC0 USBPD_GET_STATUS
Get USB PD contract status and source PDOs.

**Response payload:**
```
0       present         bool    HUSB238 found on I2C
1       attached        bool    USB Type-C attached
2       cc_direction    bool    false=CC1, true=CC2
3       pd_response     u8      PD negotiation response code
4       voltage_code    u8      Negotiated voltage enum
5       current_code    u8      Negotiated current enum
6       voltage_v       f32     Negotiated voltage (V)
10      current_a       f32     Negotiated current (A)
14      power_w         f32     Computed power (W)
Per PDO (6x: 5V, 9V, 12V, 15V, 18V, 20V):
+0      detected        bool    PDO available from source
+1      max_current     u8      Max current code
18+12   selected_pdo    u8      Currently selected PDO register
```

#### 0xC1 USBPD_SELECT_PDO
Select a voltage PDO for next negotiation.

**Request payload:**
```
0       voltage         u8      Voltage enum (1=5V, 2=9V, 3=12V, 4=15V, 5=18V, 6=20V)
```

#### 0xC2 USBPD_GO
Trigger PD re-negotiation or other command.

**Request payload:**
```
0       command         u8      0x01=SELECT_PDO, 0x04=GET_SRC_CAP, 0x10=HARD_RESET
```

### 6.14 Waveform Generator

#### 0xD0 START_WAVEGEN
Start waveform generation on a channel. Automatically sets channel function.

**Request payload:**
```
0       channel         u8      Channel (0-3)
1       waveform        u8      0=sine, 1=square, 2=triangle, 3=sawtooth
2       freq_hz         f32     Frequency in Hz (0.01-100)
6       amplitude       f32     Amplitude (V or mA depending on mode)
10      offset          f32     DC offset
14      mode            u8      0=voltage, 1=current
```

**Response:** echo of payload

#### 0xD1 STOP_WAVEGEN
Stop waveform generation. Returns channel to HIGH_IMP.

**Request payload:** (empty)
**Response:** (empty)

### 6.15 MUX Switch Matrix (0x90-0x92)

See existing documentation for 0x90 MUX_SET_ALL, 0x91 MUX_GET_ALL, 0x92 MUX_SET_SWITCH.

### 6.16 Scope API (HTTP Polling)

The Scope API provides a lightweight HTTP endpoint for polling pre-processed
10 ms scope buckets. This complements the binary SCOPE_DATA (0x81) push stream
and is intended for browser-based or WiFi clients that cannot use the BBP link.

#### `GET /api/scope?since=<seq>`

Poll for scope buckets with sequence numbers greater than `since`. The server
returns all buffered buckets newer than the given sequence number (up to the
ring-buffer depth, typically 500 buckets / 5 seconds).

**Query parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `since` | u32 | Yes | Last sequence number the client has seen. Pass `0` on first call to get the latest available window. |

**Response:** JSON array of scope bucket objects.

**Scope bucket format:**

```json
{
  "seq":          <u32>,      // Monotonic bucket sequence number
  "timestamp_ms": <u32>,      // Bucket timestamp (ms since boot)
  "count":        <u16>,      // Number of ADC samples aggregated in this bucket
  "channels": [               // Array of 4 channel objects (index = channel)
    {
      "avg": <f32>,           // Average converted value (V or mA)
      "min": <f32>,           // Minimum value in bucket
      "max": <f32>            // Maximum value in bucket
    },
    ...                       // (4 entries, one per channel)
  ]
}
```

**Binary-equivalent layout** (matches SCOPE_DATA event 0x81):

```
Offset  Field           Type    Description
0       seq             u32     Monotonic bucket sequence number
4       timestamp_ms    u32     Bucket timestamp (ms since boot)
8       count           u16     Number of samples in bucket

Per channel (4x, stride = 12):
+0      avg             f32     Average value (V or mA)
+4      min             f32     Minimum value
+8      max             f32     Maximum value

Total: 10 + (4 x 12) = 58 bytes per bucket
```

**Usage pattern:**

1. Client sends `GET /api/scope?since=0` to bootstrap (receives the latest window).
2. Client records the highest `seq` from the response.
3. Client polls at ~100 ms intervals: `GET /api/scope?since=<last_seq>`.
4. Server returns only new buckets (typically 10 per 100 ms poll).
5. If the client falls behind (gap in `seq`), it should reset with `since=0`.

**Notes:**
- Each bucket covers a 10 ms wall-clock window.
- The `count` field indicates how many raw ADC samples were aggregated; it varies
  with the configured ADC sample rate.
- Channels that are not in an ADC-reading function report `avg = min = max = 0.0`.

### 6.17 WiFi Management

Control and monitor the ESP32 WiFi subsystem (AP + STA dual-mode).

#### 0xE1 WIFI_GET_STATUS
Get current WiFi status including AP and STA information.

**Request payload:** (empty)

**Response payload:**
```
Offset  Field           Type    Description
0       connected       bool    STA connected to external network
+0      sta_ssid_len    u8      Length of STA SSID string
+1      sta_ssid        bytes   STA SSID (up to 32 bytes, not null-terminated)
+0      sta_ip_len      u8      Length of STA IP string
+1      sta_ip          bytes   STA IP address string (up to 16 bytes)
+0      rssi            i32     STA RSSI in dBm (as u32 on wire, interpret as signed)
+0      ap_ssid_len     u8      Length of AP SSID string
+1      ap_ssid         bytes   AP SSID (up to 32 bytes)
+0      ap_ip_len       u8      Length of AP IP string
+1      ap_ip           bytes   AP IP address string (up to 16 bytes)
+0      ap_mac_len      u8      Length of AP MAC string
+1      ap_mac          bytes   AP MAC address string (up to 18 bytes, "XX:XX:XX:XX:XX:XX")
```

All strings are length-prefixed (u8 length + raw bytes, no null terminator).

**Web API equivalent:** `GET /api/wifi` returns JSON:
```json
{
  "connected": true,
  "staSSID": "MyNetwork",
  "staIP": "192.168.1.42",
  "rssi": -55,
  "apSSID": "BugBuster",
  "apIP": "192.168.4.1",
  "apMAC": "AA:BB:CC:DD:EE:FF"
}
```

#### 0xE2 WIFI_CONNECT
Connect to a WiFi network in STA mode. Blocks for up to 10 seconds waiting for connection.

**Request payload:**
```
Offset  Field           Type    Description
0       ssid_len        u8      Length of SSID (1-32)
1       ssid            bytes   Network SSID
+0      pass_len        u8      Length of password (0-64)
+1      pass            bytes   Network password
```

**Response payload:**
```
Offset  Field           Type    Description
0       success         bool    true if connected
```

**Web API equivalent:** `POST /api/wifi/connect` with body `{"ssid":"...","password":"..."}` returns `{"success":true,"ip":"192.168.1.42"}`

Credentials are automatically saved to NVS on successful connect and restored on boot.

#### 0xE4 WIFI_SCAN
Scan for available WiFi networks. Blocks for ~3-5 seconds while the radio scans all channels.

**Request payload:** (empty)

**Response payload:**
```
Offset  Field           Type    Description
0       count           u8      Number of networks found (max 20)
```

Followed by `count` entries, each:
```
Offset  Field           Type    Description
+0      ssid_len        u8      Length of SSID string (0-32)
+1      ssid            bytes   Network SSID
+0      rssi            i8      Signal strength in dBm (signed)
+1      auth            u8      Auth mode (0=OPEN, 3=WPA2, 4=WPA/WPA2, 6=WPA3)
```

Results are sorted by signal strength (strongest first), deduplicated by SSID.

**Web API equivalent:** `GET /api/wifi/scan` returns JSON:
```json
{
  "networks": [
    {"ssid": "MyNetwork", "rssi": -45, "auth": 3},
    {"ssid": "Neighbor", "rssi": -72, "auth": 4}
  ]
}
```

### 6.18 Level Shifter Control

#### 0xE0 SET_LSHIFT_OE
Enable or disable the TXS0108E level shifter output (controls signal routing between ESP32 GPIOs and the MUX matrix).

**Request payload:**
```
Offset  Field           Type    Description
0       on              bool    true = enable level shifter, false = disable (high-Z)
```

**Response payload:** (empty)

**Web API equivalent:** `POST /api/lshift/oe` with body `{"on": true}`

### 6.19 SPI Clock Control

#### 0xE3 SET_SPI_CLOCK
Change the AD74416H SPI clock speed at runtime. Useful for testing signal integrity at different bus speeds. The AD74416H supports up to 20 MHz.

**Request payload:**
```
Offset  Field           Type    Description
0       clock_hz        u32     SPI clock speed in Hz (100,000 - 20,000,000)
```

**Response payload:** (empty on success)

Pauses ADC polling, removes and re-adds the SPI device with the new clock, then verifies by writing/reading the SCRATCH register. Returns `ERR_SPI_FAIL` if verification fails.

**CLI equivalent:** `spiclock 10000000`

### 6.20 Firmware Management (HTTP only)

These endpoints are available only via the HTTP REST API (not BBP).

#### GET /api/device/version
Returns firmware version, build metadata, and OTA partition information.

**Response:**
```json
{
  "version": "Mar 30 2026",
  "date": "Mar 30 2026",
  "time": "01:30:00",
  "idfVersion": "v5.4-dirty",
  "fwMajor": 1,
  "fwMinor": 0,
  "fwPatch": 0,
  "protoVersion": 4,
  "partition": "app0",
  "partitionSize": 1703936,
  "nextPartition": "app1",
  "nextPartitionSize": 1703936
}
```

#### POST /api/ota/upload
Upload a firmware binary for OTA update. The request body is the raw `firmware.bin` file (application/octet-stream). Maximum size: 2 MB.

The device writes to the inactive OTA partition, validates the image, sets it as the boot partition, sends a success response, then reboots after 1 second.

**Request:** Binary body (firmware.bin, max 2 MB)

**Response (success):**
```json
{
  "success": true,
  "bytesWritten": 1056576,
  "partition": "app1"
}
```

**Response (failure):**
```json
{"error": "OTA write failed"}
```

If the new firmware fails to boot (crashes before `esp_ota_mark_app_valid_cancel_rollback()`), the bootloader automatically reverts to the previous partition.

---

## 7. Streaming Protocol

The primary advantage of BBP over HTTP: continuous, push-based data delivery
with no polling overhead.

### 7.1 ADC Stream

#### 0x60 START_ADC_STREAM
Begin streaming raw ADC data from selected channels.

**Request payload:**
```
0       channel_mask    u8      Bitmask of channels to stream (bit0=chA ... bit3=chD)
1       divider         u8      Sample rate divider (1 = every sample, N = every Nth)
```

The divider allows the host to reduce throughput when full-rate data is not
needed. A divider of 0 is treated as 1 (no division).

**Response payload:**
```
0       channel_mask    u8      Confirmed active channels
1       divider         u8      Confirmed divider
2       sample_rate     u16     Effective per-channel rate in SPS (after divider)
```

Once started, the device pushes `ADC_DATA` events (0x80) continuously until
stopped.

#### 0x61 STOP_ADC_STREAM
Stop ADC data streaming.

**Request payload:** (empty)
**Response payload:** (empty)

#### 0x80 ADC_DATA (Event)
Unsolicited ADC data batch pushed by the device.

**Event payload:**
```
Offset  Field               Type    Description
0       channel_mask        u8      Active channel bitmask
1       base_timestamp_us   u32     Timestamp of first sample (us, wrapping)
5       sample_count        u16     Number of samples in this batch
7       samples             ...     Packed sample data

Each sample (per active channel, in mask order):
  +0    raw_code            u24     24-bit ADC code (LE)
```

**Sample size per batch entry** = 3 bytes x (number of set bits in channel_mask)

Example: 4 channels active, 50 samples per batch:
- Payload = 7 + (50 x 12) = 607 bytes
- At 9.6 kSPS: ~192 batches/sec (50 samples each) -> ~117 KB/s

**Batching strategy (firmware):**
- Collect samples into a buffer
- Flush when buffer reaches ~50 samples OR 5 ms elapsed (whichever comes first)
- This balances latency (~5 ms worst case) against framing overhead

### 7.2 Scope Stream

#### 0x62 START_SCOPE_STREAM
Begin streaming pre-processed scope data (10 ms buckets with min/max/avg,
equivalent to `GET /api/scope` but push-based).

**Request payload:** (empty)

**Response payload:** (empty)

#### 0x63 STOP_SCOPE_STREAM
Stop scope data streaming.

**Request payload:** (empty)
**Response payload:** (empty)

#### 0x81 SCOPE_DATA (Event)
Unsolicited scope bucket pushed by the device every 10 ms.

**Event payload:**
```
Offset  Field           Type    Description
0       seq             u32     Monotonic bucket sequence number
4       timestamp_ms    u32     Bucket timestamp (ms since boot)
8       count           u16     Number of samples in bucket

Per channel (4x, stride = 12):
+0      avg             f32     Average value (V or mA)
+4      min             f32     Minimum value
+8      max             f32     Maximum value

Total: 10 + (4 x 12) = 58 bytes per event
```

### 7.3 Alert Events

#### 0x82 ALERT_EVENT (Event)
Pushed when new alert bits are set in ALERT_STATUS or SUPPLY_ALERT_STATUS
(compared to previous poll cycle). Sent from fault monitor task (~200ms poll).

**Event payload:**
```
0       alert_status        u16     Current global ALERT_STATUS
2       supply_alert_status u16     Current SUPPLY_ALERT_STATUS
4       ch_alert_a          u8      Channel A alert status (low byte)
5       ch_alert_b          u8      Channel B alert status (low byte)
6       ch_alert_c          u8      Channel C alert status (low byte)
7       ch_alert_d          u8      Channel D alert status (low byte)
```

### 7.4 DIN Event

#### 0x83 DIN_EVENT (Event)
Pushed on digital input state change (edge detection).

**Event payload:**
```
0       channel         u8      Channel (0-3)
1       state           bool    New comparator output state
2       counter         u32     Updated event counter
```

### 7.5 PCA Fault Event

#### 0x84 PCA_FAULT_EVENT (Event)
Pushed when a PCA9535 input change is detected (e-fuse fault, power-good change).
If auto-disable is enabled (default), the faulted e-fuse is automatically disabled
before this event is sent.

**Event payload:**
```
0       type            u8      0=efuse_trip, 1=efuse_clear, 2=pg_lost, 3=pg_restored
1       channel         u8      E-fuse index (0-3) or PG source (0=logic, 1=vadj1, 2=vadj2)
2       timestamp_ms    u32     Milliseconds since boot (little-endian)
```

### 7.6 LA Done Event

#### 0x85 LA_DONE_EVENT (Event)
Pushed when logic analyzer capture completes on the RP2040 HAT. The RP2040
sends an unsolicited UART frame which the ESP32 forwards as this BBP event.

**Event payload:**
```
0       state           u8      LA state (3 = DONE)
1       channels        u8      Number of channels captured
2       samples_captured u32    Samples captured (LE)
6       total_samples   u32     Target depth (LE)
10      actual_rate_hz  u32     Actual sample rate achieved (LE)
```

The host should respond by reading the captured data via the RP2040 USB bulk
endpoint (interface 3, EP 0x87 IN).

---

## 8. Error Handling

### 8.1 Error Response Format

When a command fails, the device sends an ERR message (MSG_TYPE = 0x04) with
the same SEQ as the failed command.

**Error payload:**
```
0       error_code      u8      Error code (see below)
1       cmd_id          u8      The CMD_ID that failed
2..N    message         u8[]    Optional ASCII error description (not null-terminated)
```

### 8.2 Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0x01 | ERR_INVALID_CMD | Unknown CMD_ID |
| 0x02 | ERR_INVALID_CHANNEL | Channel index out of range |
| 0x03 | ERR_INVALID_PARAM | Parameter value out of valid range |
| 0x04 | ERR_SPI_FAIL | SPI communication error with AD74416H |
| 0x05 | ERR_QUEUE_FULL | Command queue is full (try again) |
| 0x06 | ERR_BUSY | Resource busy (mutex timeout) |
| 0x07 | ERR_INVALID_STATE | Operation not valid in current device state |
| 0x08 | ERR_CRC_FAIL | CRC mismatch (informational, frame was discarded) |
| 0x09 | ERR_FRAME_TOO_LARGE | Decoded frame exceeds max size |
| 0x0A | ERR_STREAM_ACTIVE | Stream already active (stop first) |

### 8.3 Host Timeout Strategy

- **Command timeout:** 500 ms (commands are queued, may wait for processor)
- **Handshake timeout:** 2000 ms
- **Ping timeout:** 200 ms

On timeout, the host should:
1. Retry once with the same SEQ
2. If retry fails, send PING to check connectivity
3. If PING fails, assume disconnection and close the port

---

## 9. Flow Control

### 9.1 Device -> Host (Backpressure)

USB CDC has built-in flow control via the USB protocol itself. If the host
does not read fast enough, `tud_cdc_write()` will block or return short.

The firmware handles this by:
- Dropping the oldest ADC stream batch if the CDC TX buffer is full
- Incrementing a dropped-batch counter (reported in the next ADC_DATA header)
- Never blocking the ADC poll task

If the host detects gaps in the `base_timestamp_us` field, it knows samples
were dropped due to backpressure.

### 9.2 Host -> Device

The device processes commands sequentially via the command queue. If the queue
is full, the device returns ERR_QUEUE_FULL. The host should wait and retry.

Commands are lightweight (no large uploads), so host->device flow control is
not a practical concern.

---

## 10. Implementation Notes

### 10.1 Firmware Integration Points

**Files to modify:**
- `cli.cpp` -- Add magic byte detection in the CLI input loop
- `serial_io.h/cpp` -- Add binary mode flag and raw read/write functions
- New file: `bbp.h/cpp` -- Protocol handler (COBS codec, message dispatch, stream management)
- `tasks.cpp` -- Hook ADC poll to push data to BBP stream buffer when active

**Task architecture:**
- BBP runs in the main loop task (Core 0) alongside CLI, since only one is
  active at a time
- ADC data is pushed from the ADC poll task (Core 1) into a lock-free ring
  buffer; the BBP task drains and sends it
- Commands received via BBP are enqueued using the existing `sendCommand()` path

### 10.2 Host Application Notes

**Opening the port:**
- On all platforms, CDC #0 appears as a standard serial/COM port:
  - macOS: `/dev/cu.usbmodemXXXX`
  - Linux: `/dev/ttyACM0`
  - Windows: `COMx` (via usbser.sys, no driver install needed)
- Open at any baud rate (CDC ignores baud rate, it's USB-native)
- Set DTR high (signals connection to device)
- Send the handshake magic bytes
- Wait for 8-byte handshake response
- Begin COBS-framed communication

**Recommended host libraries:**
- Cross-platform serial: `serialport` (Rust), `pyserial` (Python),
  `SerialPort` (Node.js), `libserialport` (C/C++)
- All of these handle CDC/ACM ports transparently

### 10.3 Identifying BugBuster Ports

The TinyUSB descriptor exposes:
- **VID/PID:** As configured in `tusb_config.h` (Espressif defaults or custom)
- **Manufacturer string:** Should be set to `"BugBuster"`
- **Product string:** Should be set to `"BugBuster Universal Debugger"`
- **Serial number:** Unique per device (ESP32 MAC-based)

The host app should enumerate serial ports and match on VID/PID or
manufacturer/product strings to auto-detect the device. CDC #0 vs #1 is
distinguished by the interface number in the USB descriptor (interface 0 =
CLI/BBP, interface 2 = UART bridge).

---

## 11. Example Flows

### 11.1 Connection and Status Query

```
Host                                    Device
  │                                       │
  │──── 0xBB 0x42 0x55 0x47 ────────────>│  (magic bytes)
  │                                       │
  │<──── 0xBB 0x42 0x55 0x47 0x04 ───────│  (ACK, proto v4, fw version)
  │         0x01 0x06 0x00                │
  │                                       │  (device enters binary mode)
  │                                       │
  │──── [COBS: CMD seq=1 GET_STATUS] ───>│
  │                                       │
  │<──── [COBS: RSP seq=1 status...] ────│
  │                                       │
```

### 11.2 Configure Channel and Start ADC Stream

```
Host                                    Device
  │                                       │
  │── CMD seq=2 SET_CHANNEL_FUNC ───────>│  ch=0, func=3 (VIN)
  │   [0x01][0x02,0x00][0x10][0x00,0x03] │
  │                                       │
  │<── RSP seq=2 ────────────────────────│  OK, ch=0, func=3
  │                                       │
  │── CMD seq=3 SET_ADC_CONFIG ─────────>│  ch=0, mux=0, range=0, rate=13
  │                                       │
  │<── RSP seq=3 ────────────────────────│  OK
  │                                       │
  │── CMD seq=4 START_ADC_STREAM ───────>│  mask=0x01, divider=1
  │                                       │
  │<── RSP seq=4 ────────────────────────│  mask=0x01, div=1, rate=9600
  │                                       │
  │<── EVT ADC_DATA (batch 1) ──────────│  50 samples, ch0 only
  │<── EVT ADC_DATA (batch 2) ──────────│  50 samples
  │<── EVT ADC_DATA (batch 3) ──────────│  50 samples
  │    ... continuous ...                 │
  │                                       │
  │── CMD seq=5 STOP_ADC_STREAM ────────>│
  │                                       │
  │<── RSP seq=5 ────────────────────────│  OK, streaming stopped
  │                                       │
```

### 11.3 Graceful Disconnect

```
Host                                    Device
  │                                       │
  │── CMD seq=99 DISCONNECT ────────────>│
  │                                       │
  │<── RSP seq=99 ──────────────────────│  OK
  │                                       │  (device reverts to CLI)
  │<──── "\r\n[CLI Ready]\r\n" ─────────│  (text mode)
  │                                       │
```

---

## 12. Protocol Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-03-27 | Initial specification |
| 1.1 | 2026-03-28 | Added I2C device commands: DS4424 IDAC (0xA0-A3), PCA9535 GPIO expander (0xB0-B2), HUSB238 USB-PD (0xC0-C2), Waveform generator (0xD0-D1) |
| 1.2 | 2026-03-28 | Added UI-driven calibration commands: IDAC_CAL_ADD_POINT (0xA4), IDAC_CAL_CLEAR (0xA5), IDAC_CAL_SAVE (0xA6) |
| 1.3 | 2026-03-29 | GET_STATUS now includes diagnostic slots; IDAC calibration commands (0xA4-0xA6) fully documented; added Section 6.16 Scope API (HTTP polling endpoint) |
| 1.4 | 2026-03-28 | Added WiFi management commands: WIFI_GET_STATUS (0xE1), WIFI_CONNECT (0xE2); Section 6.17; added SET_LSHIFT_OE (0xE0) to Appendix A |
| 1.5 | 2026-03-29 | Added WIFI_SCAN (0xE4); WiFi credentials persist in NVS |
| 1.6 | 2026-03-30 | Added SET_LSHIFT_OE (0xE0), SET_SPI_CLOCK (0xE3) sections; OTA update endpoint (POST /api/ota/upload); device version endpoint (GET /api/device/version); A/B OTA partition table with rollback |
| 1.7 | 2026-03-31 | Added SET_RTD_CONFIG (0x1D) command for RTD excitation current selection (125/250 µA); GET_STATUS per-channel payload extended by 2 bytes (rtd_excitation_ua u16, stride 26→28, total 147→155 bytes); adc_value for RES_MEAS now returned in Ohms (R = V_adc / I_exc) |

---

## Appendix A: Quick Reference -- Command ID Table

| CMD_ID | Name | Direction | Payload (Req) | Web API Equivalent |
|--------|------|-----------|---------------|-------------------|
| 0x01 | GET_STATUS | H->D | -- | `GET /api/status` |
| 0x02 | GET_DEVICE_INFO | H->D | -- | `GET /api/device/info` |
| 0x03 | GET_FAULTS | H->D | -- | `GET /api/faults` |
| 0x04 | GET_DIAGNOSTICS | H->D | -- | `GET /api/diagnostics` |
| 0x10 | SET_CHANNEL_FUNC | H->D | ch, func | `POST /api/channel/X/function` |
| 0x11 | SET_DAC_CODE | H->D | ch, code | `POST /api/channel/X/dac` |
| 0x12 | SET_DAC_VOLTAGE | H->D | ch, V, bipolar | `POST /api/channel/X/dac` |
| 0x13 | SET_DAC_CURRENT | H->D | ch, mA | `POST /api/channel/X/dac` |
| 0x14 | SET_ADC_CONFIG | H->D | ch, mux, rng, rate | `POST /api/channel/X/adc/config` |
| 0x15 | SET_DIN_CONFIG | H->D | ch, thresh, ... | `POST /api/channel/X/din/config` |
| 0x16 | SET_DO_CONFIG | H->D | ch, mode, ... | `POST /api/channel/X/do/config` |
| 0x17 | SET_DO_STATE | H->D | ch, on | `POST /api/channel/X/do/set` |
| 0x18 | SET_VOUT_RANGE | H->D | ch, bipolar | `POST /api/channel/X/vout/range` |
| 0x19 | SET_CURRENT_LIMIT | H->D | ch, limit | `POST /api/channel/X/ilimit` |
| 0x1A | SET_AVDD_SELECT | H->D | ch, sel | `POST /api/channel/X/avdd` |
| 0x1B | GET_ADC_VALUE | H->D | ch | `GET /api/channel/X/adc` |
| 0x1C | GET_DAC_READBACK | H->D | ch | `GET /api/channel/X/dac/readback` |
| 0x1D | SET_RTD_CONFIG | H->D | ch, current | `POST /api/channel/X/rtd/config` |
| 0x20 | CLEAR_ALL_ALERTS | H->D | -- | `POST /api/faults/clear` |
| 0x21 | CLEAR_CHANNEL_ALERT | H->D | ch | `POST /api/faults/clear/X` |
| 0x22 | SET_ALERT_MASK | H->D | masks | `POST /api/faults/mask` |
| 0x23 | SET_CH_ALERT_MASK | H->D | ch, mask | `POST /api/faults/mask/X` |
| 0x30 | SET_DIAG_CONFIG | H->D | slot, src | `POST /api/diagnostics/config` |
| 0x40 | GET_GPIO_STATUS | H->D | -- | `GET /api/gpio` |
| 0x41 | SET_GPIO_CONFIG | H->D | gpio, mode, pd | `POST /api/gpio/X/config` |
| 0x42 | SET_GPIO_VALUE | H->D | gpio, val | `POST /api/gpio/X/set` |
| 0x50 | GET_UART_CONFIG | H->D | -- | `GET /api/uart/config` |
| 0x51 | SET_UART_CONFIG | H->D | bridge cfg | `POST /api/uart/X/config` |
| 0x52 | GET_UART_PINS | H->D | -- | `GET /api/uart/pins` |
| 0x60 | START_ADC_STREAM | H->D | mask, div | (new, no web equiv) |
| 0x61 | STOP_ADC_STREAM | H->D | -- | (new) |
| 0x62 | START_SCOPE_STREAM | H->D | -- | `GET /api/scope` (push) |
| 0x63 | STOP_SCOPE_STREAM | H->D | -- | (new) |
| 0x70 | DEVICE_RESET | H->D | -- | `POST /api/device/reset` |
| 0x71 | REGISTER_READ | H->D | addr | CLI `rreg` |
| 0x72 | REGISTER_WRITE | H->D | addr, val | CLI `wreg` |
| 0x73 | SET_WATCHDOG | H->D | enable, timeout | (new, USB only) |
| 0x90 | MUX_SET_ALL | H->D | 4 states | (new) |
| 0x91 | MUX_GET_ALL | H->D | -- | (new) |
| 0x92 | MUX_SET_SWITCH | H->D | dev, sw, state | (new) |
| 0xA0 | IDAC_GET_STATUS | H->D | -- | `GET /api/idac` |
| 0xA1 | IDAC_SET_CODE | H->D | ch, code | `POST /api/idac/code` |
| 0xA2 | IDAC_SET_VOLTAGE | H->D | ch, voltage | `POST /api/idac/voltage` |
| 0xA3 | IDAC_CALIBRATE | H->D | ch, step, settle | (reserved) |
| 0xA4 | IDAC_CAL_ADD_POINT | H->D | ch, code, voltage | (new) |
| 0xA5 | IDAC_CAL_CLEAR | H->D | ch | (new) |
| 0xA6 | IDAC_CAL_SAVE | H->D | -- | (new) |
| 0xB0 | PCA_GET_STATUS | H->D | -- | `GET /api/ioexp` |
| 0xB1 | PCA_SET_CONTROL | H->D | ctrl, on | `POST /api/ioexp/control` |
| 0xB2 | PCA_SET_PORT | H->D | port, val | (new) |
| 0xB3 | PCA_SET_FAULT_CFG | H->D | auto_dis, log | `POST /api/ioexp/fault_config` |
| 0xB4 | PCA_GET_FAULT_LOG | H->D | -- | `GET /api/ioexp/faults` |
| 0xC5 | HAT_GET_STATUS | H->D | -- | `GET /api/hat` |
| 0xC6 | HAT_SET_PIN | H->D | pin, func | `POST /api/hat/config` |
| 0xC7 | HAT_SET_ALL_PINS | H->D | 4× func | `POST /api/hat/config` |
| 0xC8 | HAT_RESET | H->D | -- | `POST /api/hat/reset` |
| 0xC9 | HAT_DETECT | H->D | -- | `POST /api/hat/detect` |
| 0xCA | HAT_SET_POWER | H->D | conn, enable | `POST /api/hat/power` |
| 0xCB | HAT_GET_POWER | H->D | -- | `GET /api/hat/power` |
| 0xCC | HAT_SET_IO_VOLTAGE | H->D | voltage_mv | `POST /api/hat/io_voltage` |
| 0xCD | HAT_SETUP_SWD | H->D | voltage_mv, conn | `POST /api/hat/setup_swd` |
| 0xCF | HAT_LA_CONFIG | H->D | ch, rate, depth | LA: configure capture |
| 0xD5 | HAT_LA_ARM | H->D | -- | LA: arm trigger |
| 0xD6 | HAT_LA_FORCE | H->D | -- | LA: force trigger |
| 0xD7 | HAT_LA_STATUS | H->D | -- | LA: get capture status |
| 0xD8 | HAT_LA_READ | H->D | offset, len | LA: read data chunk |
| 0xD9 | HAT_LA_STOP | H->D | -- | LA: stop capture |
| 0xDA | HAT_LA_TRIGGER | H->D | type, channel | LA: set trigger |
| 0xC0 | USBPD_GET_STATUS | H->D | -- | `GET /api/usbpd` |
| 0xC1 | USBPD_SELECT_PDO | H->D | voltage | `POST /api/usbpd/select` |
| 0xC2 | USBPD_GO | H->D | command | (new) |
| 0xD0 | START_WAVEGEN | H->D | ch,wf,f,a,o,m | (new) |
| 0xD1 | STOP_WAVEGEN | H->D | -- | (new) |
| 0xE0 | SET_LSHIFT_OE | H->D | on | `POST /api/lshift/oe` |
| 0xE1 | WIFI_GET_STATUS | H->D | -- | `GET /api/wifi` |
| 0xE2 | WIFI_CONNECT | H->D | ssid, pass | `POST /api/wifi/connect` |
| 0xE3 | SET_SPI_CLOCK | H->D | hz (u32) | (new) |
| 0xE4 | WIFI_SCAN | H->D | -- | `GET /api/wifi/scan` |
| 0xFE | PING | H->D | token | (new) |
| 0xFF | DISCONNECT | H->D | -- | (new) |

## Appendix B: Event ID Table

| EVT_ID | Name | Direction | Trigger |
|--------|------|-----------|---------|
| 0x80 | ADC_DATA | D->H | ADC stream batch ready |
| 0x81 | SCOPE_DATA | D->H | 10 ms scope bucket complete |
| 0x82 | ALERT_EVENT | D->H | Alert condition detected |
| 0x83 | DIN_EVENT | D->H | Digital input state change |
| 0x84 | PCA_FAULT_EVENT | D->H | E-fuse trip, PG change |
| 0x85 | LA_DONE_EVENT | D->H | Logic analyzer capture complete |

## Appendix C: Wire Format Example

**Example: SET_DAC_VOLTAGE on channel 0 to 5.0V unipolar, SEQ=42**

Raw message (before COBS):
```
Byte  Hex   Field
0     01    MSG_TYPE = CMD
1     2A    SEQ low = 42
2     00    SEQ high = 0
3     12    CMD_ID = SET_DAC_VOLTAGE
4     00    channel = 0
5     00    voltage = 5.0f (IEEE 754: 0x40A00000)
6     00
7     A0
8     40
9     00    bipolar = false
10    XX    CRC-16 low
11    XX    CRC-16 high
```

After COBS encoding + 0x00 delimiter: ~14 bytes on the wire.
