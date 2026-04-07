# BugBuster HAT — RP2040 Firmware

Firmware for the BugBuster HAT expansion board, based on a fork of
[raspberrypi/debugprobe](https://github.com/raspberrypi/debugprobe).

## Architecture

- **debugprobe core** (unmodified): CMSIS-DAP v2, SWD via PIO 0, CDC UART bridge, SWO
- **BugBuster extensions** (this code): UART command handler, power management, HVPAK, logic analyzer

### Module Structure

```
src/
├── bb_main.c              — FreeRTOS command task, UART dispatcher, IRQ signaling
├── bb_main_integrated.c   — debugprobe integration (FreeRTOS task creation)
├── bb_config.h            — Pin definitions, protocol constants, command IDs
├── bb_protocol.c/h        — HAT UART framing (CRC-8, sync byte 0xAA, frame timeout)
├── bb_power.c/h           — Connector power enable/disable, ADC current sense, fault detection
├── bb_hvpak.c/h           — HVPAK I2C level translator voltage control (STUB — see Known Limitations)
├── bb_pins.c/h            — EXP_EXT pin routing (SWDIO/SWCLK/GPIO/TRACE)
├── bb_swd.c/h             — SWD status queries (target detect is STUB)
├── bb_la.c/h              — Logic analyzer engine: PIO 1 capture, DMA with IRQ completion
├── bb_la.pio              — PIO capture programs (1ch, 2ch, 4ch)
├── bb_la_trigger.pio      — PIO trigger programs (rising, falling, high, low)
├── bb_la_rle.c/h          — Run-length encoding compression for LA data
├── bb_la_usb.c/h          — LA USB transport helpers (CDC streaming + vendor bulk readout)
└── bb_usb_descriptors.c   — USB descriptors (CMSIS-DAP + CDC + LA vendor interface)
```

### Task/Thread Model (FreeRTOS)

| Task | Priority | Purpose |
|------|----------|---------|
| `bb_cmd_task` | tskIDLE+1 | UART command processing, subsystem polling (1ms) |
| `usb_thread` | tskIDLE+2 | TinyUSB event handler |
| `dap_task` | tskIDLE+1 | CMSIS-DAP command processing (debugprobe) |

### Communication Flows

- **ESP32 ↔ RP2040**: UART0 @ 921600 baud, HAT protocol framing (0xAA sync, CRC-8)
- **USB CMSIS-DAP**: HID interface (EP 0x04/0x85) — inherited from debugprobe
- **USB CDC**: UART bridge (EP 0x81/0x02/0x83)
- **USB LA Streaming**: CDC serial port (start/stop control + raw sample stream)
- **USB LA Bulk Readout**: Vendor bulk interface (EP 0x06/0x87) for completed captures

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
| GPIO8 | IRQ (shared with BugBuster) | Open-drain (active low, 2ms pulse) |
| GPIO9 | LED_STATUS | Output |
| GPIO10-13 | EXP_EXT_1-4 (to HVPAK) | Configurable |
| GPIO14-17 | LA capture inputs | Input (PIO 1) |
| GPIO20 | FAULT_A (overcurrent) | Input (active low) |
| GPIO21 | FAULT_B (overcurrent) | Input (active low) |
| GPIO25 | LED_ACTIVITY (onboard) | Output |
| GPIO26 | ADC0 — Current sense A | Analog input |
| GPIO27 | ADC1 — Current sense B | Analog input |

*Pin assignments are preliminary and will be finalized with HAT PCB layout.*

## Known Limitations

1. **HVPAK driver is a stub** — I2C address (0x48) and register map are placeholders. `SET_IO_VOLTAGE` commands will silently fail. Requires actual HVPAK datasheet to implement.
2. **SWD target detection always returns false** — `bb_swd_detect_target()` is not yet hooked into debugprobe's low-level SWD API.
3. **Power fault pin polarity assumed active-low** — Needs confirmation from HAT schematic.
4. **ADC current sense has no calibration** — Readings may be 5-10% off without offset/gain compensation.
5. **GPIO8 IRQ signaling** — Now implemented (power fault + LA done events trigger a 2ms low pulse).

## Logic Analyzer

- **Channels**: 1, 2, or 4 (GPIO14-17)
- **Sample rate**: Up to system clock (125 MHz) via PIO clock divider
- **Buffer**: 200 KB SRAM (~51K 32-bit words)
- **Modes**: Raw (DMA with completion IRQ) or RLE-compressed (PIO FIFO polling)
- **Trigger**: Rising/falling/both-edge, level high/low on any channel (PIO-to-PIO hardware trigger)
- **DMA**: Dynamically claimed channel with interrupt-driven completion detection
