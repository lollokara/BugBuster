# BugBuster HAT — RP2040 Firmware

Firmware for the BugBuster HAT expansion board, based on a fork of
[raspberrypi/debugprobe](https://github.com/raspberrypi/debugprobe).

## Architecture

- **debugprobe core** (unmodified): CMSIS-DAP v2, SWD via PIO 0, CDC UART bridge, SWO
- **BugBuster extensions** (this code): UART command handler, power management, HVPAK, logic analyzer

## Build

Requires Pico SDK 2.0+ and arm-none-eabi-gcc.

```bash
git submodule update --init --recursive
mkdir build && cd build
cmake -DPICO_BOARD=bugbuster_hat ..
make -j$(nproc)
```

## Pin Assignments

| Pin | Function | Direction |
|-----|----------|-----------|
| GPIO0 | UART0 TX (to BugBuster) | Output |
| GPIO1 | UART0 RX (from BugBuster) | Input |
| GPIO2 | SWCLK (to target via HVPAK) | Output |
| GPIO3 | SWDIO (to target via HVPAK) | Bidirectional |
| GPIO4 | EN_A (connector A power) | Output |
| GPIO5 | EN_B (connector B power) | Output |
| GPIO6 | HVPAK_SDA (I2C to HVPAK) | Bidirectional |
| GPIO7 | HVPAK_SCL (I2C to HVPAK) | Output |
| GPIO8 | IRQ (shared with BugBuster) | Open-drain |
| GPIO9 | LED_STATUS | Output |
| GPIO10-13 | EXP_EXT_1-4 (to HVPAK) | Configurable |
| GPIO14-17 | LA capture inputs | Input (PIO 1) |
| GPIO25 | LED_ACTIVITY (onboard) | Output |

*Pin assignments are preliminary and will be finalized with HAT PCB layout.*
