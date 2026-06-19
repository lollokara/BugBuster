#pragma once

// DAQ HAT (ESP32-C6) shared configuration
// The C6 is the wireless / display co-processor on the DAQ HAT. Its sole job
// is to drive the on-board ST7789 status display and render measurement data
// pushed to it by the ESP32-P4 over an internal UART link.
//
// Adjust pin assignments to match the final PCB rev.

// ---------------------------------------------------------------------------
// UART link to the ESP32-P4 application processor
// ---------------------------------------------------------------------------
#define DAQ_UART_TX_PIN   21
#define DAQ_UART_RX_PIN   20
#define DAQ_UART_BAUD     921600
#define DAQ_UART_PORT     1      // UART1 (UART0 = USB-serial console)

// ---------------------------------------------------------------------------
// ST7789 display (SPI 4-wire). The silkscreen labels the data lines SCL/SDA
// but the panel is driven as SPI (SCL = SCLK, SDA = MOSI), NOT I2C.
//
//   Panel pin   Net      ESP32-C6 GPIO
//   ---------   ------   -------------
//   SCL         SCLK     IO4
//   SDA         MOSI     IO3
//   RST         RESET    IO2
//   DC          D/CX     IO1
//   CS          CS       IO0
//   BL          BACKLT   IO10
// ---------------------------------------------------------------------------
#define DISP_PIN_SCLK     4
#define DISP_PIN_MOSI     3
#define DISP_PIN_RST      2
#define DISP_PIN_DC       1
#define DISP_PIN_CS       0
#define DISP_PIN_BL       10

// ST7789 panel geometry for the ER-TFTM2.25-1 (240x320 controller windowed
// down to a 76x284 visible glass). The controller RAM is offset, so the
// visible area starts at (col,row) = (0x52, 0x12) in the panel's NATIVE
// (portrait) frame.
#define DISP_NATIVE_W     76
#define DISP_NATIVE_H     284
#define DISP_COL_OFFSET   0x52
#define DISP_ROW_OFFSET   0x12

// The UI is drawn in LANDSCAPE. We rotate in hardware (MADCTL swap/mirror) so
// there is no per-pixel rotation cost. The framebuffer and all UI code use
// these landscape logical dimensions.
#define DISP_WIDTH        284          // landscape width  (along native rows)
#define DISP_HEIGHT       76           // landscape height (along native cols)
#define DISP_SWAP_XY      true
#define DISP_MIRROR_X     false
#define DISP_MIRROR_Y     true
// Gaps in the swapped (landscape) frame: X runs along the native row axis,
// Y along the native column axis. Flip these if the image is shifted on the
// real glass.
#define DISP_GAP_X        DISP_ROW_OFFSET
#define DISP_GAP_Y        DISP_COL_OFFSET

// Backlight (IO10) is ACTIVE LOW: the panel LED is on when the pin is driven
// LOW. display_set_backlight() inverts internally, so level 255 == full
// brightness (pin held low).
#define DISP_BL_ACTIVE_LOW  1

// Panel color inversion. This glass shows TRUE colors with INVON OFF — with it
// ON a black framebuffer rendered as white. Set to 1 only if colors come out
// inverted on a different panel batch.
#define DISP_INVERT  0

// SPI clock for the panel. ST7789 tolerates ~40-62 MHz; stay conservative
// for the flex cable.
#define DISP_SPI_HZ       (40 * 1000 * 1000)
#define DISP_SPI_HOST     1     // SPI2_HOST

// ---------------------------------------------------------------------------
// Front-panel navigation buttons (active-low, internal pull-ups), wired to the
// ESP32-C6. Avoided strapping pins (GPIO8/9/15) and the display/UART pins.
//   UP   -> IO5
//   DOWN -> IO6
//   OK   -> IO7   (hold = Back)
// ---------------------------------------------------------------------------
#define BTN_PIN_UP        5
#define BTN_PIN_DOWN      6
#define BTN_PIN_OK        7

// ---------------------------------------------------------------------------
// Debug: per-phase frame profiler. Set to 1 to log how long each rendering
// phase (bg/header/cards/flush) takes so we can target optimizations. Logs
// averages every DISP_PERF_PERIOD frames. Set to 0 for production.
// ---------------------------------------------------------------------------
#define DISP_PERF_LOG     1
#define DISP_PERF_PERIOD  60
