# RP2040 HAT PCB Reference

**Status:** source-of-truth notes from the arrived HAT PCB schematic.
**Created:** 2026-05-28.
**Use with:** `Firmware/RP2040/src/bb_config.h`, `Firmware/ESP32/src/hat/hat.h`, `Firmware/HAT_Protocol.md`, and `.omx/plans/prd-rp2040-hat-pcb-bringup-refactor.md`.

This document supersedes older preliminary HAT pin-map comments for the new RP2040 HAT PCB. It is a reference for firmware, host API, UI, and test work; it is not an implementation record.

## RP2040 Pin Map

| RP2040 GPIO | Net / function | Notes |
|---:|---|---|
| 0 | ESPRX / HAT COM TX | UART0 HAT bus from RP2040 to ESP32 receive. Existing `BB_UART_TX_PIN` already uses GPIO0. |
| 1 | ESPTX / HAT COM RX | UART0 HAT bus from ESP32 transmit to RP2040. Existing `BB_UART_RX_PIN` already uses GPIO1. |
| 2 | LA CH1 low-speed route | Routed to ESP32 board EXT channel through the 4 muxes. This replaces the older SWCLK assignment. |
| 3 | LA CH2 low-speed route | Routed to ESP32 board EXT channel through the 4 muxes. This replaces the older SWDIO assignment. |
| 4 | LA CH3 low-speed route | Routed to ESP32 board EXT channel through the 4 muxes. This replaces older connector power enable use. |
| 5 | LA CH4 low-speed route | Routed to ESP32 board EXT channel through the 4 muxes. This replaces older connector power enable use. |
| 6 | I2C SDA | Shared HAT I2C bus. |
| 7 | I2C SCL | Shared HAT I2C bus. |
| 8 | HAT INT | Active-low interrupt to ESP32. Also use for status/event notification; do not send unsolicited UART frames. |
| 9 | WS2812B data | Drives 8 addressable connector/status LEDs. |
| 10 | General IO | Broken out on Conn1 pin 2 through high-speed level shifter. |
| 11 | General IO | Broken out on Conn1 pin 4 through high-speed level shifter. |
| 12 | General IO | Broken out on Conn1 pin 3 through high-speed level shifter. |
| 13 | General IO | Broken out on Conn2 pin 5 through high-speed level shifter. |
| 14 | General IO | Broken out on Conn2 pin 4 through high-speed level shifter. |
| 15 | General IO | Broken out on Conn2 pin 3 through high-speed level shifter. |
| 16 | SWDIO | Dedicated debug connector pin 3. |
| 17 | TRACE / SWO | Dedicated debug connector pin 4. |
| 18 | SWCLK | Dedicated debug connector pin 2. |
| 19 | Level-shifter OE | Output enable for both HAT level shifters. Required for any shifted external IO. |
| 20 | General IO | Broken out on Conn2 pin 2 through high-speed level shifter. |
| 21 | General IO | Broken out on Conn2 pin 1 through high-speed level shifter. |
| 22 | Level-shifter DIR | Direction pin for the 8 general-purpose outputs: GPIO10-GPIO15 plus GPIO20-GPIO21. Needed for high-speed LA routing mode. |
| 23 | VADJ3 enable | Enables the adjustable supply for Conn1 / HAT IO bank. |
| 24 | 3V3_ADJ enable | Enables adjustable 3.3 V logic-compartment supply. Required before shifted IO can work. |
| 25 | VADJ4 enable | Enables the adjustable supply for SWD connector / second HAT supply bank. |
| 26 | VADJ3 current ADC | LTM8083 output current monitor for VADJ3. Current sense resistor is 50 milliohm. |
| 27 | VADJ4 current ADC | LTM8083 output current monitor for VADJ4. Current sense resistor is 50 milliohm. |
| 28 | VADJ3 voltage sense ADC | 110k top / 10k bottom divider. Ratio: 12:1, so `Vrail = Vadc * 12`. |
| 29 | VADJ4 voltage sense ADC | 110k top / 10k bottom divider. Ratio: 12:1, so `Vrail = Vadc * 12`. |

## Connectors

### SWD Connector

| Pin | Signal |
|---:|---|
| 1 | VADJ4 |
| 2 | SWCLK |
| 3 | SWDIO |
| 4 | TRACE / SWO |
| 5 | GND |

### Extra IO Connector 1

| Pin | Signal |
|---:|---|
| 1 | VADJ3 |
| 2 | IO10 |
| 3 | IO12 |
| 4 | IO11 |
| 5 | GND |

### Extra IO Connector 2

| Pin | Signal |
|---:|---|
| 1 | IO21 |
| 2 | IO20 |
| 3 | IO15 |
| 4 | IO14 |
| 5 | IO13 |

## Supplies And Measurement

The HAT uses a DS4424 on the HAT I2C bus. Only three DS4424 outputs are used:

| DS4424 output | Rail |
|---:|---|
| OUT0 | 3V3_ADJ |
| OUT1 | VADJ3 |
| OUT2 | VADJ4 |
| OUT3 | unused |

Calibration is required for VADJ3 and VADJ4. For 3V3_ADJ, the HAT has no local readback path, so firmware should request or import the calibration data from the ESP32/mainboard 3.3 V calibration because the circuit is the same.

Implementation guidance: the ESP32 firmware already has the canonical DS4424 driver in `Firmware/ESP32/src/hal/ds4424.cpp`. Use it as the reference for the signed source/sink register format, full-scale current assumptions, voltage-to-code behavior, calibration point interpolation, and save/load semantics. The HAT firmware should adapt only the RP2040 I2C and persistence layers where necessary.

Voltage sense:

```text
VADJ3_V = ADC_GPIO28_V * ((110k + 10k) / 10k) = ADC_GPIO28_V * 12
VADJ4_V = ADC_GPIO29_V * ((110k + 10k) / 10k) = ADC_GPIO29_V * 12
```

LTM8083 current monitor:

```text
VISMON = 10 * V(ISP - ISN) + 250 mV
I_LOAD = (VISMON - 0.250 V) / (10 * RCS)
RCS = 0.050 ohm
I_LOAD = (VISMON - 0.250 V) / 0.5
```

Clamp negative computed current to zero unless a signed diagnostic mode is explicitly added.

## Level Shifter Rules

- All external IOs except onboard/internal-only nets are level shifted.
- 3V3_ADJ must be enabled and settled before enabling shifted IO.
- GPIO19 enables both level shifters.
- GPIO22 selects direction for the eight high-speed shifted IOs: GPIO10-GPIO15 and GPIO20-GPIO21.
- Firmware should fail safe: shifted outputs disabled, DIR safe/default, supplies off, WS2812 LEDs red/off if initialization fails.

## Logic Analyzer Routing

Two LA routes are required:

| Route | Pins | Intended use |
|---|---|---|
| Low-speed route | GPIO2-GPIO5 | Four channels, routed to ESP32 EXT channel through the 4 muxes. |
| High-speed route | GPIO10-GPIO15 / GPIO20-GPIO21 bank | Connector route through the high-speed level shifter, using GPIO22 DIR. Conn1 exposes three useful high-speed channels. |

The desktop app must let the user choose between low-speed mux route and high-speed Conn1 route. The high-speed route has only three Conn1 channels.

## WS2812B LED Order And Meanings

GPIO9 drives eight WS2812B LEDs placed near output connectors on the HAT and main PCB.

| LED | Meaning | Status colors |
|---:|---|---|
| 1 | RP2040 status | Green: good. Red: error. |
| 2 | Conn2 | Green: IOs mapped/configured. Off: no IO active. |
| 3 | Conn1 | Green: configured and supply active. Blue: supply active, no IO configured. Purple: IO active, no supply. Off: unconfigured. |
| 4 | Mainboard IOBLOCK 1 | Same connector status schema as LED 3. |
| 5 | Mainboard IOBLOCK 2 | Same connector status schema as LED 3. |
| 6 | Mainboard IOBLOCK 3 | Same connector status schema as LED 3. |
| 7 | Mainboard IOBLOCK 4 | Same connector status schema as LED 3. |
| 8 | SWD connector | Same connector status schema as LED 3, keyed to VADJ4 and SWD route/configuration. |

Boot animation requirement:

1. Fade each LED on and off from LED 1 to LED 8 like a wave.
2. Pulse all LEDs green.
3. Enter normal status mode.

The exact runtime LED schema can be refined later, but firmware should expose HAT-bus messages that let the ESP32/mainboard update connector status without hardcoding all policy on the RP2040.

## Existing Code That Is Currently Stale For This PCB

- `Firmware/RP2040/src/bb_config.h` still maps SWD to GPIO2/3/29, connector power to GPIO4/5, LA to GPIO14-GPIO17, and current sense as 100 milliohm shunts.
- `Firmware/RP2040/src/bb_power.c` currently models only two connector enables/currents and averages four ADC samples.
- `Firmware/ESP32/src/hat/hat.h` mirrors older HAT function slots and two-connector power state.
- `DesktopApp/BugBuster/src/tabs/hat.rs` presents four EXP_EXT functions and old connector A/B power controls.
- `DesktopApp/BugBuster/src/tabs/la.rs` has LA channel controls but no route selector.
- `DesktopApp/BugBuster/src/tabs/calibration.rs` calibrates mainboard DS4424 rails only and is not gated by HAT detection.

## Open Decisions

- Final HAT UART command IDs for LED status, HAT rail set/get/status, HAT rail calibration, and LA route selection.
- Exact LED policy for transient states: calibration running, overcurrent, SWD target attached, and LA capture active.
- Whether high-speed DIR is a global output/input mode or should be tracked per connector workflow with hard interlocks.
- How ESP32 exports its 3V3_ADJ calibration data to the HAT: one-shot HAT command, shared BBP command, or host-mediated calibration import.
