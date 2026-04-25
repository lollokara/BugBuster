# BugBuster — AD74416H Engineering Reference

Cross-referenced against AD74416H datasheet Rev. 0 (3/2025, 106 pages) — full 106-page coverage.
Update whenever a bug is understood, a discrepancy is found, or a design decision is made.
Last deep-dive: all 106 pages read and cross-referenced (2026-03-31).

---

## Table of Contents

1. [Device Overview](#1-device-overview)
2. [Channel Functions — Complete Reference](#2-channel-functions--complete-reference)
3. [ADC — Complete Reference](#3-adc--complete-reference)
4. [DAC — Reference](#4-dac--reference)
5. [RTD Measurement — Complete Reference](#5-rtd-measurement--complete-reference)
6. [Current Measurement — Reference](#6-current-measurement--reference)
7. [Digital Input / Output](#7-digital-input--output)
8. [Diagnostics](#8-diagnostics)
9. [Register Map — Complete Reference](#9-register-map--complete-reference)
10. [Alert and Status Registers](#10-alert-and-status-registers)
11. [Protocol and Architecture Notes](#11-protocol-and-architecture-notes)
12. [Known Bugs and Implementation Notes](#12-known-bugs-and-implementation-notes)
13. [Electrical Specifications Summary](#13-electrical-specifications-summary)
14. [SPI Protocol — Complete Reference](#14-spi-protocol--complete-reference)
15. [Power Supply and Adaptive Power Switching](#15-power-supply-and-adaptive-power-switching)
16. [GPIO — Complete Reference](#16-gpio--complete-reference)
17. [Reset, Watchdog, and Fault Handling — Complete Reference](#17-reset-watchdog-and-fault-handling--complete-reference)
18. [Voltage Output — Feedback Sensing Modes](#18-voltage-output--feedback-sensing-modes)
19. [Digital Input — Complete Reference](#19-digital-input--complete-reference)
20. [Digital Output — Complete Reference](#20-digital-output--complete-reference)
21. [3-Wire RTD — Complete Reference](#21-3-wire-rtd--complete-reference)
22. [DAC Slew Rate Control](#22-dac-slew-rate-control)
23. [External Components and Board Design Reference](#23-external-components-and-board-design-reference)
24. [WS2812B Status LEDs](#24-ws2812b-status-leds)
25. [ADGS2414D Safety — Write-Verify and Fault Recovery](#25-adgs2414d-safety--write-verify-and-fault-recovery)
26. [ADGS2414D — Datasheet Reference](#26-adgs2414d--datasheet-reference)
27. [PCA9535A — Datasheet Reference](#27-pca9535a--datasheet-reference)
28. [DS4424 — Datasheet Reference](#28-ds4424--datasheet-reference)
29. [AD74416H — Extended Reference](#29-ad74416h--extended-reference)
30. [HAT Expansion Board System](#30-hat-expansion-board-system)
31. [AI Interface — MCP Server](#31-ai-interface--mcp-server-pythonbugbuster_mcp)
32. [Testing Infrastructure](#32-testing-infrastructure)

---

## 1. Device Overview

The AD74416H is a quad-channel, software-configurable I/O IC for industrial control applications.
Each of the four channels (A–D) can be independently configured as one of 12 functional modes.

**Key specifications:**
- 24-bit Σ-Δ ADC — one shared ADC, sequenced across channels and diagnostic slots
- 16-bit DAC per channel
- Integrated HART modem per channel (1200 baud)
- SPI interface with optional hardware CRC
- Internal 2.5V reference (or external via REFIO pin)
- Internal RSENSE = 12 Ω — used for current sensing and voltage output current readback
- Dual AVDD rails (AVDD_HI, AVDD_LO) with adaptive power switching for IOUT modes
- 6 GPIO pins (A–F): configurable as GPO, GPI, or HART modem interface
- Package: 64-lead LFCSP, −40°C to +105°C

**External sense pin naming used in datasheet:**

| Pin name (datasheet) | Firmware alias | Description |
|----------------------|---------------|-------------|
| I/OP_x (IOx_H)      | HF terminal   | High-side I/O screw terminal |
| I/ON_x (IOx_L)      | LF terminal   | Low-side I/O screw terminal  |
| SENSEHF_x           | —             | High-frequency sense (connected to I/OP_x via 2kΩ RC filter externally) |
| SENSELF_x           | —             | Low-frequency sense (connected to I/ON_x via 2kΩ RC filter externally)  |
| VSENSEP_x           | —             | Kelvin sense positive (for 4-wire voltage output feedback)               |
| VSENSEN_x           | —             | Kelvin sense negative                                                     |

**Power-up sequence:**
1. Apply DGND, AGND, AVSS
2. Apply DVCC, AVCC, then AVDD_HI/AVDD_LO
3. Wait minimum settling time before SPI transactions
4. All channels default to HIGH_IMP after reset

**Software reset:**
Write 0x15FA then 0xAF51 to CMD_KEY (0x74) as consecutive SPI writes with no intervening transactions.

---

## 2. Channel Functions — Complete Reference

### 2.1 CH_FUNC_SETUP Register

Address: `0x01 + channel × 0x0C`   (per-channel stride = 0x0C = 12 decimal, confirmed)

| Code | Firmware Enum Name         | Description                              |
|------|----------------------------|------------------------------------------|
| 0    | CH_FUNC_HIGH_IMP           | High impedance — safe default after reset |
| 1    | CH_FUNC_VOUT               | Voltage output, 0–12V or ±12V            |
| 2    | CH_FUNC_IOUT               | Current output, 0–25 mA                  |
| 3    | CH_FUNC_VIN                | Voltage input                             |
| 4    | CH_FUNC_IIN_EXT_PWR        | Current input, externally powered         |
| 5    | CH_FUNC_IIN_LOOP_PWR       | Current input, loop powered               |
| 6    | *(not defined)*            | Not a valid function in AD74416H — code 6 is skipped |
| 7    | CH_FUNC_RES_MEAS           | Resistance / RTD measurement              |
| 8    | CH_FUNC_DIN_LOGIC          | Digital input, logic level                |
| 9    | CH_FUNC_DIN_LOOP           | Digital input, loop powered               |
| 10   | CH_FUNC_IOUT_HART          | Current output with HART modem            |
| 11   | CH_FUNC_IIN_EXT_PWR_HART   | Current input ext. powered + HART         |
| 12   | CH_FUNC_IIN_LOOP_PWR_HART  | Current input loop powered + HART         |

> **Note:** The AD74416H has no thermocouple mode. Thermocouple measurement is done via VIN
> mode with CONV_RANGE = 6 (±104 mV). Code 6 in CH_FUNC_SETUP is reserved/invalid.

### 2.2 Hardware Auto-Set ADC Defaults (Datasheet Table 22)

When CH_FUNC_SETUP is written the hardware automatically initialises ADC_CONFIG. The firmware
reads these back then applies corrections where needed.

| Function             | Auto CONV_MUX       | Auto CONV_RANGE       | Firmware override?                           |
|----------------------|---------------------|-----------------------|----------------------------------------------|
| HIGH_IMP             | 0 — LF→AGND         | 0 — 0 to 12V          | None                                         |
| VOUT                 | 1 — HF→LF (RSENSE)  | 2 — ±312.5 mV         | None (reads RSENSE voltage for current limit)|
| IOUT                 | 0 — LF→AGND         | 0 — 0 to 12V          | None (reads compliance voltage)              |
| VIN                  | 0 — LF→AGND         | 0 — 0 to 12V          | None                                         |
| IIN_EXT_PWR          | 1 — HF→LF           | 3 — −312.5mV to 0V    | ⚠️ Range → 2 (±312.5 mV) — see BUG-01        |
| IIN_LOOP_PWR         | 1 — HF→LF           | 3 — −312.5mV to 0V    | ⚠️ Range → 2 (±312.5 mV) — see BUG-01        |
| RES_MEAS             | 3 — LF→VSENSEN      | 5 — 0 to 625 mV       | ⚠️ MUX → 0 (LF→AGND) — see BUG-03           |
| DIN_LOGIC            | (invalid)           | (invalid)             | Excluded from ADC conversion mask — BUG-04   |
| DIN_LOOP             | (invalid)           | (invalid)             | Excluded from ADC conversion mask — BUG-04   |
| IOUT_HART            | 0 — LF→AGND         | 0 — 0 to 12V          | None                                         |
| IIN_EXT_PWR_HART     | 1 — HF→LF           | 3 — −312.5mV to 0V    | ⚠️ Range → 2 (±312.5 mV)                     |
| IIN_LOOP_PWR_HART    | 1 — HF→LF           | 3 — −312.5mV to 0V    | ⚠️ Range → 2 (±312.5 mV)                     |

### 2.3 RTD_CONFIG Defaults After CH_FUNC_SETUP Write

The datasheet states these are set automatically when any CH_FUNC_SETUP is written (page 55):
- `RTD_MODE_SEL = 0` (3-wire mode by default)
- `RTD_CURRENT = 1` (1 mA excitation)
- `RTD_ADC_REF = 0` (non-ratiometric, 2.5V reference)

For BugBuster's 2-wire-only RTD path, the firmware writes `RTD_CONFIG = 0x0005`
(`RTD_MODE_SEL = 1`, `RTD_CURRENT = 1`) after `setChannelFunction()` for `RES_MEAS`
so the mode matches the datasheet's 2-wire configuration.

### 2.4 Channel Initialisation Timing

After writing CH_FUNC_SETUP, wait before starting ADC conversions:
- IOUT_HART: 4.2 ms (`CHANNEL_SWITCH_HART_US = 4200`)
- All other functions: 300 µs (`CHANNEL_SWITCH_US = 300`)

---

## 3. ADC — Complete Reference

### 3.1 ADC_CONFIG Register

Address: `0x02 + channel × 0x0C`

```
Bits [15:12] — reserved
Bits [11:8]  — CONV_RATE  (4 bits)   ⚠️ NOT [9:6] — see BUG-10
Bit  [7]     — reserved
Bits [6:4]   — CONV_RANGE (3 bits)
Bit  [3]     — reserved
Bits [2:0]   — CONV_MUX   (3 bits)
```

### 3.2 CONV_MUX (bits [2:0])

| Code | Firmware Enum                | Datasheet Name              | What it measures                              |
|------|------------------------------|-----------------------------|-----------------------------------------------|
| 0    | ADC_MUX_LF_TO_AGND           | SENSELF to AGND             | Voltage at I/ON_x terminal vs AGND            |
| 1    | ADC_MUX_HF_TO_LF             | SENSEHF to SENSELF          | Differential voltage across internal RSENSE   |
| 2    | ADC_MUX_VSENSEN_TO_AGND      | VSENSEN to AGND             | VSENSEN_x pin vs AGND                         |
| 3    | ADC_MUX_LF_TO_VSENSEN        | SENSELF to VSENSEN          | I/ON terminal vs VSENSEN (3-wire RTD path)    |
| 4    | ADC_MUX_AGND_TO_AGND         | AGND to AGND                | Self-test / zero-offset calibration           |

### 3.3 CONV_RANGE (bits [6:4])

All parameters verified against datasheet Table 24.

| Code | Firmware Enum                | Range                  | v_offset   | v_span    |
|------|------------------------------|------------------------|------------|-----------|
| 0    | ADC_RNG_0_12V                | 0V to +12V             | 0.000 V    | 12.000 V  |
| 1    | ADC_RNG_NEG12_12V            | −12V to +12V           | −12.000 V  | 24.000 V  |
| 2    | ADC_RNG_NEG0_3125_0_3125V    | ±312.5 mV              | −0.3125 V  | 0.6250 V  |
| 3    | ADC_RNG_NEG0_3125_0V         | −312.5 mV to 0V        | −0.3125 V  | 0.3125 V  |
| 4    | ADC_RNG_0_0_3125V            | 0V to +312.5 mV        | 0.000 V    | 0.3125 V  |
| 5    | ADC_RNG_0_0_625V             | 0V to +625 mV          | 0.000 V    | 0.6250 V  |
| 6    | ADC_RNG_NEG104MV_104MV       | ±104.16 mV             | −0.104 V   | 0.2083 V  |
| 7    | ADC_RNG_NEG2_5_2_5V          | ±2.5V                  | −2.500 V   | 5.000 V   |

### 3.4 CONV_RATE (bits [11:8])

| Code (binary) | Firmware Enum      | Output Data Rate    | HART Rejection |
|---------------|--------------------|---------------------|----------------|
| 0b0000        | ADC_RATE_10SPS_H   | 10 SPS high-res     | −96 dB         |
| 0b0001        | ADC_RATE_20SPS     | 20 SPS              | N/A            |
| 0b0011        | ADC_RATE_20SPS_H   | 20 SPS high-res     | −96 dB         |
| 0b0100        | ADC_RATE_200SPS_H1 | 200 SPS moderate    | −64 dB         |
| 0b0110        | ADC_RATE_200SPS_H  | 200 SPS high-res    | −90 dB         |
| 0b1000        | ADC_RATE_1_2KSPS   | 1.2 kSPS            | N/A            |
| 0b1001        | ADC_RATE_1_2KSPS_H | 1.2 kSPS high-res   | −57 dB         |
| 0b1100        | ADC_RATE_4_8KSPS   | 4.8 kSPS            | N/A            |
| 0b1101        | ADC_RATE_9_6KSPS   | 9.6 kSPS            | N/A            |

Firmware default for all channels: `ADC_RATE_20SPS` (hardware default after reset is 10 SPS).

### 3.5 ADC Transfer Function

```c
V = v_offset + (ADC_CODE / 16,777,216.0f) × v_span
```

- `ADC_CODE`: 24-bit unsigned integer from ADC_RESULT_UPR[n] bits [23:16] combined with
  ADC_RESULT[n] bits [15:0]. **Read UPR first** — reading UPR latches the lower word.
- `ADC_FULL_SCALE = 16,777,216 = 2²⁴` — confirmed correct.
- `v_offset`, `v_span`: from table in §3.3 above.

ADC saturation codes (set when input exceeds range):
- Positive saturation: 0xFFFFFF → ADC_ERR raised in ALERT_STATUS
- Negative saturation: 0x000000 → ADC_ERR raised in ALERT_STATUS

### 3.6 ADC Conversion Control

Register `ADC_CONV_CTRL` at address 0x39 (corrected per datasheet Table 55 / ad74416h_regs.h):

```
Bits [3:0]   — CONV_A–D_EN:     bit 0=ChA, 1=ChB, 2=ChC, 3=ChD (per-channel ADC enable)
Bits [7:4]   — DIAG_0–3_EN:     bit 4=slot0, 5=slot1, 6=slot2, 7=slot3 (diagnostic slot enable)
Bits [9:8]   — CONV_SEQ:        0=idle, 1=single, 2=continuous, 3=stop
Bits [12:10] — CONV_RATE_DIAG:  diagnostic conversion rate select
Bit  [13]    — ADC_RDY_CTRL:    0=ADC_RDY pulses per channel; 1=pulses only for last channel
```

DIN_LOGIC and DIN_LOOP channels must be **excluded** from CONV_A–D_EN — including them causes ADC_ERR.

### 3.7 Auto-Ranging (Firmware, tasks.cpp)

Implemented in `taskAdcPoll`. Evaluates every sample after hardware read, before state write-back.

**Over-range thresholds:**
- Positive: `raw >= 0xFF0000` (top ~0.4% of 24-bit span)
- Negative (bipolar ranges only): `raw <= 0x00FFFF` (bottom ~0.4%)

**Bipolar ranges detected:** NEG12_12V, NEG0_3125_0_3125V, NEG0_3125_0V, NEG104MV_104MV, NEG2_5_2_5V

**Range promotion order (`nextWiderRange()`):**
```
0_0_3125V → 0_0_625V → NEG2_5_2_5V → 0_12V → NEG12_12V (max)
NEG104MV_104MV → NEG0_3125_0_3125V → NEG2_5_2_5V → 0_12V → NEG12_12V
NEG0_3125_0V → NEG2_5_2_5V → 0_12V → NEG12_12V
```

**Debounce:** 500 ms per channel — prevents queue flooding when signal stays saturated.

Changing ADC_CONFIG requires stopping conversion, applying the change, then restarting. The firmware
uses a SPI bus grant handshake between the ADC poll task and the command processor task.

---

## 4. DAC — Reference

### 4.1 DAC Registers

- `DAC_CODE[ch]`:   `0x0A + ch × 0x0C` — staging register (write-only)
- `DAC_ACTIVE[ch]`: `0x0C + ch × 0x0C` — active output code (read-only)

Writing DAC_CODE stages the value. The output updates when DAC update key `0x1C7D` is written to
CMD_KEY (0x74). The firmware writes DAC_CODE then immediately triggers the update.

### 4.2 DAC Transfer Functions (Verified vs Datasheet Table 31)

**Unipolar voltage, 0 to +12V:**
```c
code = (V_out / 12.0f) × 65535
V_out = (code / 65535.0f) × 12.0f
```

**Bipolar voltage, −12V to +12V** (requires `VOUT_RANGE = 1` in OUTPUT_CONFIG):
```c
code = ((V_out + 12.0f) / 24.0f) × 65535
V_out = (code / 65535.0f) × 24.0f − 12.0f
```

**Current output, 0 to 25 mA:**
```c
code = (I_mA / 25.0f) × 65535
I_mA = (code / 65535.0f) × 25.0f
```

`DAC_FULL_SCALE = 65535` — correct per datasheet (code 65535 = full scale, not 65536).

### 4.3 OUTPUT_CONFIG Register

Address: `0x05 + ch × 0x0C`

| Bit(s) | Name           | Description                                                      |
|--------|----------------|------------------------------------------------------------------|
| 2      | VOUT_RANGE     | 0 = unipolar (0–12V), 1 = bipolar (±12V)                        |
| 3      | VOUT_4W_EN     | Enable 4-wire voltage output sensing                             |
| [5:4]  | SLEW_EN        | DAC slew rate: 00=off, 01=slow, 10=HART slew, 11=medium          |
| 6      | DO_MODE        | Digital output mode                                              |
| 7      | DO_SRC_SEL     | Digital output source select                                     |
| 8      | I_LIMIT        | Short-circuit current limit: 0 = 16mA, 1 = 8mA                  |
| [11:10]| AVDD_SELECT    | AVDD source: 00=lock to AVDD_HI, 10=track supply (IOUT modes)   |

---

## 5. RTD Measurement — Complete Reference

### 5.1 RTD_CONFIG Register

Address: `0x06 + ch × 0x0C`

| Bit | Name           | Values                                                                |
|-----|----------------|-----------------------------------------------------------------------|
| 0   | RTD_CURRENT    | **0 = 500 µA, 1 = 1000 µA (1 mA)** ⚠️ NOT 125/250 µA               |
| 1   | RTD_EXC_SWAP   | 0 = normal, 1 = swap excitation current direction                     |
| 2   | RTD_MODE_SEL   | 0 = 3-wire mode, 1 = 2-wire mode                                      |
| 3   | RTD_ADC_REF    | 0 = non-ratiometric (2.5V reference), 1 = ratiometric (1V reference)  |

Sources: AD74416H datasheet Table 6 (excitation current 500 µA typ), page 55 (RTD_CURRENT=1 selects 1mA).

### 5.2 Excitation Current vs Resistance Range

| RTD_CURRENT | I_excitation | Max R (range 5, 0–625mV) | Max R (range 0, 0–12V) |
|-------------|--------------|--------------------------|------------------------|
| 0           | 500 µA       | 1250 Ω                   | 24 kΩ                  |
| 1           | 1000 µA (1mA)| 625 Ω                    | 12 kΩ                  |

**PT100** (100 Ω at 0°C, ~390 Ω at 850°C): use 1 mA. 390 × 1mA = 390 mV ✓ in range 5 (0–625 mV).

**PT1000** (1000 Ω at 0°C, ~3850 Ω at 850°C): use 500 µA. At 850°C: 3850 × 500µA = 1.925V.
Auto-ranging promotes to range 7 (±2.5V). Or manually set range 0 (0–12V).

### 5.3 2-Wire RTD Wiring and Firmware Configuration

Connect the RTD between the I/OP_x (HF) and I/ON_x (LF) screw terminals.

Firmware writes on entering RES_MEAS:
```
RTD_CONFIG = 0x0005  (RTD_CURRENT=1 → 1mA; RTD_MODE_SEL=1 → 2-wire; RTD_ADC_REF=0 → non-ratiometric)
CONV_MUX   = 0       (SENSELF to AGND — measures V(I/ON terminal) − V(AGND) = I_EXC × R_RTD)
CONV_RANGE = 5       (0–625 mV — hardware auto-set, kept as-is; auto-ranging promotes as needed)
```

### 5.4 Resistance Calculation Formula (Non-Ratiometric, 2-Wire)

```c
V_adc = v_offset + (ADC_CODE / 16,777,216.0f) × v_span   // from adcCodeToVoltage()
R_RTD = V_adc / I_excitation                               // I_excitation in Amps
```

Fallback excitation in firmware: 1000 µA (1 mA) — if `rtdExcitationUa == 0`.

### 5.5 Ratiometric Formula (NOT used by firmware, provided for reference)

Used only when `RTD_ADC_REF = 1` and `RTD_MODE_SEL = 0` (3-wire):
```
R_RTD = (ADC_CODE / (16,777,216 × ADC_GAIN)) × R_REF
R_REF ≈ 2012 Ω (internal reference resistor 2000 Ω + RSENSE 12 Ω)
```

### 5.6 RTD Alert Sequence

If RTD_CONFIG is not written before starting ADC conversion on a RES_MEAS channel:
1. VIOUT current source finds no load
2. `VIOUT_SHUTDOWN` set in CHANNEL_ALERT_STATUS[ch] (bit 6)
3. Propagates to `CH_x_ALERT` in ALERT_STATUS (bits 8–11)

The firmware writes RTD_CONFIG immediately after `setChannelFunction()`. When leaving RES_MEAS
(transitioning to HIGH_IMP), firmware writes `RTD_CONFIG = 0x0000` to disable the excitation.

---

## 6. Current Measurement — Reference

### 6.1 Internal RSENSE

RSENSE = 12 Ω internal, 0.1%, 10 ppm/°C. Firmware constant: `DEFAULT_RSENSE_OHM = 12.0f`.

Used for:
- **IIN modes**: sensing 4–20 mA current loop signal
- **VOUT mode**: measuring output current for short-circuit detection
- **IOUT mode**: optional compliance verification

### 6.2 Current Input (IIN_EXT_PWR, IIN_LOOP_PWR)

ADC measures differential voltage across RSENSE (CONV_MUX=1, SENSEHF to SENSELF).

```c
I_mA = (V_adc / RSENSE) × 1000.0f
V_adc = adcCodeToVoltage(raw, ADC_RNG_NEG0_3125_0_3125V)
```

For 4–20 mA: V_RSENSE = 48 mV to 240 mV (positive). The ±312.5 mV range (range 2) covers this
fully and avoids ADC_ERR from the hardware's incorrect negative-only default.

### 6.3 Voltage Output Current Readback (VOUT mode)

Hardware auto-sets CONV_MUX=1 (HF→LF, across RSENSE), CONV_RANGE=2 (±312.5 mV). The firmware
stores the raw ADC voltage for VOUT, not a current. The DAC sets the output voltage; the ADC
measures the sense resistor for short-circuit compliance monitoring.

### 6.4 Current Output Compliance Voltage (IOUT mode)

Hardware auto-sets CONV_MUX=0 (LF→AGND, 0–12V). Reads the load voltage at the output terminal.
Not used directly in the firmware conversion but stored as `adcValue` for diagnostics.

---

## 7. Digital Input / Output

### 7.1 Digital Input (DIN_LOGIC, DIN_LOOP)

DIN uses the comparator path, not the ADC path. The comparator result is available in:
- `DIN_COMP_OUT` register (0x3E), bits [3:0], one per channel
- `DIN_COUNTER_UPR` (0x4F + ch×2) + `DIN_COUNTER` (0x50 + ch×2) — 32-bit edge counter

**COMPARATOR_EN** (DIN_CONFIG0 bit 13): must be set to enable the comparator block.
> ⚠️ **BUG-06**: `configureDin()` in ad74416h.cpp does not set this bit. See §12.

**DIN_CONFIG0 (0x03 + ch×0x0C) key fields:**

| Bits  | Name             | Description                             |
|-------|------------------|-----------------------------------------|
| [4:0] | DEBOUNCE_TIME    | Debounce time code (0–31)               |
| 6     | DEBOUNCE_MODE    | 0=count both edges, 1=count valid edges |
| [11:7]| DIN_SINK         | Current sink code (IEC 61131-2)         |
| 12    | DIN_SINK_RANGE   | 0=range 0 (2.8 kΩ), 1=range 1 (1 kΩ)  |
| 13    | COMPARATOR_EN    | Enable comparator (⚠️ must be set)      |
| 14    | DIN_INV_COMP_OUT | Invert comparator output                |
| 15    | COUNT_EN         | Enable edge counter                     |

**DIN_CONFIG1 (0x04 + ch×0x0C) key fields:**

| Bits  | Name             | Description                                     |
|-------|------------------|-------------------------------------------------|
| [6:0] | COMP_THRESH      | Comparator threshold code                       |
| 7     | DIN_THRESH_MODE  | 0=AVDD_HI-relative threshold, 1=fixed 0.5V steps|
| 8     | DIN_OC_DET_EN    | Open-circuit detect enable                      |
| 9     | DIN_SC_DET_EN    | Short-circuit detect enable                     |
| 10    | DIN_INPUT_SELECT | 0=VIOUT_x (unbuffered), 1=VSENSEP_x (buffered)  |

### 7.2 Digital Output

DO uses the DAC/VIOUT path to drive a FET. Key OUTPUT_CONFIG fields for DO modes (bits 6–9).
DO timing registers: DO_EXT_CONFIG (0x08 + ch×0x0C) — T1 and T2 timing for DO_MODE=1 (timed).

---

## 8. Diagnostics

### 8.1 DIAG_ASSIGN Register (0x3A)

Four 4-bit source selects packed in one 16-bit register:
```
[3:0]  = slot 0 source
[7:4]  = slot 1 source
[11:8] = slot 2 source
[15:12]= slot 3 source
```

Results in `ADC_DIAG_RESULT[n]` at 0x49–0x4C (stride 1). These are **16-bit** values (not 24-bit).

### 8.2 Diagnostic Source Codes (Table 30)

| Code | Source       | Formula                                                    | Range       |
|------|--------------|------------------------------------------------------------|-------------|
| 0    | AGND         | `V = (code / 65536) × 2.5`                                | 0–2.5V      |
| 1    | Temperature  | `T(°C) = (code − 2034) / 8.95 − 40`                      | −40–105°C   |
| 2    | DVCC         | `V = (code / 65536) × (25/3)`                             | 0–8.3V      |
| 3    | AVCC         | `V = (code / 65536) × 17.5`                               | 0–17.5V     |
| 4    | LDO1V8       | `V = (code / 65536) × 7.5`                                | 0–7.5V      |
| 5    | AVDD_HI      | `V = (code / 65536) × (25 / 0.52)`                        | 0–48V       |
| 6    | AVDD_LO      | `V = (code / 65536) × (25 / 0.52)`                        | 0–48V       |
| 7    | AVSS         | `V = (code / 65536 × 31.017) − 20`                        | −20–+11V    |
| 8    | LVIN         | `V = (code / 65536) × 2.5`                                | 0–2.5V      |
| 9    | DO_VDD       | `V = (code / 65536) × (25 / 0.64)`                        | 0–39V       |
| 10   | VSENSEP_x    | `(code/65536 × 50) − 20` (DIN_THRESH_MODE=1 default)      | −20V to +30V |
| 11   | VSENSEN_x    | `(code/65536 × 50) − 20`                                  | −20V to +30V |
| 12   | I_DO_SRC_x   | `(code/65536 × 0.5)` — voltage across R_SET (÷ R_SET for A)| 0–0.5V/R_SET |
| 13   | AVDD_x       | `(code/65536) × (25/0.52)`                                | 0–48V        |

> Source 10 note: DIN_THRESH_MODE=0 uses `(code/65536 × 60) − AVDD_HI` (range −AVDD_HI to +60V−AVDD_HI)
> but requires runtime AVDD_HI. Firmware defaults to Mode=1 formula. Source 12 returns V across R_SET; divide by R_SET (Ω) for current in Amps.
> ✅ **BUG-07 Fixed**: All sources 0–13 now have explicit cases in `diagCodeToValue()`. See §12.

### 8.3 Firmware Default Diagnostic Slots

| Slot | Source | Measures        |
|------|--------|-----------------|
| 0    | 1      | Die temperature |
| 1    | 5      | AVDD_HI voltage |
| 2    | 2      | DVCC voltage    |
| 3    | 3      | AVCC voltage    |

Read every ~1 second (fault monitor task, every 5th 200 ms iteration). Slot 0 stored as
`g_deviceState.dieTemperature`.

---

## 9. Register Map — Complete Reference

### 9.1 Per-Channel Registers

Stride: 0x0C (12 decimal). Address = `base + channel × 0x0C`, channel = 0–3.

| Base | Name              | Access | Reset  | Key Fields                                              |
|------|-------------------|--------|--------|---------------------------------------------------------|
| 0x01 | CH_FUNC_SETUP     | R/W    | 0x0000 | [3:0] CH_FUNC bits (function code)                     |
| 0x02 | ADC_CONFIG        | R/W    | varies | [11:8] CONV_RATE, [6:4] CONV_RANGE, [2:0] CONV_MUX    |
| 0x03 | DIN_CONFIG0       | R/W    | 0x0000 | [13] COMPARATOR_EN, [11:7] DIN_SINK, [4:0] DEBOUNCE   |
| 0x04 | DIN_CONFIG1       | R/W    | 0x0049 | [6:0] COMP_THRESH, [7] THRESH_MODE                     |
| 0x05 | OUTPUT_CONFIG     | R/W    | 0x0000 | [11:10] AVDD_SELECT, [8] I_LIMIT, [3] VOUT_4W_EN, [2] VOUT_RANGE |
| 0x06 | RTD_CONFIG        | R/W    | 0x0000 | [3] RTD_ADC_REF, [2] RTD_MODE_SEL, [1] RTD_EXC_SWAP, [0] RTD_CURRENT |
| 0x07 | FET_LKG_COMP      | R/W    | 0x0000 | FET leakage compensation                               |
| 0x08 | DO_EXT_CONFIG     | R/W    | 0x0000 | [12] DO_DATA, [11:7] DO_T2, [6:2] DO_T1, [1] DO_SRC_SEL, [0] DO_MODE |
| 0x09 | I_BURNOUT_CONFIG  | R/W    | 0x0000 | [2] BRN_VIOUT_POL, [1:0] BRN_VIOUT_CURR               |
| 0x0A | DAC_CODE          | W      | 0x0000 | [15:0] DAC code (staging register)                     |
| 0x0B | DAC_CLR_EN        | R/W    | 0x0000 | [0] DAC_CLR_EN                                         |
| 0x0C | DAC_ACTIVE        | R      | 0x0000 | [15:0] Active DAC output code (read-only)              |

### 9.2 Global Registers

| Address | Name                 | Access | Reset  | Key Fields                                              |
|---------|----------------------|--------|--------|---------------------------------------------------------|
| 0x32    | GPIO_CONFIG[A]       | R/W    | 0x0000 | [6] GP_WK_PD_EN, [5] GPI_DATA, [4] GPO_DATA, [2:0] GPIO_SELECT |
| 0x33    | GPIO_CONFIG[B]       | R/W    | 0x0000 | Same layout as A                                        |
| 0x34    | GPIO_CONFIG[C]       | R/W    | 0x0000 |                                                         |
| 0x35    | GPIO_CONFIG[D]       | R/W    | 0x0000 |                                                         |
| 0x36    | GPIO_CONFIG[E]       | R/W    | 0x0000 |                                                         |
| 0x37    | GPIO_CONFIG[F]       | R/W    | 0x0000 |                                                         |
| 0x38    | PWR_OPTIM_CONFIG     | R/W    | 0x0000 | [13] REF_EN (must be set to enable internal 2.5V ref)  |
| 0x39    | ADC_CONV_CTRL        | R/W    | 0x0000 | [3:0] CH_EN, [7:4] DIAG_EN, [9:8] CONV_SEQ, [12:10] CONV_RATE_DIAG, [13] ADC_RDY_CTRL |
| 0x3A    | DIAG_ASSIGN          | R/W    | 0x0000 | [15:12] slot3, [11:8] slot2, [7:4] slot1, [3:0] slot0 |
| 0x3B    | WDT_CONFIG           | R/W    | 0x0000 | Watchdog timer                                          |
| 0x3E    | DIN_COMP_OUT         | R      | 0x0000 | [3:0] comparator outputs, one per channel              |
| 0x3F    | ALERT_STATUS         | R/W1C  | 0x0001 | See §10.1; bit0=RESET, bit2=SUPPLY_ERR, bit3=SPI_ERR, bits8–11=CH_ALERT, bits12–15=HART_ALERT |
| 0x40    | LIVE_STATUS          | R      | 0x0001 | Real-time status (not latched)                         |
| 0x41    | ADC_RESULT_UPR[A]    | R      | 0x0000 | [7:0]=ADC[23:16], [9:8]=SEQ_CNT, [12:10]=RNG, [15:13]=MUX — read UPR first! |
| 0x42    | ADC_RESULT[A]        | R      | 0x0000 | [15:0] of 24-bit ADC result — read UPR first!          |
| 0x43    | ADC_RESULT_UPR[B]    | R      | 0x0000 |                                                         |
| 0x44    | ADC_RESULT[B]        | R      | 0x0000 |                                                         |
| 0x45    | ADC_RESULT_UPR[C]    | R      | 0x0000 |                                                         |
| 0x46    | ADC_RESULT[C]        | R      | 0x0000 |                                                         |
| 0x47    | ADC_RESULT_UPR[D]    | R      | 0x0000 |                                                         |
| 0x48    | ADC_RESULT[D]        | R      | 0x0000 |                                                         |
| 0x49    | ADC_DIAG_RESULT[0]   | R      | 0x0000 | 16-bit diagnostic slot 0 result                        |
| 0x4A    | ADC_DIAG_RESULT[1]   | R      | 0x0000 |                                                         |
| 0x4B    | ADC_DIAG_RESULT[2]   | R      | 0x0000 |                                                         |
| 0x4C    | ADC_DIAG_RESULT[3]   | R      | 0x0000 |                                                         |
| 0x4D    | LAST_ADC_RESULT_UPR  | R      | 0x0000 | Last converted channel result (UPR) — read first       |
| 0x4E    | LAST_ADC_RESULT      | R      | 0x0000 | Last converted channel result (lower 16)               |
| 0x4F    | DIN_COUNTER_UPR[A]   | R      | 0x0000 | Upper 16 bits of 32-bit DIN edge counter — read first  |
| 0x50    | DIN_COUNTER[A]       | R      | 0x0000 | Lower 16 bits                                           |
| 0x51    | DIN_COUNTER_UPR[B]   | R      | 0x0000 |                                                         |
| 0x52    | DIN_COUNTER[B]       | R      | 0x0000 |                                                         |
| 0x53    | DIN_COUNTER_UPR[C]   | R      | 0x0000 |                                                         |
| 0x54    | DIN_COUNTER[C]       | R      | 0x0000 |                                                         |
| 0x55    | DIN_COUNTER_UPR[D]   | R      | 0x0000 |                                                         |
| 0x56    | DIN_COUNTER[D]       | R      | 0x0000 |                                                         |
| 0x57    | SUPPLY_ALERT_STATUS  | R/W1C  | 0x0000 | See §10.3                                              |
| 0x58    | CHANNEL_ALERT_STATUS[A] | R/W1C | 0x0000 | See §10.2                                           |
| 0x59    | CHANNEL_ALERT_STATUS[B] | R/W1C | 0x0000 |                                                      |
| 0x5A    | CHANNEL_ALERT_STATUS[C] | R/W1C | 0x0000 |                                                      |
| 0x5B    | CHANNEL_ALERT_STATUS[D] | R/W1C | 0x0000 |                                                      |
| 0x5C    | ALERT_MASK           | R/W    | 0x0000 | Same bit layout as ALERT_STATUS                        |
| 0x5D    | SUPPLY_ALERT_MASK    | R/W    | 0x0000 |                                                         |
| 0x5E    | CHANNEL_ALERT_MASK[A]| R/W    | 0x0000 |                                                         |
| 0x5F    | CHANNEL_ALERT_MASK[B]| R/W    | 0x0000 |                                                         |
| 0x60    | CHANNEL_ALERT_MASK[C]| R/W    | 0x0000 |                                                         |
| 0x61    | CHANNEL_ALERT_MASK[D]| R/W    | 0x0000 |                                                         |
| 0x6E    | READ_SELECT          | R/W    | 0x0000 | [8:0] READBACK_ADDR — selects register for burst read  |
| 0x6F    | BURST_READ_SEL       | R/W    | 0xFFFF | Burst read register selection mask                     |
| 0x73    | THERM_RST            | R/W    | 0x0000 | [0] enable thermal reset at ~140°C                     |
| 0x74    | CMD_KEY              | W      | —      | 0x15FA+0xAF51=reset, 0x1C7D=DAC update                |
| 0x75    | BROADCAST_CMD_KEY    | W      | —      | Broadcast equivalents of CMD_KEY                       |
| 0x76    | SCRATCH[0]           | R/W    | 0x0000 | Used by firmware for SPI health check                  |
| 0x7A    | GENERIC_ID           | R      | 0x0006 | [2:0] device type: AD74416H = 6                        |
| 0x7B    | SILICON_REV          | R      | 0x0002 | [7:0] silicon revision                                  |

### 9.3 ADC_RESULT_UPR Field Layout

Corrected per datasheet Table 61 / ad74416h_regs.h (`ADC_RESULT_UPR_CONV_RES_MASK = 0xFF << 0`):

```
Bits [7:0]   — ADC_CODE[23:16]  (top 8 bits of 24-bit result; occupies the full lower byte)
Bits [9:8]   — CONV_SEQ_COUNT   (2-bit rolling counter, increments per completed sequence)
Bits [12:10] — CONV_RES_RANGE   (active CONV_RANGE code at time of conversion)
Bits [15:13] — CONV_RES_MUX     (active ADC MUX setting at time of conversion)
```

ADC_RESULT[n] bits [15:0] = ADC_CODE[15:0] (bottom 16 bits). Read UPR first — reading UPR latches the lower word.

### 9.4 CMD_KEY Values

| Value  | Action                              |
|--------|-------------------------------------|
| 0x15FA | Software reset key 1 (must follow immediately with 0xAF51) |
| 0xAF51 | Software reset key 2                |
| 0x1C7D | DAC update — latches DAC_CODE to output on all channels |
| 0x0000 | No-op                               |

Broadcast CMD_KEY (0x75): 0x1A78 + 0xD203 = broadcast reset; 0x964E = broadcast DAC update.

---

## 10. Alert and Status Registers

### 10.1 ALERT_STATUS (0x3F) — Global Alert

Write 1 to clear (W1C). The ALERT pin (active-low open-drain) asserts when any unmasked bit is set.

Corrected per ADI no-OS driver register map / ad74416h_regs.h:

| Bit | Name            | Description                                               |
|-----|-----------------|-----------------------------------------------------------|
| 0   | RESET_OCCURRED  | Device reset since last clear (set on power-up)           |
| 1   | —               | Reserved                                                  |
| 2   | SUPPLY_ERR      | Supply voltage error (see SUPPLY_ALERT_STATUS 0x57)       |
| 3   | SPI_ERR         | SPI framing or CRC error                                  |
| 4   | TEMP_ALERT      | Die temperature ≥ 115 °C                                  |
| 5   | ADC_ERR         | ADC conversion error (invalid MUX or saturation)          |
| 6–7 | —               | Reserved                                                  |
| 8   | CH_ALERT_A      | Channel A alert (see CHANNEL_ALERT_STATUS_A 0x58)         |
| 9   | CH_ALERT_B      | Channel B alert (see CHANNEL_ALERT_STATUS_B 0x59)         |
| 10  | CH_ALERT_C      | Channel C alert (see CHANNEL_ALERT_STATUS_C 0x5A)         |
| 11  | CH_ALERT_D      | Channel D alert (see CHANNEL_ALERT_STATUS_D 0x5B)         |
| 12  | HART_ALERT_A    | Channel A HART alert (see HART_ALERT_STATUS_A 0x80)       |
| 13  | HART_ALERT_B    | Channel B HART alert                                      |
| 14  | HART_ALERT_C    | Channel C HART alert                                      |
| 15  | HART_ALERT_D    | Channel D HART alert                                      |

Reset value = 0x0001 (RESET_OCCURRED bit 0 set at power-up). Note: an earlier version of this section incorrectly placed fields at the wrong bits; this corrected layout matches the ad74416h_regs.h definitions.

### 10.2 CHANNEL_ALERT_STATUS[n] (0x58–0x5B)

Same bit layout for all four channels. W1C. CHANNEL_ALERT_MASK[n] (0x5E–0x61) masks these.

| Bit | Name            | Description                                              |
|-----|-----------------|----------------------------------------------------------|
| 0   | DIN_SC          | DIN short-circuit detected                               |
| 1   | DIN_OC          | DIN open-circuit detected                                |
| 2   | DO_SC           | Digital output short-circuit                             |
| 3   | DO_TIMEOUT      | Digital output short-circuit timeout                     |
| 4   | ANALOG_IO_SC    | Analog I/O short-circuit detected                        |
| 5   | ANALOG_IO_OC    | Analog I/O open-circuit detected                         |
| 6   | VIOUT_SHUTDOWN  | Output shutdown (RTD no-load, current limit, thermal)    |

### 10.3 SUPPLY_ALERT_STATUS (0x57)

W1C. SUPPLY_ALERT_MASK (0x5D) uses the same bit layout.

| Bit | Name         | Description              |
|-----|--------------|--------------------------|
| 0   | CAL_MEM_ERR  | Calibration memory error |
| 1   | AVSS_ERR     | AVSS supply out of range |
| 2   | DVCC_ERR     | DVCC supply error        |
| 3   | AVCC_ERR     | AVCC supply error        |
| 4   | DO_VDD_ERR   | DO_VDD supply error      |
| 5   | AVDD_LO_ERR  | AVDD_LO out of range     |
| 6   | AVDD_HI_ERR  | AVDD_HI out of range     |

### 10.4 LIVE_STATUS (0x40)

Real-time (not latched). Useful for polling current device state without affecting ALERT_STATUS.

| Bits  | Name                | Description                           |
|-------|---------------------|---------------------------------------|
| 0     | SUPPLY_STATUS       | 1 = all supplies OK                   |
| 1     | ADC_BUSY            | 1 = ADC conversion in progress        |
| 2     | ADC_DATA_RDY        | 1 = new ADC data available (continuous mode; auto-clears after 25µs) |
| 3     | TEMP_ALERT          | 1 = die temperature alert active      |
| [7:4] | DIN_STATUS_A–D      | DIN comparator output, one per channel|
| [11:8]| DO_STATUS_A–D       | Digital output status                 |
| [15:12]| ANALOG_IO_STATUS_A–D | Analog I/O operational status        |

---

## 11. Protocol and Architecture Notes

### 11.1 FreeRTOS Task Architecture

All three main tasks run on Core 1.

| Task                  | Priority | Cadence         | Responsibility                                          |
|-----------------------|----------|-----------------|---------------------------------------------------------|
| taskAdcPoll           | 3        | Dynamic (≥1 ms) | ADC readback, scope buffer, auto-ranging commands       |
| taskFaultMonitor      | 4        | 200 ms fixed    | Alert status, DIN comparator, GPIO read, diagnostics (every 5th cycle ≈1s) |
| taskCommandProcessor  | 2        | portMAX_DELAY   | All SPI write operations via g_cmdQueue                 |

Global state (`g_deviceState`) protected by `g_stateMutex` (FreeRTOS mutex). All tasks must hold
the mutex before read/write of any DeviceState field.

**SPI bus grant handshake:** when CMD_ADC_CONFIG needs to stop/reconfigure the ADC, the command
processor sets `g_spi_bus_request = true` and waits up to 200 ms for `g_spi_bus_granted`. The ADC
poll task checks this flag each iteration and yields.

### 11.2 USB CDC / BBP Protocol

The ESP32-S3 TinyUSB composite device exposes two CDC interfaces:
- **CDC #0**: BBP binary protocol AND CLI text (shared). No ASCII text while `bbpIsActive()` is true.
- **CDC #1+**: UART bridge forwarding.

COBS-framed binary protocol on CDC#0. Frame layout (pre-COBS):
```
[msg_type: 1B] [seq: 2B LE] [cmd_id: 1B] [payload: 0–N B] [CRC16-CCITT: 2B LE]
```
CRC-16/CCITT (poly 0x1021, init 0xFFFF) covers everything except the CRC bytes themselves.

**GET_STATUS per-channel payload**: stride = 28 bytes, total = 155 bytes for 4 channels + overhead.

### 11.3 BBP Idle Timeout

Firmware exits binary mode if no frame is received within `BBP_IDLE_TIMEOUT_MS`. After this the
`bbpExitBinaryMode()` function is called — it must NOT write any text to CDC#0 (would be received
as a binary frame by a reconnecting host and fail CRC).

### 11.4 Scope Buffer

Ring buffer of 256 buckets (`SCOPE_BUF_SIZE`), each covering `SCOPE_BUCKET_MS = 10 ms`
(~100 buckets/second). Per-bucket data: per-channel vMin, vMax, vSum, count, timestamp_ms.

The ADC poll task accumulates samples into `sb.cur`. When `SCOPE_BUCKET_MS` elapses and `count > 0`,
the current bucket is committed to `sb.buckets[head]`, `head` is advanced, and `seq` is incremented.
The HTTP and BBP scope endpoints drain completed buckets since the caller's last `seq` value.

### 11.5 Auto-Ranging Interaction with CMD_ADC_CONFIG

Auto-ranging enqueues `CMD_ADC_CONFIG` commands from the ADC poll task. The command processor
handles these by:
1. Setting `g_spi_bus_request = true` (asks poll task to yield)
2. Waiting up to 200 ms for `g_spi_bus_granted`
3. Calling `startAdcConversion(false, 0, 0)` to stop conversion
4. Calling `configureAdc(ch, mux, range, rate)` to reconfigure
5. Calling `startAdcConversion(true, chMask, diagMask)` to restart
6. Clearing `g_spi_bus_request`

The poll task's 500 ms debounce per channel prevents queue flooding during sustained saturation.

### 11.6 I2C Device Integration

I2C bus shared by DS4424 (4-channel IDAC), HUSB238 (USB-PD controller), PCA9535 (16-bit GPIO expander).
Bus speed: 100 kHz (breadboard), 400 kHz (PCB). All I2C state mirrored in DeviceState (idac, usbpd, ioexp).

### 11.7 MUX Switch Matrix (ADGS2414D)

4× ADGS2414D octal SPST switches daisy-chained on a dedicated SPI bus.
- PCB: GPIO21 CS, 4 devices; breadboard: GPIO12 CS, 1 device
- Dead time: `ADGS_DEAD_TIME_MS = 100 ms` between switch operations
- Level shifter OE: `PIN_LSHIFT_OE` (GPIO14)

### 11.8 HART Modem Initialisation

Required sequence (per datasheet pages 31–32):
1. Configure channel to IOUT_HART / IIN_EXT_PWR_HART / IIN_LOOP_PWR_HART
2. Wait: 4.2 ms for IOUT_HART; 300 µs for IIN variants
3. Wait for `HART_COMPL_SETTLED` bit in OUTPUT_CONFIG to assert
4. Set SLEW_EN = 0b10 (SLEW_HART_COMPL) in OUTPUT_CONFIG
5. Set MODEM_PWRUP in HART_CONFIG[ch]
6. Write preamble count to HART_TX_PREM[ch] (default 5 bytes)
7. Load TX FIFO via HART_TX[ch] (depth: 32 bytes)
8. Set RTS in HART_MCR[ch] to start transmission

HART FIFO depth: 32 bytes TX, 32 bytes RX. Baud: 1200 baud (standard HART).

---

## 12. Known Bugs and Implementation Notes

### BUG-01: IIN Modes — ADC_ERR from Negative-Only Range

**Status: Fixed.**

Hardware auto-sets CONV_RANGE=3 (−312.5mV to 0V) for IIN_EXT_PWR, IIN_LOOP_PWR, and HART variants.
The 4–20mA sense voltage across RSENSE (12Ω) is always positive (48–240mV). The negative-only range
triggers ADC_ERR on every sample.

**Fix:** `CMD_SET_CHANNEL_FUNC` overrides `hwRange` to `ADC_RNG_NEG0_3125_0_3125V` (±312.5mV)
for all four IIN function codes after reading back the hardware default.

---

### BUG-02: RES_MEAS — VIOUT_SHUTDOWN Without RTD_CONFIG Write

**Status: Fixed.**

Without writing RTD_CONFIG, the VIOUT current source has no load on entering RES_MEAS and immediately
asserts VIOUT_SHUTDOWN (CHANNEL_ALERT_STATUS bit 6) → CH_x_ALERT in ALERT_STATUS.

**Fix:** `CMD_SET_CHANNEL_FUNC` writes `RTD_CONFIG = 0x0005` (1mA, 2-wire, non-ratiometric)
immediately after `setChannelFunction()`. Clears to 0x0000 on transition to HIGH_IMP.

---

### BUG-03: RES_MEAS — Wrong CONV_MUX for 2-Wire Operation

**Status: Fixed.**

Hardware auto-sets CONV_MUX=3 (SENSELF to VSENSEN — the 3-wire RTD path). For 2-wire RTD,
CONV_MUX=0 (SENSELF to AGND) is needed: measures V(I/ON terminal)−V(AGND) = I_EXC × R_RTD directly.

**Verified:** 330Ω × 1mA = 330mV → measured ~323Ω (7Ω = lead resistance + tolerance). ✓

**Fix:** `CMD_SET_CHANNEL_FUNC` forces `hwMux = ADC_MUX_LF_TO_AGND` for RES_MEAS.

---

### BUG-04: DIN Channels — ADC_ERR When Included in Conversion Mask

**Status: Fixed.**

DIN_LOGIC and DIN_LOOP use the comparator path. Including them in ADC_CONV_CTRL CH_EN causes ADC_ERR.

**Fix:** All three locations where the channel enable mask is built (`CMD_SET_CHANNEL_FUNC`,
`CMD_ADC_CONFIG`, ADC poll initialisation) exclude channels in DIN_LOGIC or DIN_LOOP state.
DIN state is read from DIN_COMP_OUT (0x3E) and DIN_COUNTER registers, not from ADC conversions.

---

### BUG-05: USB Disconnection from ASCII Text on CDC#0 During BBP Session

**Status: Fixed.**

ASCII debug text written to CDC#0 while a BBP binary session is active fails COBS/CRC, causing
the desktop to time out and disconnect after 3 consecutive failures.

**Fix:** `bbpExitBinaryMode()` no longer writes `"\r\n[CLI Ready]\r\n"` to CDC#0. All debug
logging is routed away from CDC#0 while `bbpIsActive()` is true.

---

### BUG-06: DIN Comparator May Be Inactive — COMPARATOR_EN Not Set ✅ FIXED

**Status: Fixed in `ad74416h.cpp`.**

`configureDin()` in `ad74416h.cpp` built and wrote DIN_CONFIG0 without ever setting the
`COMPARATOR_EN` bit (DIN_CONFIG0 bit 13, mask `DIN_CONFIG0_COMPARATOR_EN_MASK`). Per the
datasheet, COMPARATOR_EN must be set for the digital input comparator block to function in
DIN_LOGIC and DIN_LOOP modes.

**Impact was:** DIN state reads (DIN_COMP_OUT, DIN_COUNTER) unreliable. The `dinState`
field in ChannelState and the DIN counter shown in the UI may not have reflected actual input state.

**Fix applied:** Added `cfg0 |= DIN_CONFIG0_COMPARATOR_EN_MASK;  // bit 13: enable comparator block`
in `configureDin()` in `ad74416h.cpp`, before the `writeRegister` call for DIN_CONFIG0.

---

### BUG-07: diagCodeToValue() Sources 10–13 Use Incorrect Formula ✅ FIXED

**Status: Fixed in `ad74416h.cpp`.**

`diagCodeToValue()` handled sources 0–9. Sources 10–13 fell to `default: return (code/65536.0f) × 2.5f`
which is only valid for AGND (source 0) and LVIN (source 8).

**Correct formulas from datasheet Table 30:**

| Source | Signal        | Formula                                           | Range           |
|--------|---------------|---------------------------------------------------|-----------------|
| 10     | VSENSEP_x     | `(code/65536 × 50) − 20` (DIN_THRESH_MODE=1)     | −20V to +30V    |
| 11     | VSENSEN_x     | `(code/65536 × 50) − 20`                         | −20V to +30V    |
| 12     | I_DO_SRC_x    | `(code/65536 × 0.5)` → voltage across R_SET      | 0–0.5V / R_SET  |
| 13     | AVDD_x        | `(code/65536) × (25/0.52)`                       | 0–48V           |

> Note: Source 10 has two sub-modes. DIN_THRESH_MODE=0 formula is `(code/65536 × 60) − AVDD_HI`
> but requires runtime AVDD_HI supply voltage. Mode=1 (default) formula is used here.
> Source 12 returns voltage across R_SET; caller must divide by R_SET for actual current.

**Fix applied:** Added explicit `case 10:` through `case 13:` in `diagCodeToValue()` in `ad74416h.cpp`.

---

### BUG-08: Stale Comment in tasks.h — rtdExcitationUa Range ⚠️ Documentation only

Comment says: `// RTD excitation current in µA (125 or 250; 0 when not in RES_MEAS)`

Actual values stored: **0, 500, or 1000 µA** (0 = not in RES_MEAS, 500 = 500µA, 1000 = 1mA).
The code is correct; only the comment is wrong.

---

### BUG-09: Stale Comment in tasks.cpp — RTD Current Description ✅ FIXED

**Status: Fixed in `tasks.cpp` and `webserver.cpp`.**

Comment at the CMD_SET_CHANNEL_FUNC / RES_MEAS block said "Default: 250 µA excitation (RTD_CURRENT=1)".
Per datasheet, RTD_CURRENT=1 is **1 mA**, not 250 µA. The code correctly stores 1000.

**Fix applied:** Comment updated to "1 mA excitation (RTD_CURRENT=1)". Also fixed matching stale comment
in `webserver.cpp` RTD config handler ("default 250 µA" → "default 1 mA (RTD_CURRENT bit set)").

---

### BUG-10: Stale Comment in ad74416h_regs.h — CONV_RATE Bit Position ✅ FIXED

**Status: Fixed in `ad74416h_regs.h`.**

AdcRate enum definition comment said `[9:6] CONV_RATE`. Actual bit field is **[11:8]** per datasheet.
The shift (=8) and mask (=0x0F) values were always correct — only the Doxygen comment was wrong.

**Fix applied:** Comment updated to `ADC_CONFIG[11:8] CONV_RATE`.

---

### BUG-11: Wrong Excitation Current Values Throughout — RTD_CURRENT=0/1 Meaning

**Status: Fixed in firmware and all frontend files.**

Historical documentation and all comments assumed RTD_CURRENT=0 → 125µA, 1 → 250µA.
Per AD74416H datasheet (Table 6, page 55): **RTD_CURRENT=0 → 500µA, RTD_CURRENT=1 → 1mA**.

This caused resistance readings to be ~4× too high (dividing by 250µA instead of 1mA).

**Files corrected:** tasks.cpp, tasks.h, bbp.cpp, webserver.cpp, http_transport.rs, commands.rs,
tauri_bridge.rs, data/index.html, Docs/knowledge.md, BugBusterProtocol.md.

---

### BUG-12: Wrong CONV_MUX Sequence During RTD Fix Iterations

**Status: Resolved (superseded by BUG-03 fix).**

During debugging the incorrect resistance reading, three MUX values were tried:
- MUX=3 (hardware default, LF→VSENSEN): 1635Ω → ~1.63× too high even with 1mA correction
- MUX=0 (LF→AGND): 323mV → 323mΩ @ 1mA → **correct** ✓
- MUX=1 (HF→LF, across RSENSE): 1975mV → nonsensical for RTD measurement

MUX=0 (SENSELF to AGND) is the correct measurement for 2-wire RTD. Final fix uses MUX=0.

---

### BUG-13: DAC Output Never Updates — CMD_KEY 0x1C7D Not Written ✅ FIXED

**Status: Fixed in `ad74416h_regs.h` and `ad74416h.cpp`.**

The AD74416H DAC uses a staged write model: writing to the `DAC_CODE` register (0x16–0x19) only
loads a staging register. The output only changes when `CMD_KEY = 0x1C7D` is written to register
0x74. Without this, all DAC outputs remain at their reset value (0V for VOUT, 0mA for IOUT) regardless
of the value written to `DAC_CODE`.

`setDacCode()` in `ad74416h.cpp` wrote only to `DAC_CODE` and never issued the CMD_KEY update.
The constant `CMD_KEY_DAC_UPDATE = 0x1C7D` was also missing from `ad74416h_regs.h`.

**Fix applied:**
- Added `#define CMD_KEY_DAC_UPDATE 0x1C7D` to `ad74416h_regs.h`
- Added `_spi.writeRegister(REG_CMD_KEY, CMD_KEY_DAC_UPDATE)` immediately after the `DAC_CODE` write
  in `setDacCode()`. All callers (`setDacVoltage`, `setDacCurrent`, and the implicit channel-function
  initialisation) now correctly latch the DAC output.

---

### BUG-14: Range 6 ADC Precision Truncated — ±104.17 mV Range ✅ FIXED

**Status: Fixed in `ad74416h.cpp`.**

`_adc_range_params[6]` had `v_offset = -0.104f` and `v_span = 0.208f`.
The exact values are ±(25/6)/2.4 ≈ ±104.1667 mV, giving:

- Correct `v_offset` = **−0.104167 V**
- Correct `v_span`   = **0.208333 V**

The truncation introduced a worst-case error of ~1.67 mV (≈16 LSB at 24-bit resolution).

**Fix applied:** Updated `_adc_range_params[6]` to `{ -0.104167f, 0.208333f }`.

---

### BUG-15: Web UI `isDin()` Matches Wrong Function Codes ✅ FIXED

**Status: Fixed in `data/index.html`.**

`isDin(ch)` tested `f===7||f===8` — code 7 is `RES_MEAS`, code 8 is `DIN_LOGIC`.
DIN channels are `DIN_LOGIC` (8) and `DIN_LOOP` (9).

`hasDo(ch)` had the same off-by-one: tested `f===1||f===2||f===7||f===8`.

**Fix applied:**
- `isDin`: `f===7||f===8` → `f===8||f===9`
- `hasDo`: `f===1||f===2||f===7||f===8` → `f===1||f===2||f===8||f===9`

---

### BUG-16: Web UI `isIinExt()` / `isIinLoop()` Match Wrong HART Function Codes ✅ FIXED

**Status: Fixed in `data/index.html`.**

`fnMap`: `IOUT_HART:10, IIN_EXT_PWR_HART:11, IIN_LOOP_PWR_HART:12`

- `isIinExt()` tested `f===4||f===10` — code 10 is `IOUT_HART` (wrong). Should be `IIN_EXT_PWR_HART` = 11.
- `isIinLoop()` tested `f===5||f===11` — code 11 is `IIN_EXT_PWR_HART` (wrong). Should be `IIN_LOOP_PWR_HART` = 12.

Effect: the "External" / "Loop" pill indicators in the IIN panel highlighted the wrong mode for HART channels,
and `IOUT_HART` would have been mis-identified as an IIN channel.

**Fix applied:**
- `isIinExt`:  `f===4||f===10` → `f===4||f===11`
- `isIinLoop`: `f===5||f===11` → `f===5||f===12`

---

### BUG-17: Web UI `alertBits` Table Has Wrong Bit Positions for ALERT_STATUS ✅ FIXED

**Status: Fixed in `data/index.html`.**

The old `alertBits` array placed fields at completely wrong bit positions relative to the actual
ALERT_STATUS register (0x3F) layout confirmed by `ad74416h_regs.h`:

| Old (wrong)           | Correct                        |
|-----------------------|--------------------------------|
| bit 1 = CAL_MEM       | bit 1 = reserved               |
| bit 2 = SPI_CRC       | bit 2 = SUPPLY_ERR             |
| bit 3 = SPI_SCLK      | bit 3 = SPI_ERR                |
| bit 4 = ADC_ERR       | bit 4 = TEMP_ALERT             |
| bit 5 = SUPPLY        | bit 5 = ADC_ERR                |
| bit 6 = TEMP          | bits 6–7 = reserved            |
| bit 7 = CH_D          | bit 8 = CH_ALERT_A (not C!)    |
| bit 8 = CH_C          | bit 9 = CH_ALERT_B             |
| bit 9 = CH_B          | bit 10 = CH_ALERT_C            |
| bit 10 = CH_A         | bit 11 = CH_ALERT_D            |
| (missing bits 11–15)  | bits 12–15 = HART_ALERT_A–D    |

**Fix applied:** `alertBits` replaced with correct layout including all HART alert bits.

---

## 13. Electrical Specifications Summary

Cross-referenced from datasheet Tables 1–13 (Rev. 0, 3/2025). All at T_A = −40°C to +105°C unless stated.
Test conditions: AVDD_HI = AVDD_LO = +24V, AVSS = −15V, AGND = DGND = 0V, REFIO = 2.5V (ideal), DVCC = 3.3V, AVCC = 5V.

### 13.1 Voltage Output (Table 1)

| Parameter                | Min    | Typ   | Max    | Unit      | Notes                                  |
|--------------------------|--------|-------|--------|-----------|----------------------------------------|
| Resolution               | 16     |       |        | bits      |                                        |
| Output range (unipolar)  | 0      |       | 12     | V         |                                        |
| Output range (bipolar)   | −12    |       | +12    | V         | VOUT_RANGE = 1 in OUTPUT_CONFIG        |
| TUE                      | −0.2   |       | +0.2   | % FSR     |                                        |
| TUE at 25°C              | −0.1   |       | +0.1   | % FSR     |                                        |
| INL                      | −12.0  |       | +12.0  | LSB       | Guaranteed monotonic                   |
| Offset error             | −5.5   |       | +5.5   | mV        | Code 0x0000 loaded to DAC              |
| Headroom                 | 1.85   |       |        | V         | AVDD_HI minus I/OP_x to source 12V    |
| Footroom                 | 1.85   |       |        | V         | I/OP_x minus AVSS to sink −12V        |
| Short-circuit current    |        | 16    |        | mA        | I_LIMIT = 0 (default)                 |
| Short-circuit current    |        | 8     |        | mA        | I_LIMIT = 1                            |
| SC alert activation time |        | 4     |        | ms        | ALARM_DEG_PERIOD = 0                   |
| SC alert activation time |        | 20    |        | ms        | ALARM_DEG_PERIOD = 1                   |
| Max capacitive load      |        |       | 10     | nF        | No C_COMP                              |
| Max capacitive load      |        |       | 2      | µF        | C_COMP = 220pF                         |
| DC output impedance      |        | 0.1   |        | Ω         |                                        |
| Settling time            |        | 40    |        | µs        | 11V step (0.5→11.5V), ±0.05%FSR       |
| Settling time (±22V step)|        | 60    |        | µs        | No C_COMP                              |
| Settling (with C_COMP)   |        | 300   |        | µs        | 11V step, C_COMP = 220pF               |
| Output noise             |        | 0.5   |        | LSB p-p   | 0.1–10Hz bandwidth                    |
| Noise spectral density   |        | 540   |        | nV/√Hz    | 0V–12V range, 1kHz                    |
| Noise spectral density   |        | 750   |        | nV/√Hz    | ±12V range, 1kHz                      |

### 13.2 Current Output (Table 2)

| Parameter              | Min   | Typ   | Max   | Unit   | Notes                                            |
|------------------------|-------|-------|-------|--------|--------------------------------------------------|
| Resolution             | 16    |       |       | bits   |                                                  |
| Output range           | 0     |       | 25    | mA     |                                                  |
| TUE                    | −0.2  |       | +0.2  | % FSR  | Internal R_SENSE not used for regulation         |
| TUE at 25°C            | −0.1  |       | +0.1  | % FSR  |                                                  |
| Offset error           | −10   |       | +10   | µA     |                                                  |
| Headroom               | 3.8   |       |       | V      | AVDD_HI minus I/OP_x to source 25mA             |
| Open-circuit voltage   |       | AVDD_x|       | V      | Output swings to supply when loop open           |
| OC alert time          |       | 4     |       | ms     | ALARM_DEG_PERIOD = 0                             |
| OC alert time          |       | 20    |       | ms     | ALARM_DEG_PERIOD = 1                             |
| Output impedance       |       | 20    |       | MΩ     |                                                  |
| Settling time          |       | 10    |       | µs     | 3.2→23mA step, ±100µA window                    |
| Settling (HART slew)   |       | 60    |       | ms     | 3.2→23mA, HART slew enabled                     |
| Output noise           |       | 0.38  |       | LSB p-p| 0.1–10Hz                                        |
| Noise SD               |       | 2     |       | nA/√Hz | 1kHz, 12.5mA output                             |

### 13.3 Voltage Input (Table 3)

| Parameter              | Min     | Typ   | Max     | Unit      | Notes                        |
|------------------------|---------|-------|---------|-----------|------------------------------|
| Resolution             | 24      |       |         | bits      |                              |
| Input range (SENSELF)  | 0 / −12 |       | 12      | V         | Selectable via CONV_RANGE    |
| TUE                    | −0.1    |       | +0.1    | % FSR     |                              |
| TUE at 25°C            | −0.01   |       | +0.01   | % FSR     |                              |
| Normal mode rejection  |         | 80    |         | dB        | 50Hz ± 1Hz and 60Hz ± 1Hz   |
| Input bias current     | −30     |       | +30     | nA        |                              |
| Input bias at 25°C     |         | ±6    |         | nA        |                              |

### 13.4 Current Input — Externally Powered (Table 4)

| Parameter                      | Min    | Typ  | Max   | Unit | Notes                                          |
|--------------------------------|--------|------|-------|------|------------------------------------------------|
| Resolution                     | 24     |      |       | bits |                                                |
| Input range                    | 0      |      | 25    | mA   | Sensed across 12Ω R_SENSE                     |
| Short-circuit current limit    | 25     | 29   | 35    | mA   | Non-programmable                               |
| TUE                            | −0.1   |      | +0.1  | % FSR|                                                |
| Input impedance (no HART)      |        | 80   |       | Ω    | Includes 12Ω R_SENSE                          |
| Input impedance (HART)         | 270    |      | 390   | Ω    | Includes 12Ω R_SENSE                          |
| Compliance (no HART)           |        | 2.0  | 2.6   | V    | Min voltage at I/OP_x to sink 25mA            |
| Compliance (HART)              |        | 6.4  | 7.5   | V    | Min voltage at I/OP_x to sink 20mA with HART  |

### 13.5 Current Input — Loop Powered (Table 5)

| Parameter                      | Min    | Typ  | Max    | Unit | Notes                                             |
|--------------------------------|--------|------|--------|------|---------------------------------------------------|
| Input range                    | 0      |      | 25     | mA   | Sensed across 12Ω R_SENSE                        |
| HART mode current limit        | 25     | 30   | 35     | mA   | Non-programmable in HART mode                    |
| Input impedance (no HART)      |        | 120  |        | Ω    | Includes 12Ω R_SENSE                             |
| Input impedance (HART)         | 270    |      | 460    | Ω    | Includes 12Ω R_SENSE                             |
| Headroom (no HART)             | 5.0    | 4.0  |        | V    | AVDD_HI minus I/OP_x                             |
| Headroom (HART)                | 8.0    | 6.6  |        | V    | AVDD_HI minus I/OP_x with HART                   |

### 13.6 2-Wire RTD Measurement (Table 6)

| Parameter           | Min   | Typ   | Max   | Unit | Notes                                                         |
|---------------------|-------|-------|-------|------|---------------------------------------------------------------|
| Input range         | 0.001 |       | 4     | kΩ   | 2-wire RTD supported                                          |
| Excitation current  |       | 500   |       | µA   | RTD_CURRENT = 0 (bit 0 = 0)                                  |
| Open-circuit detect |       | 3.85  |       | V    | At SENSEHF_x — voltage > this = open circuit                 |
| Gain error (25°C)   | −0.034|       | +0.034| %    | 100Ω–4kΩ, 500µA, 0–12V range                                |
| Offset error (25°C) | −0.301|       | +0.301| Ω    | 100Ω–4kΩ range                                               |

### 13.7 3-Wire RTD Measurement (Table 7)

| Parameter                   | Min   | Typ | Max   | Unit    | Notes                                         |
|-----------------------------|-------|-----|-------|---------|-----------------------------------------------|
| Input range                 | 0.001 |     | 4     | kΩ      |                                               |
| Excitation current          |       | 500 |       | µA      | RTD_CURRENT = 0                               |
| Excitation current          |       | 1   |       | mA      | RTD_CURRENT = 1                               |
| Current matching (500µA)    | −0.45 |     | +0.45 | %       | Match between I1 and I2                       |
| Current matching (1mA)      | −0.3  |     | +0.3  | %       |                                               |
| Current matching drift      |       | 5   |       | ppm/°C  |                                               |
| Open-circuit detect (VSENSEN)|      | 2.4 |       | V       |                                               |
| Open-circuit detect (SENSEHF)|      | 3.7 |       | V       |                                               |
| Accuracy: 1Ω–40Ω            |       |     |       |         | 1mA, ±104mV range (e.g. Cu10)                |
| Gain error (25°C)           | −0.030|     | +0.030| %       |                                               |
| Offset error (25°C)         | −0.021|     | +0.021| Ω       |                                               |
| Accuracy: 10Ω–400Ω          |       |     |       |         | 1mA, 0–625mV range (e.g. Pt100)              |
| Gain error (25°C)           | −0.015|     | +0.015| %       |                                               |
| Offset error (25°C)         | −0.034|     | +0.034| Ω       |                                               |

### 13.8 Digital Input Logic (Table 8)

| Parameter                    | Min    | Typ     | Max    | Unit | Notes                                         |
|------------------------------|--------|---------|--------|------|-----------------------------------------------|
| Unbuffered input data rate   |        |         | 200    | kHz  | Via VIOUT_x (DIN_INPUT_SELECT = 0)            |
| Buffered input data rate     |        | 20      |        | kHz  | Via VSENSEP_x (DIN_INPUT_SELECT = 1)          |
| Input voltage range          | −45    |         | +45    | V    |                                               |
| Open-circuit detect current  | 0.05   |         | 0.35   | mA   | For IEC 61131-2 Type 3D compliance            |
| Short-circuit detect current | 6      |         |        | mA   | For IEC 61131-2 Type 3D                       |
| Current sink Range 0 max     |        |         | 3.7    | mA   | 2.8kΩ series resistor                         |
| Current sink Range 0 res.    |        | 120     |        | µA   |                                               |
| Current sink Range 1 max     |        |         | 7.4    | mA   | 1kΩ series resistor                           |
| Current sink Range 1 res.    |        | 240     |        | µA   |                                               |
| Threshold range              | AVSS+2 |         | AVDD−1.5| V   | Programmable trip level                       |
| Fixed threshold resolution   |        | 0.5     |        | V    | DIN_THRESH_MODE = 1                           |
| Fixed threshold hysteresis   |        | 0.5     |        | V    |                                               |
| Threshold @ code 55          | 8.0    | 8.5     | 8.8    | V    | IEC 61131-2 Type I/II/III rising trip         |

### 13.9 Digital Output (Table 10)

| Parameter              | Min  | Typ  | Max  | Unit | Notes                                                |
|------------------------|------|------|------|------|------------------------------------------------------|
| DO_VDD supply range    | 10   | 24   | 35   | V    | Field supply                                         |
| SC voltage Vsc1        | 160  |      | 240  | mV   | With 0.15Ω R_SET → clamp at ~1.3A                   |
| SC voltage Vsc2        | 80   |      | 120  | mV   | With 0.15Ω R_SET → clamp at ~667mA                  |
| SC clamp time          |      | 2    |      | µs   | Time for short-circuit clamp to engage at 0Ω         |
| T1 time range          | 0.1  |      | 100  | ms   | Programmable                                         |
| T2 time range          | 0.1  |      | 100  | ms   | Programmable                                         |
| FET on time (t_ON)     |      | 15   |      | µs   | SYNC rising → 90% settle, C_ISS < 500pF             |
| FET off time (t_OFF)   |      | 4    |      | µs   | SYNC rising → FET disable                            |
| Gate drive voltage     | −12  | −10  | −8   | V    | DO_SRC_GATE_x with respect to DO_VDD                |

### 13.10 ADC Specifications (Table 11)

| Parameter              | Min  | Typ    | Max   | Unit    | Notes                                          |
|------------------------|------|--------|-------|---------|------------------------------------------------|
| Resolution             | 24   |        |       | bits    | No missing codes                               |
| Max conversion rate    |      | 19.2   |       | kSPS    | Diagnostics only (19.2kSPS)                    |
| Normal max rate        |      | 9.6    |       | kSPS    | Channel measurements                           |
| CMRR                   |      | 95     |       | dB      |                                                |
| Abs. input voltage     | AVSS+2|       | AVDD_HI−0.2|V  | At SENSEHF_x or SENSELF_x                     |
| TUE (±12V range)       | −0.1 |        | +0.1  | % FSR   |                                                |
| Temp sensor accuracy   |      | ±5     |       | °C      | Guaranteed by design                           |
| Temp sensor resolution |      | 0.11   |       | °C      |                                                |

### 13.11 HART Modem (Table 12)

| Parameter              | Min  | Typ  | Max  | Unit   | Notes                                            |
|------------------------|------|------|------|--------|--------------------------------------------------|
| Modem power-up time    |      | 50   |      | µs     | After MODEM_PWRUP bit set                        |
| DCD assert threshold   | 85   | 100  | 110  | mV p-p | Range for carrier detect assertion               |
| Mark frequency         |      | 1200 |      | Hz     | Logic "1"                                        |
| Space frequency        |      | 2200 |      | Hz     | Logic "0"                                        |
| Frequency error        | −1.0 |      | +1.0 | %      |                                                  |
| TX output (IOUT mode)  | 400  |      | 600  | mV p-p | At I/OP_x, 500Ω load                            |
| TX output (IIN mode)   | 400  |      | 600  | mV p-p | At I/OP_x, 1kΩ load                             |
| FIFO depth             |      | 32   |      | bytes  | Both TX and RX FIFOs                             |

### 13.12 General Specifications (Table 13) — Key Values

| Parameter                  | Min   | Typ   | Max   | Unit    | Notes                                          |
|----------------------------|-------|-------|-------|---------|------------------------------------------------|
| Internal reference output  | 2.495 | 2.500 | 2.505 | V       | T_A = 25°C; REF_EN bit in PWR_OPTIM_CONFIG    |
| Reference temp coefficient |       | 10    |       | ppm/°C  |                                                |
| Reference drift vs time    |       | 500   |       | ppm FSR | After 1000 hrs, T_A = 90°C                    |
| Sense pin input bias       | −25   | ±2    | +25   | nA      | SENSEHF, SENSELF, VSENSEP, VSENSEN             |
| Burnout currents           |       | 0.1/1/10|    | µA      | 3 programmable levels (source or sink)         |
| Temperature alert          |       | 115   |       | °C      | Junction temp; sets TEMP_ALERT in ALERT_STATUS |
| Temperature reset          |       | 145   |       | °C      | Junction temp; full device reset if enabled    |
| Logic high input (V_IH)    | 0.7×DVCC|    |       | V       | SCLK, SDI, RESET, SYNC, GPIO_x (inputs)        |
| Logic low input (V_IL)     |       |       | 0.2×DVCC| V    |                                                |
| GPIO_A–D drive (source/sink)|      | 3     |       | mA      |                                                |
| GPIO_E–F drive (source/sink)|      | 250   |       | µA      | Reduced drive, no HART routing                 |
| ALERT/ADC_RDY sink         |       | 2.5   |       | mA      | Open-drain outputs, V_OL < 0.4V               |
| Device power-up time       |       | 1     |       | ms      | After supplies applied                         |
| Device reset time          |       | 1     |       | ms      | Calibration memory reload                      |
| Channel init time          |       | 300   |       | µs      | After CH_FUNC_SETUP write (non-HART)           |
| IOUT_HART channel init     |       | 4.2   |       | ms      | VIOUT_DRV_EN_DLY = 0                           |
| AVDD_HI                    | 6     | 24    | 28.8  | V       |                                                |
| AVDD_LO                    | 6     | 14.5  | 28.8  | V       |                                                |
| AVSS                       | −18   | −15   | −2.5  | V       |                                                |
| DVCC                       | 2.7   | 3.3   | 5.5   | V       |                                                |
| AVCC                       | 4.5   | 5.0   | 5.5   | V       |                                                |
| DO_VDD                     | 10    | 24    | 35    | V       | Field supply for digital output FET            |

### 13.13 Power Supply Undervoltage Thresholds (falling)

| Supply   | Threshold | Action on trigger                                                |
|----------|-----------|------------------------------------------------------------------|
| AVDD_HI  | 5.4V      | AVDD_HI_ERR in SUPPLY_ALERT_STATUS; channels revert to HIGH_IMP |
| AVDD_LO  | 5.4V      | AVDD_LO_ERR                                                      |
| AVSS     | −1.6V     | AVSS_ERR                                                         |
| AVCC     | 4.1V      | AVCC_ERR; disables digital output channels                       |
| DVCC     | 2.3V      | DVCC_ERR; triggers internal POR                                  |
| DO_VDD   | 9.2V      | DO_VDD_ERR; disables digital output channels                     |

### 13.14 SPI Timing (Table 14)

| Parameter | Symbol | Value (30pF load) | Value (50pF load) | Unit |
|-----------|--------|-------------------|-------------------|------|
| SCLK cycle time | t1 | 50 min | 62.5 min | ns |
| SCLK high time | t2 | 17 min | 23 min | ns |
| SCLK low time | t3 | 17 min | 23 min | ns |
| SYNC high time | t6 | 420 min | 420 min | ns |
| Data setup time | t7 | 5 min | 5 min | ns |
| Data hold time | t8 | 5 min | 5 min | ns |
| SCLK→SDO valid | t10 | 23 max | 28 max | ns |
| SYNC fall→SDO valid | t11 | 22 max | 25 max | ns |
| ADC_RDY pulse | t14 | 25 typ | 25 typ | µs |

Max SCLK frequency: 20 MHz (t1 = 50ns min).

### 13.15 Absolute Maximum Ratings (Table 15)

> ⚠️ Exceeding these values may permanently damage the device.

| Parameter | Rating |
|-----------|--------|
| AVDD_HI, AVDD_LO to AGND | −0.3V to +36V |
| AVSS to AGND | −20V to +0.3V |
| DVCC, AVCC to AGND | −0.3V to +6V |
| DO_VDD to AGND | −0.3V to +40V |
| REFIO, LVIN to AGND | −0.3V to AVCC + 0.3V |
| SENSEHF_x, SENSELF_x, VSENSEP_x, VSENSEN_x to AGND | ±50V |
| VIOUT_x, CCOMP_x to AGND | ±50V |
| Digital inputs (RESET, SYNC, SCLK, SDI) | −0.3V to DVCC + 0.3V |
| Operating temperature | −40°C to +105°C |
| Storage temperature | −65°C to +150°C |
| T_J maximum | 125°C |
| Thermal resistance θJA | 23.8°C/W |
| Thermal resistance θJC | 0.8°C/W |
| ESD: HBM | ±1750V (Class 1C) |
| ESD: FICDM | ±500V (Class C2a) |

---

## 14. SPI Protocol — Complete Reference

### 14.1 Physical Interface

- **Mode**: CPOL=0, CPHA=0 — data clocked on rising edge, sampled on falling edge of SCLK
- **Compatible with**: SPI, QSPI™, MICROWIRE™, DSP standards
- **Max clock**: 20 MHz
- **Addressing**: 2-bit device address via AD0/AD1 pins — up to 4 devices on one bus
- **Frame width**: 40 bits (32 data + 8 CRC) — exactly 40 SCLK falling edges required per frame

### 14.2 SPI Write Frame (MSB first)

```
Bit 39    : 0  (write indicator)
Bit 38    : 0
Bits[37:36]: Device address (set by AD0/AD1 pins)
Bits[35:32]: Reserved (set to 0)
Bits[31:24]: Register address (8-bit)
Bits[23:8] : Data (16-bit)
Bits[7:0]  : CRC-8 (polynomial below)
```

SYNC must be held low for the full 40 clocks then returned high. Writes that do not end with
exactly 40 falling SCLK edges are ignored and SPI_ERR is asserted in ALERT_STATUS.

### 14.3 SPI 2-Stage Read (Readback)

Reading requires **two consecutive SPI frames**:

**Stage 1 — Request:** Write to READ_SELECT register (address 0x6E):
```
Bits[39:38]= 00 (write), Bits[37:36]= device addr, Bits[31:24]= 0x6E,
Bits[23:16]= 0x00, Bits[15:8]= readback_addr[7:0], Bits[7:0]= CRC
```

**Stage 2 — Read:** Send any valid frame (NOP or another write). Data shifts out on SDO:
```
Bit 39    : 1  (read indicator, validates frame — detects stuck SDO low)
Bit 38    : 0
Bits[37:36]: Device address
Bit 35    : ALERT (mirrors ALERT pin status)
Bit 34    : HART_ALERT (OR of all HART_ALERT_x bits)
Bit 33    : CHANNEL_ALERT (OR of all CH_x_ALERT bits)
Bit 32    : ADC_RDY (mirrors LIVE_STATUS ADC_DATA_RDY bit)
Bits[31:24]: READBACK_ADDR (echoes what was requested)
Bits[23:8] : Register data (16-bit)
Bits[7:0]  : CRC-8
```

> Every SPI read transaction simultaneously returns device status bits (ALERT, HART_ALERT,
> CHANNEL_ALERT, ADC_RDY) — these can be used for fast polling without separate reads.

### 14.4 SPI CRC

**Polynomial:** C(x) = x⁸ + x² + x¹ + 1 (0x07 in standard notation)

The 8-bit CRC covers the full 32 data bits. The device checks CRC on every write. On failure:
- Data is ignored (register not written)
- `SPI_ERR` bit set in ALERT_STATUS
- ALERT pin asserts (if SPI_ERR not masked)

Clear SPI_ERR by writing 1 to that bit. To ignore CRC errors, mask via ALERT_MASK.

### 14.5 Burst Read Mode

Allows reading multiple consecutive registers in one extended transaction:

1. Configure `BURST_READ_SEL` (0x6F) — bitmask of which registers to include
2. Set `READ_SELECT` (0x6E) to the address of the first register
3. Issue standard 2-stage readback (Stage 1 + first read of Stage 2)
4. Keep SYNC low; each additional 24 SCLK clocks clock out one more register's 16-bit data + 8-bit CRC
5. Unselected registers in `BURST_READ_SEL` are skipped
6. Return SYNC high to end burst

> Total SCLK rising edges for burst: 40 + (n × 24), where n = number of additional registers.
> Default BURST_READ_SEL = 0xFFFF (all registers included).
> **Writes are not supported during burst mode.**

The firmware currently reads ADC results individually (not in burst mode) via the SPI grant handshake.

### 14.6 SPI SCLK Count Validation

The AD74416H validates exactly 40 falling SCLK edges per standard frame. Wrong count → `SPI_ERR`.
For burst, it validates 40 + (n × 24) rising edges. This protects against partial transmissions
in noisy environments.

### 14.7 ADC_RDY Signal Behaviour

- `ADC_RDY` physical pin: active-low open-drain, pulses low for ~25µs when new data available
- `ADC_RDY` bit in SDO read frame: reflects `LIVE_STATUS.ADC_DATA_RDY` (not identical to pin)
- `ADC_RDY_CTRL` bit in ADC_CONV_CTRL[13]: when set, ADC_RDY only pulses for the last channel
  in the conversion sequence (not for every channel)

---

## 15. Power Supply and Adaptive Power Switching

### 15.1 Supply Rail Summary

| Rail     | Min   | Typ   | Max   | Description                                      |
|----------|-------|-------|-------|--------------------------------------------------|
| AVDD_HI  | 6V    | 24V   | 28.8V | High-side analog supply; headroom rail           |
| AVDD_LO  | 6V    | 14.5V | 28.8V | Low-side analog supply; adaptive switching target|
| AVSS     | −18V  | −15V  | −2.5V | Negative analog supply (footroom)               |
| AVCC     | 4.5V  | 5V    | 5.5V  | Low-voltage analog supply                        |
| DVCC     | 2.7V  | 3.3V  | 5.5V  | Digital supply (sets logic levels)               |
| DO_VDD   | 10V   | 24V   | 35V   | Field supply for external PFET digital output    |
| LVIN     | 0     |       | 2.5V  | Low-voltage auxiliary input (diagnostic only)    |

> **Rule:** AVDD_HI must always be ≥ AVDD_LO. In single-supply configurations, tie AVDD_HI
> and AVDD_LO together via a 2kΩ resistor and place a Schottky diode (BAT41KFILM) between them.
> DVCC and AVCC may be connected together if fewer supply rails are desired.

### 15.2 Adaptive Power Switching

When a channel is in IOUT or IOUT_HART mode, the AD74416H adaptively selects which AVDD rail
(HI or LO) provides the output drive current, based on the required headroom:

- High-current / high-compliance output → AVDD_HI provides the drive
- Low-compliance output → AVDD_LO is sufficient → saves ~40% of power dissipation

This switching is **automatic** when CH_FUNC = IOUT or IOUT_HART. Manual lock available via
`AVDD_SELECT` bits [11:10] in `OUTPUT_CONFIG[n]`:

| AVDD_SELECT | Behaviour                    |
|-------------|------------------------------|
| 0b00        | Lock to AVDD_HI              |
| 0b10        | Track (adaptive, default for IOUT) |

### 15.3 Recommended Dual-AVDD Supply Pairs (Table 20)

| AVDD_HI       | AVDD_LO       | Max Load (Ω) |
|---------------|---------------|--------------|
| 25.1V ± 5%    | 14.3V ± 5%    | 800          |
| 24.0V ± 5%    | 13.7V ± 5%    | 760          |
| 22.5V ± 5%    | 13.0V ± 5%    | 700          |
| 21.2V ± 5%    | 12.3V ± 5%    | 650          |
| 19.8V ± 5%    | 11.6V ± 5%    | 600          |
| 18.5V ± 5%    | 10.9V ± 5%    | 550          |
| 17.2V ± 5%    | 10.3V ± 5%    | 500          |

> Max load current assumed to be 25mA.

### 15.4 Typical Quiescent Currents

| Configuration               | AVDD_HI | AVDD_LO | AVSS   | DVCC   | AVCC   |
|-----------------------------|---------|---------|--------|--------|--------|
| 4× HIGH_IMP, ADC disabled   | 7.5mA   | 0.3mA   | 7.8mA  | 2.8mA  | 3.4mA  |
| 4× Active analog/DIN, ADC on| 10.0mA  | 0.7mA   | 8.0mA  | 2.8mA  | 7.2mA  |
| 4× VOUT, ADC off            | 11.0mA  | —       | 11.0mA | 2.8mA  | 8.5mA  |
| 4× RES_MEAS (1mA exc.), ADC | 16.5mA  | 0.7mA   | 8.0mA  | 2.8mA  | 7.2mA  |

### 15.5 Decoupling Recommendations (Table 38)

| Rail         | Capacitors                  | Notes                          |
|--------------|-----------------------------|--------------------------------|
| AVDD_HI      | 10µF (50V) + 100nF (50V)   | Both in parallel close to IC   |
| AVDD_LO      | 10µF (50V) + 100nF (50V)   |                                |
| AVSS         | 10µF (50V) + 100nF (50V)   |                                |
| AVCC         | 10µF (16V) + 100nF (16V)   |                                |
| DVCC         | 10µF (16V) + 100nF (16V)   |                                |
| DO_VDD       | 10µF (100V)                 |                                |
| LDO1V8       | 2.2µF (6.3V) — C0805C225K9RAC7800 | Place close to LDO1V8 pin |
| REFIO        | 22–50nF (6.3V)              | Anti-aliasing on REFIO pin     |

---

## 16. GPIO — Complete Reference

### 16.1 GPIO Pin Overview

The AD74416H has 6 GPIO pins: GPIO_A through GPIO_F.

| GPIO  | HART Capable | Drive Strength | Note                                   |
|-------|-------------|----------------|----------------------------------------|
| GPIO_A | Yes        | Full (3mA)     | HART modem UART routing supported      |
| GPIO_B | Yes        | Full (3mA)     |                                        |
| GPIO_C | Yes        | Full (3mA)     |                                        |
| GPIO_D | Yes        | Full (3mA)     |                                        |
| GPIO_E | No         | Reduced (250µA)| 200ns lower latency than A–D           |
| GPIO_F | No         | Reduced (250µA)|                                        |

All GPIO pins have a weak internal pull-down enabled by default (`GP_WK_PD_EN` bit). Must be
disabled for output use (write 0 to `GP_WK_PD_EN` in `GPIO_CONFIG[n]`).

### 16.2 GPIO_CONFIG[n] Registers (0x32–0x37)

| Bits  | Name            | Description                                             |
|-------|-----------------|---------------------------------------------------------|
| [2:0] | GPIO_SELECT     | 000=Hi-Z, 010=GPO/GPI (SEL_GPIO), 011=SEL_DIN output   |
| 4     | GPO_DATA        | Output value when GPIO_SELECT = 010                     |
| 5     | GPI_DATA        | Input value when configured as input (read-only)        |
| 6     | GP_WK_PD_EN     | Enable weak pull-down (default 1 — disable for outputs) |

GPIO_SELECT codes:
- `0b000` = High impedance (default after reset)
- `0b010` = SEL_GPIO — drive GPO_DATA out or read GPI_DATA in
- `0b011` = SEL_DIN — output the debounced DIN comparator signal for the corresponding channel

### 16.3 Monitoring DIN Comparator on GPIO

To route the DIN comparator output to a GPIO pin (useful for high-speed hardware monitoring):

1. Set GPIO_SELECT = 0b010, write GPO_DATA to desired initial state (GPIO_CONFIG)
2. Configure channel function to DIN_LOGIC or DIN_LOOP (CH_FUNC_SETUP)
3. Configure comparator in DIN_CONFIG0 (set COMPARATOR_EN, DIN_INV_COMP_OUT as needed)
4. Wait for debounce period (DEBOUNCE_TIME in DIN_CONFIG0)
5. Change GPIO_SELECT to 0b011 (SEL_DIN) in GPIO_CONFIG

### 16.4 HART GPIO Routing

HART UART signals (TX, RX, RTS) for any channel can be routed to GPIO_A–D:
- `HART_GPIO_IF_CONFIG` register: selects which GPIO pins carry the HART UART interface
- `HART_GPIO_MON_CONFIG[n]` registers: selects which GPIO pins monitor HART signals for a specific channel
- GPIO_E and GPIO_F **cannot** carry HART signals

---

## 17. Reset, Watchdog, and Fault Handling — Complete Reference

### 17.1 Reset Methods

All reset types produce identical results: registers → default values, channels → HIGH_IMP,
calibration memory reload begins (1ms). After reset, `RESET_OCCURRED` bit is set in ALERT_STATUS.

| Method | How | Notes |
|--------|-----|-------|
| Hardware reset | Pulse RESET pin low ≥ 50µs | Pin 49 (active low) |
| Software reset | Write 0x15FA then 0xAF51 to CMD_KEY (0x74) | No intervening SPI frames |
| Broadcast software reset | Write 0x1A78 then 0xD203 to BROADCAST_CMD_KEY (0x75) | Resets all devices on bus |
| Thermal reset | Die T_J reaches 145°C | Only if THERM_RST_EN bit set in THERM_RST (0x73) |
| Watchdog reset | No valid SPI frame within WDT timeout | Only if WDT_EN bit set in WDT_CONFIG (0x3B) |
| POR | DVCC or LDO1V8 falls below threshold | Internal power-on reset |

> ⚠️ After reset: do not access registers until calibration memory has reloaded (1ms).
> If SPI is attempted too early, `CAL_MEM_ERR` in SUPPLY_ALERT_STATUS will be set. Device must
> be reset again if CAL_MEM_ERR is asserted.

### 17.2 Software Reset Procedure

```
1. Write 0x15FA to CMD_KEY (address 0x74)
2. Write 0xAF51 to CMD_KEY (address 0x74) — MUST be consecutive, no other SPI frame between
3. Wait ≥ 1ms for calibration memory reload
4. Read and clear ALERT_STATUS (write 0xFFFF)
5. Resume normal operation
```

### 17.3 Watchdog Timer (WDT)

Protects against SPI communication loss. When enabled, the AD74416H resets if no valid SPI
frame arrives within the configured timeout.

Enable: set `WDT_EN` bit in `WDT_CONFIG` (0x3B). The first SPI transaction after enable
starts the countdown. The timer resets on every valid SPI frame.

| WDT_TIMEOUT Code (Hex) | Timeout |
|------------------------|---------|
| 0                      | 1ms     |
| 1                      | 5ms     |
| 2                      | 10ms    |
| 3                      | 25ms    |
| 4                      | 50ms    |
| 5                      | 100ms   |
| 6                      | 250ms   |
| 7                      | 500ms   |
| 8                      | 750ms   |
| 9                      | 1000ms  |
| A                      | 2000ms  |
| other                  | 1000ms  |

### 17.4 Fault and Alert Architecture

```
                       ALERT pin (active low, open-drain)
                             ↑ (any unmasked bit)
                       ALERT_STATUS (0x3F)
                      /         |          \           \
            CH_x_ALERT    HART_ALERT_x   SUPPLY_ERR   ADC_ERR
                ↓               ↓              ↓
   CHANNEL_ALERT_STATUS[n]  HART_ALERT_STATUS[n]  SUPPLY_ALERT_STATUS
```

**Alert register hierarchy:**
1. `ALERT_STATUS` (0x3F): top-level; ALERT pin asserts when any unmasked bit is set
2. `CHANNEL_ALERT_STATUS[n]` (0x58–0x5B): per-channel faults (OC, SC, VIOUT_SHUTDOWN)
3. `SUPPLY_ALERT_STATUS` (0x57): power supply undervoltage faults + CAL_MEM_ERR
4. `HART_ALERT_STATUS[n]`: HART communication errors and FIFO status

**Clearing alerts:**
- Write 1 to each bit to clear (Write-1-to-Clear = W1C)
- Write 0xFFFF to clear all bits in a register in one operation
- Sub-registers (CHANNEL_ALERT_STATUS, SUPPLY_ALERT_STATUS, HART_ALERT_STATUS) must be
  cleared BEFORE clearing ALERT_STATUS, otherwise ALERT_STATUS bits may immediately re-assert

**LIVE_STATUS (0x40):** Real-time mirror; bits clear automatically when condition resolves.
Use for polling without disturbing latched ALERT_STATUS.

**Masking:** Each alert register has a paired mask register. Writing 1 to a mask bit prevents
that alert from asserting the ALERT pin (alert is still visible in the status register).

### 17.5 Burnout Currents

Burnout currents enable open-circuit detection on voltage and resistance measurement channels.
When the I/OP_x terminal is floating (open circuit), the burnout current pulls the input to
AVDD_HI, and the ADC generates a conversion result at or near full scale.

Configure via `I_BURNOUT_CONFIG[n]` (0x09 + ch × 0x0C):

| Field          | Bits | Description                                      |
|----------------|------|--------------------------------------------------|
| BRN_VIOUT_CURR | [1:0]| Current level: 00=off, 01=0.1µA, 10=1µA, 11=10µA|
| BRN_VIOUT_POL  | 2    | Polarity: 0=source (default for VIN), 1=sink     |

Burnout currents apply to the VIOUT_x pin (visible at I/OP_x screw terminal).
Recommend enabling burnout (source) in HIGH_IMP mode to detect disconnected sensors before
switching to an active function.

### 17.6 Thermal Management

- Temperature alert at 115°C (±5°C): `TEMP_ALERT` bit in ALERT_STATUS
- Temperature reset at 145°C: full device reset, triggered only if `THERM_RST_EN=1` in THERM_RST (0x73)
- Thermal resistance θJA = 23.8°C/W — PCB must have thermal vias and copper pour
- Maximum junction temperature T_J = 125°C — must not be exceeded
- Power dissipation managed by adaptive power switching (saves ~40% in IOUT mode)

> With 4 channels all active at 25mA IOUT, AVDD_HI = 24V, AVSS = −15V, load at 6V:
> P_dissipated ≈ 4 × (24V − 6V) × 25mA = 1.8W → ΔT = 1.8W × 23.8°C/W ≈ 42.8°C at T_A = 25°C → T_J ≈ 68°C ✓
> Use dual AVDD (Table 20 recommended pairs) to reduce headroom voltage.

---

## 18. Voltage Output — Feedback Sensing Modes

### 18.1 Overview

Voltage output channels support three feedback topologies. Select via `VOUT_4W_EN` bit in `OUTPUT_CONFIG[n]`:

| Mode    | Connection              | VOUT_4W_EN | Use case                              |
|---------|-------------------------|------------|---------------------------------------|
| 2-wire  | I/OP_x → VSENSEP_x (serial R) | 0    | Simple loads, no separate sense wires |
| 3-wire  | I/OP_x → ISP_x, VSENSEP_x sense | 0  | Remote load, one Kelvin sense wire    |
| 4-wire  | ISP_x + ISN_x Kelvin sense | 1         | Precision loads, two sense wires      |

### 18.2 2-Wire Feedback

Connect the I/OP_x screw terminal directly to the VSENSEP_x pin through a 2kΩ serial protection
resistor (always required). The VSENSEP_x serial resistor limits input current during high-voltage events.

No external 10kΩ feedback resistor needed for 2-wire. The ADC default range is ±312.5mV across RSENSE.

### 18.3 3-Wire Feedback

Use an ISP_x terminal connected to VSENSEP_x via a 10kΩ feedback resistor. The VSENSEP_x pin
senses the actual load voltage. `VOUT_4W_EN = 0` (same register setting as 2-wire).

The spare VSENSEN_x pin may be used as an auxiliary ADC input (diagnostic source 11 = VSENSEN_x).

### 18.4 4-Wire Feedback

Both ISP_x (→ VSENSEP_x) and ISN_x (→ VSENSEN_x) are connected. The AD74416H measures the
differential voltage at the load Kelvin sense points.

Required external components:
- 10kΩ between VSENSEP_x and I/OP_x
- 100kΩ pull-down between VSENSEN_x and AGND (prevents faulty state if ISN_x disconnects)

Set `VOUT_4W_EN = 1` in OUTPUT_CONFIG[n].

### 18.5 ADC in Voltage Output Mode

Hardware auto-sets: MUX=1 (SENSEHF to SENSELF, across internal RSENSE), RANGE=2 (±312.5mV).

This measures the current through RSENSE for short-circuit monitoring:
```
I_RSENSE = (V_MIN + (ADC_CODE/16,777,216) × V_RANGE) / R_SENSE
         = (−0.3125 + (ADC_CODE/16,777,216) × 0.625) / 12
```

Where V_MIN = −0.3125V, V_RANGE = 0.625V. Negative result = current sinking (AD74416H sourcing).

---

## 19. Digital Input — Complete Reference

### 19.1 Input Signal Path

Signal enters at I/OP_x screw terminal, routed via:
- **Buffered path** (VSENSEP_x): lower speed (≤20kHz), filtered, recommended for normal use
- **Unbuffered path** (VIOUT_x): high speed (≤200kHz), no buffer, use `DIN_INPUT_SELECT = 1` in DIN_CONFIG1

Select path via `DIN_INPUT_SELECT` bit in DIN_CONFIG1:
- 0 = VIOUT_x (unbuffered, high speed — default)
- 1 = VSENSEP_x (buffered)

> Note: If using unbuffered mode while sourcing/sinking current to the load, account for the
> voltage drop across RSENSE (12Ω) and VIOUT line protector (15Ω) when setting threshold voltage.

### 19.2 Threshold Formulas

**AVDD_HI-relative mode** (DIN_THRESH_MODE = 0, DIN_CONFIG1 bit 7 = 0):
```
V_THRESH = V_AVDD_HI × ((Code − 48) / 50)
```

**Fixed voltage mode** (DIN_THRESH_MODE = 1, DIN_CONFIG1 bit 7 = 1):
```
V_THRESH = V_REFIO × ((Code − 38) / 5)
```
where V_REFIO = 2.5V (internal reference). Resolution ≈ 0.5V. Hysteresis ≈ 0.5V.

Maximum code = 98 (7 bits, max programmable value). Full range: AVSS+2V to AVDD_HI−1.5V.

### 19.3 IEC 61131-2 Recommended Configurations

**Type I and Type III:**
```
DIN_SINK_RANGE = 0     (2.8kΩ range)
DIN_SINK = 0x14        (decimal 20 → ~2.4mA typical sink)
DIN_THRESH_MODE = 1    (fixed voltage mode)
COMP_THRESH = 0x37     (decimal 55 → ~8.5V trip point)
```
Result: 2.4mA current sink, 8.5V rising trip, IEC 61131-2 Type I/III compliant.

**Type II:**
```
DIN_SINK_RANGE = 1     (1kΩ range)
DIN_SINK = 0x1D        (decimal 29 → ~6.96mA typical sink)
DIN_THRESH_MODE = 1
COMP_THRESH = 0x37
```
Result: 6.96mA current sink, 8.5V rising trip, IEC 61131-2 Type II compliant.

**Type 3D (with OC/SC detection):**
```
DIN_SINK_RANGE = 0
DIN_SINK = 0x0F        (decimal 15 → ~1.6mA typical)
DIN_OC_DET_EN = 1      (enable open-circuit detection)
DIN_SC_DET_EN = 1      (enable short-circuit detection)
DIN_THRESH_MODE = 1
COMP_THRESH = 0x37
```
Result: 1.6mA sink, 8.5V trip, OC detected if sink < 220µA, SC detected if > 4mA extra sink.

### 19.4 Debounce Times (Table 21)

| Code (Hex) | Time (ms)  | Code | Time (ms) | Code | Time (ms) |
|------------|------------|------|-----------|------|-----------|
| 0x00       | Bypass     | 0x09 | 0.1301    | 0x12 | 1.8008    |
| 0x01       | 0.0130     | 0x0A | 0.1805    | 0x13 | 2.4008    |
| 0x02       | 0.0187     | 0x0B | 0.2406    | 0x14 | 3.2008    |
| 0x03       | 0.0244     | 0x0C | 0.3203    | 0x15 | 4.2008    |
| 0x04       | 0.0325     | 0x0D | 0.4203    | 0x16 | 5.6008    |
| 0x05       | 0.0423     | 0x0E | 0.5602    | 0x17 | 7.5007    |
| 0x06       | 0.0561     | 0x0F | 0.7504    | 0x18 | 10.0007   |
| 0x07       | 0.0756     | 0x10 | 1.0008    | 0x1A | 18.0006   |
| 0x08       | 0.1008     | 0x11 | 1.3008    | 0x1F | 75.0000   |

**Debounce Mode 0 (default):** Counts edge samples in both directions; changes state when target count reached even if signal glitches back.

**Debounce Mode 1:** Resets counter on reversal; input must be stable at new state for full debounce time.

### 19.5 32-Bit Edge Counter

Available in DIN_LOGIC and DIN_LOOP modes. Counts debounced edges.

- Enable: `COUNT_EN` bit 15 in DIN_CONFIG0
- Read: upper 16 bits from `DIN_COUNTER_UPR[n]` **first** (reading UPR latches lower), then `DIN_COUNTER[n]`
- Counter resets to 0 on device reset; rolls over at 0xFFFFFFFF → 0
- Counter freezes (stops counting) if COUNT_EN = 0
- Invert which edges are counted via `DIN_INV_COMP_OUT` (bit 14 of DIN_CONFIG0)

---

## 20. Digital Output — Complete Reference

### 20.1 Overview

Digital output drives an **external P-channel PFET** (AD74416H cannot source field current directly).
Recommended FET: Si7113ADN. Gate drive: −8V to −12V referenced to DO_VDD. DO_VDD = 10–35V.

Key signal pins per channel (x = A, B, C, D):
- `DO_SRC_GATE_x` (pin 22/24/28/30): gate drive to external PFET
- `DO_SRC_SNS_x` (pin 23/25/27/29): sense input (connect to drain of PFET, or to DO_VDD if not used)
- `LKG_COMP_x` (pin 2/15/34/47): FET leakage compensation input

### 20.2 Configuration Sequence

After each reset, wait 300µs before configuring digital output (SUPPLY_ALERT_STATUS undervoltage recovery).

```
1. Set DO_MODE bit in DO_EXT_CONFIG[n] (sourcing mode)
2. Set DO_SRC_SEL bit if GPIO-controlled (else SPI-controlled via DO_DATA bit)
3. Set DO_T1 and DO_T2 for short-circuit protection timing
4. Write new value to DO_EXT_CONFIG[n] register
5. To enable output: write DO_DATA = 1 in DO_EXT_CONFIG[n]
```

### 20.3 Short-Circuit Protection — T1/T2 Dual-Stage

```
FET on ─────T1─────── (higher SC limit, Vsc1) ─── if SC detected ────T2──── FET off
```

**T1 phase** (time 0 to T1): Digital output runs at higher short-circuit current limit (Vsc1 = 160–240mV across R_SET). This allows inrush current for capacitive loads. SC alert NOT generated during T1.

**T2 phase** (T1 expires and SC condition exists): Digital output runs at lower limit (Vsc2 = 80–120mV). T2 timer counts only while SC is present; decrements when SC clears. SC alert IS generated in T2.

**After T2 expires:** FET disabled automatically. `DO_TIMEOUT` bit set in `CHANNEL_ALERT_STATUS[n]`.

**Recovery:** DO_DATA = 0 → select DO_MODE → DO_DATA = 1.

Short-circuit current limits with **R_SET = 0.15Ω** (recommended):
- Vsc1: ~1.3A (V_sc1 = 160–240mV / 0.15Ω)
- Vsc2: ~667mA (V_sc2 = 80–120mV / 0.15Ω)

### 20.4 FET Leakage Compensation

External PFET leakage in OFF state can contribute errors to precision analog measurements
on adjacent channels (especially 3-wire RTD and current inputs). The compensation circuit
provides an alternative leakage path through the LKG_COMP_x pin.

Configure: set `FET_SRC_LKG_COMP_EN` bit in `FET_LKG_COMP[n]` register (0x07 + ch × 0x0C).
Hardware connection: LKG_COMP_x pin connected to drain of the external PFET (see Figure 48).

### 20.5 Current Sense Diagnostic

The digital output sourcing current can be monitored via Diagnostic source 12 (I_DO_SRC_x):
```
V_RSET = (DIAG_CODE / 65536) × 0.5    (voltage across R_SET, 0–0.5V range)
I_SOURCE = V_RSET / R_SET              (current in amps)
```
With R_SET = 0.15Ω: I_SOURCE max ≈ 3.3A.

---

## 21. 3-Wire RTD — Complete Reference

### 21.1 How 3-Wire RTD Works

Two matched excitation currents (I1 = I2 = either 500µA or 1mA) flow through two of the
RTD leads. The voltage measured from SENSELF_x to VSENSEN_x is then:

```
V_MEASURED = V(I/OP_x to I/ON_x) − V(lead3)
           = I1 × (R_RTD + RL1) − I2 × RL3
```

If RL1 = RL2 = RL3 (matched leads), lead resistance cancels:
```
V_MEASURED ≈ I1 × R_RTD    (lead resistance error removed)
```

The reference voltage for the ADC is generated across R_REF = 2kΩ + 12Ω = 2012Ω.

### 21.2 Configuration Steps for 3-Wire Pt1000

```
1. Write CH_FUNC_SETUP = 0x0007 (RES_MEAS)
2. Set RTD_MODE_SEL = 1, RTD_ADC_REF = REF1V (1V), RTD_CURRENT = 0 (500µA)
   → RTD_CONFIG = 0x000C (RTD_ADC_REF=1, RTD_MODE_SEL=1, RTD_CURRENT=0)
3. Set CONV_RANGE = RNG_0_12V (range 0) in ADC_CONFIG[n]
4. Set CONV_MUX = LF_TO_AGND (MUX 0) is NOT used — leave at hardware default (MUX 3, LF→VSENSEN)
   Note: for 3-wire, the hardware default MUX=3 (SENSELF to VSENSEN) IS correct
5. Enable conversion: CONV_SEQ = 2 (continuous), set channel bit in CH_EN
```

### 21.3 3-Wire ADC Data Interpretation

**Unipolar range (e.g. 0–12V):**
```
R_RTD = (ADC_CODE / (16,777,216 × ADC_GAIN)) × R_REF
```
Where:
- `R_REF = 2012Ω` (2kΩ filter resistor + 12Ω R_SENSE)
- `ADC_GAIN` depends on range: 0–12V → 1/4.8; 0–625mV → 1/4 (not used for 3-wire Pt1000)
- For Pt1000 with 500µA and 0–12V: ADC_GAIN = 1/4.8, so denominator = 16,777,216/4.8 = 3,495,253

**Bipolar range:**
```
R_RTD = ((ADC_CODE − 8,388,608) / (8,388,608 × ADC_GAIN)) × R_REF
```

### 21.4 2-Wire vs 3-Wire Comparison

| Aspect                | 2-Wire                              | 3-Wire                                    |
|-----------------------|-------------------------------------|-------------------------------------------|
| RTD_MODE_SEL          | 1                                   | 0                                         |
| CONV_MUX              | 0 (SENSELF→AGND)                    | 3 (SENSELF→VSENSEN) — hardware default    |
| Lead resistance error | Present — adds to reading           | Cancelled (if RL1=RL2=RL3)               |
| Reference             | Non-ratiometric (2.5V)              | Ratiometric (1V across R_REF)             |
| Best for              | Short leads, <4kΩ resistance        | Long leads, precision measurements        |
| Firmware support      | ✅ Fully implemented                 | ⚠️ Not yet implemented in firmware        |

> ⚠️ **Firmware Note:** The current firmware only supports 2-wire RTD (`RTD_MODE_SEL=1`, `MUX=0`).
> 3-wire mode (`RTD_MODE_SEL=0`, `MUX=3`) is supported by hardware but not exposed via `CMD_SET_RTD_CONFIG`.

---

## 22. DAC Slew Rate Control

### 22.1 Slew Rate Overview

The digital linear slew rate control limits the rate at which the DAC output steps to a new value.
Useful for: inductive loads (prevents di/dt ringing), HART compatibility requirements, slow settling systems.

Enable via `SLEW_EN` bits [5:4] in `OUTPUT_CONFIG[n]`:

| SLEW_EN | Mode                | Description                                               |
|---------|---------------------|-----------------------------------------------------------|
| 0b00    | Disabled (default)  | Output changes at full slew speed (hardware limited)      |
| 0b01    | SLEW_LIN            | Linear slew rate control (programmable)                   |
| 0b10    | SLEW_HART_COMPL     | HART compliant slew (prevents HART glitches)              |
| 0b11    | (reserved)          |                                                           |

### 22.2 Linear Slew Configuration

Configured in `OUTPUT_CONFIG[n]`:

| Field          | Bits  | Description                                             |
|----------------|-------|---------------------------------------------------------|
| SLEW_LIN_STEP  | [15:14]| Step size: 0=0.8%(512), 1=1.5%(960), 2=6.1%(4000), 3=22.2%(14560) |
| SLEW_LIN_RATE  | [13:12]| Update rate: 0=4.8kHz, 1=76.8kHz, 2=153.6kHz, 3=230.4kHz |

Example: SLEW_LIN_RATE=0 (4.8kHz, step every 208µs), SLEW_LIN_STEP=0 (0.8%, 512 codes):
→ 0-to-full-scale takes 128 steps × 208µs = 26.7ms

Typical slew times for zero-to-full-scale (Table 32):

| Rate    | 0.8% step | 1.5% step | 6.1% step | 22.2% step |
|---------|-----------|-----------|-----------|------------|
| 4.8kHz  | 26.7ms    | 14.4ms    | 3.54ms    | 1.04ms     |
| 76.8kHz | 1.7ms     | 898µs     | 221µs     | 65.1µs     |
| 153.6kHz| 833µs     | 449µs     | 111µs     | 32.6µs     |
| 230.4kHz| 556µs     | 299µs     | 73.8µs    | 21.7µs     |

### 22.3 HART Compliant Slew

Required for current output HART compatibility (prevents DAC glitches during HART bursts).

Sequence to enable:
1. Set channel to IOUT_HART or IIN with HART
2. Wait for HART_COMPL_SETTLED bit in OUTPUT_CONFIG[n] to assert (settling complete)
3. Set SLEW_EN = 0b10 (SLEW_HART_COMPL)

HART compliant slew settling time is 60ms typ for 3.2→23mA step.

### 22.4 Monitoring Slew Progress

`DAC_ACTIVE[n]` (0x0C + ch × 0x0C) — read-only register holds the **currently applied** DAC code
(the code actually driving the output, which changes one step at a time during slewing).

`DAC_CODE[n]` (0x0A + ch × 0x0C) — the **target** code written by the user.

> ⚠️ Do not write a new `DAC_CODE` while slewing. Wait until `DAC_ACTIVE[n]` equals `DAC_CODE[n]`.
> If slewing is disabled before the target is reached, the output stays at DAC_ACTIVE value —
> it does not snap to DAC_CODE.

---

## 23. External Components and Board Design Reference

### 23.1 Required External Components (Table 38)

| Component                 | Value        | Part No.        | Notes                                         |
|---------------------------|--------------|-----------------|-----------------------------------------------|
| SENSEF_x filter resistor  | 2kΩ (0.05%)  | RNCF0603TKY2K00 | Accuracy affects RTD specs directly           |
| SENSEF_x filter capacitor | 4.7nF        | Generic         | Anti-aliasing filter                          |
| Screw terminal cap        | 4.7nF (100V) | Generic         | Loading capacitor at I/OP_x                   |
| TVS diode                 | 36V          | SMBJ36CA        | Surge protection at screw terminals           |
| VSENSEP_x serial resistor | 2kΩ          | Generic         | Limits input current on VSENSEP during surges |
| VSENSEP_x feedback R      | 10kΩ         | Generic         | 3/4-wire voltage output feedback              |
| VSENSEN_x pull-down       | 100kΩ        | Generic         | Prevents fault if ISN_x disconnects           |
| CCOMP_x capacitor         | 220pF (100V) | Generic         | Between CCOMP_x pin and I/OP_x screw terminal |
| R_SENSE (external)        | 12Ω (0.1%, 10ppm/°C) | Generic | Accuracy directly affects current specs      |
| AVDD_HI/LO Schottky diode | 200mA, 50V   | BAT41KFILM      | Between AVDD_HI and AVDD_LO (dual-supply)     |
| Single-AVDD resistor      | 2kΩ (1%)     | Generic         | Between AVDD_HI and AVDD_LO pins              |
| LDO1V8 decoupling cap     | 2.2µF, 6.3V  | C0805C225K9RAC7800 | Close to LDO1V8 pin (pin 61)              |
| External PFET (DO)        | 100V         | Si7113ADN       | Sourcing-only digital output                  |
| R_SET (DO current limit)  | 0.15Ω        | Generic         | Determines short-circuit current limit        |
| Blocking diode (DO)       | 1A           | MSE1PB          | In series with DO field supply                |

### 23.2 Critical Board Layout Rules

1. **SENSEF filter**: place 2kΩ + 4.7nF directly at the R_SENSE pad — minimise trace inductance
2. **CCOMP_x stability**: keep capacitance between CCOMP_x and C_COMP < 10pF to AGND — prevents oscillation
3. **VSENSEP_x stability**: keep parasitic capacitance between VSENSEP_x and 2kΩ series resistor < 10pF
4. **DO_SRC_SNS_x**: route directly to R_SET pad (star connection recommended for all 4 channels)
5. **DO_VDD star ground**: use star topology for DO_VDD and R_SET returns
6. **Thermal**: minimum 4-layer PCB with thermal vias under exposed pad; connect exposed pad to AVSS
7. **SDO ground**: limit SDO ground capacitance to achieve 20MHz SPI operation
8. **Ground planes**: AGND and DGND connect to a single ground plane; I/ON_x screw terminal to same plane

### 23.3 Pin 64 (Exposed Pad)

The exposed pad on the bottom of the package **must** be connected to the AVSS pin. It is both
electrically connected and thermally required. Multiple thermal vias connect it to the board's
bottom copper layer. See JEDEC JESD-51 for via design guidelines.

---

## 24. WS2812B Status LEDs

Three WS2812B RGB LEDs chained on GPIO0 (LED_DIN), providing at-a-glance system health.

### 24.1 LED Assignments

| LED # | Index | Peripheral | Purpose |
|-------|-------|-----------|---------|
| 0 | `LED_ESP` | ESP32 / Connection | USB or WiFi client status |
| 1 | `LED_MUX` | MUX + IO Expander | ADGS2414D + PCA9535 health |
| 2 | `LED_ADC` | AD74416H ADC | SPI health + channel state |

### 24.2 Color Scheme

**LED 0 — ESP32 / Connection:**

| Color | Meaning |
|-------|---------|
| Blue | Client connected (BBP binary mode active or HTTP session) |
| Yellow | Booting / connecting (shown during startup sequence) |
| Green | Operative but no client connected (idle, ready to accept) |
| Red | Fault (unrecoverable system error) |

**LED 1 — MUX & IO Expander:**

| Color | Meaning |
|-------|---------|
| Green | All OK (ADGS2414D write-verify passing, PCA9535 detected and healthy) |
| Yellow | Not configured (PCA9535 not detected, or MUX not yet initialized) |
| Red | Fault (MUX write-verify failed after retries + soft reset recovery, or PCA9535 communication error) |

**LED 2 — ADC (AD74416H):**

| Color | Meaning |
|-------|---------|
| Green | Operative (SPI healthy, at least one channel configured beyond HIGH_IMP) |
| Yellow | Not configured (SPI OK but all 4 channels still in HIGH_IMP after boot) |
| Red | Fault (SPI verification failed, or AD74416H ALERT active) |

### 24.3 Hardware Notes

- Data line: GPIO0 (ESP32-S3), shared with BOOT strapping pin
- R5 (10 kΩ) pull-up to 3V3_BUCK ensures GPIO0 = HIGH during normal boot
- WS2812B uses GRB byte order; driven via ESP-IDF RMT peripheral at 10 MHz
- Default brightness: RGB values capped at 40 to avoid excessive current draw and eye strain
- Refresh rate: ~2 Hz (updated every 500 ms from the main loop task)

---

## 25. ADGS2414D Safety — Write-Verify and Fault Recovery

### 25.1 Write-Verify Protocol

Every switch state write to the ADGS2414D MUX matrix includes a readback verification:

1. **Write** switch states via SPI (daisy-chain or address mode)
2. **Readback** immediately: second SPI transaction reads back the switch data register
3. **Compare** read-back with intended state, byte-by-byte across all devices
4. If mismatch: **retry** up to 3 times (ADGS_MAX_RETRIES)
5. If still failing in **address mode**: attempt the datasheet software reset sequence (0xA3, 0x05 to reg 0x0B), then retry once
6. If still failing in **daisy-chain mode**: declare **FAULT** immediately, because the datasheet says configuration commands are unavailable in daisy-chain mode and exiting it requires a hardware reset
7. In FAULT: force all switches open, set fault flag, LED 1 → RED

### 25.2 Error Registers (Address Mode)

Available when ADGS_NUM_DEVICES = 1 (address mode, breadboard):

| Register | Address | Bits | Description |
|----------|---------|------|-------------|
| ERR_CONFIG | 0x02 | [2:0] | Error enable: bit 0=CRC, bit 1=SCLK count, bit 2=R/W address. Default: 0x06 |
| ERR_FLAGS | 0x03 | [2:0] | Error flags: bit 0=CRC_ERR, bit 1=SCLK_ERR, bit 2=RW_ERR. Read-only, clear with 0x6CA9 |

In daisy-chain mode (PCB, 5 devices), individual register reads are not possible.
Error detection relies on the write-verify readback mechanism instead.

### 25.3 Recovery Sequence (Daisy-Chain Mode)

When write-verify fails after retries in daisy-chain mode:

1. Do **not** attempt software reset through SPI
2. Datasheet rule: in daisy-chain mode all commands target `SW_DATA`, so configuration writes such as `SOFT_RESETB` are not possible
3. Datasheet rule: exiting daisy-chain mode requires a **hardware reset**
4. Therefore the firmware can only declare **FAULT**, open all switches for safety, and wait for a hardware reset / power cycle

---

## 26. ADGS2414D — Datasheet Reference

Cross-referenced against ADGS2414D datasheet Rev. 0 (12/2023, 31 pages).

### 26.1 Device Overview

The ADGS2414D is a 0.56 Ω on-resistance, high-density octal SPST switch in a 4 mm × 5 mm
30-terminal LGA package. Eight independent switches (S1–S8 to D1–D8), SPI-controlled with
error detection. BugBuster uses 5 devices: U10, U11, U16, U17 (main MUX matrix) + U23 (self-test).

**Key Specifications:**
- R_ON: 0.56 Ω typical, 1.0 Ω max (±15V dual supply, 25°C)
- ΔR_ON (match between channels): 0.045 Ω typ, 0.12 Ω max
- R_FLAT(ON) (flatness across signal range): 0.004 Ω typ
- Continuous current per channel: 768 mA (1 ch on), 439 mA (8 ch on) at 25°C, dual supply
- Continuous current derated at temperature: 313/232 mA at 85°C, 122/112 mA at 125°C
- THD: -122 dB at 1 kHz (R_L = 1 kΩ, 20V p-p, dual supply)
- -3 dB bandwidth: 171 MHz typical
- Insertion loss: -0.06 dB at 1 MHz
- Off isolation: -76 dB at 100 kHz
- Channel-to-channel crosstalk: -85 dB at 100 kHz
- Break-before-make switching: guaranteed (t_D = 349 ns min, 429 ns typ)
- On time (t_ON): 600 ns typ, 749 ns max (dual supply)
- Off time (t_OFF): 196 ns typ, 254 ns max

### 26.2 Power Supply

| Parameter | Min | Max | Unit | Notes |
|-----------|-----|-----|------|-------|
| Dual Supply V_DD | ±4.5 | ±16.5 | V | V_DD to V_SS ≤ 33V |
| Single Supply V_DD | +5 | +20 | V | V_SS = GND |
| Logic Supply V_L | 2.7 | 5.5 | V | Also RESET/V_L pin |
| I_DD (quiescent) | — | 440 | µA | All switches open |
| I_DD (active, 50 MHz SCLK) | — | 9.0 | mA | V_L = 5.5V |

### 26.3 SPI Interface

- SPI Mode 0 (CPOL=0, CPHA=0) or Mode 3 (CPOL=1, CPHA=1)
- Max SCLK frequency: 50 MHz
- Data captured on rising edge of SCLK, propagated on falling edge
- CS active low; frame synchronization signal
- Minimum CS high time between commands: 20 ns (t_11)
- SDO has integrated pullup to V_L

### 26.4 Register Map

| Reg Addr | Name | Default | R/W | Description |
|----------|------|---------|-----|-------------|
| 0x01 | SW_DATA | 0x00 | R/W | Switch enable bits: bit N = switch N+1 (1=closed, 0=open) |
| 0x02 | ERR_CONFIG | 0x06 | R/W | Error detection enable: bit0=CRC, bit1=SCLK_ERR, bit2=RW_ERR |
| 0x03 | ERR_FLAGS | 0x00 | R | Error flags: bit0=CRC_ERR_FLAG, bit1=SCLK_ERR_FLAG, bit2=RW_ERR_FLAG |
| 0x05 | BURST_EN | 0x00 | R/W | bit0=BURST_MODE_EN (consecutive SPI without CS toggle) |
| 0x0B | SOFT_RESETB | 0x00 | W | Software reset: write 0xA3 then 0x05 consecutively |

### 26.5 Address Mode (Default)

- 16-bit SPI frame: [R/W(1) | Addr(7) | Data(8)]
- SDO outputs alignment byte 0x25 during first 8 bits, then register data during last 8
- Register write occurs on 16th SCLK rising edge
- With CRC enabled: 24-bit frame [R/W(1) | Addr(7) | Data(8) | CRC(8)]
- CRC polynomial: x^8 + x^2 + x^1 + 1, seed = 0

### 26.6 Daisy-Chain Mode

- Enter: send command 0x2500 (16-bit); SDO echoes 0x25 confirming entry
- All commands target SW_DATA register only (no register configuration changes possible)
- Each device = 8-bit shift register; SDO of one device → SDI of next
- First byte sent reaches the LAST device in chain
- N devices require N × 8 SCLK cycles per frame
- Exit: hardware reset only (RESET/V_L pin LOW, then HIGH + 120 µs wait)

### 26.7 Error Detection Features

**CRC Error Detection** (CRC_ERR_EN, bit 0 of ERR_CONFIG):
- Extends frame by 8 SCLK cycles (24-bit total in address mode)
- Polynomial: x^8 + x^2 + x^1 + 1 with seed 0
- Calculated over: R/W bit, address bits [6:0], data bits [7:0]
- CRC checked on 24th rising edge; write blocked if CRC mismatch
- Disabled by default

**SCLK Count Error** (SCLK_ERR_EN, bit 1 of ERR_CONFIG):
- Counts SCLK edges per CS frame; expects exactly 16 (or 24 with CRC)
- In burst mode: multiples of 16 (or 24) expected
- In daisy-chain: multiples of N×8
- Enabled by default

**Invalid R/W Address Error** (RW_ERR_EN, bit 2 of ERR_CONFIG):
- Detects reads to write-only registers, writes to read-only registers, access to nonexistent addresses
- Detected on 9th SCLK rising edge (write is blocked)
- Enabled by default

**Clearing Error Flags**: Send special 16-bit SPI command 0x6CA9 (does not trigger RW address error)

### 26.8 Power-On Reset

After V_L power-up or hardware/software reset, wait minimum **120 µs** before any SPI command.
Ensure V_L does not drop during the initialization phase.

### 26.9 Absolute Maximum Ratings

| Parameter | Rating |
|-----------|--------|
| V_DD to V_SS | 35 V |
| V_DD to GND | -0.3 V to +25 V |
| V_SS to GND | +0.3 V to -25 V |
| Analog inputs (S_x, D_x) | V_SS - 0.3V to V_DD + 0.3V |
| Digital inputs | -0.3 V to +6 V |
| Peak current per channel | 1180 mA (pulsed, 1 ms, 10% duty cycle) |
| Operating temperature | -40°C to +125°C |
| θ_JA | 56.81 °C/W |

---

## 27. PCA9535A — Datasheet Reference

Cross-referenced against PCA9535A datasheet (NXP, 24 pages).

### 27.1 Device Overview

The PCA9535A is a 16-bit I2C GPIO expander with interrupt output. Two 8-bit ports (Port 0, Port 1),
each pin independently configurable as input or output. Open-drain INT output for input change notification.
BugBuster uses U20 at I2C address 0x23 (7-bit) for power management and e-fuse control.

### 27.2 Electrical Specifications

| Parameter | Min | Typ | Max | Unit |
|-----------|-----|-----|-----|------|
| VCC | 2.3 | — | 5.5 | V |
| I2C clock (f_SCL) | — | — | 400 | kHz |
| Input low voltage (V_IL) | — | — | 0.3×VCC | V |
| Input high voltage (V_IH) | 0.7×VCC | — | — | V |
| Output low voltage (V_OL) | — | — | 0.4 | V (at 10 mA sink) |
| INT output low (V_OL) | — | — | 0.4 | V (at 10 mA sink) |
| Power-on reset time | — | — | — | Internal POR |

### 27.3 Register Map

| Reg Addr | Name | R/W | Default | Description |
|----------|------|-----|---------|-------------|
| 0x00 | Input Port 0 | R | — | Current state of Port 0 pins (read clears INT for this port) |
| 0x01 | Input Port 1 | R | — | Current state of Port 1 pins |
| 0x02 | Output Port 0 | R/W | 0xFF | Output state for Port 0 (only affects pins configured as outputs) |
| 0x03 | Output Port 1 | R/W | 0xFF | Output state for Port 1 |
| 0x04 | Polarity Inv. Port 0 | R/W | 0x00 | 1 = invert corresponding input bit when read |
| 0x05 | Polarity Inv. Port 1 | R/W | 0x00 | 1 = invert corresponding input bit when read |
| 0x06 | Configuration Port 0 | R/W | 0xFF | 1 = input, 0 = output (default: all inputs) |
| 0x07 | Configuration Port 1 | R/W | 0xFF | 1 = input, 0 = output (default: all inputs) |

### 27.4 Interrupt (INT) Pin

- Open-drain output, active LOW
- Asserted when any input pin changes from the value last read from the Input Port register
- **Cleared by**: reading the Input Port register that caused the change, OR the input returning to its previous state
- INT is level-triggered (stays low as long as input differs from last-read value)
- Requires external pull-up resistor (BugBuster: 5.1 kΩ to 3.3V on I2C bus)
- Connected to ESP32-S3 GPIO4 in PCB mode (PIN_MUX_INT)

### 27.5 Power-On Reset

On power-up:
- All ports configured as **inputs** (Configuration registers = 0xFF)
- Output registers set to **0xFF** (high)
- Polarity inversion registers set to **0x00** (no inversion)
- No internal pull-ups or pull-downs on I/O pins

**IMPORTANT**: Input pins with no external pull-up/pull-down will float. The EFUSE_FLT_x
fault pins from TPS1641 have external pull-ups (active-low fault signaling).

### 27.6 BugBuster Pin Mapping

**Port 0 — Power Management:**

| Bit | Pin | Name | Direction | Description |
|-----|-----|------|-----------|-------------|
| 0 | P0.0 | LOGIC_PG | Input | Main logic power good |
| 1 | P0.1 | VADJ1_PG | Input | V_ADJ1 regulator power good |
| 2 | P0.2 | VADJ1_EN | Output | V_ADJ1 regulator enable |
| 3 | P0.3 | VADJ2_EN | Output | V_ADJ2 regulator enable |
| 4 | P0.4 | VADJ2_PG | Input | V_ADJ2 regulator power good |
| 5 | P0.5 | EN_15V_A | Output | ±15V analog supply enable |
| 6 | P0.6 | EN_MUX | Output | MUX VCC power enable |
| 7 | P0.7 | EN_USB_HUB | Output | USB hub enable |

**Port 1 — E-Fuse Control:**

| Bit | Pin | Name | Direction | Description |
|-----|-----|------|-----------|-------------|
| 0 | P1.0 | EFUSE_EN_1 | Output | E-Fuse 1 enable (→ connector P1) |
| 1 | P1.1 | EFUSE_FLT_1 | Input | E-Fuse 1 fault (active LOW from TPS1641) |
| 2 | P1.2 | EFUSE_EN_2 | Output | E-Fuse 2 enable (→ connector P2) |
| 3 | P1.3 | EFUSE_FLT_2 | Input | E-Fuse 2 fault (active LOW) |
| 4 | P1.4 | EFUSE_EN_3 | Output | E-Fuse 3 enable (→ connector P3) |
| 5 | P1.5 | EFUSE_FLT_3 | Input | E-Fuse 3 fault (active LOW) |
| 6 | P1.6 | EFUSE_EN_4 | Output | E-Fuse 4 enable (→ connector P4) |
| 7 | P1.7 | EFUSE_FLT_4 | Input | E-Fuse 4 fault (active LOW) |

### 27.7 Firmware Configuration

On init, firmware writes:
- Config Port 0: 0x13 (bits 0,1,4 = input for PG; bits 2,3,5,6,7 = output for enables)
- Config Port 1: 0xAA (odd bits = input for FLT; even bits = output for EN)
- Output Port 0: 0x00 (all enables OFF — safe start)
- Output Port 1: 0x00 (all e-fuses OFF — safe start)
- Polarity Inversion: 0x00 for both ports (no inversion)

---

## 28. DS4424 — Datasheet Reference

Cross-referenced against DS4422/DS4424 datasheet Rev. 2 (Maxim/Analog Devices, 11 pages).

### 28.1 Device Overview

The DS4424 is a 4-channel I2C programmable current DAC (IDAC). Each output (OUT0–OUT3) can
sink or source current with 7-bit resolution (127 steps each direction). Used in BugBuster to adjust
DC-DC converter output voltages by injecting current into the feedback network.

**BugBuster Channel Mapping:**
- IDAC0 (OUT0, reg 0xF8): Level shifter voltage (LTM8078 Out2, ~3.3V adjustable)
- IDAC1 (OUT1, reg 0xF9): V_ADJ1 (LTM8063 #1, 3–15V, feeds connectors P1+P2)
- IDAC2 (OUT2, reg 0xFA): V_ADJ2 (LTM8063 #2, 3–15V, feeds connectors P3+P4)
- IDAC3 (OUT3, reg 0xFB): Not connected

### 28.2 Electrical Specifications

| Parameter | Min | Typ | Max | Unit | Notes |
|-----------|-----|-----|-----|------|-------|
| VCC | 2.7 | — | 5.5 | V | |
| Full-scale output current (I_FS) | — | — | 200 | µA | Set by R_FS resistor |
| Output current range | -I_FS | 0 | +I_FS | µA | Sink and source |
| Zero current leakage (I_ZERO) | -1 | — | +1 | µA | At code 0 |
| Differential linearity (DNL) | -0.5 | — | +0.5 | LSB | |
| Integral linearity (INL) | -1 | — | +1 | LSB | |
| Output current variation (VCC) | — | 0.32/0.42 | — | %/V | Source/sink |
| I2C clock (f_SCL) | 0 | — | 400 | kHz | |
| **OUT pin absolute max** | — | — | **VCC + 0.5** | **V** | **Exceeding damages device** |

### 28.3 I2C Interface

- Slave address (BugBuster): 0x10 (7-bit), set by A0=A1=GND → 0x20 (8-bit write), 0x21 (8-bit read)
- Standard I2C protocol: START → slave addr + R/W → register addr → data → STOP
- Read: requires repeated START (write register address, then read data byte)

**I2C Address Table:**

| A1 | A0 | 7-bit Address | 8-bit Address |
|----|-----|--------------|---------------|
| GND | GND | 0x10 | 0x20 |
| GND | VCC | 0x30 | 0x60 |
| VCC | GND | 0x50 | 0xA0 |
| VCC | VCC | 0x70 | 0xE0 |

### 28.4 Register Map

| Register Address | Output | Description |
|-----------------|--------|-------------|
| 0xF8 | OUT0 | Current source 0 control |
| 0xF9 | OUT1 | Current source 1 control |
| 0xFA | OUT2 | Current source 2 (DS4424 only) |
| 0xFB | OUT3 | Current source 3 (DS4424 only) |

**Register Format:**

| Bit 7 | Bits [6:0] | Meaning |
|-------|------------|---------|
| S (Sign) | D6–D0 (Magnitude) | S=0: sink current; S=1: source current |

- Code 0x00 = zero output current (power-on default, safe)
- Code 0x7F = maximum sink current (I_FS)
- Code 0xFF = maximum source current (I_FS)
- Magnitude 0 always outputs zero current regardless of sign bit

### 28.5 Current and Voltage Formulas

**Full-scale current (set by external R_FS resistor):**
```
R_FS = (V_RFS × 127) / (16 × I_FS)
```
Where V_RFS = 0.976V (internal reference on FS pin).

**Output current from DAC code:**
```
I_OUT = (DAC_magnitude / 127) × I_FS
```
Direction determined by sign bit: S=0 → sink (into pin), S=1 → source (out of pin).

**Voltage adjustment formula (BugBuster topology):**
```
V_OUT = V_FB × (1 + R_INT/R_FB) + I_DAC × R_INT
```
Where:
- V_FB = regulator feedback reference voltage
- R_INT = series resistor in feedback network (249 kΩ in BugBuster)
- R_FB = parallel resistor to GND (sets midpoint voltage)
- I_DAC = signed current (sink raises V_OUT, source lowers V_OUT)

**Step size per DAC code:**
```
ΔV = I_FS / 127 × R_INT
```
For BugBuster (I_FS=50µA, R_INT=249kΩ): ΔV ≈ 98 mV/step.

### 28.6 Power-On Behavior

- All outputs default to **zero current** (register value 0x00) — safe
- Device does not affect regulator output until host writes a nonzero code
- VCC decoupling: 0.01 µF + 0.1 µF ceramic recommended

### 28.7 Critical Safety Notes

1. **OUT pin voltage limit**: Absolute maximum is VCC + 0.5V. In BugBuster, VCC = 3.3V so
   OUT pins must stay below 3.8V. The feedback divider network ensures the FB node voltage
   stays well below this (~0.8V at midpoint), so this is not a concern during normal operation.

2. **Power rail ordering**: The DS4424 VCC should be brought up before or simultaneously with
   the power rail of the DC-DC converter it controls. In BugBuster, DS4424 VCC (3.3V_BUCK)
   is always-on before PCA9535 enables the VADJ regulators → ordering is correct by design.

3. **No output clamp**: The DS4424 does not have output voltage clamps. If the feedback node
   voltage exceeds VCC + 0.5V (e.g., during regulator startup transients), current may flow
   through the ESD protection diodes into VCC, potentially damaging the device.

---

## 29. AD74416H — Extended Reference

Additional details from the AD74416H datasheet (Rev. 0, 106 pages) that supplement the main sections above.

### 29.1 Watchdog Timer (WDT)

The AD74416H includes a programmable watchdog timer that monitors SPI activity. If no valid SPI
transaction occurs within the timeout period, the device resets (all channels → HIGH_IMP).

**WDT_CONFIG register** bits:
- WDT_EN: Enable watchdog (0=disabled, 1=enabled)
- WDT_TIMEOUT: 4-bit field selecting timeout period

| WDT_TIMEOUT | Timeout |
|-------------|---------|
| 0 | 1 ms |
| 1 | 5 ms |
| 2 | 10 ms |
| 3 | 25 ms |
| 4 | 50 ms |
| 5 | 100 ms |
| 6 | 250 ms |
| 7 | 500 ms |
| 8 | 750 ms |
| 9 | 1000 ms |
| A | 2000 ms |

The WDT is zeroed on every valid SPI transaction (read or write). If timeout expires, the device
resets and sets RESET_OCCURRED in ALERT_STATUS.

**BugBuster default**: WDT is **disabled**. As an interactive development tool, users may pause
SPI communication during debugging. An optional BBP command allows enabling it.

### 29.2 Burst Read Mode

Sequential reading of multiple registers without re-issuing the read address:
1. Enable required BURST_READ_SEL bits in BURST_READ_SEL register
2. Set READBACK_ADDR in READ_SELECT register to first register
3. Keep SYNC low after second frame; additional 24 clocks per register
4. Each subsequent register outputs 16-bit data + 8-bit CRC on SDO

Useful for fast ADC result reads (ADC_RESULT_UPR0 + ADC_RESULT0 for all 4 channels).

### 29.3 FET Leakage Compensation

The external PFET used for digital output may have off-state leakage that affects precision
analog measurements (especially RTD and low-current sensing).

- Enable via FET_LKG_COMP[n] register: set FET_SRC_LKG_COMP_EN bit
- Connect LKG_COMP_x pin to PFET drain
- Compensates leakage currents up to 40 µA
- Reduces voltage error from ~30 mV (typical FET leakage through 12Ω R_SENSE) to negligible

### 29.4 Software Reset

**Single-device reset** via CMD_KEY register:
1. Write 0x15FA (Software Reset Key 1) to CMD_KEY
2. Write 0xAF51 (Software Reset Key 2) to CMD_KEY
3. Wait 1 ms for reset cycle

**Broadcast reset** (all devices on SPI bus) via BROADCAST_CMD_KEY register:
1. Write 0x1A78 to BROADCAST_CMD_KEY
2. Write 0xD203 to BROADCAST_CMD_KEY

After reset: RESET_OCCURRED bit set in ALERT_STATUS. Clear before continuing.
Wait for calibration memory refresh (CAL_MEM_ERR in SUPPLY_ALERT_STATUS clears when ready).

### 29.5 Channel Function Switching Procedure

When changing a channel from one function to another:
1. Set CH_FUNC to HIGH_IMP (0x0000) in CH_FUNC_SETUP[n]
2. Set DAC_CODE to intended value for new function (0x0000 for outputs, or specific limit for inputs)
3. Wait 300 µs (or 4.2 ms for IOUT_HART with VIOUT_DRV_EN_DLY=0)
4. Set CH_FUNC to new function code in CH_FUNC_SETUP[n]
5. Wait channel initialization time (300 µs, or 4.2 ms for IOUT_HART)
6. Begin operation (update DAC code, start ADC, etc.)

**Important**: The DAC_CODE register is NOT reset when changing channel functions. Always
set DAC_CODE to the intended value before or immediately after selecting the new function.

### 29.6 Digital Output — Re-enable After Short-Circuit Timeout

If a DO short-circuit persists beyond T2 timeout, the FET is automatically disabled.
To re-enable after the fault is cleared:
1. Set DO_DATA = 0 (FET off) in DO_EXT_CONFIG[n]
2. Select a DO_MODE in DO_EXT_CONFIG[n] (re-power the output circuit)
3. Set DO_DATA = 1 (FET on)

### 29.7 ADC Conversion Rate: 19.2 kSPS

The 19.2 kSPS rate is available **only for diagnostic measurements** (set via CONV_RATE_DIAG
in ADC_CONV_CTRL). It is not available for channel ADC conversions. At 19.2 kSPS, 50/60 Hz
rejection is disabled.

### 29.8 ADC_RDY Pin Behavior

The ADC_RDY pin behavior depends on the ADC_RDY_CTRL bit in ADC_CONV_CTRL:

**ADC_RDY_CTRL = 0** (default, asserts at end of sequence):
- Single conversion: deasserts when CONV_SEQ command written, asserts when sequence completes
- Continuous: deasserts on CONV_SEQ write, asserts after 25 µs when results available

**ADC_RDY_CTRL = 1** (asserts at end of each conversion):
- Single conversion: deasserts on CONV_SEQ, asserts after each individual channel conversion
- Continuous: deasserts on each conversion start, asserts after 25 µs (or when >1 conversion enabled)
- Read LAST_ADC_RESULT_UPR[n] and LAST_ADC_RESULT[n] for per-conversion results

### 29.9 Diagnostics — Complete Formula Table

All diagnostics use DIAG_CODE from ADC_DIAG_RESULT[n] registers. Measurement range is 2.5V.

| DIAG_ASSIGN | Diagnostic | Formula | Range |
|-------------|-----------|---------|-------|
| 0b0000 | AGND | V = (DIAG_CODE / 65,536) × 2.5 | 0–2.5V |
| 0b0001 | Temperature (°C) | T = (DIAG_CODE - 20,034) / 8.95 - 40 | -40 to +175°C |
| 0b0010 | DVCC | V = (DIAG_CODE / 65,536) × (25/3) | 0–8.3V |
| 0b0011 | AVCC | V = (DIAG_CODE / 65,536) × 17.5 | 0–17.5V |
| 0b0100 | LDO1V8 | V = (DIAG_CODE / 65,536) × 7.5 | 0–7.5V |
| 0b0101 | AVDD_HI | V = (DIAG_CODE / 65,536) × (25/0.52) | 0–48V |
| 0b0110 | AVDD_LO | V = (DIAG_CODE / 65,536) × (25/0.52) | 0–48V |
| 0b0111 | AVSS | V = (DIAG_CODE / 65,536) × 31.017 - 20 | -20 to +11V |
| 0b1000 | LVIN | V = (DIAG_CODE / 65,536) × 2.5 | 0–2.5V |
| 0b1001 | DO_VDD | V = (DIAG_CODE / 65,536) × (25/0.64) | 0–39V |
| 0b1010 | VSENSEP_x | V = (DIAG_CODE / 65,536) × 60 - AVDD_HI | varies |
| 0b1011 | VSENSEN_x | V = (DIAG_CODE / 65,536) × 50 - 20 | -20 to +30V |
| 0b1100 | I_DO_SRC (R_SET current) | I = (DIAG_CODE / 65,536) × 0.5 / R_SET | 0–3.3A |
| 0b1101 | AVDD_x (per-channel) | V = (DIAG_CODE / 65,536) × (25/0.52) | 0–48V |

### 29.10 DAC Transfer Function

| DAC Code | ±12V Range | 0V to 12V Range | 0 to 25mA Range |
|----------|-----------|-----------------|-----------------|
| 0x0000 | -12V | 0V | 0 mA |
| 0x0001 | -12V + 1 LSB | 12V / 65,536 | 25mA / 65,536 |
| 0x8000 | 0V | 6V | 12.5 mA |
| 0xFFFE | +12V - 2 LSB | 12V × (65,534/65,536) | 25mA × (65,534/65,536) |
| 0xFFFF | +12V - 1 LSB | 12V × (65,535/65,536) | 25mA × (65,535/65,536) |

1 LSB = Full Scale / 65,536. For ±12V: 1 LSB = 24V/65,536 ≈ 366 µV. For 0–12V: 1 LSB ≈ 183 µV.

### 29.11 ADC Noise (Peak-to-Peak, in LSBs, Inputs Shorted)

| Rate | +12V (24-bit) | ±12V (24-bit) | ±2.5V (24-bit) | 0.625V (24-bit) | ±0.3125V (24-bit) | ±104mV (24-bit) |
|------|--------------|--------------|---------------|----------------|------------------|----------------|
| 10 SPS_H | 23.4 | 11.7 | 13.3 | 52.2 | 48.0 | 129.0 |
| 20 SPS | 44.8 | 22.4 | 25.6 | 104.4 | 95.6 | 257.7 |
| 200 SPS | 85.2 | 42.6 | 48.2 | 186.0 | 175.1 | 480.3 |
| 1.2 kSPS | 297.0 | 148.5 | 168.8 | 693.0 | 627.0 | 1696.0 |
| 4.8 kSPS | 723.2 | 361.6 | 430.9 | 2077.6 | 1620.7 | 4407.3 |
| 9.6 kSPS | 1417.2 | 708.6 | 877.3 | 4674.4 | 3170.0 | 8854.8 |

### 29.12 Register Defaults by Channel Function

When a channel function is selected via CH_FUNC_SETUP[n], multiple registers are automatically
configured. Key defaults (see datasheet Table 22 for complete table):

- **HIGH_IMP**: ADC MUX = SENSELF to AGND, range 0–12V, comparator disabled
- **VOUT**: ADC MUX = SENSEHF to SENSELF, range ±0.3125V (current feedback), comparator disabled
- **IOUT**: ADC MUX = SENSELF to AGND, range 0–12V, adaptive power switching = TRACK
- **VIN**: ADC MUX = SENSELF to AGND, range 0–12V, comparator disabled
- **IIN_EXT_PWR**: ADC MUX = SENSEHF to SENSELF, range -0.3125V to 0V, comparator enabled, AVDD = AVDD_HI/2
- **IIN_LOOP_PWR**: ADC MUX = SENSEHF to SENSELF, range 0–0.3125V, comparator enabled + inverted
- **RES_MEAS**: ADC MUX = SENSELF to VSENSEN, range 0–0.625V, comparator disabled
- **DIN_LOGIC**: ADC MUX = SENSELF to AGND, range 0–12V, comparator enabled, AVDD = AVDD_HI/2
- **DIN_LOOP**: ADC MUX = SENSELF to AGND, range 0–12V, comparator enabled + inverted

Regardless of function selection, these are always set:
- RTD_MODE_SEL = 0 (3-wire RTD mode)
- RTD_CURRENT = 1 mA
- RTD_ADC_REF = 2V
- DIN_SINK = 0 (current sink off)
- DIN_THRESH_MODE = 0 (threshold scales with AVDD_HI)

---

## 30. HAT Expansion Board System

PCB mode only. HAT (Hardware Attached on Top) boards attach to a dedicated connector on the
BugBuster PCB, providing configurable I/O expansion for SWD debugging, tracing, or general GPIO.

### 30.1 Physical Interface

| Signal | GPIO | Direction | Description |
|--------|------|-----------|-------------|
| HAT_DETECT | GPIO47 | ADC Input | Voltage divider for HAT identification |
| HAT_TX | GPIO43 (TXD0) | Output | UART TX from BugBuster to HAT |
| HAT_RX | GPIO44 (RXD0) | Input | UART RX from HAT to BugBuster |
| HAT_IRQ | GPIO15 | Open-drain bidir | Shared interrupt line |
| EXP_EXT_1..4 | via MUX | Configurable | 4 expansion I/O lines |

### 30.2 HAT Detection (ADC)

GPIO47 has a 10 kΩ pull-up to 3.3V on the BugBuster PCB. Each HAT type has a specific
pull-down resistor creating a unique voltage divider:

| Condition | Pull-down | Voltage | HAT Type |
|-----------|-----------|---------|----------|
| No HAT | None (open) | ~3.3V | NONE |
| SWD/GPIO HAT | 10 kΩ | ~1.65V | SWD_GPIO |
| Future HAT A | 4.7 kΩ | ~1.06V | (reserved) |
| Future HAT B | 22 kΩ | ~2.27V | (reserved) |

Detection uses 8-sample ADC averaging for stability. Voltage thresholds:
- \> 2.5V → No HAT
- 1.2V–2.1V → SWD/GPIO HAT
- Other ranges → reserved for future HAT types

### 30.3 UART Protocol

- **UART0** on GPIO43 (TX) / GPIO44 (RX)
- **921600 baud, 8N1** (8 data bits, no parity, 1 stop bit)
- BugBuster is always the **master** (initiates all transactions)
- HAT is the **slave** (responds to commands, can assert IRQ for attention)

**Frame format:**

```
[SYNC:0xAA] [LEN:u8] [CMD:u8] [PAYLOAD:0..N] [CRC8:u8]
```

- SYNC = 0xAA (frame start marker)
- LEN = payload length (0–32 bytes, excludes SYNC/LEN/CMD/CRC)
- CMD = command or response ID
- CRC-8 polynomial 0x07 over CMD + PAYLOAD bytes

**Commands (master → slave):**

| CMD | Name | Payload | Description |
|-----|------|---------|-------------|
| 0x01 | PING | (empty) | Check if HAT is alive |
| 0x02 | GET_INFO | (empty) | Get HAT type, firmware version |
| 0x03 | SET_PIN_CONFIG | pin(u8) + func(u8), or 4×func(u8) | Configure EXP_EXT pins |
| 0x04 | GET_PIN_CONFIG | (empty) | Read current pin config |
| 0x05 | RESET | (empty) | Reset all pins to disconnected |

**Responses (slave → master):**

| RSP | Name | Payload | Description |
|-----|------|---------|-------------|
| 0x80 | OK | (varies) | Success, echoes relevant data |
| 0x81 | ERROR | error_code(u8) | Command failed |
| 0x82 | INFO | type(u8) + fw_major(u8) + fw_minor(u8) | Response to GET_INFO |

### 30.4 EXP_EXT Pin Functions

Each of the 4 EXP_EXT lines can be independently assigned:

| Value | Name | Description |
|-------|------|-------------|
| 0 | DISCONNECTED | Pin not routed (default) |
| 1 | SWDIO | SWD data I/O (bidirectional) |
| 2 | SWCLK | SWD clock output |
| 3 | TRACE1 | Trace data 1 / SWO |
| 4 | TRACE2 | Trace data 2 |
| 5 | GPIO1 | General-purpose I/O 1 |
| 6 | GPIO2 | General-purpose I/O 2 |
| 7 | GPIO3 | General-purpose I/O 3 |
| 8 | GPIO4 | General-purpose I/O 4 |

### 30.5 Interrupt Line (GPIO15)

Open-drain, shared between BugBuster and HAT. Either side can pull low to signal attention.
BugBuster configures GPIO15 as input-output open-drain with internal pull-up.
The HAT should release the line (high-Z) after the interrupt is serviced.

### 30.6 Initialization Sequence

1. ADC detect: read GPIO47, average 8 samples, identify HAT type
2. If HAT detected: initialize UART0 at 921600 8N1
3. Send PING, wait for OK response (200 ms timeout)
4. Send GET_INFO to learn HAT type and firmware version
5. Send GET_PIN_CONFIG to read current pin assignments
6. Report state to host via BBP/HTTP

### 30.7 BBP Commands

| ID | Name | HTTP Equivalent |
|----|------|-----------------|
| 0xC5 | HAT_GET_STATUS | `GET /api/hat` |
| 0xC6 | HAT_SET_PIN | `POST /api/hat/config` |
| 0xC7 | HAT_SET_ALL_PINS | `POST /api/hat/config` |
| 0xC8 | HAT_RESET | `POST /api/hat/reset` |
| 0xC9 | HAT_DETECT | `POST /api/hat/detect` |

### 30.8 RP2040 Hardware Overview

The HAT is built around an **RP2040** (dual Cortex-M0+, 264 KB SRAM) running a modified debugprobe firmware with BugBuster extensions.

**Pin Assignments (RP2040):**

| Pin | GPIO | Function |
|-----|------|----------|
| UART TX | GPIO0 | To ESP32 RX (GPIO44) |
| UART RX | GPIO1 | From ESP32 TX (GPIO43) |
| SWCLK | GPIO2 | SWD clock (PIO 0) |
| SWDIO | GPIO3 | SWD data (PIO 0) |
| EN_A | GPIO4 | Connector A power enable |
| EN_B | GPIO5 | Connector B power enable |
| HVPAK SDA | GPIO6 | I2C1 for IO voltage control |
| HVPAK SCL | GPIO7 | I2C1 (400 kHz, addr 0x48) |
| IRQ | GPIO8 | Open-drain interrupt to ESP32 |
| LED_STATUS | GPIO9 | Status LED |
| EXT1–EXT4 | GPIO10–13 | Expansion I/O lines |
| LA_CH0 | GPIO14 | Logic analyzer channel 0 |
| LA_CH1 | GPIO15 | Logic analyzer channel 1 |
| LA_CH2 | GPIO16 | Logic analyzer channel 2 |
| LA_CH3 | GPIO17 | Logic analyzer channel 3 |
| FAULT_A | GPIO20 | Connector A overcurrent (input, active low) |
| FAULT_B | GPIO21 | Connector B overcurrent (input, active low) |
| ADC_A | GPIO26 (ADC0) | Connector A current sense |
| ADC_B | GPIO27 (ADC1) | Connector B current sense |
| LED_ACTIVITY | GPIO25 | Onboard LED |

**PIO Allocation:**
- **PIO 0:** SWD debug probe (debugprobe, unmodified)
- **PIO 1, SM 0:** Logic analyzer capture (1/2/4-channel programs)
- **PIO 1, SM 1:** Logic analyzer hardware trigger (edge/level)

**USB Composite Device:**
- CDC #0 (EP 0x81/0x02/0x83): UART bridge + LA data streaming
- HID (EP 0x04/0x85): CMSIS-DAP v2 (SWD)
- Vendor (EP 0x06/0x87): LA bulk data (legacy, replaced by CDC for gapless mode)

### 30.9 Logic Analyzer — Technical Reference

#### Capture Architecture

The logic analyzer uses PIO 1 for parallel signal capture and DMA for zero-copy transfer to SRAM.

**Buffer:** 76 KB static SRAM (BB_LA_BUFFER_SIZE = 77,824 bytes = 19,456 × uint32_t words)

**Sample Packing (raw mode):**

| Channels | Samples per 32-bit word | Max samples (76 KB buffer) |
|----------|------------------------|---------------------------|
| 1 | 32 | 622,592 |
| 2 | 16 | 311,296 |
| 4 | 8 | 155,648 |

**Sample Rate:** Configurable from ~1 Hz to 125 MHz. Actual rate = `sys_clk / round(sys_clk / desired_rate)` due to PIO clock divider quantization. sys_clk = 125 MHz.

#### Trigger Modes

| Type | Value | PIO Program | Description |
|------|-------|-------------|-------------|
| NONE | 0 | — | Capture starts immediately on arm |
| RISING | 1 | `wait 0 pin 0; wait 1 pin 0; irq set 0` | Rising edge |
| FALLING | 2 | `wait 1 pin 0; wait 0 pin 0; irq set 0` | Falling edge |
| BOTH | 3 | Software fallback | Any edge change |
| HIGH | 4 | `wait 1 pin 0; irq set 0` | Level high |
| LOW | 5 | `wait 0 pin 0; irq set 0` | Level low |

Trigger SM 1 fires PIO1_IRQ_0 → handler enables capture SM 0 → DMA begins filling buffer.

#### Capture States

| State | Value | Description |
|-------|-------|-------------|
| IDLE | 0 | Not configured or stopped |
| ARMED | 1 | Trigger configured, waiting |
| CAPTURING | 2 | Trigger fired, DMA active |
| DONE | 3 | Buffer full, data ready |
| STREAMING | 4 | Continuous double-buffered mode |
| ERROR | 5 | Capture failed |

#### RLE Compression

Run-length encoding compresses repetitive digital signals (typical 10–100× compression).

**RLE word format (32-bit LE):**
```
Bits [31:28] = 4-bit channel values (CH3, CH2, CH1, CH0)
Bits [27:0]  = 28-bit run length (max 268,435,455 samples per entry)
```

RLE encoding is performed by `bb_la_rle.c` after capture completes. The encoder processes raw PIO words, extracts individual samples based on channel count, and emits (value, count) pairs. Runs exceeding 28-bit max are split across multiple entries.

#### DMA Configuration

- **Transfer size:** DMA_SIZE_32 (32-bit)
- **Source:** PIO 1 RX FIFO (fixed address, no increment)
- **Destination:** Capture buffer (incrementing)
- **DREQ:** PIO 1, SM 0 RX
- **IRQ:** DMA_IRQ_0 fires on completion, sets `dma_done` flag
- **Channel:** Dynamically claimed/released per capture

#### Streaming Mode (Double-Buffered)

For continuous capture, the buffer is split into two halves (38 KB each). DMA completes one half, flags it ready, and wraps to the other. The USB/CDC layer reads the ready half while the other fills.

#### Data Readout Paths

1. **HAT UART (0xD8 HAT_LA_READ):** Chunked, 28 bytes per frame — slow, used for small captures
2. **USB CDC:** Gapless streaming via ring buffer (1024-byte CDC TX buffer) — preferred for large captures
3. **USB Vendor Bulk (legacy):** EP 0x87, 64-byte packets with 4-byte length header

When LA_DONE_EVENT (0x85) fires, the desktop app reads captured data via USB CDC for best throughput.

### 30.10 SWD Debug Probe

The RP2040 runs a fork of the Raspberry Pi **debugprobe** firmware, providing CMSIS-DAP v2 compliance. The SWD interface uses PIO 0 (SM 0 and SM 1) on GPIO2 (SWCLK) / GPIO3 (SWDIO).

**Compatible tools:** OpenOCD, pyOCD, probe-rs, VS Code Cortex-Debug

The debugprobe core is unmodified — BugBuster extensions (LA, power, HVPAK) are added alongside it. Target detection is performed via SWD DPIDR read.

### 30.11 HVPAK IO Voltage Control

The HVPAK (Renesas level translator) provides programmable IO voltage from 1.2 V to 5.5 V (default 3.3 V). Controlled via I2C1 at 400 kHz, address 0x48.

**Usage:** Set IO voltage before SWD or LA operations to match target voltage levels. The `HAT_SETUP_SWD` command (0xCD) automatically configures HVPAK voltage, enables the selected connector, and routes SWD pins.

### 30.12 Power Management

Each connector (A/B) has independent:
- **Power enable:** GPIO4 (A) / GPIO5 (B), active high
- **Overcurrent detection:** GPIO20 (A) / GPIO21 (B), active low
- **Current sensing:** ADC0 (A) / ADC1 (B) via sense resistor

The `HAT_SET_POWER` (0xCA) and `HAT_GET_POWER` (0xCB) commands control power state and read current/fault status.

---

## 31. AI Interface — MCP Server (`python/bugbuster_mcp/`)

### 31.1 Overview

`bugbuster_mcp` is an MCP (Model Context Protocol) server that exposes BugBuster's hardware capabilities to AI models (Claude, etc.) as structured tools, resources, and prompt templates. It wraps the existing Python library (`python/bugbuster/`) — no firmware changes required.

**Location:** `python/bugbuster_mcp/`
**Transport:** stdio (standard for Claude Code MCP integration)
**Protocol:** MCP, using `mcp` Python SDK (FastMCP high-level API)

### 31.2 Architecture

```
bugbuster_mcp/
  server.py         FastMCP instance, registers all tools/resources/prompts
  session.py        Singleton BugBuster + HAL connection manager
  config.py         Safety limits and hardware constants
  safety.py         Validation helpers (voltage limits, IO mode checks, fault detection)
  tools/            28 tool implementations in 9 groups
  resources/        bugbuster:// URI resource handlers (read-only state)
  prompts/          Guided workflow prompt templates
  __main__.py       CLI entry point
```

### 31.3 Tool Groups (28 tools total)

| Group | Tools |
|-------|-------|
| Discovery & status | device_status, device_info, check_faults, selftest |
| IO configuration | configure_io, set_supply_voltage, reset_device |
| Analog measurement | read_voltage, read_current, read_resistance |
| Analog output | write_voltage, write_current |
| Digital IO | read_digital, write_digital |
| Waveform & capture | start_waveform, stop_waveform, capture_adc_snapshot, capture_logic_analyzer |
| UART & debug | setup_serial_bridge, setup_swd, uart_config |
| Power management | usb_pd_status, usb_pd_select, power_control, wifi_status |
| Advanced (low-level) | mux_control, register_access, idac_control |

### 31.4 MUX Routing Contract

The HAL's `configure()` method (called by `configure_io`) automatically sets the ADGS2414D MUX switches. Each IO can only be in one mode at a time:
- IOs 3, 6, 9, 12 (analog-capable): routes to AD74416H channel (S2) OR ESP GPIO (S1/S3) OR HAT (S4) — mutually exclusive
- IOs 2,3,5,6,8,9,11,12 (digital-only): routes to ESP GPIO high-drive (S5/S7) OR low-drive (S6/S8)

`configure_io` must be called before any read/write operation. The MUX state is tracked in `hal._io_mode` and `hal._mux_state`.

### 31.5 Safety Layer

- E-fuse auto-enabled when configuring output modes
- Default current limit: 8 mA (not 25 mA)
- VADJ voltages above 12 V require `confirm=True`
- `mux_control` and `register_access` require `i_understand_the_risk=True`
- Post-action fault check after every output-driving tool

### 31.6 Resources

| URI | Description |
|-----|-------------|
| `bugbuster://status` | Full device state |
| `bugbuster://power` | Supply voltages, USB PD, e-fuse |
| `bugbuster://faults` | Active faults with remediation |
| `bugbuster://hat` | HAT detection and LA state |
| `bugbuster://capabilities` | Static IO/voltage/mode limits |

### 31.7 Installation and Usage

```bash
# Install uv via brew
brew install uv

# Create venv and install (from python/ directory)
uv venv --python 3.11 .venv
uv pip install --python .venv/bin/python -e ".[mcp]"

# Run the server (test)
.venv/bin/python -m bugbuster_mcp --transport usb --port /dev/cu.usbmodemXXXX
```

**Claude Code MCP config** (`~/.claude/settings.json`):
```json
{
  "mcpServers": {
    "bugbuster": {
      "command": "/path/to/BugBuster/python/.venv/bin/python",
      "args": ["-m", "bugbuster_mcp", "--transport", "usb", "--port", "/dev/cu.usbmodemXXXXXX"]
    }
  }
}
```

### 31.8 Key Design Decisions

- **HAL as primary API**: `configure_io` → `hal.configure()` handles all MUX routing, power sequencing, and AD74416H channel setup automatically
- **Streaming → snapshot**: ADC/LA streaming is converted to statistical summaries (min/max/mean/stddev/freq) for AI consumption
- **Both transports**: USB BBP for full functionality (streaming, SWD, register access); HTTP for remote/lightweight access
- **Session lifecycle**: `session.py` manages lazy connection and HAL init; `reset_device` tool triggers full reconnect

---

## 32. Testing Infrastructure

### 32.1 Hardware Test Suite (`tests/`)

Pytest-based integration tests that run against real hardware connected via USB. 12 test modules covering all device subsystems.

| Module | Scope |
|--------|-------|
| `test_01_core` | Ping, status, reset, firmware info |
| `test_02_channels` | All 12 channel functions, ADC/DAC read/write |
| `test_03_gpio` | AD74416H GPIO pins A–F |
| `test_04_mux` | MUX matrix routing (32 SPST switches) |
| `test_05_power` | DCDC supplies, IDAC voltage tuning, e-fuses |
| `test_06_usbpd` | USB Power Delivery negotiation, PDO selection |
| `test_07_wavegen` | Waveform generator (sine, square, triangle, sawtooth) |
| `test_08_wifi` | WiFi AP/STA modes, scan, connect |
| `test_09_selftest` | Built-in diagnostic self-test |
| `test_10_streaming` | ADC streaming, scope bucket aggregation |
| `test_11_hat` | HAT detection, power, SWD, LA (requires `--hat` flag) |
| `test_12_faults` | Alert register, fault injection, clearing |

**Configuration:** `tests/conftest.py` provides fixtures for device discovery, connection, and cleanup. `tests/run_tests.py` orchestrates test execution with device detection.

**HTTP API tests** (`tests/http_api/test_http_endpoints.py`): Validates 14 REST endpoint contracts (GET/POST responses, error codes, payload shapes).

### 32.2 Desktop App E2E Tests (`DesktopApp/BugBuster/tests/e2e/`)

WebDriverIO + Tauri driver end-to-end tests for the desktop application. Currently covers app launch, connection screen, and toast notification system. No device required for disconnected-state tests.

### 32.3 Running Tests

```bash
# Hardware tests (device must be connected)
cd tests
pip install -r requirements-test.txt
python run_tests.py              # full suite
pytest device/test_02_channels.py -v  # single module
pytest device/test_11_hat.py --hat    # HAT-specific

# Desktop E2E tests
cd DesktopApp/BugBuster/tests/e2e
npm install && npm test
```

### 32.4 Jupyter Test Dashboard

`notebooks/test_dashboard.ipynb` provides an interactive test orchestration dashboard for running and reviewing test results with visual output.
