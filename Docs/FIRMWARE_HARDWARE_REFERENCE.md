# BugBuster_DAQ — Firmware Hardware Reference
## For firmware agent use — verified against live Altium netlist

**Date produced:** 2026-06-19  
**Netlist source:** `BugBuster_DAQ.PrjPcb` — forced recompile, **1,215 pins, live state**  
**Ground truth:** All pin/net assignments verified directly from the compiled Altium netlist.  
**Datasheets:** All device electrical facts (addresses, thresholds, timings) sourced from manufacturer PDFs in `C:\Users\Lorenzo\Downloads\Datasheets\` and cited. Datasheets are considered reliable; schematic docs prior to this one are NOT — they describe earlier design iterations.

> ⚠ **The earlier markdown docs in this folder (`ESP32-P4_DAQ_Architecture_and_Pinout.md`, `POWER_TREE.md`, `knowledge.md`, `PROBLEMS_FOUND.md`, `GPIO_BOOT_STATE.md`) describe intermediate design states and contain outdated GPIO assignments. Do not use them as firmware references. Use this document only.**

---

## 1. System Overview

A **3-channel, 24-bit precision data-acquisition board** for characterising a Device Under Test (DUT):

| Block | Part | Designator | Role |
|---|---|---|---|
| Host MCU | ESP32-P4 (in Waveshare module) | U30 | Dual-core RISC-V 400 MHz, runs all firmware |
| WiFi/BLE coprocessor | ESP32-C6 (in same module) | U30 (internal) | Wireless connectivity, controlled by P4 via UART |
| DAQ channel 1 | ADAQ7769-1 | U1 | 24-bit, 1 MSPS, integrated PGA + AAF + ADC |
| DAQ channel 2 | ADAQ7769-1 | U22 | Same — shares SPI bus with U23 |
| DAQ channel 3 | ADAQ7769-1 | U23 | Same — shares SPI bus with U22 |
| Analog mux (±15V) | ADG5204 | U24 | 4:1 mux, routes CSA signals to ADAQ1 |
| Analog mux (24V) | ADG5204 | U25 | 4:1 mux, routes signals to ADAQ3 |
| HV switch bank 1 | ADG6412 | U9 | Quad SPST, handles V_DUT path |
| HV switch bank 2 | ADG6412 | U13 | Quad SPST, bypasses R27 (2Ω shunt) |
| DUT power supply | LTM8056 | U27 | Buck-boost, 0–~20V programmable |
| DUT supply control | DS4424 | U26 | I2C quad current DAC, sets V_DUT and I_limit |
| Current sense amps | AD8411A | U10, U14, U16 | High-side current sense, gain 50 V/V |
| OC comparators | ADCMP600 | U8, U11, U15, U17 | SET/RESET comparators for fault latch |
| SR fault latches | SN74HCS02 | U12 | 2× NOR-gate SR latches (51Ω and 2Ω ranges) |
| Precision VREF | ADR4540 | U18 | 4.096 V reference for all 3 ADAQs |
| MCLK | SiT8208AI | Y1 | 16.384 MHz MEMS oscillator |
| MCLK buffer | CDCLVC1104 | U20 | 1:4 fan-out, gated by 3V3 power-good |
| Temp sensor 1 | AD7415 | U2 | I2C, monitors ADG area |
| Temp sensor 2 | AD7415 | U28 | I2C, monitors power area |
| Buck-boost for DUT | LTM8056 | U27 | DUT supply |
| Main regulator | LTM8078 | U4 | Dual step-down: 5V3_BUCK + 3V3_ESP |
| 3V3 LDO (analog) | TPS74601 | U3 | 3.30V analog rail, software-gated |
| Bipolar switcher | ADP5071 | U5 | ±26V boost/inverting for PGA supplies |
| +24V LDO | ADP7142 | U6 | +24.1V from 26V_BOOST, PGA positive rail |
| −24V LDO | ADP7182 | U7 | −24.0V from −26V_BOOST, PGA negative rail |
| 5V reference | LT6656-5 | U21 | ~5V ref → comparator threshold network |
| Op-amp buffer | ADA4051-1 | U19 | 250mV offset reference for AD8411A VREF |
| Status LEDs | WS2812B ×8 | (LED array) | Addressable RGB, driven by P4 or C6 |

---

## 2. Connectors

| Connector | Part | Function |
|---|---|---|
| J1 | USB4085-GF-A (USB-C) | **Program/debug port.** VBUS → 5V_BUCK (through D3 reverse-protect diode). ESP32-P4 USB-Serial-JTAG/FS (GPIO24/25). CC1/CC2 pulled to GND via 5.1kΩ (device-mode detection). |
| J2 | PR20204VBDN (8-pos) | **External power harness (REQUIRED).** Pins: 1=+15V_ANA, 2=VADJ1_BUCK(NC), 3=−15V_ANA, 4=VADJ2_BUCK(NC), 5=20V_USB, 6=5V_BUCK, 7/8=GND. Board CANNOT run without J2 — 20V_USB feeds LTM8078, ±15V_ANA feeds U24. |
| J3 | PR20204VBDN | Reserved. 4 GND pins + 4 signal pins with no connections — do not use. |
| J4 | (TBD type) | **ESP32-C6 JTAG.** Pins: 1=C6_MTMS, 3=C6_MTDI, 4=C6_MTCK, 5=C6_MTDO, 6=C6_IO13. For C6 firmware debug. |
| J5 | PR20204VBDN | **Expansion.** Pin 1=GPIO6, pin 5=GPIO27, pin 7=GPIO28, plus USB HS OTG (USB_HS_N/USB_HS_P from module pins 48/49). |
| P1 | 691382010004 | DUT current-return terminal. Carries `OUT_DUT` net + R30 (50mΩ shunt). |

---

## 3. Power Tree

### 3.1 Rails

| Rail | Voltage | Source | Status |
|---|---|---|---|
| `20V_USB` | ~20V (external) | J2 pin 5 | Always on when J2 connected |
| `5V_BUCK` | ~5V | J1 VBUS → D3, OR J2 pin 6 | Always on (diode ORed) |
| `5V3_BUCK` | **5.31V** | LTM8078 (U4) VOUT1 | Always on when 20V_USB present |
| `3V3_ESP` | **3.30V** | LTM8078 (U4) VOUT2 | Always on when 20V_USB present |
| `3V3` | **3.30V** | TPS74601 (U3) | **Software-gated via GPIO42** |
| `26V_BOOST` | **26.1V** | ADP5071 (U5) boost | **Software-gated via GPIO54** |
| `−26V_BOOST` | **−26.6V** | ADP5071 (U5) inverting | Same enable as 26V_BOOST |
| `24V_LDO` | **24.1V** | ADP7142 (U6) | **Software-gated via GPIO3** |
| `−24V_LDO` | **−24.0V** | ADP7182 (U7) | Same enable as 24V_LDO (shared net) |
| `+15V_ANA` | ~15V (external) | J2 pin 1 | Always on when J2 connected |
| `−15V_ANA` | ~−15V (external) | J2 pin 3 | Always on when J2 connected |
| `5V_LDO` | ~5V | ADAQ7769-1 U1 internal LDO out | On when 5V3_BUCK + U1 powered |
| `5V_LDO1` | ~5V | ADAQ7769-1 U22 internal LDO out | On when 5V3_BUCK + U22 powered |
| `5V_LDO2` | ~5V | ADAQ7769-1 U23 internal LDO out | On when 5V3_BUCK + U23 powered |
| `REF_OUT` | **4.096V** | ADR4540 (U18) | On when 5V_LDO (U1) powered |
| `V_DUT` | 0–~20V programmable | LTM8056 (U27) | **Software-gated via GPIO4** |

### 3.2 Firmware Power Sequencing

All HV rails are **disabled at boot** by hardware pull-down resistors. Firmware must explicitly enable them.

```
Boot state → All pull-downs hold rails off:
  GPIO54 → R41 (100kΩ to GND) → ADP5071 EN = LOW → ±26V OFF ✅
  GPIO3  → R2  (100kΩ to GND) → ADP7142/7182 EN = LOW → ±24V OFF ✅
  GPIO42 → R20 (100kΩ to GND) → TPS74601 EN = LOW → 3V3 OFF ✅
  GPIO4  → R3  (100kΩ to GND) → LTM8056 RUN = LOW → V_DUT OFF ✅

Always-on at boot (LTM8078 hardwired):
  3V3_ESP (ESP32-P4 module power) — always on once J2 is connected
  5V3_BUCK (analog IC supply) — always on once J2 is connected
```

**Recommended firmware enable sequence:**
1. Configure GPIO54, GPIO3, GPIO42, GPIO4 as outputs, hold LOW (redundant with pull-downs, but explicit)
2. Enable `3V3` analog rail: **GPIO42 HIGH** → TPS74601 EN → wait for PG signal on GPIO41 ⚠
3. Enable ±26V: **GPIO54 HIGH** → ADP5071 EN1+EN2 → allow ~1ms for rails to settle
4. Enable ±24V: **GPIO3 HIGH** → ADP7142 + ADP7182 EN (shared net) → allow ~1ms
5. Release ADAQ resets: **GPIO2 HIGH** (already HIGH due to WPU, but drive it explicitly)
6. Enable V_DUT: **GPIO4 HIGH** → LTM8056 RUN active

> ⚠ **GPIO41 (`NetR51_1`) identity:** The netlist shows GPIO41 → a resistor (R51), likely the TPS74601 PG pull-up. Verify in schematic that R51 connects TPS74601's PG pin to 3V3_ESP (open-drain PG needs pull-up), with GPIO41 reading the pulled-up level. If confirmed, poll GPIO41 in step 2 before proceeding.

> ⚠ **LTM8078 PG1/PG2 (5V3_BUCK and 3V3_ESP power-good) are UNMONITORED.** Nets `NetU4_A2` (PG1) and `NetU4_B1` (PG2) are dangling single-pin nets — no pull-up, no GPIO connection. Firmware has no hardware indication of these two critical rail failures.

### 3.3 DUT Supply Control

The LTM8056 (U27) output voltage and current limit are both set by the DS4424 (U26) current-output DAC over I2C.

**Verified from live netlist:**
- `DS4424 OUT0` → `I_FB_DCDC` → LTM8056 CTL pin (current limit control)
- `DS4424 OUT1` → `V_FB_DCDC` → LTM8056 FB pin (output voltage set)

> ⚠ **Older docs have these swapped.** Trust the live netlist: **channel 0 = current limit, channel 1 = voltage**.

**Voltage formula** (`ltm8056fa.pdf` p.7):
```
V_DUT = 1.2 × (1 + R56/R57) − R56 × I_DAC_ch1
      = 1.2 × (1 + 90.9k/11.3k) − 90.9kΩ × I_DAC_ch1
      = 10.85V − 90.9kΩ × I_DAC_ch1
```
With DS4424 I_FS = 100µA (set by hardware Rfs resistors):
- V_DUT_max ≈ **19.94V** (I_DAC_ch1 at −100µA)
- V_DUT_min ≈ **1.76V** (I_DAC_ch1 at +100µA)

**Current limit formula** (LTM8056 CTL pin, `ltm8056fa.pdf` p.8): Full-scale limit = 2.636A (58mV / 22mΩ sense resistor R55). DS4424 OUT0 sinks current to reduce CTL below 1.2V threshold, reducing the limit.

**Input current monitoring:**
- `IINMON` (LTM8056 L3 pin) → `NetU27_L3` → **GPIO22 (ADC1_CH6)**. Range: 0–1.0V at 0–2.5A input.
- `IOUTMON` (LTM8056 L2 pin) → `NetU27_L2` → **GPIO23 (ADC1_CH7)**. Range: 0–1.2V at 0–2.636A output.

Both signals are within ADC1 input range (0–3.1V per ESP32-P4 datasheet §4.1.1). Use ADC1 channels 6 and 7.

---

## 4. ESP32-P4 Module Pin Map (Complete, Verified)

Module: **Waveshare ESP32-P4-Module** (SoC: ESP32-P4NRW32, 16MB flash, 32MB stacked PSRAM).  
88 castellated pins. Net names below are from the live compiled netlist.

> **Module-internal resources (not on castellations):**  
> — Flash: separate physical IC on module, wired to chip's dedicated flash pins (not GPIO-numbered, not on castellations)  
> — PSRAM: stacked in-package, completely internal  
> — Both are fully independent of the GPIO pin numbering; GPIO0–54 are unaffected.

| Module Pin | Chip Signal | Net (live) | Firmware Function | Notes |
|---|---|---|---|---|
| 1 | GND | GND | — | |
| 2 | LNA_OUT | NetC85_2 | C6 antenna | RF only |
| 3 | GND | GND | — | |
| 4 | C6_U0RXD | C6_TX | C6 UART RX (see §5) | P4 GPIO32 drives this as TX |
| 5 | C6_U0TXD | C6_RX | C6 UART TX (see §5) | P4 GPIO33 receives from C6 |
| 6 | C6_IO15 | NetR73_2 | C6-driven LED data | Through resistor R73 |
| 7 | C6_IO14 | NetR42_2 | C6 GPIO (via R42) | Not floating — check R42 destination |
| 8 | C6_IO13 | NetJ4_6 | C6 JTAG (J4 pin 6) | |
| 9 | C6_IO9/BOOT | C6_BOOT | C6 boot mode strap | P4 GPIO44 drives this |
| 10 | C6_IO8/BOOT_EN | C6_RST | C6 boot-enable strap | P4 GPIO43 drives this |
| 11 | C6_IO7/MTDO | NetJ4_5 | C6 JTAG MTDO (J4 pin 5) | |
| 12 | C6_IO6/MTCK | NetJ4_4 | C6 JTAG MTCK (J4 pin 4) | |
| 13 | C6_IO5/MTDI | NetJ4_3 | C6 JTAG MTDI (J4 pin 3) | |
| 14 | C6_IO4/MTMS | NetJ4_1 | C6 JTAG MTMS (J4 pin 1) | |
| 15 | GND | GND | — | |
| 16 | GPIO1 | NetU30_16 | **FREE** (1-pin, floating) | LP domain |
| 17 | GPIO2 | NetU1_C9 | All 3 ADAQ *RST | LP, IE+WPU at reset → HIGH at boot ✅ |
| 18 | GPIO3 | NetR2_2 | ±24V LDO EN (OUTPUT) | R2 100kΩ to GND — LOW at boot |
| 19 | GPIO4 | NetR3_2 | LTM8056 RUN (OUTPUT) | R3 100kΩ to GND — LOW at boot |
| 20 | GPIO5 | NetU22_E14 | ADAQ2 *DRDY (INPUT) | DRDY for U22 |
| 21 | GPIO6 | NetJ5_1 | J5 expansion pin 1 | LP, TOUCH_CH5 |
| 22 | GPIO7 | NetU22_E13 | ADAQ2 *CS (OUTPUT) | Native SPI2 CS0 pad |
| 23 | GPIO8 | NetU22_E12 | ADAQ2+3 SDI/MOSI (OUTPUT) | Native SPI2 MOSI pad |
| 24 | GPIO9 | NetU22_E11 | ADAQ2+3 SCLK (OUTPUT) | Native SPI2 CLK pad |
| 25 | GPIO10 | NetU22_E10 | ADAQ2+3 DOUT/MISO (INPUT) | Native SPI2 MISO pad; shared, safe per ADAQ t12 |
| 26 | GPIO11 | NetU1_E14 | ADAQ1 *DRDY (INPUT) | Separate DRDY for U1 |
| 27 | GPIO12 | NetU1_E13 | ADAQ1 *CS (OUTPUT) | |
| 28 | GPIO13 | NetU1_E12 | ADAQ1 SDI/MOSI (OUTPUT) | |
| 29 | GPIO20 | NetU1_E11 | ADAQ1 SCLK (OUTPUT) | ADC1_CH4 — not usable as analog here |
| 30 | GPIO21 | NetU1_E10 | ADAQ1 DOUT/MISO (INPUT) | ADC1_CH5 — not usable as analog here |
| 31 | GPIO22 | NetU27_L3 | **IINMON — ADC1_CH6 (INPUT)** | LTM8056 input current mon, 0–1V |
| 32 | GPIO23 | NetU27_L2 | **IOUTMON — ADC1_CH7 (INPUT)** | LTM8056 output current mon, 0–1.2V |
| 33 | GND | GND | — | |
| 34–39 | DSI_DATAx/CLK | NetU30_34–39 | Dedicated MIPI-DSI | Not usable as GPIO |
| 40 | GND | GND | — | |
| 41–46 | CSI_DATAx/CLK | NetU30_41–46 | Dedicated MIPI-CSI | Not usable as GPIO |
| 47 | GND | GND | — | |
| 48 | USB_DM | USB_HS_N | USB 2.0 HS OTG D− | Fixed-function, → J5 |
| 49 | USB_DP | USB_HS_P | USB 2.0 HS OTG D+ | Fixed-function, → J5 |
| 50 | GPIO24/USB1P1_N0 | USB_FS_N | USB-Serial-JTAG D− → J1 | Boot: blank/blank |
| 51 | GPIO25/USB1P1_P0 | USB_FS_P | USB-Serial-JTAG D+ → J1 | Boot: USB pull-up enabled (HIGH) |
| 52 | GND | GND | — | |
| 53 | GPIO26 | OK_KEY | **Button: OK/Enter (INPUT)** | VDD_IO_4; configure INPUT_PULLUP |
| 54 | GPIO27 | NetJ5_5 | J5 expansion pin 5 | USB1P1_P1 alt — not a strap |
| 55 | GPIO28 | NetJ5_7 | J5 expansion pin 7 | |
| 56 | GPIO29 | NetU23_E14 | ADAQ3 *DRDY (INPUT) | Separate DRDY for U23 |
| 57 | GPIO30 | NetU23_E13 | ADAQ3 *CS (OUTPUT) | |
| 58 | GPIO31 | NetU30_58 | **FREE** (1-pin, floating) | VDD_IO_4 |
| 59 | GPIO32 | C6_TX | C6 UART **TX from P4** (OUTPUT) | P4 drives → C6 U0RXD |
| 60 | GPIO33 | C6_RX | C6 UART **RX to P4** (INPUT) | P4 receives ← C6 U0TXD |
| 61 | GPIO34 | NetU30_61 | **FREE** (1-pin, floating) | Strapping pin: JTAG src select |
| 62 | GPIO35 | P4_BOOT | Boot-mode strap (INPUT) | WPU → HIGH = SPI Boot (normal). Pull LOW to force download |
| 63 | GPIO36 | NetDNP_1 | DNP 100kΩ pull-down ⚠ | Strapping pin! See §4.1 |
| 64 | GPIO37 | NetTX_1 | UART0 TX (default IOMUX) | Strapping pin — also serial console TX |
| 65 | GPIO38 | NetRX_1 | UART0 RX (default IOMUX) | Strapping pin — also serial console RX |
| 66 | ESP_LDO_VO4 | NetU30_66 | Internal 1.8V LDO out | Do not load. 1-pin net (correct) |
| 67 | GPIO39 | NetR68_1 | I2C SDA (I/O) | R68 pull-up to 3V3_ESP |
| 68 | GPIO40 | NetR69_1 | I2C SCL (OUTPUT) | R69 pull-up to 3V3_ESP |
| 69 | GPIO41 | NetR51_1 | TPS74601 PG monitor (INPUT) ⚠ | Verify R51 connection — likely PG pull-up |
| 70 | GPIO42 | NetR20_1 | 3V3 LDO EN (OUTPUT) | R20 100kΩ to GND — LOW at boot |
| 71 | GPIO43 | C6_RST | C6 BOOT_EN strap (OUTPUT) | Controls C6 download mode |
| 72 | GPIO44 | C6_BOOT | C6 BOOT strap (OUTPUT) | Controls C6 download mode |
| 73 | GPIO45 | DN_KEY | **Button: Down/decrease (INPUT)** | VDD_IO_5; configure INPUT_PULLUP |
| 74 | GPIO46 | UP_KEY | **Button: Up/increase (INPUT)** | VDD_IO_5; configure INPUT_PULLUP |
| 75 | GPIO47 | NetU25_14 | ADG5204 U25 **A1** (OUTPUT) | Mux address bit for U25 |
| 76 | GPIO48 | NetU25_15 | ADG5204 U25 **A0** (OUTPUT) | Mux address bit for U25 |
| 77 | GPIO49 | NetU24_16 | ADG5204 U24+U25 **EN** (OUTPUT) | Shared enable for both U24 and U25 |
| 78 | GPIO50 | NetU24_15 | ADG5204 U24 **A0** (OUTPUT) | Mux address bit for U24 |
| 79 | GPIO51 | NetU24_14 | ADG5204 U24 **A1** (OUTPUT) | Mux address bit for U24 |
| 80 | GPIO52 | NetR25_2 | ADG6412 U9 bypass (OUTPUT) | Via resistor; HIGH = bypass closed |
| 81 | GPIO53 | NetR28_2 | ADG6412 U13 bypass (OUTPUT) | Via resistor; HIGH = bypass closed |
| 82 | GPIO54 | NetR41_1 | ADP5071 EN (OUTPUT) | R41 100kΩ to GND — LOW at boot |
| 83 | GND | GND | — | |
| 84 | VBAT | 3V3_ESP | Module power | |
| 85 | ESP_3V3 | 3V3_ESP | Module power | |
| 86 | ESP_3V3 | 3V3_ESP | Module power | |
| 87 | ESP_EN | NetC91_2 | CHIP_PU → RC filter → 3V3_ESP | Decoupled, must not float |
| 88 | GPIO0 | NetU30_88 | **FREE** (1-pin, floating) | Not a strap on P4; XTAL_32K_N alt |

### 4.1 Strapping Pin Summary

| GPIO | Pin | Boot strap function | Board state | Risk |
|---|---|---|---|---|
| GPIO35 | 62 | Boot mode (0=download, 1=SPI boot) | `P4_BOOT` pulled HIGH by internal WPU → normal boot. User button pulls LOW to force download | Safe by design |
| GPIO36 | 63 | Boot mode + UART0 ROM print ctrl | Connected to `DNP` component (100kΩ to GND). If DNP resistor **is installed**, GPIO36 floats LOW at strap-sample → changes boot mode. If not installed, floats → default. **Clarify assembly intent.** | ⚠ Medium risk |
| GPIO37 | 64 | Boot mode + UART0 TXD (default) | `NetTX_1` (UART0 TX) — floats as strap, then becomes console TX | Safe (floats at strap time) |
| GPIO38 | 65 | Boot mode + UART0 RXD (default) | `NetRX_1` (UART0 RX) — same | Safe |
| GPIO34 | 61 | JTAG source select | 1-pin net (floating). **No internal pull** — if JTAG source selection matters, add external pull | Note only |

### 4.2 IO Power Domains

| Domain | GPIOs | Key implication |
|---|---|---|
| VDD_LP (LP + HP shared) | 0–15 | GPIO0–15 — LP core powered; LP-UART default on GPIO14/15 |
| VDD_IO_0 | 16–23 | GPIO20–23 only (16–19 not on module). ADC1_CH4–CH7. |
| VDD_IO_4 | 24–38 | USB-FS, UART0 default, SPI2 8-line path |
| VDD_IO_5 | 39–48 | SDIO1 default at IO-MUX level (benign, nothing uses SDIO1) |
| VDD_IO_6 | 49–54 | ADC2_CH0–CH5. ALL currently consumed by digital outputs. |

**ADC availability on this board:**
- ADC1: CH6 (GPIO22 → IINMON) and CH7 (GPIO23 → IOUTMON) — **in use**; CH4/CH5 (GPIO20/21) consumed by ADAQ1 SPI. No free ADC1 channels.
- ADC2: All 6 channels (GPIO49–54) consumed by digital outputs. **No free ADC2 channels.**
- GPIO16–19 are **not broken out** on this Waveshare module. Do not reference them.

### 4.3 Default-JTAG GPIO Caution

GPIO2/3/4/5 are the ESP32-P4's default JTAG pad signals (MTCK/MTDI/MTMS/MTDO). They are used here for ADAQ RST, ±24V EN, LTM8056 RUN, and ADAQ2 DRDY. **Use USB-Serial-JTAG (via J1, GPIO24/25) for debugging** — do not connect a JTAG probe to the JTAG pads without first disabling the pad-JTAG interface in firmware, or the DAQ signals will contend.

---

## 5. ESP32-C6 Internal Wiring Map

The Waveshare module houses both the ESP32-P4 and an **ESP32-C6FH8** on one substrate. Understanding what is module-internal (fixed, carrier-board cannot change) vs. carrier-board-added (our schematic choices) is critical.

### 5.1 Module-Internal Wiring (Fixed — not changeable by the carrier board)

| Item | Detail |
|---|---|
| C6 power | C6 shares `ESP_3V3`/`VBAT` with the P4. No independent power-down. |
| C6 CHIP_PU (hardware enable) | Tied permanently HIGH via R76 (10kΩ to ESP_3V3) **inside the module**. Not broken out to any castellated pin. **The C6 cannot be hardware-reset or power-gated independently.** |
| C6 crystal | Y3, 40 MHz, with its own dedicated load caps (15pF × 2) and ferrite — fully independent of the P4 crystal. |
| C6 antenna | Module pin 2 (LNA_OUT) → internal matching network → C6 chip ANT pin. RF only, not a GPIO. |
| C6 flash | C6FH8 has 8MB embedded flash (in-package) — no external flash component on the module. |

### 5.2 Carrier-Board Wiring (Connections from our schematic)

| Module Pin | C6 Signal | Net | P4 Signal | Firmware use |
|---|---|---|---|---|
| 4 | C6_U0RXD | C6_TX | GPIO32 | P4 **transmits** on GPIO32 → into C6's RX. Configure GPIO32 as UART TX in ESP-IDF. |
| 5 | C6_U0TXD | C6_RX | GPIO33 | P4 **receives** on GPIO33 ← from C6's TX. Configure GPIO33 as UART RX. |
| 6 | C6_IO15 | NetR73_2 | — | C6-driven (likely WS2812B LED chain) via resistor R73 |
| 7 | C6_IO14 | NetR42_2 | — | Connected via R42 — verify destination (new connection vs. old docs) |
| 8 | C6_IO13 | NetJ4_6 | — | C6 JTAG auxiliary — goes to J4 connector |
| 9 | C6_IO9/BOOT | C6_BOOT | GPIO44 | P4 GPIO44 drives C6 boot-mode strap |
| 10 | C6_IO8/BOOT_EN | C6_RST | GPIO43 | P4 GPIO43 drives C6 boot-enable strap |
| 11 | C6_IO7/MTDO | NetJ4_5 | — | C6 JTAG MTDO → J4 |
| 12 | C6_IO6/MTCK | NetJ4_4 | — | C6 JTAG MTCK → J4 |
| 13 | C6_IO5/MTDI | NetJ4_3 | — | C6 JTAG MTDI → J4 |
| 14 | C6_IO4/MTMS | NetJ4_1 | — | C6 JTAG MTMS → J4 |

> **`C6_RST` / `C6_BOOT` naming caveat:** These net names are misleading. Because C6's CHIP_PU is permanently HIGH inside the module (§5.1), **P4 cannot hardware-reset the C6**. What these signals actually do is put the C6 into **UART download mode** (same mechanism as P4's own GPIO35 strap). To reflash the C6 via P4: drive GPIO43 LOW (BOOT_EN=0) + GPIO44 LOW (BOOT=0), power-cycle is not possible but a C6 software reset (RTC_CNTL) can approximate one. The C6 only enters download mode at next boot.

**C6 UART direction (firmware perspective from P4):**
- **P4 GPIO32 = UART TX** (output) → goes into C6 U0RXD (pin 4)
- **P4 GPIO33 = UART RX** (input) ← comes from C6 U0TXD (pin 5)

Use a P4 UART peripheral (UART1 or UART2, routed via GPIO matrix) configured on GPIO32/33. UART0 is occupied by the console (GPIO37/38).

**Free C6 GPIOs** (not used in our schematic):
- C6_IO14 (module pin 7): connected via R42 — destination unclear, not fully free
- All C6 JTAG pins (module pins 11–14) go to J4 — accessible externally for C6 debugging

---

## 6. SPI Bus Map

### 6.1 Architecture

Two independent SPI buses for the three ADAQ7769-1 channels:

| Bus | SPI Controller | Channels served | Routing |
|---|---|---|---|
| Bus B | GP-SPI2 (SPI2 in ESP-IDF) | ADAQ2 (U22) + ADAQ3 (U23) — shared | Native IOMUX on GPIO7–10 for SCLK/MOSI/MISO/CS(U22); U23 CS via GPIO matrix |
| Bus A | GP-SPI3 (SPI3 in ESP-IDF) | ADAQ1 (U1) — dedicated | All via GPIO matrix (no native IOMUX) |

> Both GP-SPI2 and GP-SPI3 support DMA (verified from TRM Table 43.10-1). LP-SPI has no DMA — do not use LP-SPI.  
> Max SPI clock from ADAQ timing spec (`tSCLK` minimum 50ns): **20 MHz**. The controller supports 80 MHz but the ADAQ itself caps at 20 MHz.

### 6.2 SPI Pin Assignments

**Bus B — GP-SPI2 (ADAQ2 + ADAQ3 shared):**

| Signal | GPIO | Module Pin | ESP-IDF SPI role | Native IOMUX? |
|---|---|---|---|---|
| SCLK | GPIO9 | 24 | sclk_io_num | Yes (SPI2_CK) |
| MOSI (SDI) | GPIO8 | 23 | mosi_io_num | Yes (SPI2_D) |
| MISO (DOUT) | GPIO10 | 25 | miso_io_num | Yes (SPI2_Q) |
| CS — ADAQ2 (U22) | GPIO7 | 22 | device cs_io_num | Yes (SPI2_CS0) |
| CS — ADAQ3 (U23) | GPIO30 | 57 | device cs_io_num | Via GPIO matrix |
| DRDY — ADAQ2 | GPIO5 | 20 | interrupt GPIO | — |
| DRDY — ADAQ3 | GPIO29 | 56 | interrupt GPIO | — |

**Bus A — GP-SPI3 (ADAQ1 dedicated):**

| Signal | GPIO | Module Pin | ESP-IDF SPI role | Native IOMUX? |
|---|---|---|---|---|
| SCLK | GPIO20 | 29 | sclk_io_num | No (via matrix) |
| MOSI (SDI) | GPIO13 | 28 | mosi_io_num | No (via matrix) |
| MISO (DOUT) | GPIO21 | 30 | miso_io_num | No (via matrix) |
| CS — ADAQ1 (U1) | GPIO12 | 27 | device cs_io_num | No (via matrix) |
| DRDY — ADAQ1 | GPIO11 | 26 | interrupt GPIO | — |

### 6.3 ADAQ Bus Sharing Safety

U22 and U23 share SCLK/MOSI/MISO (nets `NetU22_E11`, `NetU22_E12`, `NetU22_E10`). This is safe because:  
Per ADAQ7769-1 datasheet timing spec t12: CS rising edge → DOUT high-impedance ≤ 7ns (at 3.3V). With separate CS lines (GPIO7 for U22, GPIO30 for U23), the MISO bus is properly tristated. Only operate in **4-wire SPI mode** (CS toggling per transaction); do NOT use 3-wire/CS-tied-low mode on these devices.

### 6.4 ADAQ Reset and Sync

- **All 3 ADAQ *RST** tied together on `NetU1_C9` → GPIO2. Single shared reset — resetting one resets all three simultaneously. GPIO2 has a weak internal pull-up (IE+WPU) after reset, holding *RST HIGH (out-of-reset) at boot.
- **SYNC:** U1 is sync master. `DAQ_SYNC` net carries U1's *SYNC_OUT to U22 and U23 *SYNC_IN. U22/U23 *SYNC_OUT are floating (unused — correct for this topology).
- **MCLK:** SiT8208 Y1 (16.384 MHz) → CDCLVC1104 U20 → 3 separate outputs (CLOCK_0→U1, CLOCK_1→U22, CLOCK_2→U23). Clock buffer 1G enable is driven by TPS74601 PG — MCLK is physically gated until 3V3 is confirmed good. CLOCK_3 output is unloaded (floating).

### 6.5 SPI Configuration for ESP-IDF

```c
// Bus B (GP-SPI2) — ADAQ2 + ADAQ3
spi_bus_config_t bus_b = {
    .mosi_io_num   = 8,
    .miso_io_num   = 10,
    .sclk_io_num   = 9,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
};
// Device: ADAQ2 (U22)
spi_device_interface_config_t adaq2_cfg = {
    .clock_speed_hz = 20 * 1000 * 1000,   // 20 MHz max (ADAQ tSCLK)
    .mode           = 0,                    // CPOL=0, CPHA=0 — verify with ADAQ datasheet
    .spics_io_num   = 7,
    .queue_size     = 1,
};
// Device: ADAQ3 (U23) — same bus, different CS
spi_device_interface_config_t adaq3_cfg = { .spics_io_num = 30, /* rest same */ };

// Bus A (GP-SPI3) — ADAQ1
spi_bus_config_t bus_a = {
    .mosi_io_num = 13, .miso_io_num = 21, .sclk_io_num = 20,
};
// Device: ADAQ1 (U1)
spi_device_interface_config_t adaq1_cfg = { .spics_io_num = 12, /* same settings */ };
```

> ⚠ Verify SPI mode (CPOL/CPHA) from ADAQ7769-1 datasheet timing diagrams before using mode 0. The ADAQ datasheet `adaq7769-1.pdf` is the authoritative source.

---

## 7. I2C Bus

**Bus:** GPIO39 (SDA) / GPIO40 (SCL)  
**Pull-ups:** R68 (SDA, 10kΩ to 3V3_ESP), R69 (SCL, 10kΩ to 3V3_ESP)  
**Power:** 3V3_ESP (always-on) supplies the pull-ups; devices powered from separate rails

```c
i2c_config_t i2c_cfg = {
    .mode          = I2C_MODE_MASTER,
    .sda_io_num    = 39,
    .scl_io_num    = 40,
    .sda_pullup_en = GPIO_PULLUP_DISABLE,  // external R68 pull-up
    .scl_pullup_en = GPIO_PULLUP_DISABLE,  // external R69 pull-up
    .master.clk_speed = 400000,            // 400 kHz Fast mode — verify against each device
};
```

### 7.1 Device Address Map

| Designator | Part | Wiring (from live netlist) | 7-bit I2C Address | Datasheet ref |
|---|---|---|---|---|
| U26 | DS4424 | A0=GND, A1=GND | **0x10** | `DS4422-DS4424.pdf` Table 1 — the datasheet prints "20h" which is the pre-shifted 8-bit write byte; the true 7-bit address is 0x10. Do not use 0x20. |
| U2 | AD7415 | VDD=5V3_BUCK, AS=GND | **0x49** | `ad7414_7415.pdf` Table 4: AD7415-0 with AS=GND → `1001001` = 0x49 |
| U28 | AD7415 | VDD=5V_BUCK, AS=5V_BUCK (=VDD) | **0x4A** | Same datasheet Table 4: AS=VDD → `1001010` = 0x4A |

No address collisions. All three addresses (0x10, 0x49, 0x4A) are unique.

> ⚠ **U2 and U28 run on different supply rails** (5V3_BUCK vs. 5V_BUCK). Both are 5V-range devices; verify both rails are present before polling.

### 7.2 DS4424 DAC Register Usage

From `DS4422-DS4424.pdf`:
- **Channel 0 register (address 0xF8):** Controls `I_FB_DCDC` → LTM8056 CTL pin → **DUT current limit**
- **Channel 1 register (address 0xF9):** Controls `V_FB_DCDC` → LTM8056 FB pin → **DUT output voltage**
- Channels 2 and 3 are unused (OUT2/OUT3 are floating single-pin nets, FS2/FS3 also floating)

Each register is an 8-bit signed value where bit 7 = direction (1=source/push, 0=sink/pull) and bits 6:0 = magnitude. See datasheet §Register Map for exact encoding.

---

## 8. Analog Front-End — Mux Control

### 8.1 ADG5204 U24 (±15V_ANA supply, feeds ADAQ channel signals)

| GPIO | Function |
|---|---|
| GPIO49 | U24 EN + U25 EN (**shared**, enables both muxes simultaneously) |
| GPIO50 | U24 A0 (address bit 0) |
| GPIO51 | U24 A1 (address bit 1) |

> ⚠ **Boot-time:** GPIO49 defaults to inert/blank (no pull). The ADG5204 EN pin has no confirmed pull-down in the netlist. If EN floats HIGH at boot, the mux may enable with indeterminate address. Verify against `adg5204.pdf` whether EN has an internal pull-down. Add 100kΩ pull-down to GND on `NetU24_16` if no internal pull exists.

Channel selection (A1:A0 → Sx):
| A1 | A0 | Selected channel |
|---|---|---|
| 0 | 0 | S1 |
| 0 | 1 | S2 |
| 1 | 0 | S3 (→ EXT_A0, floating on U24) |
| 1 | 1 | S4 (→ NetU24_9, floating — unconnected input) |

### 8.2 ADG5204 U25 (24V_LDO supply, feeds ADAQ channel signals)

| GPIO | Function |
|---|---|
| GPIO49 | U25 EN (same net as U24 EN — both enabled/disabled together) |
| GPIO47 | U25 A1 (address bit 1) |
| GPIO48 | U25 A0 (address bit 0) |

### 8.3 ADG6412 U9 and U13 (Bypass Switches)

- **GPIO52 → `NetR25_2`** → via resistor → ADG6412 U9 control (all 4 switches in parallel). U9 runs on 24V_LDO/GND. HIGH = bypass closed.
- **GPIO53 → `NetR28_2`** → via resistor → ADG6412 U13 control. U13 bridges across R27 (2Ω shunt). HIGH = R27 bypassed (≈0.125Ω path).

---

## 9. Overcurrent Protection (Hardware, Independent of Firmware)

The SR latches (U12) on the 51Ω and 2Ω current-sense ranges operate independently of firmware:

| Range | Shunt | Current-sense amp | SET comparator | RESET comparator | SET threshold | RESET threshold |
|---|---|---|---|---|---|---|
| 51Ω | R24 (51Ω ±5%) | U10 (AD8411A) | U8 (ADCMP600) | U11 (ADCMP600) | **1.434 mA** | **30.1 µA** |
| 2Ω | R27 (2Ω ±1%) | U14 (AD8411A) | U15 (ADCMP600) | U17 (ADCMP600) | **36.56 mA** | **768 µA** |

The latch fires (Q HIGH) when CSA output > 3.906V (SET threshold) and holds until CSA drops below 325mV (RESET threshold). Bypass switches (via U12 Q output → U9/U13 control) route current around the shunt when a fault is latched.

**50mΩ range (R30, U16) has NO hardware latch protection.** ADAQ U22 saturates at 1.538A, AD8411A clips at ~2.01A; the only backstop is LTM8056's hardware current limit at 2.636A. Firmware must poll the ADAQ output for saturation (0xFFFFFF) to detect over-range on this channel.

---

## 10. User Interface (Buttons)

Three buttons, verified from live netlist:

| GPIO | Net | Button | Domain | Boot state |
|---|---|---|---|---|
| GPIO26 | OK_KEY | OK / Enter | VDD_IO_4 | Inert/blank — configure INPUT_PULLUP in firmware |
| GPIO45 | DN_KEY | Down / decrease | VDD_IO_5 | Inert/blank — configure INPUT_PULLUP |
| GPIO46 | UP_KEY | Up / increase | VDD_IO_5 | Inert/blank — configure INPUT_PULLUP |

Configure as active-low inputs (button pressed = LOW). The WS2812B status LEDs (8×, net `LED_DIN`) are driven by C6_IO15 (module pin 6) via resistor R73 — controlled by the ESP32-C6.

---

## 11. Known Open Issues for Firmware

| # | Severity | Description | Action |
|---|---|---|---|
| P-GPIO41 | ⚠ Unverified | GPIO41 → `NetR51_1` — function assumed to be TPS74601 PG monitor but R51 destination not confirmed from netlist alone. | Verify R51 connects TPS74601 PG to 3V3_ESP pull-up, and the other side to GPIO41. If confirmed, poll GPIO41 HIGH before enabling 3V3-dependent peripherals. |
| P-R42 | ⚠ Unverified | C6_IO14 (module pin 7) → `NetR42_2` — destination unknown. Was floating in old designs. New connection added. | Trace R42 in schematic. This is a new C6 I/O path not in any previous doc. |
| P-LTM8078-PG | 🔴 Not monitored | LTM8078 PG1 (`NetU4_A2`) and PG2 (`NetU4_B1`) are dangling — firmware has no indication if 5V3_BUCK or 3V3_ESP fails. | Accept limitation or add resistors + GPIO connections. GPIO31 and GPIO34 are free candidates. |
| P-DNP | 🟡 Clarify | GPIO36 (strapping pin) → `NetDNP_1` — 100kΩ pull-down component designated "DNP". If installed, changes boot mode. | Confirm with assembler: is this resistor installed or not? |
| P-ADG5204-EN | 🟡 Verify | GPIO49 → U24+U25 EN — no pull-down confirmed on `NetU24_16`. Boot state of EN may be floating. | Check ADG5204 datasheet `adg5204.pdf` for internal pull. If none, add 100kΩ external pull-down to GND. |
| P-OC-50m | 🟡 Documented | 50mΩ range (U22 channel) has no hardware OC flag between ADAQ saturation (1.538A) and LTM8056 trip (2.636A). | Firmware: monitor ADAQ2 output for 0xFFFFFF (saturation) and treat as an over-range fault. Document in firmware. |
| P-5pct | 🟢 Low | R24 (51Ω shunt) is ±5% tolerance — dominates error budget on that range. | Accept ±5% accuracy on 51Ω range, or add per-board calibration in firmware. |
| P-CR1 | 🟢 Low | CR1 diode orientation (flyback across DUT path) — cathode/anode not verified from netlist alone. | Confirm in schematic before first power-up with inductive DUT. |
| P-SYNC-U23 | 🟢 Info | U23 *SYNC_OUT is floating (1-pin net). U1 is sync master — correct for this 3-device topology. No action. | None |
| P-CLOCK3 | 🟢 Info | CDCLVC1104 CLOCK_3 output is unloaded (1-pin net). No action. | None |

---

## 12. Quick Reference — All GPIO Assignments

| GPIO | Module Pin | Direction | Function | Domain |
|---|---|---|---|---|
| 0 | 88 | — | **FREE** | LP |
| 1 | 16 | — | **FREE** | LP |
| 2 | 17 | OUTPUT (WPU boot) | All 3 ADAQ *RST (active-low) | LP |
| 3 | 18 | OUTPUT | ±24V LDO EN (HIGH=on) | LP |
| 4 | 19 | OUTPUT | LTM8056 RUN (HIGH=DUT on) | LP |
| 5 | 20 | INPUT | ADAQ2 *DRDY | LP |
| 6 | 21 | I/O | J5 expansion | LP |
| 7 | 22 | OUTPUT | ADAQ2 *CS (SPI2 native) | LP |
| 8 | 23 | OUTPUT | ADAQ2+3 SDI/MOSI (SPI2 native) | LP |
| 9 | 24 | OUTPUT | ADAQ2+3 SCLK (SPI2 native) | LP |
| 10 | 25 | INPUT | ADAQ2+3 DOUT/MISO (SPI2 native) | LP |
| 11 | 26 | INPUT | ADAQ1 *DRDY | LP |
| 12 | 27 | OUTPUT | ADAQ1 *CS | LP |
| 13 | 28 | OUTPUT | ADAQ1 SDI/MOSI | LP |
| 14–19 | — | — | NOT ON THIS MODULE | — |
| 20 | 29 | OUTPUT | ADAQ1 SCLK | VDD_IO_0 |
| 21 | 30 | INPUT | ADAQ1 DOUT/MISO | VDD_IO_0 |
| 22 | 31 | INPUT (ADC) | **IINMON** (ADC1_CH6, 0–1V) | VDD_IO_0 |
| 23 | 32 | INPUT (ADC) | **IOUTMON** (ADC1_CH7, 0–1.2V) | VDD_IO_0 |
| 24 | 50 | I/O | USB-Serial-JTAG D− (J1) | VDD_IO_4 |
| 25 | 51 | I/O | USB-Serial-JTAG D+ (J1) | VDD_IO_4 |
| 26 | 53 | INPUT | **OK_KEY button** | VDD_IO_4 |
| 27 | 54 | I/O | J5 expansion | VDD_IO_4 |
| 28 | 55 | I/O | J5 expansion | VDD_IO_4 |
| 29 | 56 | INPUT | ADAQ3 *DRDY | VDD_IO_4 |
| 30 | 57 | OUTPUT | ADAQ3 *CS | VDD_IO_4 |
| 31 | 58 | — | **FREE** | VDD_IO_4 |
| 32 | 59 | OUTPUT | C6 UART TX (from P4 to C6) | VDD_IO_4 |
| 33 | 60 | INPUT | C6 UART RX (from C6 to P4) | VDD_IO_4 |
| 34 | 61 | — | **FREE** (strapping — no pull) | VDD_IO_4 |
| 35 | 62 | INPUT | P4_BOOT strap / download button | VDD_IO_4 |
| 36 | 63 | — | DNP component — **clarify assembly** | VDD_IO_4 |
| 37 | 64 | OUTPUT | UART0 TX (console) | VDD_IO_4 |
| 38 | 65 | INPUT | UART0 RX (console) | VDD_IO_4 |
| 39 | 67 | I/O | I2C SDA | VDD_IO_5 |
| 40 | 68 | OUTPUT | I2C SCL | VDD_IO_5 |
| 41 | 69 | INPUT | TPS74601 PG (3V3 power-good) ⚠ | VDD_IO_5 |
| 42 | 70 | OUTPUT | 3V3 LDO EN (HIGH=3V3 on) | VDD_IO_5 |
| 43 | 71 | OUTPUT | C6 BOOT_EN strap | VDD_IO_5 |
| 44 | 72 | OUTPUT | C6 BOOT strap | VDD_IO_5 |
| 45 | 73 | INPUT | **DN_KEY button** | VDD_IO_5 |
| 46 | 74 | INPUT | **UP_KEY button** | VDD_IO_5 |
| 47 | 75 | OUTPUT | U25 A1 (mux address) | VDD_IO_5 |
| 48 | 76 | OUTPUT | U25 A0 (mux address) | VDD_IO_5 |
| 49 | 77 | OUTPUT | U24+U25 EN (mux enable) | VDD_IO_6 |
| 50 | 78 | OUTPUT | U24 A0 (mux address) | VDD_IO_6 |
| 51 | 79 | OUTPUT | U24 A1 (mux address) | VDD_IO_6 |
| 52 | 80 | OUTPUT | ADG6412 U9 bypass | VDD_IO_6 |
| 53 | 81 | OUTPUT | ADG6412 U13 bypass | VDD_IO_6 |
| 54 | 82 | OUTPUT | ±26V ADP5071 EN (HIGH=on) | VDD_IO_6 |

---

*This document was produced by reading the live compiled Altium netlist (`proj_get_nets`, force_recompile=true, 1,215 pins) on 2026-06-19. It supersedes all earlier pinout/architecture documents in this folder.*
