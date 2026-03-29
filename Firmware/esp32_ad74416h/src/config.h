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
#define BREADBOARD_MODE  1

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
#define PIN_MUX_CS      GPIO_NUM_12   // Breadboard: GPIO12
#define ADGS_NUM_DEVICES   1          // Single device on breadboard
#else
#define PIN_MUX_CS      GPIO_NUM_21   // PCB: SPI_CS_MUX
#define ADGS_NUM_DEVICES   4          // 4x daisy-chain on PCB
#endif
#define PIN_LSHIFT_OE   GPIO_NUM_14   // Level shifter OE (TXS0108E U13+U15)

#define ADGS_NUM_SWITCHES  8
#define ADGS_DEAD_TIME_MS  100  // Dead time between switch-off and switch-on

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
#define WIFI_SSID       "BugBuster"
#define WIFI_PASSWORD   "bugbuster123"
#define WIFI_CHANNEL    1
#define WIFI_MAX_CONN   4

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
#define DAC_FULL_SCALE          65535U
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
