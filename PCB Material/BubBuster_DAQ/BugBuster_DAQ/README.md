# BugBuster DAQ

High-performance standalone data acquisition board based on the ESP32-P4 dual-core RISC-V module. Provides three 24-bit, 1 MSPS alias-free ADC channels, high-voltage analog MUX switching, precision current sensing, and a local LCD display with navigation controls for fully autonomous operation.

![BugBuster DAQ — Top](PCB%20Picture.png)

![BugBuster DAQ — Bottom](PCB%20Picture%20Back.png)

---

## Specifications

| Parameter | Value |
|-----------|-------|
| MCU | ESP32-P4 — dual-core 32-bit RISC-V, up to 400 MHz, Wi-Fi via external antenna |
| RF Antenna | PRO-OB-440 — 2.4 GHz stamped metal, surface mount |
| ADC | 3× ADAQ7769-1BBCZ — 24-bit, 1 MSPS, alias-free, programmable gain µModule |
| High-Voltage MUX | 2× ADG5204BCPZ — high-voltage latch-up proof 4-ch analog multiplexer |
| Precision Switch | 2× ADG6412BCPZ — 0.5 mΩ RON, ±20 V, quad SPST analog switch |
| Current Sensing | 3× AD8411AWBRMZ — current-sense amplifier, gain 50 V/V, BW 2.7 MHz, input 2–70 V |
| Comparators | 4× ADCMP600BKSZ — rail-to-rail, very fast, 2.5–5.5 V |
| Op-Amp | ADA4051-1AKSZ — rail-to-rail, 125 kHz BW, low-offset |
| Voltage References | ADR4540CRZ (4 V, 0.02% accuracy) + LT6656BCS6-5 (5 V, 0.1%) |
| Clock | SIT8208AI — 25 MHz MEMS oscillator, LVCMOS, low-jitter |
| Clock Distribution | CDCLVC1104PWR — 1:4 buffer, up to 250 MHz |
| I2C Current DAC | DS4424+ — 4-ch, 7-bit (supply voltage trim) |
| Temperature Sensors | 2× AD7415ARTZ-0500RL7 — ±0.5°C accuracy, 10-bit, I2C |
| Comparator Logic | SN74HCS02QPWRQ1 — quad 2-input NOR gate |
| Display | LCD via FPC — J4 (8-circuit, 0.5 mm pitch ZIF, right-angle SMT) |
| User Controls | 5× tactile switches: BOOT, RST, UP, DOWN, OK |
| Status LEDs | 8× WS2812B RGB 2020 (D4–D11) |
| USB | USB-C (USB4085-GF-A, 24-pin) |
| Output Connector | P1 — 4-position 2.54 mm screw terminal (Wurth 691382010004) |
| Main Supply | LTM8078EY#PBF — dual 40 V silent switcher (5 V + 3.3 V) |
| Boost/SEPIC | ADP5071ACPZ — adjustable dual output DC-DC |
| Positive LDOs | 2× ADP7142ACPZN — 200 mA, adjustable |
| Negative LDOs | 2× ADP7182ACPZN — 200 mA, adjustable negative output |
| Aux LDO | TPS74601PDRVT — adjustable 1 A LDO |
| Aux Converter | LTM8056MPY — 1.2–48 V adjustable DC-DC |
| CAD Tool | Altium Designer |

---

## Key ICs

| Reference | Part | Function |
|-----------|------|----------|
| U30 | ESP32-P4 | Main MCU — RISC-V, Wi-Fi, control |
| U1, U22, U23 | ADAQ7769-1BBCZ | 24-bit 1 MSPS ADC µModule (3 channels) |
| U24, U25 | ADG5204BCPZ | High-voltage 4-ch analog multiplexer |
| U9, U13 | ADG6412BCPZ | 0.5 mΩ quad SPST analog switch (±20 V) |
| U10, U14, U16 | AD8411AWBRMZ | Current sense amplifier (50 V/V gain) |
| U8, U11, U15, U17 | ADCMP600BKSZ | Rail-to-rail fast comparator |
| U19 | ADA4051-1AKSZ | Rail-to-rail op-amp |
| U18 | ADR4540CRZ | 4 V precision voltage reference (0.02%) |
| U21 | LT6656BCS6-5 | 5 V precision voltage reference (0.1%) |
| Y1 | SIT8208AI | 25 MHz MEMS oscillator |
| U20 | CDCLVC1104PWR | 1:4 clock buffer (250 MHz) |
| U4 | LTM8078EY#PBF | Dual 40 V silent switcher (5 V + 3.3 V) |
| U5 | ADP5071ACPZ | Adjustable boost/SEPIC DC-DC |
| U6, U29 | ADP7142ACPZN | 200 mA positive adjustable LDO |
| U7, U31 | ADP7182ACPZN | 200 mA negative adjustable LDO |
| U26 | DS4424+ | 4-ch I2C current DAC |
| U2, U28 | AD7415ARTZ-0500 | ±0.5°C digital temperature sensor (I2C) |
| U27 | LTM8056MPY | 1.2–48 V adjustable DC-DC |
| U3 | TPS74601PDRVT | Adjustable 1 A LDO |

---

## Schematic Sheets

| Sheet | File | Contents |
|-------|------|----------|
| 1 | `ESP32P4.SchDoc` | MCU, antenna, boot/reset, USB, oscillator |
| 2 | `DAQ.SchDoc` | ADAQ7769 ADCs, ADG5204/6412 MUX, AD8411 current sensing, ADCMP600 comparators |
| 3 | `IOs.SchDoc` | Output connectors, navigation switches, WS2812B LEDs |
| 4 | `LCD.SchDoc` | Display interface — FPC connector J4, signal conditioning |
| 5 | `Power.SchDoc` | LTM8078, ADP5071, ADP7142/7182, DS4424, voltage references |
