# Logic Analyzer HAT - RP2040 firmware

Firmware for the BugBuster Logic Analyzer HAT, built on a fork of
[raspberrypi/debugprobe](https://github.com/raspberrypi/debugprobe).

Version `bb-hat-5.0`.

The debugprobe core is unmodified: CMSIS-DAP v2, SWD over PIO 0, the CDC UART
bridge, and SWO. Everything under `bb_*` is the BugBuster extension - the HAT
UART command handler, power management, and the logic-analyzer engine on PIO 1.

## Build

Needs Pico SDK 2.0+ and `arm-none-eabi-gcc` (verified against
arm-gnu-toolchain 13.3.rel1).

```bash
git submodule update --init --recursive
mkdir build && cd build
cmake -DPICO_BOARD=bugbuster_hat ..
make -j
```

Produces `bugbuster_hat.uf2`. Flash by holding BOOTSEL and copying it to the
`RPI-RP2` volume, or:

```bash
picotool reboot -f -u && picotool load -x build/bugbuster_hat.uf2
```

After reflashing the RP2040, reset the ESP32 (DTR/RTS) so its `hat_init()` runs
again and re-detects the HAT.

The version lives in **two** places and CI only checks the first:
`CMakeLists.txt:PROBE_VERSION` (the USB descriptor string) and
`bb_main.c:BB_HAT_FW_MAJOR/MINOR` (what the HAT reports over the UART PING).
Bump both together.

## Modules

```
src/
  bb_main.c            FreeRTOS command task, UART dispatch, IRQ signalling
  bb_main_integrated.c debugprobe integration and task creation
  bb_config.h          Pin map, protocol constants, command IDs
  bb_protocol.c        HAT UART framing - 0xAA sync, CRC-8, frame timeout
  bb_power.c           Rail enables, ADC current sense, fault detection
  bb_pins.c            HAT IO bank routing
  bb_swd.c             SWD status and target detect (line reset + DPIDR read)
  bb_la.c              LA engine - PIO 1 capture, double-buffered DMA
  bb_la.pio            Capture programs (1 / 2 / 4 channel)
  bb_la_trigger.pio    Hardware trigger programs (rising, falling, high, low)
  bb_la_rle.c          Run-length encoding for capture data
  bb_la_usb.c          Vendor-bulk endpoint lifecycle and streaming
  bb_usb_descriptors.c USB descriptors - CMSIS-DAP + CDC + LA vendor interface
  bb_hat_v2.c          HAT v2 command surface (rails, LEDs, LA route, calibration)
  bb_fw_update.c       OTA image reception from the ESP32
  bb_ws2812.pio        WS2812B status LED driver
```

## Tasks

| Task | Core | Priority | Purpose |
|---|---|---|---|
| `bb_cmd_task` | 1 | tskIDLE+1 | HAT UART commands, 1 ms subsystem polling |
| `usb_thread` | 0 | tskIDLE+2 | TinyUSB event handling |
| `dap_task` | - | tskIDLE+1 | CMSIS-DAP processing (from debugprobe) |

The core affinity is deliberate. Two rules follow from it and must not be
broken:

1. **Never call TinyUSB endpoint functions from `bb_cmd_task`** - no
   `write_clear`, `read_flush`, `fifo_clear` or `abort_xfer`. TinyUSB is
   single-threaded. Set `s_need_endpoint_rearm` instead and let
   `bb_la_usb_send_pending()` drain it on the USB thread.
2. **Never call `bb_la_log()` from inside `bb_la_usb_send_pending()`** - it
   races Core 1's HAT UART and emits spurious `0x11 BBP_ERR_TIMEOUT`.

`tud_vendor_n_write_clear()` is reserved for genuine stuck-endpoint recovery
with a bounded timeout. The RP2040 SIE `abort_done` register hangs after roughly
ten calls with no pending transfer, so routine cleanup uses `soft_reset()`
instead.

## USB interfaces

| Interface | Class | Endpoints | Purpose |
|---|---|---|---|
| 0 | vendor bulk | `0x04` OUT / `0x85` IN | CMSIS-DAP v2 - OpenOCD, pyOCD, probe-rs |
| 1 | CDC-ACM | `0x81` notif, `0x02` OUT, `0x83` IN | Target UART bridge |
| 3 | vendor bulk | `0x06` OUT / `0x87` IN | Logic-analyzer data stream |

`tud_descriptor_configuration_cb()` rewrites the LA interface's
`bInterfaceSubClass`/`bInterfaceProtocol` to `0xFF/0xFF` at runtime. **Do not
remove this.** Without it the custom CMSIS-DAP driver claims the LA endpoints,
which breaks OpenOCD (`CMD_INFO failed`) and leaves `tud_vendor_n_mounted()`
false for the LA stream.

Sanity check with the probe attached:

```bash
pyocd list          # expect: BugBuster HAT (CMSIS-DAP + LA)
openocd -c "adapter driver cmsis-dap" \
        -c "cmsis_dap_vid_pid 0x2E8A 0x000C" \
        -c "adapter speed 1000" -c "transport select swd" \
        -c "init" -c "shutdown"
```

The VID/PID `0x2E8A:0x000C` is inherited from debugprobe, intentionally.

## Pin map

Authoritative source: [bb_config.h](src/bb_config.h). Board-level detail
including connector pinouts, rail topology and the LED scheme is in
[../../Docs/la-hat-hardware.md](../../Docs/la-hat-hardware.md).

| GPIO | Function |
|---|---|
| 0 / 1 | UART0 TX / RX - HAT bus to the ESP32, 921600 8N1 |
| 2–5 | LA channels 0–3 (PIO 1), low-speed route through the mainboard MUX |
| 6 / 7 | I²C1 SDA / SCL - DS4424 rail trim DAC |
| 8 | IRQ to the ESP32 - open drain, active low |
| 9 | WS2812B data - 8 status LEDs |
| 10–15 | Level-shifted HAT IO (Conn1 and Conn2) |
| 16 / 17 / 18 | SWDIO / TRACE-SWO / SWCLK - dedicated debug connector |
| 19 | Level-shifter output enable |
| 20 / 21 | Level-shifted HAT IO (Conn2) |
| 22 | Level-shifter direction |
| 23 / 24 / 25 | VADJ3 / 3V3_ADJ / VADJ4 enables |
| 26 / 27 | VADJ3 / VADJ4 current-monitor ADC (50 mΩ shunt) |
| 28 / 29 | VADJ3 / VADJ4 voltage-sense ADC (110k/10k divider, ×12) |

## Already implemented

Read the source before proposing work in these areas - they are done, benched,
and easy to regress:

- PIO hardware triggers and RLE compression (`bb_la.c`, `bb_la_rle.c`)
- Double-buffered DMA streaming and the LA-done IRQ (`bb_la.c`)
- Vendor-bulk endpoint lifecycle, rearm and STOP preflight (`bb_la_usb.c`)
- The USB descriptor subclass patch (`bb_usb_descriptors.c`)
- SWD teardown and cleanup (`bb_swd.c`)

## See also

- [../hat-uart-protocol.md](../hat-uart-protocol.md) - ESP32 ↔ HAT wire format
- [../la-hat-architecture.md](../la-hat-architecture.md) - subsystem architecture
- [../../Docs/logic-analyzer.md](../../Docs/logic-analyzer.md) - capture modes,
  routing and host-side streaming
