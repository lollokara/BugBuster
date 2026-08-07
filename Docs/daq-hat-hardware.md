# Power Profiler Pro HAT - hardware reference

Pin, net, rail and address map for the BugBuster_DAQ board (ESP32-P4 + ESP32-C6).
This is the authoritative source for firmware: every assignment below was read
from the compiled Altium netlist of `BugBuster_DAQ.PrjPcb` (force-recompiled
2026-06-23; 1,215 pins, 248 BOM components), not from schematic screenshots or
memory. Component behaviour is taken from the vendor datasheets in
[Datasheets/](Datasheets/).

When the netlist and this document disagree, the netlist wins - recompile and
update this file.

## 1. System Overview

A **3-channel, 24-bit precision data-acquisition board** for characterising a Device Under Test (DUT):

| Block | Part | Designator | Role |
|---|---|---|---|
| Host MCU | ESP32-P4 (in Waveshare module) | U30 | Dual-core RISC-V 400 MHz, runs all firmware |
| WiFi/BLE coprocessor | ESP32-C6 (in same module) | U30 (internal) | Wireless connectivity, controlled by P4 via UART |
| DAQ channel 1 | ADAQ7769-1 | U1 | 24-bit 1 MSPS, input via U24 mux, PGA supply ±15V_LDO |
| DAQ channel 2 | ADAQ7769-1 | U22 | Same - direct CSA_005 input (no mux), PGA supply ±15V_LDO |
| DAQ channel 3 | ADAQ7769-1 | U23 | Same - input via U25 mux, PGA supply ±24V_LDO |
| Analog mux (±15V) | ADG5204 | U24 | 4:1 mux, routes to ADAQ1 (U1). VDD=+15V_LDO |
| Analog mux (+24V) | ADG5204 | U25 | 4:1 mux, routes to ADAQ3 (U23). VDD=+24V_LDO |
| HV switch bank 1 | ADG6412 | U9 | Quad SPST, VDD=+15V_LDO |
| HV switch bank 2 | ADG6412 | U13 | Quad SPST, bridges R27 (2Ω shunt). VDD=+15V_LDO |
| DUT power supply | LTM8056 | U27 | Buck-boost, 0–~20V programmable |
| DUT supply control | DS4424 | U26 | I2C quad current DAC: OUT0=I_limit, OUT1=V_DUT |
| Current sense amps | AD8411A | U10, U14, U16 | High-side current sense, gain 50 V/V |
| OC comparators | ADCMP600 | U8, U11, U15, U17 | SET/RESET comparators for fault latch |
| SR fault latches | SN74HCS02 | U12 | 2× NOR-gate SR latches (51Ω and 2Ω ranges) |
| Precision VREF | ADR4540 | U18 | 4.096 V reference for all 3 ADAQs |
| MCLK oscillator | SiT8208AI | Y1 | 16.384 MHz MEMS oscillator |
| MCLK buffer | CDCLVC1104 | U20 | 1:4 fan-out, 1G gated by TPS74601 PG |
| Temp sensor 1 | AD7415 | U2 | I2C 0x49, monitors ADG area. VDD=5V3_BUCK |
| Temp sensor 2 | AD7415 | U28 | I2C 0x4A, monitors power area. VDD=5V_BUCK |
| Main dual regulator | LTM8078 | U4 | Dual step-down: 5V3_BUCK (5.31V) + 3V3_ESP (3.30V) |
| 3V3 LDO (analog) | TPS74601 | U3 | 3.30V analog rail, software-gated via GPIO42 |
| Bipolar switcher | ADP5071 | U5 | ±26V boost/inverting (±26V_BOOST), EN via GPIO54 |
| +15V LDO | ADP7142 | U6 | +15V_LDO from 26V_BOOST, EN via GPIO3 |
| −15V LDO | ADP7182 | U7 | −15V_LDO from −26V_BOOST, EN via GPIO3 (shared) |
| +24V LDO | ADP7142 | U29 | +24V_LDO from 26V_BOOST, EN via GPIO3 (shared) |
| −24V LDO | ADP7182 | U31 | −24V_LDO from −26V_BOOST, EN via GPIO3 (shared) |
| 5V reference | LT6656-5 | U21 | ~5V ref → comparator threshold network |
| Op-amp buffer | ADA4051-1 | U19 | 250mV offset reference for AD8411A VREF |
| Status LEDs | WS2812B ×8 | (LED array) | Addressable RGB, driven by C6_IO15 via R73 |
| Buzzer | PRO-OB-440 | E1 | Driven by C6_IO14 via R42 |
| **Unknown IC** | SOT-23-3 | **IC1** | ⛔ **UNIDENTIFIED** - "Taken From Chinese PCB" in Altium. Do not rely on until identified. |

---

## 2. Connectors

| Connector | Part | Function |
|---|---|---|
| J1 | USB4085-GF-A (USB-C) | **Program/debug port.** VBUS → 5V_BUCK (through D3 reverse-protect diode). ESP32-P4 USB-Serial-JTAG/FS (GPIO24/25). CC1/CC2 pulled to GND via 5.1kΩ (device-mode detection). |
| J2 | PR20204VBDN (8-pos) | **External power harness.** Pin 5=20V_USB (required - feeds LTM8078 U4), pin 6=5V_BUCK. Pins 1 and 3 are likely NC - ±15V is generated internally by U6/U7; confirm against the schematic before wiring. Pins 2/4 (VADJ1_BUCK/VADJ2_BUCK) are NC. Pins 7/8=GND. |
| J3 | PR20204VBDN | **Partial connection.** 4 GND pins. Pin 2 → U25 S1 (ADG5204 mux input). Remaining signal pins (4/6/8) appear NC - verify in schematic before using as reserved expansion. |
| J4 | (connector TBD) | **ESP32-C6 JTAG.** Pins: 1=C6_MTMS, 3=C6_MTDI, 4=C6_MTCK, 5=C6_MTDO, 6=C6_IO13. For C6 firmware debug. |
| J5 | PR20204VBDN | **Expansion.** Pin 1=GPIO6, pin 5=GPIO27, pin 7=GPIO28, plus USB HS OTG (USB_HS_N/USB_HS_P from module pins 48/49). |
| P1 | 691382010004 | DUT current-return terminal. Carries `OUT_DUT` net + R30 (50mΩ shunt). |

---

## 3. Power Tree

### 3.1 Rails

| Rail | Voltage | Source | Enable |
|---|---|---|---|
| `20V_USB` | ~20V (external) | J2 pin 5 | Always on when J2 connected |
| `5V_BUCK` | ~5V | J1 VBUS → D3 OR J2 pin 6 | Always on |
| `5V3_BUCK` | **5.31V** | LTM8078 (U4) VOUT1 | Always on when 20V_USB present |
| `3V3_ESP` | **3.30V** | LTM8078 (U4) VOUT2 | Always on when 20V_USB present |
| `3V3` | **3.30V** | TPS74601 (U3) | **GPIO42 HIGH** (R20 100kΩ pull-down at boot) |
| `26V_BOOST` | **26.1V** | ADP5071 (U5) boost output | **GPIO54 HIGH** (R41 100kΩ pull-down at boot) |
| `−26V_BOOST` | **−26.6V** | ADP5071 (U5) inverting output | Same as 26V_BOOST (shared enable) |
| `+15V_LDO` | **~15V** | ADP7142 (U6) from 26V_BOOST | **GPIO3 HIGH** - shared with U7/U29/U31 |
| `−15V_LDO` | **~−15V** | ADP7182 (U7) from −26V_BOOST | **GPIO3 HIGH** - shared |
| `+24V_LDO` | **~24V** | ADP7142 (U29) from 26V_BOOST | **GPIO3 HIGH** - shared |
| `−24V_LDO` | **~−24V** | ADP7182 (U31) from −26V_BOOST | **GPIO3 HIGH** - shared |
| `5V_LDO` | ~5V | ADAQ7769-1 U1 internal LDO out | On when 5V3_BUCK + U1 powered |
| `5V_LDO1` | ~5V | ADAQ7769-1 U22 internal LDO out | On when 5V3_BUCK + U22 powered |
| `5V_LDO2` | ~5V | ADAQ7769-1 U23 internal LDO out | On when 5V3_BUCK + U23 powered |
| `REF_OUT` | **4.096V** | ADR4540 (U18) | On when 5V_LDO (U1) powered |
| `V_DUT` | 0–~20V prog. | LTM8056 (U27) | **GPIO4 HIGH** → LTM8056 RUN |

> ⚠ **All 4 HV LDOs (U6, U7, U29, U31) enable and disable together** via GPIO3. There is no independent control - enabling ±26V then GPIO3 brings up all four rails simultaneously.

### 3.2 Rail-to-Consumer Map

| Rail | Consumers |
|---|---|
| `+15V_LDO` | ADAQ1 (U1) VDD_PGA, ADAQ2 (U22) VDD_PGA, ADG6412 U9 VDD, ADG6412 U13 VDD, ADG5204 U24 VDD |
| `−15V_LDO` | ADAQ1 (U1) VSS_PGA, ADAQ2 (U22) VSS_PGA, ADG6412 U9 VSS, ADG6412 U13 VSS, ADG5204 U24 VSS |
| `+24V_LDO` | ADAQ3 (U23) VDD_PGA, ADG5204 U25 VDD |
| `−24V_LDO` | ADAQ3 (U23) VSS_PGA, ADG5204 U25 VSS |
| `5V3_BUCK` | ADAQ1/2/3 IN_LDO, AD7415 U2 VDD, WS2812B array VDD, DS4424 VCC |
| `3V3_ESP` | ESP32-P4 module (VBAT/ESP_3V3), I2C pull-ups R68/R69 |
| `3V3` | TPS74601 output → CDCLVC1104 U20 VDD, ADAQ logic power |
| `5V_LDO` (from U1) | ADR4540 (U18) VIN → REF_OUT |
| `REF_OUT` (4.096V) | All 3 ADAQ7769-1 REF+ pins |
| `5V_BUCK` | AD7415 U28 VDD |

> Note: ADAQ supply pins (VDD_PGA, VSS_PGA, IN_LDO, etc.) are filtered through ferrite beads (e.g., NetFB1_2, NetFB2_2, etc.) in the netlist. Each supply pin sees the ferrite-filtered version of its rail.

### 3.3 Firmware Power Sequencing

All HV rails are **disabled at boot** by hardware pull-down resistors on GPIO enable lines.

```
Boot state (all pull-downs active):
  GPIO54 → R41 (100kΩ to GND) → ADP5071 EN = LOW  → ±26V_BOOST OFF ✅
  GPIO3  → R2  (100kΩ to GND) → U6/U7/U29/U31 EN = LOW → ALL HV LDOs OFF ✅
  GPIO42 → R20 (100kΩ to GND) → TPS74601 EN = LOW → 3V3 OFF ✅
  GPIO4  → R3  (100kΩ to GND) → LTM8056 RUN = LOW → V_DUT OFF ✅

Always-on at boot (LTM8078 hardwired to 20V_USB):
  3V3_ESP (ESP32-P4 power) - always on
  5V3_BUCK (analog IC power) - always on
```

**Recommended firmware enable sequence:**
1. Configure GPIO54, GPIO3, GPIO42, GPIO4 as outputs; drive LOW (redundant safety, matches pull-downs)
2. Enable `3V3` analog rail: **GPIO42 HIGH** → TPS74601 EN. Poll **GPIO41** (TPS74601 PG pull-up) until HIGH before proceeding. PG HIGH gates CDCLVC1104 (U20) 1G - MCLK to all ADAQs only flows once 3V3 PG is confirmed.
3. Enable ±26V: **GPIO54 HIGH** → ADP5071 EN1+EN2 → ±26V_BOOST. Allow ≥1ms for rails to settle.
4. Enable all HV LDOs: **GPIO3 HIGH** → U6+U7+U29+U31 enable simultaneously. Allow ≥1ms.
   - +15V_LDO / −15V_LDO power ADAQ1/2 PGA, ADG6412, ADG5204 U24
   - +24V_LDO / −24V_LDO power ADAQ3 PGA, ADG5204 U25
5. Release ADAQ resets: drive **GPIO2 HIGH** (boot state WPU already holds HIGH, but be explicit). All 3 ADAQs come out of reset simultaneously.
6. Configure mux addresses (GPIO47–51) before enabling mux EN (GPIO49).
7. Enable V_DUT: **GPIO4 HIGH** → LTM8056 RUN active. Program DS4424 voltage/current first.

> ⚠ **GPIO41 (`NetR51_1`):** netlist shows 4 members - TPS74601 PG, CDCLVC1104 1G, GPIO41, R51 pull-up. R51 connects TPS74601 PG (open-drain) to 3V3_ESP. GPIO41 reads this pulled-up level. Poll GPIO41 HIGH in step 2 before proceeding.

> ⚠ **LTM8078 PG1/PG2 are UNMONITORED.** Nets `NetU4_A2` (PG1) and `NetU4_B1` (PG2) are dangling - firmware cannot detect 5V3_BUCK or 3V3_ESP failure in hardware.

### 3.4 DUT Supply Control

The LTM8056 (U27) output voltage and current limit are set by DS4424 (U26) current-output DAC over I2C.

**DS4424 → LTM8056 mapping (from the netlist):**
- `DS4424 OUT0` → `I_FB_DCDC` → LTM8056 CTL pin = **current limit control**
- `DS4424 OUT1` → `V_FB_DCDC` → LTM8056 FB pin = **output voltage set**

**Voltage formula** (LTM8056 datasheet p.7, R56=90.9kΩ, R57=11.3kΩ):
```
V_DUT = 1.2 × (1 + 90.9k/11.3k) − 90.9kΩ × I_DAC_ch1
      ≈ 10.85V − 90.9kΩ × I_DAC_ch1
```
With DS4424 I_FS = 100µA: V_DUT range ≈ 1.76V (DAC max sink) to 19.94V (DAC max source).

**Current limits:**
- Input current limit: 50mV / 20mΩ (R53) = **2.5A**
- Output current limit: 58mV / 22mΩ (R55) = **2.636A** (hardware ceiling; DS4424 OUT0 reduces this)

**DUT current monitoring (ADC inputs):**
- `IINMON` → `NetU27_L3` → **GPIO22 (ADC1_CH6).** Range: 0–1.0V at 0–2.5A input.
- `IOUTMON` → `NetU27_L2` → **GPIO23 (ADC1_CH7).** Range: 0–1.2V at 0–2.636A output.

**Freewheeling diode CR1:** Anode=NetCR1_1 (shunt side, between R27/R30), Cathode=V_DUT. Provides a recirculation path for inductive DUTs. Orientation confirmed from netlist.

---

## 4. ESP32-P4 Module Pin Map (Complete, Verified)

Module: **Waveshare ESP32-P4-Module** (SoC: ESP32-P4NRW32, 16MB flash, 32MB stacked PSRAM).  
88 castellated pins. Net names from live compiled netlist.

> Flash and PSRAM are on-module, wired to dedicated chip pins that are NOT part of GPIO0–54. They are fully independent of GPIO numbering.

| Module Pin | Chip Signal | Net (live) | Firmware Function | Notes |
|---|---|---|---|---|
| 1 | GND | GND | - | |
| 2 | LNA_OUT | NetC85_2 | C6 antenna | RF only |
| 3 | GND | GND | - | |
| 4 | C6_U0RXD | C6_TX | C6 UART RX (§5) | P4 GPIO32 drives this as TX |
| 5 | C6_U0TXD | C6_RX | C6 UART TX (§5) | P4 GPIO33 receives from C6 |
| 6 | C6_IO15 | NetR73_2 | C6 → WS2812B LED data | Via resistor R73 |
| 7 | C6_IO14 | NetR42_2 | C6 → E1 buzzer | Via R42; PRO-OB-440 buzzer |
| 8 | C6_IO13 | NetJ4_6 | C6 JTAG → J4 pin 6 | |
| 9 | C6_IO9/BOOT | C6_BOOT | C6 boot-mode strap | P4 GPIO44 drives |
| 10 | C6_IO8/BOOT_EN | C6_RST | C6 boot-enable strap | P4 GPIO43 drives |
| 11 | C6_IO7/MTDO | NetJ4_5 | C6 JTAG MTDO → J4 pin 5 | |
| 12 | C6_IO6/MTCK | NetJ4_4 | C6 JTAG MTCK → J4 pin 4 | |
| 13 | C6_IO5/MTDI | NetJ4_3 | C6 JTAG MTDI → J4 pin 3 | |
| 14 | C6_IO4/MTMS | NetJ4_1 | C6 JTAG MTMS → J4 pin 1 | |
| 15 | GND | GND | - | |
| 16 | GPIO1 | NetU30_16 | **FREE** (1-pin floating) | LP domain |
| 17 | GPIO2 | NetU1_C9 | All 3 ADAQ *RST (shared) | LP, IE+WPU at reset → HIGH at boot ✅ |
| 18 | GPIO3 | NetR2_2 | **ALL 4 HV LDO EN** (OUTPUT) | R2 100kΩ to GND → LOW at boot; enables U6+U7+U29+U31 together |
| 19 | GPIO4 | NetR3_2 | LTM8056 RUN (OUTPUT) | R3 100kΩ to GND → LOW at boot |
| 20 | GPIO5 | NetU22_E14 | ADAQ2 *DRDY (INPUT) | DRDY for U22 |
| 21 | GPIO6 | NetJ5_1 | J5 expansion pin 1 | LP, TOUCH_CH5 |
| 22 | GPIO7 | NetU22_E13 | ADAQ2 *CS (OUTPUT) | Native SPI2 CS0 pad |
| 23 | GPIO8 | NetU22_E12 | ADAQ2+3 SDI/MOSI (OUTPUT) | Native SPI2 MOSI pad |
| 24 | GPIO9 | NetU22_E11 | ADAQ2+3 SCLK (OUTPUT) | Native SPI2 CLK pad |
| 25 | GPIO10 | NetU22_E10 | ADAQ2+3 DOUT/MISO (INPUT) | Native SPI2 MISO; shared, safe per ADAQ t12 |
| 26 | GPIO11 | NetU1_E14 | ADAQ1 *DRDY (INPUT) | Separate DRDY for U1 |
| 27 | GPIO12 | NetU1_E13 | ADAQ1 *CS (OUTPUT) | |
| 28 | GPIO13 | NetU1_E12 | ADAQ1 SDI/MOSI (OUTPUT) | |
| 29 | GPIO20 | NetU1_E11 | ADAQ1 SCLK (OUTPUT) | ADC1_CH4 - not usable as analog |
| 30 | GPIO21 | NetU1_E10 | ADAQ1 DOUT/MISO (INPUT) | ADC1_CH5 - not usable as analog |
| 31 | GPIO22 | NetU27_L3 | **IINMON ADC1_CH6 (INPUT)** | LTM8056 input current mon, 0–1V |
| 32 | GPIO23 | NetU27_L2 | **IOUTMON ADC1_CH7 (INPUT)** | LTM8056 output current mon, 0–1.2V |
| 33 | GND | GND | - | |
| 34–39 | DSI_DATAx/CLK | NetU30_34–39 | Dedicated MIPI-DSI | Not usable as GPIO |
| 40 | GND | GND | - | |
| 41–46 | CSI_DATAx/CLK | NetU30_41–46 | Dedicated MIPI-CSI | Not usable as GPIO |
| 47 | GND | GND | - | |
| 48 | USB_DM | USB_HS_N | USB 2.0 HS OTG D− | Fixed-function → J5 |
| 49 | USB_DP | USB_HS_P | USB 2.0 HS OTG D+ | Fixed-function → J5 |
| 50 | GPIO24/USB1P1_N0 | USB_FS_N | USB-Serial-JTAG D− → J1 | Boot: blank/blank |
| 51 | GPIO25/USB1P1_P0 | USB_FS_P | USB-Serial-JTAG D+ → J1 | Boot: USB pull-up enabled (HIGH) |
| 52 | GND | GND | - | |
| 53 | GPIO26 | OK_KEY | **Button: OK/Enter (INPUT)** | VDD_IO_4; configure INPUT_PULLUP |
| 54 | GPIO27 | NetJ5_5 | J5 expansion pin 5 | |
| 55 | GPIO28 | NetJ5_7 | J5 expansion pin 7 | |
| 56 | GPIO29 | NetU23_E14 | ADAQ3 *DRDY (INPUT) | Separate DRDY for U23 |
| 57 | GPIO30 | NetU23_E13 | ADAQ3 *CS (OUTPUT) | |
| 58 | GPIO31 | NetU30_58 | **FREE** (1-pin floating) | VDD_IO_4 |
| 59 | GPIO32 | C6_TX | C6 UART TX from P4 (OUTPUT) | P4 drives → C6 U0RXD |
| 60 | GPIO33 | C6_RX | C6 UART RX to P4 (INPUT) | P4 receives ← C6 U0TXD |
| 61 | GPIO34 | NetU30_61 | **FREE** (1-pin floating) | Strap: JTAG src select, no pull |
| 62 | GPIO35 | P4_BOOT | Boot/download button (INPUT) | WPU → HIGH = SPI boot; LOW = download |
| 63 | GPIO36 | NetDNP_1 | DNP 100kΩ pull-down ⚠ | Strap pin - see §4.1 |
| 64 | GPIO37 | NetTX_1 | UART0 TX/console (OUTPUT) | Strap + console TX |
| 65 | GPIO38 | NetRX_1 | UART0 RX/console (INPUT) | Strap + console RX |
| 66 | ESP_LDO_VO4 | NetU30_66 | Internal 1.8V LDO out | Do not load. 1-pin net |
| 67 | GPIO39 | NetR68_1 | I2C SDA (I/O) | R68 pull-up to 3V3_ESP |
| 68 | GPIO40 | NetR69_1 | I2C SCL (OUTPUT) | R69 pull-up to 3V3_ESP |
| 69 | GPIO41 | NetR51_1 | **TPS74601 PG monitor (INPUT)** | R51 pull-up; HIGH = 3V3 good + MCLK enabled |
| 70 | GPIO42 | NetR20_1 | 3V3 LDO EN (OUTPUT) | R20 100kΩ to GND → LOW at boot |
| 71 | GPIO43 | C6_RST | C6 BOOT_EN strap (OUTPUT) | Puts C6 in download mode (see §5) |
| 72 | GPIO44 | C6_BOOT | C6 BOOT strap (OUTPUT) | Puts C6 in download mode (see §5) |
| 73 | GPIO45 | DN_KEY | **Button: Down/decrease (INPUT)** | VDD_IO_5; configure INPUT_PULLUP |
| 74 | GPIO46 | UP_KEY | **Button: Up/increase (INPUT)** | VDD_IO_5; configure INPUT_PULLUP |
| 75 | GPIO47 | NetU25_14 | ADG5204 U25 A1 (OUTPUT) | Mux U25 address bit 1 |
| 76 | GPIO48 | NetU25_15 | ADG5204 U25 A0 (OUTPUT) | Mux U25 address bit 0 |
| 77 | GPIO49 | NetU24_16 | ADG5204 U24+U25 EN (OUTPUT) | Shared enable for both muxes |
| 78 | GPIO50 | NetU24_15 | ADG5204 U24 A0 (OUTPUT) | Mux U24 address bit 0 |
| 79 | GPIO51 | NetU24_14 | ADG5204 U24 A1 (OUTPUT) | Mux U24 address bit 1 |
| 80 | GPIO52 | NetR25_2 | ADG6412 U9 bypass (OUTPUT) | Via resistor; HIGH = bypass closed |
| 81 | GPIO53 | NetR28_2 | ADG6412 U13 bypass (OUTPUT) | Via resistor; HIGH = R27 bypassed |
| 82 | GPIO54 | NetR41_1 | ADP5071 EN (OUTPUT) | R41 100kΩ to GND → LOW at boot |
| 83 | GND | GND | - | |
| 84 | VBAT | 3V3_ESP | Module power | |
| 85 | ESP_3V3 | 3V3_ESP | Module power | |
| 86 | ESP_3V3 | 3V3_ESP | Module power | |
| 87 | ESP_EN | NetC91_2 | CHIP_PU → RC filter → 3V3_ESP | RST button hardware path |
| 88 | GPIO0 | NetU30_88 | **FREE** (1-pin floating) | XTAL_32K_N alt |

### 4.1 Strapping Pin Summary

| GPIO | Pin | Boot strap function | Board state | Risk |
|---|---|---|---|---|
| GPIO35 | 62 | Boot mode (LOW=download, HIGH=SPI boot) | P4_BOOT - OK button pulls LOW for download mode; internal WPU → HIGH = normal boot | Safe by design |
| GPIO36 | 63 | Boot mode + UART0 ROM print ctrl | DNP 100kΩ pull-down. If installed, boot may be affected. | ⚠ Clarify assembly intent |
| GPIO37 | 64 | Boot mode + UART0 TXD default | NetTX_1 (console TX) - floats as strap, then console TX | Safe |
| GPIO38 | 65 | Boot mode + UART0 RXD default | NetRX_1 (console RX) - same | Safe |
| GPIO34 | 61 | JTAG source select | 1-pin net (floating) - no internal pull | Note: if JTAG select matters, add external pull |

### 4.2 IO Power Domains

| Domain | GPIOs | Key implication |
|---|---|---|
| VDD_LP (LP + HP) | 0–15 | GPIO0–15; LP-UART default on GPIO14/15 |
| VDD_IO_0 | 16–23 | GPIO20–23 only (16–19 not broken out on module). ADC1_CH4–CH7. |
| VDD_IO_4 | 24–38 | USB-FS, UART0 default, SPI2 8-line path |
| VDD_IO_5 | 39–48 | I2C, C6 control, buttons, mux address |
| VDD_IO_6 | 49–54 | ADC2_CH0–CH5 - ALL consumed by digital outputs |

**ADC availability:**
- ADC1_CH6 (GPIO22 → IINMON) and CH7 (GPIO23 → IOUTMON) - **in use**
- ADC1_CH4/CH5 (GPIO20/21): consumed by ADAQ1 SPI
- ADC2 all channels (GPIO49–54): consumed by digital outputs
- **No free ADC channels on this board.**
- GPIO16–19 are NOT broken out on this Waveshare module - do not reference them.

### 4.3 Default-JTAG GPIO Caution

GPIO2/3/4/5 are the ESP32-P4's default JTAG pad signals (MTCK/MTDI/MTMS/MTDO). They are used here for ADAQ RST, HV LDO EN, LTM8056 RUN, and ADAQ2 DRDY. **Use USB-Serial-JTAG (via J1, GPIO24/25)** for debugging - do not connect a JTAG probe to the JTAG pads without first disabling the pad-JTAG interface in firmware.

---

## 5. ESP32-C6 Internal Wiring Map

### 5.1 Module-Internal Wiring (Fixed)

| Item | Detail |
|---|---|
| C6 power | Shares `ESP_3V3`/`VBAT` with P4. No independent power-down. |
| C6 CHIP_PU | Permanently HIGH via R76 (10kΩ inside module). C6 cannot be hardware-reset independently. |
| C6 crystal | Y3, 40 MHz, independent of P4 crystal. |
| C6 flash | C6FH8 has 8MB embedded flash (in-package). |

### 5.2 Carrier-Board Wiring

| Module Pin | C6 Signal | Net | P4 Signal | Firmware use |
|---|---|---|---|---|
| 4 | C6_U0RXD | C6_TX | GPIO32 | P4 **TX** → C6 RX |
| 5 | C6_U0TXD | C6_RX | GPIO33 | P4 **RX** ← C6 TX |
| 6 | C6_IO15 | NetR73_2 | - | C6 drives WS2812B LED chain via R73 |
| 7 | C6_IO14 | NetR42_2 | - | C6 drives E1 buzzer (PRO-OB-440) via R42 |
| 8 | C6_IO13 | NetJ4_6 | - | C6 JTAG aux → J4 |
| 9 | C6_IO9/BOOT | C6_BOOT | GPIO44 | P4 GPIO44 → C6 boot-mode strap |
| 10 | C6_IO8/BOOT_EN | C6_RST | GPIO43 | P4 GPIO43 → C6 boot-enable strap |
| 11 | C6_IO7/MTDO | NetJ4_5 | - | C6 JTAG MTDO → J4 |
| 12 | C6_IO6/MTCK | NetJ4_4 | - | C6 JTAG MTCK → J4 |
| 13 | C6_IO5/MTDI | NetJ4_3 | - | C6 JTAG MTDI → J4 |
| 14 | C6_IO4/MTMS | NetJ4_1 | - | C6 JTAG MTMS → J4 |

> **C6 reset/boot caveat:** C6 CHIP_PU is permanently HIGH inside the module - P4 cannot hardware-reset C6. GPIO43/44 put C6 into UART download mode at next boot. To reflash C6 via P4: drive GPIO43 LOW + GPIO44 LOW, then software-reset C6 (C6 RTC_CNTL register). C6 only enters download mode on next reboot.

**P4 UART configuration for C6:**
- GPIO32 = UART TX (OUTPUT) - use UART1 or UART2 via GPIO matrix; UART0 is the console on GPIO37/38
- GPIO33 = UART RX (INPUT)

---

## 6. SPI Bus Map

### 6.1 Architecture

| Bus | SPI Controller | Channels | Routing |
|---|---|---|---|
| Bus B | GP-SPI2 | ADAQ2 (U22) + ADAQ3 (U23) shared | Native IOMUX on GPIO7–10; U23 CS via GPIO matrix |
| Bus A | GP-SPI3 | ADAQ1 (U1) dedicated | All via GPIO matrix |

> GP-SPI2 and GP-SPI3 both support DMA (TRM Table 43.10-1). Do not use LP-SPI - no DMA.  
> ADAQ max SPI clock: **20 MHz** (tSCLK ≥ 50ns per ADAQ7769-1 datasheet).

### 6.2 SPI Pin Assignments

**Bus B - GP-SPI2 (ADAQ2 + ADAQ3):**

| Signal | GPIO | Module Pin | ESP-IDF role | Native IOMUX? |
|---|---|---|---|---|
| SCLK | GPIO9 | 24 | sclk_io_num | Yes (SPI2_CK) |
| MOSI (SDI) | GPIO8 | 23 | mosi_io_num | Yes (SPI2_D) |
| MISO (DOUT) | GPIO10 | 25 | miso_io_num | Yes (SPI2_Q) |
| CS - ADAQ2 (U22) | GPIO7 | 22 | cs_io_num | Yes (SPI2_CS0) |
| CS - ADAQ3 (U23) | GPIO30 | 57 | cs_io_num | Via GPIO matrix |
| DRDY - ADAQ2 | GPIO5 | 20 | interrupt GPIO | - |
| DRDY - ADAQ3 | GPIO29 | 56 | interrupt GPIO | - |

**Bus A - GP-SPI3 (ADAQ1):**

| Signal | GPIO | Module Pin | ESP-IDF role | Native IOMUX? |
|---|---|---|---|---|
| SCLK | GPIO20 | 29 | sclk_io_num | No (matrix) |
| MOSI (SDI) | GPIO13 | 28 | mosi_io_num | No (matrix) |
| MISO (DOUT) | GPIO21 | 30 | miso_io_num | No (matrix) |
| CS - ADAQ1 (U1) | GPIO12 | 27 | cs_io_num | No (matrix) |
| DRDY - ADAQ1 | GPIO11 | 26 | interrupt GPIO | - |

### 6.3 ADAQ Bus Sharing Safety

U22 and U23 share SCLK/MOSI/MISO. Safe because per ADAQ7769-1 timing spec t12: CS rising → DOUT high-Z ≤ 7ns. Use **4-wire SPI mode** (CS toggles per transaction). Never tie CS permanently LOW on a shared bus.

### 6.4 ADAQ Reset and Sync

- **All 3 ADAQ *RST** share `NetU1_C9` → GPIO2. Single shared reset. GPIO2 WPU holds *RST HIGH (out of reset) at boot. Resetting one resets all three.
- **SYNC:** U1 is master. `DAQ_SYNC` carries U1 *SYNC_OUT → U22/U23 *SYNC_IN. U22/U23 *SYNC_OUT are floating (correct for this topology).
- **MCLK:** SiT8208 Y1 (16.384 MHz) → CDCLVC1104 U20 → CLOCK_0→U1, CLOCK_1→U22, CLOCK_2→U23. U20 1G enable is driven by TPS74601 PG (via GPIO41 net) - MCLK is hardware-gated until 3V3 is good. CLOCK_3 is unloaded (1-pin net).

### 6.5 SPI Configuration (ESP-IDF)

```c
// Bus B (GP-SPI2) - ADAQ2 + ADAQ3
spi_bus_config_t bus_b = {
    .mosi_io_num   = 8,
    .miso_io_num   = 10,
    .sclk_io_num   = 9,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
};
spi_device_interface_config_t adaq2_cfg = {
    .clock_speed_hz = 20 * 1000 * 1000,  // 20 MHz max
    .mode           = 0,                   // verify CPOL/CPHA from adaq7769-1.pdf timing diagrams
    .spics_io_num   = 7,
    .queue_size     = 1,
};
spi_device_interface_config_t adaq3_cfg = { .spics_io_num = 30, /* rest same */ };

// Bus A (GP-SPI3) - ADAQ1
spi_bus_config_t bus_a = {
    .mosi_io_num = 13, .miso_io_num = 21, .sclk_io_num = 20,
};
spi_device_interface_config_t adaq1_cfg = { .spics_io_num = 12 };
```

---

## 7. I2C Bus

**SDA:** GPIO39 (R68, 10kΩ pull-up to 3V3_ESP)  
**SCL:** GPIO40 (R69, 10kΩ pull-up to 3V3_ESP)

```c
i2c_config_t i2c_cfg = {
    .mode             = I2C_MODE_MASTER,
    .sda_io_num       = 39,
    .scl_io_num       = 40,
    .sda_pullup_en    = GPIO_PULLUP_DISABLE,  // external R68
    .scl_pullup_en    = GPIO_PULLUP_DISABLE,  // external R69
    .master.clk_speed = 400000,               // 400 kHz - verify against each device
};
```

### 7.1 Device Address Map

| Designator | Part | Address pin wiring | 7-bit Address | Source |
|---|---|---|---|---|
| U26 | DS4424 | A0=GND, A1=GND | **0x10** | DS4422-DS4424.pdf Table 1 - datasheet shows 0x20h as 8-bit write byte; true 7-bit = 0x10 |
| U2 | AD7415 | AS=GND, VDD=5V3_BUCK | **0x49** | ad7414_7415.pdf Table 4: AS=GND → 0x49 |
| U28 | AD7415 | AS=VDD, VDD=5V_BUCK | **0x4A** | Same table: AS=VDD → 0x4A |

No address collisions. U2 runs on 5V3_BUCK; U28 runs on 5V_BUCK - verify both rails are up before polling.

> The I²C bus has exactly three devices: DS4424 (U26), AD7415 (U2), AD7415 (U28).
> There is no I²C GPIO expander on this board.

### 7.2 DS4424 DAC Register Usage

| Channel | Register | Wiring | Firmware use |
|---|---|---|---|
| OUT0 | 0xF8 | `I_FB_DCDC` → LTM8056 CTL | **DUT current limit** (OUT0 sinks to reduce limit below 2.636A) |
| OUT1 | 0xF9 | `V_FB_DCDC` → LTM8056 FB | **DUT output voltage** |
| OUT2/3 | - | Floating | Not used |

Register format: bit 7 = direction (1=source/push, 0=sink/pull), bits 6:0 = magnitude. See DS4422-DS4424.pdf §Register Map.

---

## 8. Analog Front-End

### 8.1 ADAQ Channel Signal Routing

| ADAQ | Designator | D1 Input source | PGA supply |
|---|---|---|---|
| Channel 1 | U1 | **U24 D output** (via ADG5204 mux) | ±15V_LDO (U6/U7) |
| Channel 2 | U22 | **CSA_005 direct** (50mΩ sense output, no mux) | ±15V_LDO (U6/U7) |
| Channel 3 | U23 | **U25 D output** (via ADG5204 mux) | ±24V_LDO (U29/U31) |

U23 (ADAQ3) has a **differential IN3**: IN3_AAF+ and IN3_AAF− = OUT_DUT_SNS− (voltage-sense return). This is a bipolar voltage measurement across the DUT sense network.

All ADAQ REF+ pins share `REF_OUT` (4.096V from ADR4540 U18, powered from U1's internal LDO output 5V_LDO).

### 8.2 ADG5204 U24 (±15V_LDO supply, routes to ADAQ1)

VDD = +15V_LDO (U6), VSS = −15V_LDO (U7).

| GPIO | Function |
|---|---|
| GPIO49 | U24 EN + U25 EN (**shared** - enables both muxes simultaneously) |
| GPIO50 | U24 A0 |
| GPIO51 | U24 A1 |

Channel map (A1:A0 → Sx → signal routed to U1 IN):

| A1 | A0 | Channel | Signal |
|---|---|---|---|
| 0 | 0 | S1 | (verify in schematic) |
| 0 | 1 | S2 | (verify in schematic) |
| 1 | 0 | S3 | EXT_A0 (floating/external) |
| 1 | 1 | S4 | NetU24_9 (unconnected) |

> ⚠ GPIO49 net (`NetU24_16`): no pull-down confirmed. Check ADG5204 datasheet for internal pull. If EN can float HIGH at boot, the mux may enable with indeterminate address. Add 100kΩ external pull-down if needed.

### 8.3 ADG5204 U25 (+24V_LDO supply, routes to ADAQ3)

VDD = +24V_LDO (U29), VSS = −24V_LDO (U31).

| GPIO | Function |
|---|---|
| GPIO49 | U25 EN (same net as U24 EN) |
| GPIO47 | U25 A1 |
| GPIO48 | U25 A0 |

J3 pin 2 connects to U25 S1 - verify what signal J3 is bringing in for the ADAQ3 channel.

### 8.4 ADG6412 Bypass Switches

| GPIO | Designator | Supply | Function |
|---|---|---|---|
| GPIO52 → NetR25_2 | U9 | ±15V_LDO | Closes bypass around 51Ω shunt (R24). HIGH = bypass. |
| GPIO53 → NetR28_2 | U13 | ±15V_LDO | Closes bypass around 2Ω shunt (R27). HIGH = R27 bypassed. |

---

## 9. Overcurrent Protection (Hardware, Independent of Firmware)

| Range | Shunt | CSA | SET comparator | RESET comparator | Trip current | Reset current |
|---|---|---|---|---|---|---|
| 51Ω | R24 (51Ω ±5%) | U10 (AD8411A) | U8 (ADCMP600) | U11 (ADCMP600) | **1.434 mA** | **30.1 µA** |
| 2Ω | R27 (2Ω ±1%) | U14 (AD8411A) | U15 (ADCMP600) | U17 (ADCMP600) | **36.56 mA** | **768 µA** |

SR latch (U12) fires (Q HIGH) when CSA output > 3.906V; holds until CSA < 325mV. Bypass switches (U9/U13) route current around the shunt when latched.

**50mΩ range (R30, U16/ADAQ2-U22): NO hardware latch protection.** ADAQ saturates at 1.538A, AD8411A clips at ~2.01A, LTM8056 limits at 2.636A. Firmware must poll ADAQ2 for output code 0xFFFFFF (saturation) and treat it as an over-range fault.

---

## 10. User Interface

### 10.1 Buttons

| GPIO | Net | Button | Boot state | Note |
|---|---|---|---|---|
| GPIO26 | OK_KEY | OK / Enter | Inert/blank | Configure INPUT_PULLUP; active-low |
| GPIO45 | DN_KEY | Down / decrease | Inert/blank | Configure INPUT_PULLUP; active-low |
| GPIO46 | UP_KEY | Up / increase | Inert/blank | Configure INPUT_PULLUP; active-low |
| GPIO35 | P4_BOOT | BOOT / download mode | WPU → normal boot | Pull LOW to enter P4 download mode |
| ESP_EN | NetC91_2 | RST / hardware reset | Not a GPIO | RC + capacitor path; hardware resets the P4 |

### 10.2 Status LEDs and Buzzer

- **WS2812B ×8:** driven by **C6_IO15** (module pin 6) via R73. Controlled by ESP32-C6 firmware. P4 cannot directly drive these.
- **E1 (PRO-OB-440 buzzer):** driven by **C6_IO14** (module pin 7) via R42. Controlled by C6. Exact drive requirements (passive/active buzzer, duty cycle) - verify from PRO-OB-440 datasheet.

---

## 11. Open Issues for Firmware

| # | Severity | Description | Required action |
|---|---|---|---|
| **P-IC1** | 🔴 **BLOCKING** | IC1 (SOT-23-3, 3-pin): completely unidentified. Altium note reads "Taken From Chinese PCB". Net connections unknown. Cannot write firmware that touches this net without knowing what it is. | Identify IC1 from schematic, physical board, or original source PCB. |
| P-E1-GPIO | ⚠ Verify | E1 buzzer connected via C6_IO14 → R42. Buzzer type (active vs. passive), operating voltage, and drive requirement not verified from datasheet. | Check PRO-OB-440 datasheet. Set C6_IO14 drive level and PWM if passive. |
| P-GPIO41 | ⚠ Verify | GPIO41 `NetR51_1` is confirmed 4-pin net: TPS74601 PG + CDCLVC1104 1G + GPIO41 + R51. R51 provides pull-up to 3V3_ESP. Poll GPIO41 HIGH before enabling 3V3-dependent peripherals. | Verify schematic node matches; implement polling in sequencing code. |
| P-LTM8078-PG | 🟡 Not monitored | PG1 (`NetU4_A2`) and PG2 (`NetU4_B1`) dangling - no firmware visibility of 5V3_BUCK or 3V3_ESP failure. | Accept limitation, or add resistors + connect to GPIO31/GPIO34 (both currently free). |
| P-DNP-R | 🟡 Clarify | GPIO36 (strapping pin) → `NetDNP_1` - 100kΩ pull-down designated DNP. If assembled, boot strap may be affected. | Confirm with assembler: is this resistor fitted? |
| P-ADG5204-EN | 🟡 Verify | GPIO49 → U24+U25 EN: no pull-down on `NetU24_16` confirmed. Mux may enable with random address at boot. | Check ADG5204 datasheet for internal EN pull. Add 100kΩ external pull-down if no internal pull. |
| P-J3-S1 | 🟡 Verify | J3 pin 2 → U25 S1. What signal is brought in here? If J3 is used as an analog input to ADAQ3 via U25, this is a real input channel that needs documentation. | Trace J3 external connector wiring. |
| P-J2-15V | 🟡 Verify | J2 pins 1/3 (old ±15V_ANA): in new design U6/U7 generate ±15V internally. Verify whether J2 pins 1/3 are now NC, tied to +15V_LDO/−15V_LDO, or removed. | Check netlist for J2:1 and J2:3 net membership. |
| P-OC-50m | 🟡 Documented | 50mΩ range (U22) has no hardware OC between ADAQ saturation (1.538A) and LTM8056 trip (2.636A). | Firmware: treat ADAQ2 code 0xFFFFFF as over-range fault. |
| P-5pct | 🟢 Low | R24 (51Ω shunt) is ±5% - dominates error on 51Ω range. | Accept ±5% or implement per-board calibration. |

---

## 12. Quick Reference - All GPIO Assignments

| GPIO | Module Pin | Direction | Function | Domain |
|---|---|---|---|---|
| 0 | 88 | - | **FREE** | LP |
| 1 | 16 | - | **FREE** | LP |
| 2 | 17 | OUTPUT (WPU) | All 3 ADAQ *RST (active-low; HIGH=run) | LP |
| 3 | 18 | OUTPUT | **ALL 4 HV LDO EN** - U6+U7+U29+U31 (HIGH=±15V+±24V on) | LP |
| 4 | 19 | OUTPUT | LTM8056 RUN (HIGH=V_DUT on) | LP |
| 5 | 20 | INPUT | ADAQ2 *DRDY | LP |
| 6 | 21 | I/O | J5 expansion pin 1 | LP |
| 7 | 22 | OUTPUT | ADAQ2 *CS (SPI2 native CS0) | LP |
| 8 | 23 | OUTPUT | ADAQ2+3 MOSI (SPI2 native) | LP |
| 9 | 24 | OUTPUT | ADAQ2+3 SCLK (SPI2 native) | LP |
| 10 | 25 | INPUT | ADAQ2+3 MISO (SPI2 native, shared bus) | LP |
| 11 | 26 | INPUT | ADAQ1 *DRDY | LP |
| 12 | 27 | OUTPUT | ADAQ1 *CS | LP |
| 13 | 28 | OUTPUT | ADAQ1 MOSI | LP |
| 14–19 | - | - | **NOT ON THIS MODULE** | - |
| 20 | 29 | OUTPUT | ADAQ1 SCLK (ADC1_CH4, not usable as ADC) | VDD_IO_0 |
| 21 | 30 | INPUT | ADAQ1 MISO (ADC1_CH5, not usable as ADC) | VDD_IO_0 |
| 22 | 31 | INPUT (ADC) | **IINMON** (ADC1_CH6, 0–1V) | VDD_IO_0 |
| 23 | 32 | INPUT (ADC) | **IOUTMON** (ADC1_CH7, 0–1.2V) | VDD_IO_0 |
| 24 | 50 | I/O | USB-Serial-JTAG D− → J1 (console/flash) | VDD_IO_4 |
| 25 | 51 | I/O | USB-Serial-JTAG D+ → J1 | VDD_IO_4 |
| 26 | 53 | INPUT | **OK_KEY button** (active-low, pullup) | VDD_IO_4 |
| 27 | 54 | I/O | J5 expansion pin 5 | VDD_IO_4 |
| 28 | 55 | I/O | J5 expansion pin 7 | VDD_IO_4 |
| 29 | 56 | INPUT | ADAQ3 *DRDY | VDD_IO_4 |
| 30 | 57 | OUTPUT | ADAQ3 *CS | VDD_IO_4 |
| 31 | 58 | - | **FREE** | VDD_IO_4 |
| 32 | 59 | OUTPUT | C6 UART TX (P4→C6) | VDD_IO_4 |
| 33 | 60 | INPUT | C6 UART RX (C6→P4) | VDD_IO_4 |
| 34 | 61 | - | **FREE** (strap: no pull - check if matters) | VDD_IO_4 |
| 35 | 62 | INPUT | P4_BOOT / download button (WPU = normal) | VDD_IO_4 |
| 36 | 63 | - | DNP 100kΩ pull-down ⚠ clarify assembly | VDD_IO_4 |
| 37 | 64 | OUTPUT | UART0 TX (console) | VDD_IO_4 |
| 38 | 65 | INPUT | UART0 RX (console) | VDD_IO_4 |
| 39 | 67 | I/O | I2C SDA | VDD_IO_5 |
| 40 | 68 | OUTPUT | I2C SCL | VDD_IO_5 |
| 41 | 69 | INPUT | TPS74601 PG - **poll HIGH before enabling 3V3 peripherals** | VDD_IO_5 |
| 42 | 70 | OUTPUT | 3V3 LDO EN (HIGH=3V3 on) | VDD_IO_5 |
| 43 | 71 | OUTPUT | C6 BOOT_EN strap | VDD_IO_5 |
| 44 | 72 | OUTPUT | C6 BOOT strap | VDD_IO_5 |
| 45 | 73 | INPUT | **DN_KEY button** (active-low, pullup) | VDD_IO_5 |
| 46 | 74 | INPUT | **UP_KEY button** (active-low, pullup) | VDD_IO_5 |
| 47 | 75 | OUTPUT | U25 A1 (mux address) | VDD_IO_5 |
| 48 | 76 | OUTPUT | U25 A0 (mux address) | VDD_IO_5 |
| 49 | 77 | OUTPUT | U24+U25 EN (shared mux enable) | VDD_IO_6 |
| 50 | 78 | OUTPUT | U24 A0 (mux address) | VDD_IO_6 |
| 51 | 79 | OUTPUT | U24 A1 (mux address) | VDD_IO_6 |
| 52 | 80 | OUTPUT | ADG6412 U9 bypass (HIGH=51Ω bypassed) | VDD_IO_6 |
| 53 | 81 | OUTPUT | ADG6412 U13 bypass (HIGH=2Ω bypassed) | VDD_IO_6 |
| 54 | 82 | OUTPUT | ADP5071 EN (HIGH=±26V on) | VDD_IO_6 |

---

## 13. Full BOM Reference (248 components, 2026-06-23 netlist)

Key designators and their roles:

| Designator | Value / Part | Package | Function |
|---|---|---|---|
| U1, U22, U23 | ADAQ7769-1 | µModule (LGA) | 24-bit DAQ channel 1/2/3 |
| U2, U28 | AD7415 | SOT-23-5 | I2C temperature sensor |
| U3 | TPS74601 | SOT-23-5 | 3.30V analog LDO, GPIO42-gated |
| U4 | LTM8078 | µModule (LGA) | Dual buck: 5V3_BUCK + 3V3_ESP |
| U5 | ADP5071 | LFCSP-20 | ±26V bipolar switcher |
| U6 | ADP7142ACPZN-R7 | LFCSP-8 | +15V_LDO (from +26V_BOOST) |
| U7 | ADP7182ACPZN-R7 | LFCSP-8 | −15V_LDO (from −26V_BOOST) |
| U8, U11, U15, U17 | ADCMP600 | SOT-23-5 | OC SET/RESET comparators |
| U9, U13 | ADG6412 | LFCSP-16 | Quad SPST ±15V bypass switches |
| U10, U14, U16 | AD8411A | SOIC-8 | High-side current-sense amp (50V/V) |
| U12 | SN74HCS02 | SOIC-14 | Quad NOR → 2× SR latch |
| U18 | ADR4540 | SOT-23-5 | 4.096V precision VREF |
| U19 | ADA4051-1 | SOT-23-5 | Rail-to-rail op-amp, 250mV ref buffer |
| U20 | CDCLVC1104 | TSSOP-8 | 1:4 MCLK buffer, gated by 3V3 PG |
| U21 | LT6656-5 | SOT-23-5 | ~5V reference for comparator thresholds |
| U24, U25 | ADG5204 | TSSOP-16 | ±HV analog 4:1 mux |
| U26 | DS4424 | SOT-23-8 | I2C quad current DAC (V_DUT + I_limit) |
| U27 | LTM8056 | µModule (LGA) | Buck-boost DUT supply, 0–20V |
| U29 | ADP7142ACPZN-R7 | LFCSP-8 | +24V_LDO (from +26V_BOOST) |
| U30 | Waveshare ESP32-P4-Module | - | ESP32-P4 + ESP32-C6 dual-chip module |
| U31 | ADP7182ACPZN-R7 | LFCSP-8 | −24V_LDO (from −26V_BOOST) |
| **IC1** | **UNKNOWN** | **SOT-23-3** | ⛔ **Unidentified - must resolve before use** |
| Y1 | SiT8208AI | 2.0×2.5mm | 16.384 MHz MCLK oscillator |
| D1 | NRVBA340T3G | SMA | Reverse-protect Schottky diode (J1 VBUS → 5V_BUCK) |
| D2, D3 | (TVS/protection) | - | Transient protection |
| CR1 | (Schottky diode) | - | Freewheeling diode across DUT path; Anode=NetCR1_1, Cathode=V_DUT |
| E1 | PRO-OB-440 | - | Buzzer, driven by C6_IO14 via R42 |
| J1 | USB4085-GF-A | USB-C | Program/debug + 5V_BUCK input |
| J2 | PR20204VBDN | 8-pos wire-to-board | External power (20V_USB required) |
| J3 | PR20204VBDN | 8-pos | Partial - pin 2 → U25 S1 |
| J4 | (TBD) | Header | C6 JTAG |
| J5 | PR20204VBDN | 8-pos | Expansion + USB HS OTG |
| P1 | 691382010004 | Terminal | DUT current return (OUT_DUT, R30 50mΩ) |
| R24 | 51Ω ±5% | 0402 | Current-sense shunt (51Ω range, U10 input) |
| R27 | 2Ω ±1% | 0402 | Current-sense shunt (2Ω range, U14 input) |
| R30 | 50mΩ | 4-terminal | Current-sense shunt (50mΩ range, U16 input) |
| R53 | 20mΩ | 4-terminal | LTM8056 input current shunt (IINMON) |
| R55 | 22mΩ | 4-terminal | LTM8056 output current shunt (IOUTMON) |
| R56, R57 | 90.9kΩ, 11.3kΩ | 0402 | LTM8056 V_DUT feedback divider |
| R60 | (RT resistor) | 0402 | LTM8056 switching frequency set |
| R61, R64 | 5.1kΩ | 0402 | J1 USB-C CC1/CC2 pull-downs (device mode) |
| R68, R69 | 10kΩ | 0402 | I2C SDA/SCL pull-ups to 3V3_ESP |
| R73 | (series) | 0402 | WS2812B data line resistor (C6_IO15) |
| R42 | 1kΩ | 0402 | E1 buzzer series resistor (C6_IO14) |
