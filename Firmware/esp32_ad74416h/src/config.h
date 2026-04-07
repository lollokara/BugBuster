#pragma once

// =============================================================================
// config.h - Pin definitions and system constants for AD74416H + ESP32-S3
//            ESP-IDF native (no Arduino dependency)
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// -----------------------------------------------------------------------------
// SPI Pin Definitions (AD74416H uses SYNC as active-low chip select)
// -----------------------------------------------------------------------------
#define PIN_SDO         GPIO_NUM_8    // MISO - Serial Data Out (from AD74416H)
#define PIN_SDI         GPIO_NUM_9    // MOSI - Serial Data In  (to AD74416H)
#define PIN_SYNC        GPIO_NUM_10   // CS   - Active low frame sync (chip select)
#define PIN_SCLK        GPIO_NUM_11   // SCLK - SPI clock

// -----------------------------------------------------------------------------
// Control / Status Pin Definitions
// -----------------------------------------------------------------------------
#define PIN_RESET       GPIO_NUM_5    // Active low hardware reset
#define PIN_ADC_RDY     GPIO_NUM_6    // Open-drain, active low - ADC conversion ready
#define PIN_ALERT       GPIO_NUM_7    // Open-drain, active low - fault/alert output

// -----------------------------------------------------------------------------
// BREADBOARD_MODE: Set to 1 for breadboard testing, 0 for final PCB
// Changes: I2C pins, MUX CS pin, device count, I2C speed
// -----------------------------------------------------------------------------
#ifndef BREADBOARD_MODE
#define BREADBOARD_MODE  1
#endif

// -----------------------------------------------------------------------------
// I2C Bus Pins (shared bus: DS4424, HUSB238, PCA9535)
// -----------------------------------------------------------------------------
#if BREADBOARD_MODE
#define PIN_I2C_SDA     GPIO_NUM_1    // Breadboard: GPIO1
#define PIN_I2C_SCL     GPIO_NUM_4    // Breadboard: GPIO4
#define I2C_FREQ_HZ     100000        // 100 kHz (breadboard safe)
#else
#define PIN_I2C_SDA     GPIO_NUM_42   // PCB: ESP_SDA
#define PIN_I2C_SCL     GPIO_NUM_41   // PCB: ESP_SCL
#define I2C_FREQ_HZ     400000        // 400 kHz Fast Mode
#endif
#define I2C_PORT_NUM    I2C_NUM_0

// I2C Device Addresses (7-bit)
#define DS4424_I2C_ADDR     0x10      // A0=GND, A1=GND (7-bit: 0x10, 8-bit: 0x20)
#define HUSB238_I2C_ADDR    0x08      // Fixed
#define PCA9535_I2C_ADDR    0x23      // A2=0, A1=1, A0=1

// PCA9535 Interrupt Pin
#if BREADBOARD_MODE
#define PIN_MUX_INT     GPIO_NUM_NC   // Not connected on breadboard (conflicts with I2C_SCL on GPIO4)
#else
#define PIN_MUX_INT     GPIO_NUM_4    // PCB: PCA9535 INT output → ESP32
#endif

// -----------------------------------------------------------------------------
// ADGS2414D Mux Switch Matrix
// -----------------------------------------------------------------------------
#if BREADBOARD_MODE
#define PIN_MUX_CS          GPIO_NUM_12   // Breadboard: GPIO12
#define ADGS_NUM_DEVICES    1             // Single device on breadboard
#define ADGS_MAIN_DEVICES   1             // Main MUX devices only
#define ADGS_HAS_SELFTEST   0             // No U23 on breadboard
#else
#define PIN_MUX_CS          GPIO_NUM_21   // PCB: SPI_CS_MUX (shared for all 5 devices)
#define ADGS_NUM_DEVICES    5             // 4x main MUX + 1x self-test (U23) in daisy-chain
#define ADGS_MAIN_DEVICES   4             // U10, U11, U16, U17
#define ADGS_HAS_SELFTEST   1             // U23 available for self-test / calibration
#define ADGS_SELFTEST_DEV   4             // U23 = device index 4 in daisy-chain
#endif
#define PIN_LSHIFT_OE       GPIO_NUM_14   // Level shifter OE (TXS0108E U13+U15)

#define ADGS_NUM_SWITCHES   8
#define ADGS_DEAD_TIME_MS   100  // Dead time between switch-off and switch-on

// -----------------------------------------------------------------------------
// U23 Self-Test MUX — Switch Assignments (device index 4)
// -----------------------------------------------------------------------------
// S-side connects to e-fuse IMON pins, VADJ voltage dividers, 3V3_ADJ, and Ch D
// D-side connects to shared measurement rail (R106 = 1 MΩ to GND)

#define U23_SW_EFUSE3_IMON  0x01   // S1 (bit 0): EFUSE_MON_3
#define U23_SW_EFUSE1_IMON  0x02   // S2 (bit 1): EFUSE_MON_1
#define U23_SW_EFUSE2_IMON  0x04   // S3 (bit 2): EFUSE_MON_2
#define U23_SW_ADC_CH_D     0x08   // S4 (bit 3): AD74416H Channel D → shared rail
#define U23_SW_EFUSE4_IMON  0x10   // S5 (bit 4): EFUSE_MON_4
#define U23_SW_VADJ1        0x20   // S6 (bit 5): VADJ1_BUCK (via R107/R109 divider)
#define U23_SW_VADJ2        0x40   // S7 (bit 6): VADJ2_BUCK (via R108/R110 divider)
#define U23_SW_3V3_ADJ      0x80   // S8 (bit 7): 3V3_ADJ (VLOGIC, direct)

// VADJ voltage divider ratio: R_bottom / (R_top + R_bottom) = 100k / (34.8k + 100k)
#define VADJ_DIVIDER_RATIO  0.7418f

// E-fuse IMON scaling: V_IMON = I_OUT × G_IMON × R_IOCP
// G_IMON = 50 µA/A (typ), R_IOCP = 11 kΩ → 550 mV per amp
#define IMON_GAIN_UA_PER_A  50.0f     // TPS1641x typical gain
#define IMON_R_IOCP_OHM     11000.0f  // External IOCP resistor (same for all 4 e-fuses)
#define IMON_MV_PER_A       (IMON_GAIN_UA_PER_A * IMON_R_IOCP_OHM / 1000.0f)  // = 550 mV/A

// Safety interlock: U17 S2 (device 3, bit 1) vs U23 any switch
#define U17_S2_MASK         0x02   // U17 switch S2 = bit 1
#define U17_DEVICE_IDX      3      // U17 = device index 3

// -----------------------------------------------------------------------------
// SPI Configuration
// -----------------------------------------------------------------------------
#define SPI_CLOCK_HZ    10000000UL    // 10 MHz (AD74416H supports up to 20 MHz)

// -----------------------------------------------------------------------------
// AD74416H Device Address
// AD0=AD1=GND -> device address = 0
// -----------------------------------------------------------------------------
#define AD74416H_DEV_ADDR   0x00

// -----------------------------------------------------------------------------
// WiFi / Access Point Configuration
// -----------------------------------------------------------------------------
#define WIFI_SSID           "BugBuster"
#define WIFI_PASSWORD       "bugbuster123"
#define WIFI_STA_SSID       ""          // Empty = no auto-connect; loaded from NVS if saved
#define WIFI_STA_PASSWORD   ""
#define WIFI_CHANNEL        1
#define WIFI_MAX_CONN       4

// -----------------------------------------------------------------------------
// Timing Constants
// -----------------------------------------------------------------------------
#define RESET_PULSE_MS          10
#define POWER_UP_DELAY_MS       50
#define CHANNEL_SWITCH_US       300
#define CHANNEL_SWITCH_HART_US  4200

// -----------------------------------------------------------------------------
// AD74416H Limits
// -----------------------------------------------------------------------------
#define AD74416H_NUM_CHANNELS   4
#define DAC_FULL_SCALE          65536U  // Per datasheet: code = normalised * 65536
#define ADC_FULL_SCALE          16777216UL

// Voltage / current output spans
#define VOUT_UNIPOLAR_SPAN_V    12.0f
#define VOUT_BIPOLAR_SPAN_V     24.0f
#define VOUT_BIPOLAR_OFFSET_V   12.0f
#define IOUT_MAX_MA             25.0f

// =============================================================================
// GPIO / Timing Helpers (replace Arduino pinMode/digitalWrite/delay)
// =============================================================================

static inline void pin_mode_output(gpio_num_t pin) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

static inline void pin_mode_input_pullup(gpio_num_t pin) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
}

static inline void pin_write(gpio_num_t pin, int level) {
    gpio_set_level(pin, level);
}

static inline int pin_read(gpio_num_t pin) {
    return gpio_get_level(pin);
}

static inline void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static inline void delay_us(uint32_t us) {
    esp_rom_delay_us(us);
}

static inline uint32_t millis_now(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
