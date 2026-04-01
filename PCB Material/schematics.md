# BugBuster — Schematics Reference

On-board connections documented from the Altium schematic sheets (`ESP32.SchDoc`, `USB.SchDoc`, `Power.SchDoc`, `Analog.SchDoc`, `IOs.SchDoc`) and cross-referenced with `config.h` and the BOM.

> **Notes on confidence:**
> - Information confirmed in **both** the schematic images and firmware source is marked as verified.
> - Connections read from schematic images but not in firmware source are marked **[schematic only]**.
> - Items that are unclear or partially visible are marked **[UNSURE]**.
> - `BREADBOARD_MODE 1` is currently set in `config.h`. This changes two GPIO assignments (I2C, MUX_CS). Both the breadboard and PCB assignments are documented.

---

## Table of Contents

1. [Net / Power Rail Glossary](#1-net--power-rail-glossary)
2. [Sheet 1 — ESP32](#2-sheet-1--esp32)
3. [Sheet 2 — USB and Hub](#3-sheet-2--usb-and-hub)
4. [Sheet 3 — Power and Adjustments](#4-sheet-3--power-and-adjustments)
5. [Sheet 4 — ADC Section (AD74416H)](#5-sheet-4--adc-section-ad74416h)
6. [Sheet 5 — IOs, MUX, and Connectors](#6-sheet-5--ios-mux-and-connectors)
7. [Cross-Sheet Net Summary](#7-cross-sheet-net-summary)
8. [I2C Bus Summary](#8-i2c-bus-summary)
9. [SPI Bus Summary](#9-spi-bus-summary)

---

## 1. Net / Power Rail Glossary

| Net | Source | Description |
|-----|--------|-------------|
| `20V_USB` | HUSB238 / Q1 output | Raw negotiated USB-PD voltage (default 20 V). Powers LTM8078, LTM8063×2. |
| `5V_BUCK` | LTM8078 VOUT1 | 5 V regulated rail. Powers USB hub, LTM8049, ADR4525, level shifters. |
| `3V3_BUCK` | LTM8078 VOUT2 | 3.3 V regulated rail. Powers ESP32, PCA9535, USB hub VDD, pull-ups. |
| `+15V_ANA` | LTM8049 VOUT1P | +15 V analog supply for AD74416H AVCC1/AVDD_HI and SI7113 source bias. |
| `-15V_ANA` | LTM8049 VOUT2N | −15 V analog supply for AD74416H LVIN. |
| `3V3_ADJ` | TPS74601 OUT | Adjustable ~3.3 V for AD74416H AVDD_LO / DVCC81 [UNSURE exact rail usage]. |
| `VADJ1_BUCK` | LTM8063 U4 VOUT | Adjustable 3–15 V. Feeds output ports P1 and P2 via e-fuses. |
| `VADJ2_BUCK` | LTM8063 U6 VOUT | Adjustable 3–15 V. Feeds output ports P3 and P4 via e-fuses. |
| `2V5_REF` | ADR4525BRZ VOUT | Precision 2.5 V reference for AD74416H REFO. |
| `5V_BUCK` → `4.7 µF` | — | Input bulk capacitor on 5V rail (C14, C28, C29). |
| `ESP_SDA` | GPIO42 (PCB) | I2C data — shared bus: DS4424, HUSB238, PCA9535. |
| `ESP_SCL` | GPIO41 (PCB) | I2C clock — shared bus. |
| `SPI_SDO` | AD74416H SDO | SPI MISO (data from AD74416H to ESP32 GPIO8). |
| `SPI_SDI` | ESP32 GPIO9 | SPI MOSI (data to AD74416H). |
| `SPI_CS_ADC` | ESP32 GPIO10 | AD74416H SYNC (active-low chip select). |
| `SPI_SCLK` | ESP32 GPIO11 | SPI clock shared by AD74416H and ADGS2414D. |
| `SPI_CS_MUX` | ESP32 GPIO21 (PCB) | ADGS2414D daisy-chain chip select (via level shifter). |
| `ADC_RESET` | ESP32 GPIO5 | Active-low hardware reset for AD74416H. |
| `ADC_RDY` | AD74416H ADC_RDY* | Open-drain, active-low. ESP32 GPIO6, 100 kΩ pull-up. |
| `ADC_ALRT` | AD74416H ALERT* | Open-drain, active-low. ESP32 GPIO7, 100 kΩ pull-up. |
| `ANA_PG` | TPS74601 PG | Analog supply power-good. ESP32 GPIO48. R7 pull-up to 3V3_BUCK. [UNSURE — GPIO47 or GPIO48] |
| `LED_DIN` | ESP32 GPIO0 | WS2812B data-in chain. R5 (10 kΩ) pull-up to 3V3_BUCK. |
| `LVL_OE` | ESP32 GPIO14 | TXS0108E output-enable (active-high). |
| `MUX_INT` | PCA9535 INT | GPIO expander interrupt. GPIO4 (PCB). |
| `EXP_IO2` | ESP32 GPIO (PCB) | Second expander interrupt or secondary power-good signal. [UNSURE — net label appears near ANA_PG and MUX_INT in schematic; may be an alternate label for the same pin or a separate GPIO input] |
| `USB_UP_P/N` | TPD2E1B06DRLR | USB 2.0 upstream differential pair (hub upstream ↔ USB-C J1). |
| `USB_DF1_P/N` | USB2422 DN1 | USB 2.0 downstream port 1 (hub → ESP32 native USB GPIO19/GPIO20). |
| `USB_DF2_P/N` | USB2422 DN2 | USB 2.0 downstream port 2 (hub → external connector J5 [UNSURE]). |
| `EN_USB_HUB` | PCA9535 port | USB hub enable (R15, R16 pull-ups to 3V3_BUCK). |
| `EN_15V_A` | PCA9535 port | Enable for LTM8049 ±15 V analog supply. |
| `VADJ1_EN` | PCA9535 port | LTM8063 U4 RUN pin enable. |
| `VADJ2_EN` | PCA9535 port | LTM8063 U6 RUN pin enable. |
| `VADJ1_PG` | LTM8063 U4 PG | Power-good feedback to PCA9535. |
| `VADJ2_PG` | LTM8063 U6 PG | Power-good feedback to PCA9535. |
| `IDAC_OUT0–OUT3` | DS4424 OUT0–OUT3 | Current DAC outputs to LTM8063 FB resistor networks. |
| `3V3_ADJ_EN` | PCA9535 port | TPS74601 enable pin. |
| `EXP_TX / EXP_RX` | ESP32 UART | [UNSURE — exact GPIO and destination. Likely UART bridge to external header.] |

---

## 2. Sheet 1 — ESP32

**Component:** U1 = ESP32-S3-WROOM-1-N8R2
**Supply:** 3V3_BUCK → module pin 2 (3V3)
**Ground:** GND (multiple module ground pins)

### 2.1 Reset Circuit

```
3V3_BUCK ──── R3 (10 kΩ) ────┬──── U1 EN (pin 3)
                              │
                             R4 (10 Ω) ── RST button (430473035826) ── GND
                              │
                             C1 (1 µF) ── GND
```

- RST = tactile switch (Wurth 430473035826), momentary short to GND
- R3 provides pull-up; R4 limits inrush current through button
- C1 provides debounce / reset pulse width

### 2.2 Boot Button

```
BOOT button (430473035826): A1/A2 ── GND,  B1/B2 ── LED_DIN net
```

> **Note:** The BOOT button shares the `LED_DIN` net label in the schematic with a **PowerNets** annotation. This is the GPIO0 strapping pin used to enter download mode. The R5 pull-up ensures GPIO0 is HIGH (normal boot) when button is released. [UNSURE — verify PowerNets annotation meaning]

### 2.3 I2C Pull-up Resistors

| Ref | Value | From | To |
|-----|-------|------|----|
| R1 | 5.1 kΩ | 3V3_BUCK | ESP_SDA |
| R2 | 5.1 kΩ | 3V3_BUCK | ESP_SCL |

### 2.4 Enable Decoupling

| Ref | Value | Connected to |
|-----|-------|-------------|
| C2 | 1 µF | U1 EN — GND |
| C3 | 22 µF | U1 EN — GND |

### 2.5 GPIO Pin Assignments

Source of truth: `config.h`. PCB net names from schematic.

| GPIO | Net / Function | Direction | Pull-up / Notes |
|------|---------------|-----------|-----------------|
| GPIO0 | `LED_DIN` | OUT | R5 (10 kΩ) to 3V3_BUCK. WS2812B chain data. Also BOOT strapping pin. |
| GPIO4 | `MUX_INT` (PCB) / `I2C_SCL` (breadboard) | IN / I2C | PCB: PCA9535 INT. Breadboard: I2C SCL. |
| GPIO5 | `ADC_RESET` | OUT | Pull-up [UNSURE resistor value — likely 100 kΩ from R56 on Analog sheet]. |
| GPIO6 | `ADC_RDY` | IN | R63 (100 kΩ) pull-up to 3V3_BUCK on Analog sheet. Open-drain from AD74416H. |
| GPIO7 | `ADC_ALRT` | IN | R64 (100 kΩ) pull-up to 3V3_BUCK on Analog sheet. Open-drain from AD74416H. |
| GPIO8 | `SPI_SDO` (MISO) | IN | From AD74416H SDO. |
| GPIO9 | `SPI_SDI` (MOSI) | OUT | To AD74416H SDI. |
| GPIO10 | `SPI_CS_ADC` | OUT | AD74416H SYNC (active-low CS). |
| GPIO11 | `SPI_SCLK` | OUT | Shared SPI clock (AD74416H + ADGS2414D). |
| GPIO14 | `LVL_OE` | OUT | TXS0108E U13 and U15 output enable. |
| GPIO19 | `USB_DF1_N` | USB | Native USB D− (from hub downstream port 1). |
| GPIO20 | `USB_DF1_P` | USB | Native USB D+ (from hub downstream port 1). |
| GPIO21 | `SPI_CS_MUX` (PCB) | OUT | ADGS2414D daisy-chain CS (via level shifter). |
| GPIO41 | `ESP_SCL` (PCB) | I2C | PCB I2C clock. 5.1 kΩ pull-up R2 to 3V3_BUCK. 400 kHz fast mode on PCB. |
| GPIO42 | `ESP_SDA` (PCB) | I2C | PCB I2C data. 5.1 kΩ pull-up R1 to 3V3_BUCK. |
| GPIO47 or GPIO48 | `EXP_IO2` / `ANA_PG` | IN | R7 (10 kΩ) pull-up to 3V3_BUCK. [UNSURE — exact GPIO; ANA_PG power-good or PCA9535 INT2] |

> **Breadboard mode differences** (`BREADBOARD_MODE 1` in config.h):
> - I2C SDA → GPIO1, I2C SCL → GPIO4 (instead of GPIO42/GPIO41)
> - MUX_CS → GPIO12 (instead of GPIO21)
> - MUX_INT → NC (not connected)
> - ADGS device count → 1 (instead of 4)

**Nets visible on ESP32 left side but not yet fully traced:**

| Net label | Connected to (best guess) |
|-----------|--------------------------|
| `BLOCK1_IO1` – `BLOCK1_IO6` | PCA9535 port 0 or ADGS2414D control [UNSURE] |
| `BLOCK2_IO1` – `BLOCK2_IO6` | PCA9535 port 1 or ADGS2414D control [UNSURE] |
| `EXP_RX` / `EXP_TX` | UART bridge / external expansion [UNSURE — exact GPIO] |

### 2.6 WS2812B RGB LEDs

Three addressable LEDs (D1, D2, D3 = XL-2020RGBC-WS2812B) in daisy-chain.

| Pin | Net |
|-----|-----|
| VDD | 5V_BUCK |
| GND | GND |
| DIN (D1) | LED_DIN (from ESP32 GPIO0) |
| DOU (D1) → DIN (D2) | Internal chain |
| DOU (D2) → DIN (D3) | Internal chain |
| DOU (D3) | Not connected (×) |

---

## 3. Sheet 2 — USB and Hub

### 3.1 J1 — USB-C Device Connector (ESP32 native USB)

**Component:** USB4125-GF-A-0190 (6-pin SMD right-angle USB-C)

| J1 Pin | Net | Destination |
|--------|-----|-------------|
| VBUS (A9/B9) | VBUS | → R8 (10 Ω) → U22 VIN (HUSB238) |
| CC1 (A5) | CC1 | → HUSB238 CC1, R13 (5.1 kΩ) pull-down to GND |
| CC2 (B5) | CC2 | → HUSB238 CC2, R14 (5.1 kΩ) pull-down to GND |
| D+ (A6 / Dp1) | USB_UP_P | → D4 (TPD2E1B06DRLR) IOB1/IOB2 |
| D− (A7 / Dn1) | USB_UP_N | → D4 (TPD2E1B06DRLR) IOA1/IOA2 |
| MNT (shell) | GND | — |

### 3.2 J2 — USB-C Hub Upstream Connector

**Component:** USB4085-GF-A (24-pin SMD right-angle USB-C)

| J2 Pin group | Net | Destination |
|-------------|-----|-------------|
| VBUS (A4/A9/B4/B9) | VBUS | → D5 (NRVBA340T3G Schottky) anode |
| D5 cathode | 5V_BUCK | Main 5 V supply feed |
| CC1 (A5) / CC2 (B5) | CC1/CC2 | → U2 (USB2422) SMBCLK/SMBDATA ??? [UNSURE — likely not connected per hub config] |
| SBU1/SBU2, DP1/DN1, DP2/DN2 | USB_UP_P / USB_UP_N | → D4 ESD protection → U2 USBDP UP / USBDM UP |
| MNT | GND | — |

> R13, R14 (5.1 kΩ): CC pull-down resistors — sets J1 as UFP (device, power consumer).

### 3.3 D4 — USB ESD Protection

**Component:** TPD2E1B06DRLR (TVS diode 5.5 V / 15 V, SOT-6)

| Pin | Net |
|-----|-----|
| IOA1 | USB_UP_N (from J1) |
| IOA2 | USB_UP_N (to U2) |
| IOB1 | USB_UP_P (from J1) |
| IOB2 | USB_UP_P (to U2) |
| GND | GND |

### 3.4 D5 — VBUS Reverse-Polarity / Isolation Diode

**Component:** NRVBA340T3G (Schottky 40 V, 3 A, SMA)

| Pin | Net |
|-----|-----|
| Anode | VBUS (from J2) |
| Cathode | 5V_BUCK |

### 3.5 U22 — HUSB238 (USB-PD Sink Controller)

**I2C address: 0x08 (fixed)**

| Pin | Net | Notes |
|-----|-----|-------|
| VIN | VBUS (via R8 10 Ω) | C (1 µF) decoupling |
| GATE | Q1 gate | Via R9 (49.9 kΩ) + R10 (5.1 kΩ) voltage divider to GND |
| D+ / D− | USB data from J1 | PD communication |
| CC1 / CC2 | From J1 | Cable orientation detection |
| SCL | ESP_SCL | I2C (addr 0x08) |
| SDA | ESP_SDA | I2C |
| ISET | R11 (22.6 kΩ) → GND | Sets current limit |
| VSET | R12 = NC | [UNSURE — NC resistor in BOM] |
| GND | GND | — |

### 3.6 Q1 — AONR21307 (P-ch MOSFET, 30 V, 3×3 DFN)

Controls connection from VBUS to 20V_USB rail.

| Pin | Net |
|-----|-----|
| Source | 20V_USB |
| Gate | HUSB238 GATE (via R9/R10 divider) |
| Drain | [UNSURE — output side, feeds LTM8078 and LTM8063 VIN] |

> When HUSB238 GATE goes low (≈0 V), Q1 turns on (P-ch). The 49.9 kΩ / 5.1 kΩ divider between GATE and the rail scales the drive voltage for the gate threshold.

### 3.7 U2 — USB2422T/MJ (USB 2.0 Hub Controller)

**Crystal:** Y1 = ABM11W-24.0000MHz-8-B1U-T3 with C9, C10 (8 pF) load capacitors.

| Pin | Net | Notes |
|-----|-----|-------|
| VDD33 (×3) | 3V3_BUCK | Multiple supply pins, decoupled with C5, C6, C7 |
| VBUS DET | VBUS | Bus detect |
| USBDP UP / USBDM UP | USB_UP_P / USB_UP_N | Upstream port (to J1 via D4) |
| USBDP DN1 / USBDM DN1 | USB_DF1_P / USB_DF1_N | Downstream port 1 → ESP32 GPIO20/GPIO19 (native USB) |
| USBDP DN2 / USBDM DN2 | USB_DF2_P / USB_DF2_N | Downstream port 2 → [UNSURE — external connector?] |
| RESET N | R15 (100 kΩ) → 3V3_BUCK | Pull-up (active low reset, released at power-on) |
| EN_USB_HUB (SUSP IND / LOCAL PWR) | R16 (100 kΩ) → 3V3_BUCK | [UNSURE — whether this is directly the EN_USB_HUB signal or tied high] |
| SMBCLK / CFG SEL0 | GND | Configures hub via strapping |
| SMBDATA / NON REM1 | GND | Configures hub via strapping |
| XTALIN / XTALOUT | Y1 with C9/C10 | 24 MHz crystal |
| CRFILT | R17 (12 kΩ) → C7 → GND | PLL filter |
| RBIAS | [UNSURE — resistor to GND] | Bias current setting |
| OCS1 N, OCS2 N | [× — not connected] | Over-current sense (unused) |
| PRTPWR1, PRTPWR2 | [× or to PCA9535] | Port power control [UNSURE] |
| VSS | GND | — |

---

## 4. Sheet 3 — Power and Adjustments

### 4.1 U3 — LTM8078EY#PBF (Dual 40 V, 3 A Silent Switcher)

Generates 5V_BUCK and 3V3_BUCK from 20V_USB.

| Pin | Net | Notes |
|-----|-----|-------|
| VIN1, VIN2 | 20V_USB | Input supply, C14 (4.7 µF) + C15 (0.1 µF) decoupling |
| VOUT1 | 5V_BUCK | 5 V output; R21 (45.3 kΩ) + R23 (78.7 kΩ) on FB1 set voltage |
| VOUT2 | 3V3_BUCK | 3.3 V output; FB2 resistor network [UNSURE exact values] |
| FB1 | R21 / R23 divider | Sets VOUT1 = 5 V |
| FB2 | Resistor divider | Sets VOUT2 = 3.3 V [UNSURE — resistor refs] |
| CLKOUT | → U4/U6 SYNC (?) | [UNSURE — may synchronise the DCDC switchers] |
| PG1 | 5V power-good | [UNSURE destination] |
| PG2 | 3V3 power-good | [UNSURE destination] |
| RT | Timing resistor | [UNSURE value] |
| OM | [UNSURE] | |
| TRSS1/TRSS2 | C20 (10 nF) | Soft-start capacitors |
| BIAS | [internal] | |
| GND | GND | Multiple GND pins; C11 (0.1 µF) + C12 (22 µF) output decoupling on 5V rail; C16 (0.1 µF) + C17 (22 µF) on 3V3 rail |

> Output decoupling:
> - 5V_BUCK: C11 (0.1 µF) + C12 (22 µF)
> - 3V3_BUCK: C16 (0.1 µF) + C17 (22 µF)

### 4.2 U5 — TPS74601PDRVT (1 A LDO, Adjustable)

Generates 3V3_ADJ (adjustable ~3.3 V) for AD74416H analog sub-supply.

| Pin | Net | Notes |
|-----|-----|-------|
| IN | 5V_BUCK | C21 (1 µF) + C22 (1000 pF) input decoupling |
| EN | 3V3_ADJ_EN | Enable from PCA9535 (active-high) |
| OUT | 3V3_ADJ | C23 (1 µF) + C24 (1000 pF) output decoupling |
| FB/NC | R103 / R104 / R105 resistor network | Adjusts output voltage. R105 = 6.98 kΩ to GND; R104 = 34.8 kΩ; R103 = 100 kΩ [UNSURE exact topology] |
| PG | ANA_PG | Power-good output → ESP32 GPIO48 (via R7 pull-up to 3V3_BUCK) |
| EP | GND | Exposed thermal pad |

### 4.3 U4 and U6 — LTM8063EY#PBF (Adjustable 0.8–15 V, 2 A DCDC)

Two identical adjustable buck converters. Output voltage set by DS4424 current injection into the FB pin.

**U4** → VADJ1_BUCK (feeds P1/P2 e-fuses)
**U6** → VADJ2_BUCK (feeds P3/P4 e-fuses)

| Pin | Net (U4) | Net (U6) | Notes |
|-----|----------|----------|-------|
| VIN | 20V_USB | 20V_USB | C13 / C25 (1 µF) decoupling |
| RUN | VADJ_1_EN | VADJ_2_EN | Enable from PCA9535 (R19 / R26 100 kΩ pull-down to GND) |
| FB | R20 (45.3 kΩ) to VOUT + IDAC_OUT1 | R27 (45.3 kΩ) + IDAC_OUT2 | DS4424 current injection shifts FB voltage, adjusting Vout |
| PG | VADJ1_PG | VADJ2_PG | Power-good → PCA9535 input |
| VOUT | VADJ1_BUCK | VADJ2_BUCK | C18 / C26 (22 µF) output decoupling |
| TR/SS | C19 / C27 (10000 pF = 10 nF) | — | Soft-start cap (4 ms SS annotation visible in schematic) |
| SYNC | [UNSURE — possibly from LTM8078 CLKOUT] | — | — |
| RT | [UNSURE — resistor to GND] | — | Frequency-setting resistor |
| GND | GND | GND | Multiple pins |

> **Voltage adjustment mechanism:**
> The DS4424 current DAC output (IDAC_OUT1, IDAC_OUT2) injects or sinks a small current into the LTM8063 feedback node. This shifts the FB voltage seen by the converter, causing it to adjust its output to a different level than the R20/R27 fixed resistor alone would set.

### 4.4 U7 — LTM8049IY (Dual ±2.5 V to ±24 V DCDC)

Generates ±15 V analog supplies for the AD74416H.

| Pin group | Net | Notes |
|-----------|-----|-------|
| VIN1, VIN1 | 5V_BUCK | C (0.1 µF) decoupling |
| VOUT1P | +15V_ANA | R30 (165 kΩ) in feedback; C32 (22 µF), C34 (47 µF) output cap |
| VOUT2N | -15V_ANA | R37 (178 kΩ) in feedback; C (47 µF) output cap |
| RUN1, RUN2 | EN_15V_A | Enable from PCA9535. Soft-start cap C20 (10 nF, 20 ms SS annotation visible). |
| PG2 | ANA_PG | Power-good output. R96 (100 kΩ) pull-up to 3V3_BUCK. Same net as TPS74601 PG — both supply power-good signals are wire-OR'd onto ANA_PG. |
| FBX1 | R30 (165 kΩ) to VOUT1P | Sets +15 V feedback. |
| FBX2 | R37 (178 kΩ) to VOUT2N | Sets −15 V feedback. |
| SHARE1, SHARE2 | [UNSURE — possibly current sharing between internal converters] | |
| CLKOUT1/CLKOUT2 | [UNSURE] | |
| SYNC1, SYNC2 | [UNSURE] | |
| GND | GND | C (0.1 µF + larger bulk) decoupling caps |

### 4.5 U8 — DS4424+ (4-ch I²C Current DAC)

**I2C address: 0x10 (7-bit) / 0x20 (8-bit write address). A0=GND, A1=GND.**

> Note: The schematic shows "ADDR: 0x20" which is the 8-bit address (7-bit 0x10 shifted left). Confirmed in `config.h`: `DS4424_I2C_ADDR = 0x10`.

| Pin | Net | Notes |
|-----|-----|-------|
| VCC | 3V3_BUCK | C30 (1 µF) + C31 (1000 pF) decoupling |
| SDA | ESP_SDA | I2C data |
| SCL | ESP_SCL | I2C clock |
| A0 | GND via R32 (154 kΩ) | Address bit 0 = 0 |
| A1 | GND via R34 (154 kΩ) | Address bit 1 = 0 |
| FS0 | R35 (154 kΩ) → GND | Full-scale current range select [UNSURE exact purpose] |
| FS1–FS3 | R36 (154 kΩ) → GND | Full-scale range [UNSURE] |
| OUT0 | IDAC_OUT0 | → TPS74601 (VLOGIC / 3V3_ADJ) FB node — adjusts logic-level voltage (1.8–5.0 V) |
| OUT1 | IDAC_OUT1 | → LTM8063 U4 (VADJ1) FB node — adjusts Block 1 supply (3–15 V) |
| OUT2 | IDAC_OUT2 | → LTM8063 U6 (VADJ2) FB node — adjusts Block 2 supply (3–15 V) |
| OUT3 | IDAC_OUT3 | [UNSURE — destination; possibly unused] |
| EP | GND | — |

### 4.6 U21 — ADR4525BRZ (Precision 2.5 V Voltage Reference)

| Pin | Net | Notes |
|-----|-----|-------|
| VIN | 5V_BUCK | C77 (1 µF) + C78 (0.1 µF) decoupling |
| VOUT | 2V5_REF | → AD74416H REFO. C79 (1 µF) + C80 (1 µF) output decoupling |
| GND | GND | — |
| NC | — | Unused pins (U21B in schematic notation is the same IC, second footprint section) |

---

## 5. Sheet 4 — ADC Section (AD74416H)

### 5.1 U9 — AD74416HBCPZ (Quad-Channel Software-Configurable I/O)

**SPI device address: 0x00 (AD0=AD1=GND)**

#### Power Pins

| Pin | Net | Decoupling |
|-----|-----|-----------|
| AVCC1 | +15V_ANA | C37 (0.1 µF) + C38 (10 µF) |
| AVDD_HI | +15V_ANA | C40 (10 µF) + C41 (0.1 µF) |
| AVDD_LO | 3V3_ADJ (or 3V3_BUCK) [UNSURE] | C42 (10 µF) |
| DVCC | 3V3_BUCK | C59 (0.1 µF) |
| DVCC81 | 3V3_ADJ (or 3V3_BUCK) [UNSURE] | C36 (10 µF) |
| LVIN | -15V_ANA | C57 (0.1 µF) + C58 (10 µF) |
| REFO | 2V5_REF | C44 (0.022 µF) |
| AGND / DGND | GND | — |

#### SPI and Control Pins

| Pin | Net | Notes |
|-----|-----|-------|
| SYNC* | SPI_CS_ADC | Chip select, active-low → ESP32 GPIO10 |
| SCLK | SPI_SCLK | Clock → ESP32 GPIO11 |
| SDI | SPI_SDI | MOSI from ESP32 GPIO9 |
| SDO | SPI_SDO | MISO to ESP32 GPIO8 |
| RESET* | ADC_RESET | Active-low reset → ESP32 GPIO5. R56 (100 kΩ) pull-up to 3V3_BUCK |
| ADC_RDY* | ADC_RDY | Open-drain, active-low → ESP32 GPIO6. R63 (100 kΩ) pull-up |
| ALERT* | ADC_ALRT | Open-drain, active-low → ESP32 GPIO7. R64 (100 kΩ) pull-up |
| AD0 | GND | Device address bit 0 = 0 |
| AD1 | GND | Device address bit 1 = 0 |

#### GPIO Pins

| Pin | Net | LED |
|-----|-----|-----|
| GPIO_A | ADC_GPIO1 | R97 (133 Ω) → D8A green anode; D8 = HSMF-C165 |
| GPIO_B | ADC_GPIO2 | R100 (107 Ω) → D8B red anode |
| GPIO_C | ADC_GPIO3 | R98 (133 Ω) → D9A green anode; D9 = HSMF-C165 |
| GPIO_D | ADC_GPIO4 | R101 (107 Ω) → D9B red anode |
| GPIO_E | ADC_GPIO5 | R99 (133 Ω) → D12A green anode; D12 = HSMF-C165 |
| GPIO_F | ADC_GPIO6 | R102 (107 Ω) → D12B red anode |

All LED cathodes → GND.

### 5.2 Per-Channel Analog Frontend (×4 identical: CH_A, CH_B, CH_C, CH_D)

Each channel has a P-ch source driver MOSFET, a precision 12 Ω sense resistor for IIN measurement, and VSENSE input filtering.

#### Source Driver MOSFET

| IC | Channel | Component |
|----|---------|-----------|
| Q2 | CH_A | SI7113ADN-T1-GE3 (P-ch, 100 V, 1212-8) |
| Q3 | CH_B | SI7113ADN-T1-GE3 |
| Q4 | CH_C | SI7113ADN-T1-GE3 |
| Q5 | CH_D | SI7113ADN-T1-GE3 |

Per channel (example CH_A):

```
+15V_ANA ── R39 (150 mΩ, 0805) ── SRC_SNS_0 ── Q2 Source
                                                  Q2 Gate  ← SRC_GATE_0 (AD74416H DO_SRC_GATE_A)
                                                  Q2 Drain → LCOMP_0
                                                              LCOMP_0 → AD74416H LCOMP_A
                                                              LCOMP_0 → D6 (NSR0240V2T5G) anode
                                                  D6 cathode → VIOUT_0

DO_SRC_SNS_A ← SRC_SNS_0 (feedback to AD74416H for current sense)
```

| Net (CH_A) | AD74416H Pin | Description |
|-----------|-------------|-------------|
| SRC_GATE_0 | DO_SRC_GATE_A | Gate drive for Q2 |
| SRC_SNS_0 | DO_SRC_SNS_A | Current sense node (after 150 mΩ shunt) |
| LCOMP_0 | LCOMP_A | Load compensation input |
| VIOUT_0 | VIOUT_A | Voltage/current output |
| CCOMP_0 | CCOMP_A | Compensation cap: C45/C46 (1220 pF) to GND |

#### VSENSE Input Network (per channel)

```
VIOUT_x ──── R4x (2 kΩ) ──── VSENS_P_x ──── AD74416H VSENSEP_x
                                 │
                                C4x (1220 pF) ── GND     [UNSURE exact cap designators]

VSENS_N_x ──── R4x (2 kΩ) ──── AD74416H VSENSEN_x
                  │
                 C4x (4700 pF) ── GND

SNS_HF_x ──── AD74416H SENSEHF_x
SNS_LF_x ──── AD74416H SELSELF_x (or SENSELF_x)
```

> The exact resistor/cap designators per channel follow this pattern from the BOM:
> - 2 kΩ: R42–R43, R46–R49, R54–R55, R59–R62, R68–R69, R71–R72, R74–R75, R82–R87
> - 12 Ω (0.1%, IIN sense): R44, R45, R57, R58 (one per channel on VSENSE differential input)
> - 1220 pF (CCOMP): C45, C46, C51, C52
> - 4700 pF (HF filter): C47–C56

#### Schottky Protection Diodes

| Ref | Channel | Placement |
|-----|---------|-----------|
| D6 | CH_A | NSR0240V2T5G between LCOMP_0 and VIOUT_0 |
| D7 | CH_B | NSR0240V2T5G |
| D10 | CH_C | NSR0240V2T5G |
| D11 | CH_D | NSR0240V2T5G |

#### ADC Channel Output Connectors

Signals labeled `ADC_OUT_1` through `ADC_OUT_4` connect to J3/J4/J5 headers (M20-7830442, 8-position, 2.54 mm). [UNSURE — exact signal mapping per connector position]

---

## 6. Sheet 5 — IOs, MUX, and Connectors

### 6.1 U20 — PCA9535AHF (16-bit I²C GPIO Expander)

**I2C address: 0x23 (A2=0, A1=1, A0=1)**
24-pin HWQFN.

| Pin | Net | Notes |
|-----|-----|-------|
| SDA | ESP_SDA | I2C, pull-up on ESP32 sheet (R1 5.1 kΩ) |
| SCL | ESP_SCL | I2C, pull-up R2 |
| INT | MUX_INT / EXP_IO2 | Active-low interrupt → ESP32 GPIO4 (PCB) |
| A0 | [pulled to set addr bit 0 = 1] | [UNSURE — resistor to VCC or GND] |
| A1 | [pulled to set addr bit 1 = 1] | [UNSURE] |
| A2 | GND (addr bit 2 = 0) | [UNSURE] |
| VCC | 3V3_BUCK | — |
| GND | GND | — |

Port assignments [UNSURE — partially readable from schematic]:

| Port | Bit | Net | Function |
|------|-----|-----|---------|
| P0 | 0 | VADJ_1_EN | LTM8063 U4 enable |
| P0 | 1 | VADJ_2_EN | LTM8063 U6 enable |
| P0 | 2 | EN_15V_A | LTM8049 ±15 V enable |
| P0 | 3 | EN_USB_HUB | USB2422 enable |
| P0 | 4 | 3V3_ADJ_EN | TPS74601 enable |
| P0 | 5–7 | EFUSE_EN_1–3 [UNSURE] | TPS16410 e-fuse enables |
| P1 | 0 | EFUSE_EN_4 [UNSURE] | 4th e-fuse enable |
| P1 | 1–4 | EFUSE_FLT_1–4 [UNSURE] | E-fuse fault inputs |
| P1 | 5 | VADJ1_PG | LTM8063 U4 power-good |
| P1 | 6 | VADJ2_PG | LTM8063 U6 power-good |
| P1 | 7 | [UNSURE] | |

> Full port map is not clearly readable at the available schematic resolution. The above is based on cross-referencing firmware (`pca9535.h/.cpp`) and partial schematic reading.

### 6.2 U13 and U15 — TXS0108EPWR (8-bit Bidirectional Level Translator)

Two identical instances, one per MUX block.

| Pin | Net | Notes |
|-----|-----|-------|
| VCCA | 3V3_BUCK | Low-voltage side (ESP32 SPI logic, 3.3 V) |
| VCCB | 5V_BUCK (visible in image) | High-voltage side for ADGS logic supply. [UNSURE — could be VADJ1/VADJ2 depending on which switches are connected; schematic image shows 5V_BUCK label on VCCB] |
| OE | LVL_OE (ESP32 GPIO14) | Active-high output enable |
| A1–A8 | SPI_SDI, SPI_SCLK, SPI_CS_MUX + control signals (3.3 V side) | From ESP32 |
| B1–B8 | Level-shifted signals to ADGS2414D daisy-chain | To MUX switches |

### 6.3 U10, U11, U16, U17 — ADGS2414DBCCZ (Octal SPST Analog Switch, MUX Matrix)

Four devices daisy-chained for the 32-switch signal routing matrix (U10, U11, U16, U17).

| Pin | Net / Notes |
|-----|------------|
| VDD | VADJ1_BUCK or VADJ2_BUCK [UNSURE — which devices on which rail] |
| GND | GND |
| DIN | SPI MOSI (via TXS0108E level shifter) |
| SCLK | SPI_SCLK (via level shifter) |
| CS | SPI_CS_MUX (via level shifter, daisy-chain) |
| DOUT | → DIN of next device (daisy-chain) |
| S1–S8 (A/B pairs) | Routed to IO_Block terminal connectors and AD74416H channels. Per-device switch grouping: Group A (S1–S4 bits 0–3) → analog-capable IO (pos.1), Group B (S5–S6 bits 4–5) → digital IO (pos.2), Group C (S7–S8 bits 6–7) → digital IO (pos.3). See Section 6.5 for full mapping. |

`ADGS_NUM_DEVICES = 4` on PCB. The daisy-chain shift register is 32 bits wide (4 × 8 switches).

### 6.3a U23 — ADGS2414DBCCZ (Self-Test / Monitoring Switch)

**U23 is a dedicated self-test device.** It is **not** part of the main 4-device MUX daisy-chain.

Purpose: routes internal power rails and e-fuse IMON (current monitor) pins to **AD74416H channel D** so that the firmware can measure supply voltages and port currents using the on-chip 24-bit ADC without any external measurement instrument.

| U23 Switch | Connected signals (A/B sides) | Measured via |
|-----------|------------------------------|-------------|
| S1–S8 | Power rails (VADJ1_BUCK, VADJ2_BUCK, +15V_ANA, -15V_ANA, 3V3_BUCK, 5V_BUCK, 3V3_ADJ) + TPS16410 IMON pins | AD74416H CH_D (VIOUT_3 / VSENS_P_3) |

> The exact per-switch assignment (which rail goes to which S1–S8 position) is not fully readable at the available schematic resolution. [UNSURE — exact per-switch mapping]

### 6.4 U12, U14, U18, U19 — TPS16410DRCR (E-Fuse, 2.7–40 V, 1.8 A)

One per output port (P1–P4).

| Pin | Net | Notes |
|-----|-----|-------|
| IN | VADJ1_BUCK (U12/U14) / VADJ2_BUCK (U18/U19) | Input power from adjustable DCDC |
| OUT | → P1/P2/P3/P4 terminal block | Protected output to screw terminal |
| EN | EFUSE_EN_x from PCA9535 | Active-high enable |
| FLT | EFUSE_FLT_x to PCA9535 | Active-low fault flag |
| ILIM | R (to GND) | Sets current limit threshold |
| GND | GND | — |

### 6.5 Physical IO Architecture

The BugBuster has **12 physical IOs** organized into **2 Blocks**, each containing **2 IO_Blocks** of **3 IOs**.

```
┌─────────────────────────────────────────────────────────────────────────┐
│ BLOCK 1 — VADJ1 (3–15 V adjustable via IDAC ch 1)                     │
│                                                                         │
│   IO_Block 1 (EFUSE1)          IO_Block 2 (EFUSE2)                     │
│   ┌─────────────────────┐      ┌─────────────────────┐                 │
│   │ IO 1  — analog/HAT  │      │ IO 4  — analog/HAT  │                 │
│   │ IO 2  — digital     │      │ IO 5  — digital     │                 │
│   │ IO 3  — digital     │      │ IO 6  — digital     │                 │
│   │ VCC   GND           │      │ VCC   GND           │                 │
│   └─────────────────────┘      └─────────────────────┘                 │
├─────────────────────────────────────────────────────────────────────────┤
│ BLOCK 2 — VADJ2 (3–15 V adjustable via IDAC ch 2)                     │
│                                                                         │
│   IO_Block 3 (EFUSE3)          IO_Block 4 (EFUSE4)                     │
│   ┌─────────────────────┐      ┌─────────────────────┐                 │
│   │ IO 7  — analog/HAT  │      │ IO 10 — analog/HAT  │                 │
│   │ IO 8  — digital     │      │ IO 11 — digital     │                 │
│   │ IO 9  — digital     │      │ IO 12 — digital     │                 │
│   │ VCC   GND           │      │ VCC   GND           │                 │
│   └─────────────────────┘      └─────────────────────┘                 │
└─────────────────────────────────────────────────────────────────────────┘
```

#### VCC Requirements

Each IO_Block's VCC pin is only active when ALL of:
1. VADJ regulator enabled (VADJ1_EN or VADJ2_EN via PCA9535)
2. E-fuse enabled (EFUSE_EN_x via PCA9535)
3. E-fuse fault-free (EFUSE_FLT_x clear)
4. Supply power-good (VADJ1_PG or VADJ2_PG asserted)

#### IO Capabilities (MUX-exclusive — one function per IO at a time)

| IO | Position | MUX Options |
|----|----------|-------------|
| 1, 4, 7, 10 | 1st in each IO_Block | ESP GPIO (high drive) · ESP GPIO (low drive) · AD74416H channel · HAT passthrough |
| 2, 3, 5, 6, 8, 9, 11, 12 | 2nd/3rd in each IO_Block | ESP GPIO (high drive) · ESP GPIO (low drive) |

#### AD74416H Channel Mapping

| IO | AD74416H Channel |
|----|-----------------|
| 1 | Channel A (0) |
| 4 | Channel B (1) |
| 7 | Channel C (2) |
| 10 | Channel D (3) |

#### VLOGIC — Common Logic Level

All digital IOs are level-shifted to **VLOGIC** (1.8–5.0 V adjustable) via TXS0108E (U13, U15).
- Controlled by: DS4424 IDAC OUT0 → TPS74601 (3V3_ADJ) feedback
- Output Enable: ESP32 GPIO14 (LVL_OE) — **must be enabled** for any digital signal to pass

#### Serial Bridge

A configurable UART bridge can be routed to any 2 of the 12 IOs (TX + RX) via MUX.
The bridge connects to a secondary ESP32 serial port used by external programs.
Firmware commands: GET_UART_CONFIG (0x50), SET_UART_CONFIG (0x51), GET_UART_PINS (0x52).

#### MUX Device-to-IO_Block Mapping

Each ADGS2414D device handles one IO_Block (3 IOs).
From firmware `MUX_GPIO_MAP` in `adgs2414d.h`:

| Device | Ref  | IO_Block | IOs      | ESP GPIOs     |
|--------|------|----------|----------|---------------|
| 0      | U10  | 1        | 1, 2, 3  | 1, 2, 3       |
| 1      | U11  | 2        | 4, 5, 6  | 5, 6, 7 [!]   |
| 2      | U16  | 3        | 7, 8, 9  | 13, 12, 11 [!]|
| 3      | U17  | 4        | 10,11,12 | 10, 9, 8 [!]  |

**[!] GPIO conflict warning:** IOs 4–6 share ESP GPIOs with AD74416H control pins (RESET=5, ADC_RDY=6, ALERT=7), IOs 9–12 share with SPI (SCLK=11, CS=10, SDI=9, SDO=8). The `MUX_GPIO_MAP` in firmware may be preliminary — verify on final PCB.

#### Switch Group Structure (per device)

```
Group A (bits 0-3, S1-S4): Analog-capable IO (position 1)
  S1 = ESP GPIO high drive    S2 = AD74416H channel
  S3 = ESP GPIO low drive     S4 = HAT passthrough

Group B (bits 4-5, S5-S6): Digital IO (position 2)
  S5 = ESP GPIO high drive    S6 = ESP GPIO low drive

Group C (bits 6-7, S7-S8): Digital IO (position 3)
  S7 = ESP GPIO high drive    S8 = ESP GPIO low drive
```

#### ESP GPIO Net Names

| Block | Nets | IOs |
|-------|------|-----|
| Block 1 | `BLOCK1_IO1` – `BLOCK1_IO6` | IO 1–6 |
| Block 2 | `BLOCK2_IO1` – `BLOCK2_IO6` | IO 7–12 |

### 6.6 Terminal Block Connectors

**Component:** Wurth 691382010005 (5-position, right-angle, screw terminal)
(BOM: P1, P2, P3, P4 — one per IO_Block)

| Position | Signal | Notes |
|----------|--------|-------|
| 1 | VADJ_x (e-fuse output) | Adjustable 3–15 V power (VCC) |
| 2–4 | IO signals | Routed through ADGS2414D MUX matrix |
| 5 | GND | Signal ground |

#### Fifth MUX (U23) — Self-Test / Monitoring

U23 is a dedicated self-test ADGS2414D (NOT part of the main 4-device daisy-chain).
Routes internal power rails and e-fuse IMON pins to AD74416H CH_D for voltage/current monitoring.
**Not yet implemented in firmware or library — reserved for future use.**

---

## 7. Cross-Sheet Net Summary

| Net | Source Sheet | Destination Sheet(s) |
|-----|-------------|---------------------|
| `20V_USB` | USB (Q1 drain) | Power (LTM8078 VIN, LTM8063 VIN ×2) |
| `5V_BUCK` | Power (LTM8078 VOUT1) | ESP32 (WS2812B), USB (hub VDD, crystal), Power (LTM8049 VIN, ADR4525 VIN), ADC (partial) |
| `3V3_BUCK` | Power (LTM8078 VOUT2) | ESP32 (module VCC, pull-ups), USB (USB2422 VDD), Power (DS4424 VCC), ADC (DVCC), IOs (level shifter VCCA, PCA9535 VCC) |
| `+15V_ANA` | Power (LTM8049) | ADC (AD74416H AVCC1/AVDD_HI, Q2–Q5 source bias) |
| `-15V_ANA` | Power (LTM8049) | ADC (AD74416H LVIN) |
| `3V3_ADJ` | Power (TPS74601) | ADC (AD74416H AVDD_LO/DVCC81) [UNSURE] |
| `2V5_REF` | Power (ADR4525) | ADC (AD74416H REFO) |
| `VADJ1_BUCK` | Power (LTM8063 U4) | IOs (e-fuse U12/U14 IN, level shifter U13 VCCB) |
| `VADJ2_BUCK` | Power (LTM8063 U6) | IOs (e-fuse U18/U19 IN, level shifter U15 VCCB) |
| `ESP_SDA` | ESP32 (GPIO42 PCB) | USB (HUSB238 SDA), Power (DS4424 SDA), IOs (PCA9535 SDA) |
| `ESP_SCL` | ESP32 (GPIO41 PCB) | USB (HUSB238 SCL), Power (DS4424 SCL), IOs (PCA9535 SCL) |
| `SPI_SDI` | ESP32 (GPIO9) | ADC (AD74416H SDI) |
| `SPI_SDO` | ADC (AD74416H SDO) | ESP32 (GPIO8) |
| `SPI_CS_ADC` | ESP32 (GPIO10) | ADC (AD74416H SYNC*) |
| `SPI_SCLK` | ESP32 (GPIO11) | ADC (AD74416H SCLK), IOs (ADGS2414D SCLK via level shifter) |
| `SPI_CS_MUX` | ESP32 (GPIO21 PCB) | IOs (ADGS2414D CS via level shifter) |
| `ADC_RESET` | ESP32 (GPIO5) | ADC (AD74416H RESET*) |
| `ADC_RDY` | ADC (AD74416H ADC_RDY*) | ESP32 (GPIO6) |
| `ADC_ALRT` | ADC (AD74416H ALERT*) | ESP32 (GPIO7) |
| `LVL_OE` | ESP32 (GPIO14) | IOs (TXS0108E U13/U15 OE) |
| `MUX_INT` | IOs (PCA9535 INT) | ESP32 (GPIO4 PCB) |
| `LED_DIN` | ESP32 (GPIO0) | ESP32 sheet (D1 DIN) |
| `ANA_PG` | Power (TPS74601 PG wire-OR'd with LTM8049 PG2) | ESP32 (GPIO48). R96 (100 kΩ) pull-up to 3V3_BUCK. Both power-good outputs share the same net. |
| `IDAC_OUT1` | Power (DS4424 OUT1) | Power (LTM8063 U4 FB) |
| `IDAC_OUT2` | Power (DS4424 OUT2) | Power (LTM8063 U6 FB) |
| `VADJ_1_EN` | IOs (PCA9535 P0.0) | Power (LTM8063 U4 RUN) |
| `VADJ_2_EN` | IOs (PCA9535 P0.1) | Power (LTM8063 U6 RUN) |
| `EN_15V_A` | IOs (PCA9535 P0.2) | Power (LTM8049 RUN) |
| `EN_USB_HUB` | IOs (PCA9535 P0.3) | USB (USB2422 power enable) |
| `3V3_ADJ_EN` | IOs (PCA9535 P0.4) | Power (TPS74601 EN) |
| `USB_DF1_P/N` | USB (USB2422 DN1) | ESP32 (GPIO20/GPIO19 native USB) |
| `VADJ1_BUCK` / `VADJ2_BUCK` / `+15V_ANA` / `-15V_ANA` / `3V3_BUCK` / `5V_BUCK` / `3V3_ADJ` + IMON | Power rails + IOs (TPS16410 IMON pins) | ADC (AD74416H CH_D via U23 self-test MUX) |

---

## 8. I2C Bus Summary

All devices share one I²C bus. PCB GPIO assignment: SDA=GPIO42, SCL=GPIO41 (currently firmware uses breadboard mode: GPIO1/GPIO4).

| Device | Ref | 7-bit Address | Address Pin Config |
|--------|-----|--------------|-------------------|
| HUSB238 | U22 | `0x08` | Fixed (hardware-defined) |
| DS4424 | U8 | `0x10` | A0=GND, A1=GND |
| PCA9535AHF | U20 | `0x23` | A0=1, A1=1, A2=0 |

Pull-ups: R1 (5.1 kΩ) on SDA, R2 (5.1 kΩ) on SCL, both to 3V3_BUCK. PCB uses 400 kHz fast mode; breadboard mode uses 100 kHz.

---

## 9. SPI Bus Summary

Single SPI bus shared by AD74416H and ADGS2414D switch matrix. Separate chip selects.

| Signal | GPIO | Net | Notes |
|--------|------|-----|-------|
| MISO | GPIO8 | SPI_SDO | From AD74416H SDO |
| MOSI | GPIO9 | SPI_SDI | To AD74416H SDI |
| CS (AD74416H) | GPIO10 | SPI_CS_ADC | AD74416H SYNC*, active-low |
| SCLK | GPIO11 | SPI_SCLK | 10 MHz default, up to 20 MHz |
| CS (MUX) | GPIO21 (PCB) | SPI_CS_MUX | ADGS2414D daisy-chain, via TXS0108E level shifter |

The ADGS2414D switches are daisy-chained (4 devices on PCB). Their SPI signals pass through the TXS0108EPWR level translators (U13, U15), OE controlled by GPIO14 (`LVL_OE`). The AD74416H is on the 3.3 V side and does not need level shifting.

**Bus arbitration:** The ADC poll task (Core 1) continuously owns the SPI bus. MUX operations request the bus via atomic flags `g_spi_bus_request` / `g_spi_bus_granted` before performing any ADGS SPI transaction. Dead time between MUX switch transitions: 100 ms (`ADGS_DEAD_TIME_MS`).
