# BugBuster HAT

Extension board for the BugBuster Main PCB. Provides two independently adjustable output channels (buck-boost regulated) controlled by an RP2040 microcontroller over the HAT UART interface. Detected automatically at boot via the HAT_DETECT strapping pin on the main board.

![BugBuster HAT PCB - Top](PCB%20Picture.png)

---

## Specifications

| Parameter | Value |
|-----------|-------|
| MCU | RP2040 - dual-core ARM Cortex-M0+, 133 MHz, 40-pin QFN |
| Flash | W25Q16JVSNIQ - 16 Mbit SPI/QUAD |
| Crystal | ABM8 - 12 MHz, 20 pF load |
| Output Channels | 2× adjustable buck-boost (LTM8083IY - 36 V input, 1.5 A output each) |
| Voltage Control | DS4424+ - 4-ch I2C current DAC (injects into LTM8083 FB nodes) |
| Output Rails (LDO) | 2× TPS74601PDRVT - adjustable 1 A LDO (logic/auxiliary supplies) |
| Output Connectors | 2× 5-position 2.54 mm screw terminal (P1, P2, Wurth 691382010005) |
| HAT Interface | J2, J3, J5 - dual-row 2.54 mm pin headers (4-pin each) |
| Aux Connector | J4 - 10-position right-angle 2.54 mm header |
| USB | USB-C (USB4085-GF-A, 24-pin) - programming and/or power input |
| Level Translation | TXS0108W-Q1 - 8-bit bidirectional (RP2040 ↔ main board logic) |
| Bus Transceiver | SN74LVC8T245PWR - 24-bit bidirectional (data bus) |
| Host Interface | UART 921600 baud 8N1 (HAT_TX / HAT_RX via ESP32-S3 GPIO43/44) |
| Interrupt | HAT_IRQ - open-drain from RP2040 GPIO8, received on ESP32-S3 GPIO15 |
| Detection | HAT_DETECT strap - LOW = HAT present (ESP32-S3 GPIO47) |
| Status LEDs | 7× WS2812B RGB 2020 (D2–D9) |
| ESD Protection | TPD2E1B06DRLR on USB differential pair |
| Shunt Resistors | 2× 50 mΩ (R1, R2, 1206) - current sensing per output channel |
| CAD Tool | Altium Designer |

---

## Key ICs

| Reference | Part | Function |
|-----------|------|----------|
| U7 | RP2040 | MCU - control logic, UART interface, GPIO |
| U6 | W25Q16JVSNIQ | 16 Mbit SPI flash for RP2040 firmware |
| U1, U2 | LTM8083IY#PBF | 36 V / 1.5 A buck-boost µModule (adjustable output) |
| U4 | DS4424+ | 4-ch I2C current DAC (output voltage trim) |
| U3, U5 | TPS74601PDRVT | Adjustable 1 A LDO (auxiliary rails) |
| U8 | TXS0108W-Q1 | 8-bit bidirectional level translator |
| U9 | SN74LVC8T245PWR | 24-bit bidirectional bus transceiver |

---

## Schematic Sheets

| Sheet | File | Contents |
|-------|------|----------|
| 1 | `RP2040.SchDoc` | MCU, crystal, flash, reset/boot, USB |
| 2 | `IOs.SchDoc` | HAT interface connectors, level translation, bus transceiver |
| 3 | `Power.SchDoc` | LTM8083 buck-boost converters, DS4424 IDAC, TPS74601 LDOs |
