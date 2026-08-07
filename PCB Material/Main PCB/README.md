# BugBuster Main PCB

Four-channel software-configurable I/O instrument with analog signal conditioning, a 32-switch MUX routing matrix, dual adjustable power supplies, and USB-C PD input. Controlled via USB binary protocol or Wi-Fi REST API from the companion Tauri/Leptos desktop application.

![BugBuster Main PCB - Top](PCB%20Picture.png)

---

## Specifications

| Parameter | Value |
|-----------|-------|
| MCU | ESP32-S3-WROOM-1-N8R2 - Xtensa LX7, 240 MHz, 16 MB flash, 8 MB PSRAM |
| I/O Controller | AD74416HBCPZ - 4-ch software-configurable (24-bit ADC, 16-bit DAC, DIO, HART) |
| MUX Matrix | 5× ADGS2414D - octal SPST (32-switch main + 8-switch self-test/monitoring) |
| I2C Current DAC | DS4424+ - 4-ch, 7-bit (adjusts VADJ1/VADJ2 output rail voltages) |
| USB-C PD | HUSB238 - negotiates 5–20 V input; default 20 V |
| GPIO Expander | PCA9535AHF - 16-bit I2C (power enables, e-fuse control, fault monitoring) |
| Input Voltage | 5–20 V via USB-C PD |
| Logic / Digital Rail | 3.3 V and 5 V (LTM8078EY dual silent switcher) |
| Analog Rails | ±15 V (LTM8049IY), 3.3 V adjustable (TPS74601) |
| Adjustable Output Rails | 3–15 V × 2 (LTM8063EY × 2, voltage set via DS4424 IDAC injection) |
| Output Ports | P1–P4 - 3–15 V adjustable, 1.8 A max each (TPS16410 e-fuse protection) |
| Physical IOs | 12 total: 4 analog-capable + 8 digital; 2.54 mm screw terminals, 4 groups of 3 |
| Voltage Reference | ADR4525BRZ - 2.5 V ultralow-noise precision reference |
| Level Translation | 2× TXS0108EPWR - 8-bit bidirectional, 3.3 V ↔ 5 V |
| USB | 2× USB-C: device (ESP32 native, 2× CDC) + hub (USB2422 controller) |
| Wireless | Wi-Fi 802.11 b/g/n AP + STA, BLE 5.0 (ESP32-S3) |
| Communication | BBP binary protocol (COBS-framed, CRC-16) over USB CDC; HTTP REST over Wi-Fi |
| Status LEDs | 3× WS2812B RGB 2020 (connection, MUX/expander, AD74416H health) |
| IO Status LEDs | 3× HSMF-C165 dual-color green/red (AD74416H GPIO_A–F) |
| Layer Count | 6 |
| CAD Tool | Altium Designer |

---

## Key ICs

| Reference | Part | Function |
|-----------|------|----------|
| U1 | ESP32-S3-WROOM-1-N8R2 | Main MCU - firmware, USB, Wi-Fi |
| U9 | AD74416HBCPZ | 4-ch software-configurable I/O (SPI @ 10 MHz) |
| U10, U11, U16, U17 | ADGS2414DBCCZ | MUX switch matrix - 4× octal SPST |
| U23 | ADGS2414DBCCZ | Self-test / monitoring switch (5th daisy-chain device) |
| U3 | LTM8078EY#PBF | Dual 40 V silent switcher → 5 V + 3.3 V |
| U4, U6 | LTM8063EY#PBF | Adjustable buck 3–15 V (VADJ1, VADJ2) |
| U7 | LTM8049IY | Dual ±15 V DC-DC for analog supply |
| U5 | TPS74601PDRVT | Adjustable 1 A LDO → 3.3 V_ADJ for AD74416H |
| U8 | DS4424+ | 4-ch I2C current DAC (supply voltage adjustment) |
| U22 | HUSB238 | USB-C PD sink controller (I2C 0x08) |
| U20 | PCA9535AHF | 16-bit I2C GPIO expander (I2C 0x23) |
| U21 | ADR4525BRZ | 2.5 V precision voltage reference |
| U2 | USB2422T/MJ | USB 2.0 hub controller + 24 MHz crystal |
| U13, U15 | TXS0108EPWR | 8-bit bidirectional level translator |
| U12, U14, U18, U19 | TPS16410DRCR | E-fuse 2.7–40 V / 1.8 A (one per output port) |
| Q1 | AONR21307 | P-ch MOSFET - VBUS to 20 V_USB power switch |
| Q2–Q5 | SI7113ADN-T1-GE3 | P-ch source-driver MOSFETs (100 V, per channel) |

---

## Schematic Sheets

| Sheet | File | Contents |
|-------|------|----------|
| 1 | `ESP32.SchDoc` | MCU, reset/boot circuits, GPIO assignments, WS2812B LEDs |
| 2 | `USB.SchDoc` | USB-C connectors J1/J2, USB2422 hub, HUSB238 PD sink, ESD diode |
| 3 | `Power.SchDoc` | LTM8078/8063/8049, TPS74601 LDO, DS4424 IDAC, ADR4525 reference |
| 4 | `Analog.SchDoc` | AD74416HBCPZ, per-channel frontend (source driver, sense resistors, compensation) |
| 5 | `IOs.SchDoc` | PCA9535, TXS0108E level shifters, ADGS2414D MUX matrix, screw terminal connectors |

---

## IO Architecture

```
BLOCK 1 - VADJ1 (3–15 V)          BLOCK 2 - VADJ2 (3–15 V)
┌─────────────────────────────┐    ┌─────────────────────────────┐
│ IO_Block 1 (EFUSE1)         │    │ IO_Block 3 (EFUSE3)         │
│   IO 1 - analog + HAT       │    │   IO 7 - analog + HAT       │
│   IO 2 - digital            │    │   IO 8 - digital            │
│   IO 3 - digital            │    │   IO 9 - digital            │
├─────────────────────────────┤    ├─────────────────────────────┤
│ IO_Block 2 (EFUSE2)         │    │ IO_Block 4 (EFUSE4)         │
│   IO 4 - analog + HAT       │    │   IO 10 - analog + HAT      │
│   IO 5 - digital            │    │   IO 11 - digital           │
│   IO 6 - digital            │    │   IO 12 - digital           │
└─────────────────────────────┘    └─────────────────────────────┘
```

Each IO_Block connects to one ADGS2414D MUX device. The analog-capable IO (3rd position per block) can be switched between: ESP32 GPIO, AD74416H channel, or HAT passthrough.
