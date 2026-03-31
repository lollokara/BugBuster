# BugBuster — Engineering Knowledge Base

A living document capturing non-obvious behaviours, bugs found, architectural decisions,
and things that are easy to get wrong. Update whenever a bug is understood or a design
decision is made.

---

## Table of Contents
1. [AD74416H Register Map](#1-ad74416h-register-map)
2. [Channel Function → ADC Conversion Rules](#2-channel-function--adc-conversion-rules)
3. [USB CDC / BBP Protocol Architecture](#3-usb-cdc--bbp-protocol-architecture)
4. [Known Bugs Fixed](#4-known-bugs-fixed)

---

## 1. AD74416H Register Map

### Per-channel register stride
Each channel block is **12 registers (0x0C)** wide.
`REG_FOR_CH(n) = BASE + n * 0x0C`

| Register | Base | Notes |
|---|---|---|
| CH_FUNC_SETUP | 0x01 | Function code [3:0] |
| ADC_CONFIG | 0x02 | CONV_MUX[2:0], CONV_RANGE[6:4], CONV_RATE[11:8] |
| DIN_CONFIG0 | 0x03 | Debounce, comparator, sink |
| DIN_CONFIG1 | 0x04 | Threshold, OC/SC detect |
| OUTPUT_CONFIG | 0x05 | Slew, VOUT range, AVDD select |
| RTD_CONFIG | 0x06 | RTD excitation, mode |
| FET_LKG_COMP | 0x07 | FET leakage compensation |
| DO_EXT_CONFIG | 0x08 | Digital output timing |
| I_BURNOUT_CONFIG | 0x09 | Burnout current |
| DAC_CODE | 0x0A | 16-bit DAC code (write) |
| *(reserved)* | 0x0B | — |
| DAC_ACTIVE | 0x0C | Active DAC code (read-only) |

### Global registers

| Register | Address | Notes |
|---|---|---|
| NOP | 0x00 | |
| GPIO_CONFIG(n) | 0x32 + n | 6 GPIOs (A–F), n = 0..5 |
| PWR_OPTIM_CONFIG | 0x38 | REF_EN = bit 13 |
| ADC_CONV_CTRL | 0x39 | CONV_SEQ[9:8], DIAG_EN[7:4], CH_EN[3:0] |
| DIAG_ASSIGN | 0x3A | 4-bit source per slot, packed [15:0] |
| WDT_CONFIG | 0x3B | |
| DIN_COMP_OUT | 0x3E | One bit per channel [3:0] |
| **ALERT_STATUS** | **0x3F** | See bit map below |
| LIVE_STATUS | 0x40 | See bit map below |
| ADC_RESULT_UPR(ch) | 0x41 + ch×2 | Upper 8 bits + metadata |
| ADC_RESULT(ch) | 0x42 + ch×2 | Lower 16 bits |
| ADC_DIAG_RESULT(n) | 0x49 + n | Diagnostic slot result |
| LAST_ADC_RESULT_UPR | 0x4D | |
| LAST_ADC_RESULT | 0x4E | |
| DIN_COUNTER_UPR(ch) | 0x4F + ch×2 | Read first to latch lower |
| DIN_COUNTER(ch) | 0x50 + ch×2 | Lower 16 bits |
| SUPPLY_ALERT_STATUS | 0x57 | See bit map below |
| CHANNEL_ALERT_STATUS(ch) | 0x58 + ch | See bit map below |
| ALERT_MASK | 0x5C | |
| SUPPLY_ALERT_MASK | 0x5D | |
| CHANNEL_ALERT_MASK(ch) | 0x5E + ch | |
| READ_SELECT | 0x6E | |
| BURST_READ_SEL | 0x6F | |
| THERM_RST | 0x73 | Bit 0 = enable |
| CMD_KEY | 0x74 | Reset seq: 0x15FA then 0xAF51 |
| BROADCAST_CMD_KEY | 0x75 | |
| SCRATCH | 0x76 | |
| GENERIC_ID | 0x7A | |
| SILICON_REV | 0x7B | |
| SILICON_ID0 | 0x7D | |
| SILICON_ID1 | 0x7E | |
| HART_ALERT_STATUS(ch) | 0x80 + ch×16 | |
| HART_GPIO_IF_CONFIG | 0xC0 | |
| HART_GPIO_MON_CONFIG(n) | 0xC1 + n | |

### ALERT_STATUS (0x3F) bit map

| Bit | Name | Notes |
|---|---|---|
| 0 | RESET_OCCURRED | Device reset |
| 1 | *(reserved)* | |
| 2 | SUPPLY_ERR | |
| 3 | SPI_ERR | |
| 4 | TEMP_ALERT | |
| 5 | ADC_ERR | |
| 6–7 | *(reserved)* | |
| 8 | CH_A_ALERT | |
| 9 | CH_B_ALERT | |
| 10 | CH_C_ALERT | |
| 11 | CH_D_ALERT | |
| 12 | HART_ALERT_A | |
| 13 | HART_ALERT_B | |
| 14 | HART_ALERT_C | |
| 15 | HART_ALERT_D | |

### SUPPLY_ALERT_STATUS (0x57) bit map

| Bit | Name |
|---|---|
| 0 | CAL_MEM_ERR |
| 1 | AVSS_ERR |
| 2 | DVCC_ERR |
| 3 | AVCC_ERR |
| 4 | DO_VDD_ERR |
| 5 | AVDD_LO_ERR |
| 6 | AVDD_HI_ERR |

### CHANNEL_ALERT_STATUS (0x58 + ch) bit map

| Bit | Name |
|---|---|
| 0 | DIN_SC |
| 1 | DIN_OC |
| 2 | DO_SC |
| 3 | DO_TIMEOUT |
| 4 | ANALOG_IO_SC |
| 5 | ANALOG_IO_OC |
| 6 | VIOUT_SHUTDOWN |

### LIVE_STATUS (0x40) bit map

| Bit | Name |
|---|---|
| 0 | SUPPLY_STATUS |
| 1 | ADC_BUSY |
| 2 | ADC_DATA_RDY |
| 3 | TEMP_ALERT |
| 4–7 | DIN_STATUS_A/B/C/D |
| 8–11 | DO_STATUS_A/B/C/D |
| 12–15 | ANALOG_IO_STATUS_A/B/C/D |

---

## 2. Channel Function → ADC Conversion Rules

### Which functions participate in ADC_CONV_CTRL

**⚠️ Do NOT include these in the ADC conversion channel mask:**

| Function | Code | Reason |
|---|---|---|
| CH_FUNC_HIGH_IMP | 0 | Idle/safe state, no measurement |
| CH_FUNC_DIN_LOGIC | 8 | Uses comparator path only. Hardware auto-sets invalid CONV_MUX for ADC → triggers ADC_ERR |
| CH_FUNC_DIN_LOOP | 9 | Same as DIN_LOGIC |

**All other active functions (1–7, 10–12) use the ADC and should be included.**

Digital input state is read from `DIN_COMP_OUT` (0x3E) and `DIN_COUNTER` (0x50+ch×2),
not from ADC conversion.

### IIN mode CONV_RANGE override

**⚠️ Hardware bug / silicon behaviour:**

When CH_FUNC_SETUP is written to `IIN_EXT_PWR` (4), `IIN_LOOP_PWR` (5),
`IIN_EXT_PWR_HART` (11), or `IIN_LOOP_PWR_HART` (12), the hardware auto-sets
`CONV_RANGE = 3` (negative-only, −312.5 mV to 0 V).

This is **invalid** for current input: the sense voltage across RSENSE (12 Ω) is
always positive (0–240 mV for 4–20 mA). Starting ADC conversion with CONV_RANGE=3
immediately triggers `ADC_ERR`.

**Fix (tasks.cpp, CMD_SET_CHANNEL_FUNC handler):**
After reading back the hardware-set `hwRange`, override it:
```c
if (IIN mode) hwRange = ADC_RNG_NEG0_3125_0_3125V;  // ±312.5 mV, range 2
```

### ADC_CONFIG field positions

```
[15:12] reserved
[11:8]  CONV_RATE   (4 bits)
[7]     reserved
[6:4]   CONV_RANGE  (3 bits)
[3]     reserved
[2:0]   CONV_MUX    (3 bits)
```

### CONV_MUX values

| Code | Name | Measures |
|---|---|---|
| 0 | LF_TO_AGND | LF pin vs AGND |
| 1 | HF_TO_LF | HF vs LF (differential) |
| 2 | VSENSEN_TO_AGND | VSENSE− vs AGND (sense resistor) |
| 3 | LF_TO_VSENSEN | 4-wire sense |
| 4 | AGND_TO_AGND | Self-test / offset |

### CONV_RANGE values

| Code | Range |
|---|---|
| 0 | 0 V to +12 V |
| 1 | −12 V to +12 V |
| 2 | −312.5 mV to +312.5 mV |
| 3 | −312.5 mV to 0 V (**negative-only — avoid for IIN**) |
| 4 | 0 V to +312.5 mV |
| 5 | 0 V to +625 mV |
| 6 | −104 mV to +104 mV |
| 7 | −2.5 V to +2.5 V |

---

## 3. USB CDC / BBP Protocol Architecture

### Physical channels
The ESP32-S3 exposes a TinyUSB composite device.
- **CDC #0**: BBP binary protocol AND CLI text (both share the same channel)
- **CDC #1+**: UART bridge forwarding

### ⚠️ Shared channel hazard
CDC #0 carries both binary BBP frames and CLI/debug text. If ASCII text is written
to CDC #0 while the desktop is in binary mode, the desktop `FrameAccumulator` will
try to COBS-decode it, CRC will fail, and the pending command will time out.

After 3 consecutive poll timeouts the desktop marks the device as disconnected.

**Rules:**
- `serial_printf()` / `serial_print()` routes to CDC #0 via `usb_cdc_cli_write()`.
  **Never call these while `bbpIsActive()` returns true.**
- `bbpExitBinaryMode()` must **not** write any text to CDC #0. The `[CLI Ready]`
  message was removed for this reason.
- The firmware heartbeat in `main.cpp` is already guarded with `!bbpIsActive()`.

### BBP frame format

```
[COBS-encoded content][0x00 delimiter]
```

Pre-COBS raw message layout:
```
[msg_type: 1 B][seq: 2 B LE][cmd_id: 1 B][payload: 0–N B][CRC16-CCITT: 2 B LE]
```

- **No per-frame sync/magic byte** — only the initial handshake uses the 4-byte magic `BB 42 55 47`.
- **COBS** removes all 0x00 bytes from the payload; 0x00 is used exclusively as frame delimiter.
- **CRC-16/CCITT** (poly 0x1021, init 0xFFFF) covers everything except the CRC itself.

### BBP idle timeout
The firmware exits binary mode if no frame is received for `BBP_IDLE_TIMEOUT_MS`.
This is the most common cause of unexpected disconnections on long-idle sessions.

---

## 4. Known Bugs Fixed

### [2026-03-31] Desktop register bit maps wrong (faults.rs, diag.rs)

**Affected registers:** ALERT_STATUS (0x3F), SUPPLY_ALERT_STATUS (0x57),
CHANNEL_ALERT_STATUS (0x58+ch)

All three bit tables in the desktop app were scrambled — reserved bits were labelled,
real bits were missing or had wrong names/positions.

**Fixed:** `src/tabs/faults.rs` and `src/tabs/diag.rs` — `GLOBAL_ALERTS`,
`SUPPLY_ALERTS`, `ALERT_BITS`, `SUPPLY_BITS`, `CH_ALERTS` arrays corrected.

---

### [2026-03-31] DIN modes trigger ADC_ERR (tasks.cpp)

**Symptom:** Setting a channel to `DIN_LOGIC` (8) or `DIN_LOOP` (9) causes
`ADC_ERR` in ALERT_STATUS.

**Root cause:** The hardware sets an invalid `CONV_MUX` for DIN functions when
CH_FUNC_SETUP is written. The firmware was including DIN channels in the
`ADC_CONV_CTRL` conversion mask, causing the ADC to attempt an invalid conversion.

**Fixed:** `tasks.cpp` — three locations:
1. Polling task: skip `readAdcResult()` for DIN channels.
2. `CMD_SET_CHANNEL_FUNC` chMask build: exclude DIN functions.
3. `CMD_SET_ADC_CONFIG` chMask build: exclude DIN functions.

---

### [2026-03-31] IIN modes trigger ADC_ERR (tasks.cpp)

**Symptom:** Setting a channel to any Current Input mode triggers `ADC_ERR`.

**Root cause:** Hardware auto-sets `CONV_RANGE=3` (negative-only) for IIN modes.
The sense voltage across RSENSE (12 Ω) is always positive for 4–20 mA, so this
range is incompatible and causes an ADC error.

**Fixed:** `tasks.cpp`, `CMD_SET_CHANNEL_FUNC` handler — after reading back
hardware-set values, override `hwRange` to `ADC_RNG_NEG0_3125_0_3125V` (±312.5 mV)
for all four IIN function codes (4, 5, 11, 12).

---

### [2026-03-31] USB disconnection from `[CLI Ready]` text pollution (bbp.cpp)

**Symptom:** Desktop app disconnects ~3 minutes after connecting, with log showing:
```
WARN CRC mismatch: ... data=[0A, 5B, 43, 4C, 49, 20, 52, 65, 61, 64, 79, 5D ...]
WARN CRC mismatch: ... data=[0A, 5B, 48, 65, 61, 72, 74, 62, 65, 61, 74, 5D ...]
ERROR Status poll failed 3 consecutive times, marking disconnected
```

**Root cause:** `bbpExitBinaryMode()` wrote the string `"\r\n[CLI Ready]\r\n"` to
CDC #0. Since CDC #0 is the BBP binary channel, the desktop `FrameAccumulator`
received this ASCII text, failed COBS/CRC decoding, the pending poll timed out,
and after 3 retries the connection was dropped.

The exit is triggered by `BBP_IDLE_TIMEOUT_MS` (idle watchdog inside `bbpProcess()`).

**Fixed:** `bbp.cpp`, `bbpExitBinaryMode()` — removed the `usb_cdc_cli_write()`
call. The desktop detects binary-mode exit via timeout without needing a text signal.

---

### [2026-03-31] NoOs driver header typos (Firmware/NoOs/ad74416h.h)

Six compile-breaking defects in the Analog Devices no-OS reference driver header:

| Line | Defect | Fix |
|---|---|---|
| 145 | `AD74VOUT_4W_EN_MSK` — missing "416H" in prefix | `AD74416H_VOUT_4W_EN_MSK` |
| 174 | `NO_OS_GENAMSK(2, 1)` — typo | `NO_OS_GENMASK(2, 1)` |
| 194 | `NO_OS_BIT(7, 6)` — wrong macro for a bit range | `NO_OS_GENMASK(7, 6)` |
| 262 | `NO_OS_GENAMSK(9. 8)` — typo + period instead of comma | `NO_OS_GENMASK(9, 8)` |
| 272 | `NO_OS_GENAMSK(10 8)` — typo + missing comma | `NO_OS_GENMASK(10, 8)` |
| 385 | `NO_OS_GENAMSK(3, 2)` — typo | `NO_OS_GENMASK(3, 2)` |
