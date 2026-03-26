#pragma once

// =============================================================================
// config.h - Pin definitions and system constants for AD74416H + ESP32-S3
// =============================================================================

// -----------------------------------------------------------------------------
// SPI Pin Definitions (AD74416H uses SYNC as active-low chip select)
// -----------------------------------------------------------------------------
#define PIN_SDO         8   // MISO - Serial Data Out (from AD74416H)
#define PIN_SDI         9   // MOSI - Serial Data In  (to AD74416H)
#define PIN_SYNC        10  // CS   - Active low frame sync (chip select)
#define PIN_SCLK        11  // SCLK - SPI clock

// -----------------------------------------------------------------------------
// Control / Status Pin Definitions
// -----------------------------------------------------------------------------
#define PIN_RESET       5   // Active low hardware reset (drive LOW to reset)
#define PIN_ADC_RDY     6   // Open-drain, active low - ADC conversion ready
#define PIN_ALERT       7   // Open-drain, active low - fault/alert output

// -----------------------------------------------------------------------------
// SPI Configuration
// -----------------------------------------------------------------------------
#define SPI_CLOCK_HZ    1000000UL   // 1 MHz safe starting speed (max 20 MHz)
#define SPI_MODE        SPI_MODE2   // CPOL=1, CPHA=0

// -----------------------------------------------------------------------------
// AD74416H Device Address
// AD0=AD1=GND -> device address = 0
// Address bits [1:0] occupy D[37:36] in the SPI frame
// -----------------------------------------------------------------------------
#define AD74416H_DEV_ADDR   0x00    // AD0=AD1=GND

// -----------------------------------------------------------------------------
// WiFi / Access Point Configuration
// -----------------------------------------------------------------------------
#define WIFI_SSID       "BugBuster"
#define WIFI_PASSWORD   "bugbuster123"
#define WIFI_CHANNEL    1
#define WIFI_MAX_CONN   4

// -----------------------------------------------------------------------------
// Timing Constants (microseconds unless noted)
// -----------------------------------------------------------------------------
#define RESET_PULSE_MS          10      // Hardware reset pulse width (ms)
#define POWER_UP_DELAY_MS       50      // Post-reset stabilisation delay (ms)
#define CHANNEL_SWITCH_US       300     // Channel function switch settle (us)
#define CHANNEL_SWITCH_HART_US  4200    // IOUT_HART channel switch settle (us)

// -----------------------------------------------------------------------------
// AD74416H Limits
// -----------------------------------------------------------------------------
#define AD74416H_NUM_CHANNELS   4
#define DAC_FULL_SCALE          65535U  // 16-bit DAC full scale code
#define ADC_FULL_SCALE          16777216UL  // 2^24 for 24-bit ADC

// Voltage / current output spans
#define VOUT_UNIPOLAR_SPAN_V    12.0f   // 0 V to 12 V
#define VOUT_BIPOLAR_SPAN_V     24.0f   // -12 V to +12 V
#define VOUT_BIPOLAR_OFFSET_V   12.0f   // Offset for bipolar code conversion
#define IOUT_MAX_MA             25.0f   // 0 mA to 25 mA
