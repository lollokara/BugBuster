// =============================================================================
// adgs2414d.cpp - ADGS2414D Octal SPST Switch Driver (4x daisy-chain)
//
// Uses the same SPI bus as AD74416H but with a different CS pin (GPIO 21).
// All 4 devices are in daisy-chain mode: one CS frame sends 4 bytes.
// =============================================================================

#include "adgs2414d.h"
#include "config.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include <string.h>

static const char *TAG = "adgs2414d";

// Cached switch states for all 4 devices
static uint8_t s_mux_state[ADGS_NUM_DEVICES] = {0, 0, 0, 0};

// SPI device handle (separate from AD74416H, same bus, different CS)
static spi_device_handle_t s_spi_dev = NULL;

// Whether we're in daisy-chain mode
static bool s_daisy_chain_active = false;

// -----------------------------------------------------------------------------
// Low-level SPI helpers
// -----------------------------------------------------------------------------

// Send raw bytes on the SPI bus with MUX CS
static void spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    spi_device_transmit(s_spi_dev, &t);
}

// Send a 16-bit address-mode command to all devices (before daisy-chain)
static void adgs_address_mode_write(uint8_t reg, uint8_t data)
{
    uint8_t tx[2] = { (uint8_t)(ADGS_CMD_WRITE | (reg & 0x7F)), data };
    uint8_t rx[2] = {0};
    spi_transfer(tx, rx, 2);
}

// Enter daisy-chain mode: send 0x2500 to each device in address mode
// Per datasheet: when a device receives 0x2500, its SDO echoes 0x2500
// (alignment bits are 0x25), so all daisy-connected devices enter DC mode
// in a single frame.
static void adgs_enter_daisy_chain(void)
{
    uint8_t tx[2] = { ADGS_DAISY_CHAIN_CMD_HI, ADGS_DAISY_CHAIN_CMD_LO };
    uint8_t rx[2] = {0};
    spi_transfer(tx, rx, 2);
    s_daisy_chain_active = true;
    ESP_LOGI(TAG, "Daisy-chain mode entered");
}

// Send 4 bytes in daisy-chain mode (one byte per device)
// Byte order: first byte sent goes to last device in chain (U17),
// last byte sent goes to first device (U10).
// So we send: [dev3, dev2, dev1, dev0]
static void adgs_daisy_chain_write(const uint8_t states[ADGS_NUM_DEVICES])
{
    // Reverse order: device 3 first (reaches end of chain = U17)
    uint8_t tx[ADGS_NUM_DEVICES] = {
        states[3], states[2], states[1], states[0]
    };
    uint8_t rx[ADGS_NUM_DEVICES] = {0};
    spi_transfer(tx, rx, ADGS_NUM_DEVICES);
}

// Get the group mask for a switch index
static uint8_t get_group_mask(uint8_t sw)
{
    if (sw < 4) return ADGS_GROUP_A_MASK;
    if (sw < 6) return ADGS_GROUP_B_MASK;
    return ADGS_GROUP_C_MASK;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void adgs_init(void)
{
    // Configure CS pin as output, default HIGH (inactive)
    gpio_reset_pin(PIN_MUX_CS);
    gpio_set_direction(PIN_MUX_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_MUX_CS, 1);

    // Enable level shifters
    gpio_reset_pin(PIN_LSHIFT_OE);
    gpio_set_direction(PIN_LSHIFT_OE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LSHIFT_OE, 1);  // OE active high

    // Add SPI device on the existing SPI bus (SPI2_HOST, same as AD74416H)
    // The AD74416H SPI driver already initialized the bus on SPI2_HOST
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 1000000;  // 1 MHz (ADGS supports up to 50 MHz)
    devcfg.mode = 0;                  // SPI Mode 0 (CPOL=0, CPHA=0)
    devcfg.spics_io_num = PIN_MUX_CS; // CS on GPIO 21
    devcfg.queue_size = 4;

    esp_err_t ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return;
    }

    // Reset all switches to open
    memset(s_mux_state, 0, sizeof(s_mux_state));

    // Enter daisy-chain mode
    adgs_enter_daisy_chain();

    // Write all-zeros to ensure all switches are open
    adgs_daisy_chain_write(s_mux_state);

    ESP_LOGI(TAG, "ADGS2414D mux matrix initialized (%d devices, CS=GPIO%d, LShift OE=GPIO%d)",
             ADGS_NUM_DEVICES, PIN_MUX_CS, PIN_LSHIFT_OE);
}

void adgs_set_all_raw(const uint8_t states[ADGS_NUM_DEVICES])
{
    if (!s_daisy_chain_active) return;
    memcpy(s_mux_state, states, ADGS_NUM_DEVICES);
    adgs_daisy_chain_write(s_mux_state);
}

void adgs_set_all_safe(const uint8_t states[ADGS_NUM_DEVICES])
{
    if (!s_daisy_chain_active) return;

    // Step 1: Open all switches
    uint8_t all_open[ADGS_NUM_DEVICES] = {0, 0, 0, 0};
    adgs_daisy_chain_write(all_open);

    // Step 2: Wait dead time
    delay_ms(ADGS_DEAD_TIME_MS);

    // Step 3: Set new state
    memcpy(s_mux_state, states, ADGS_NUM_DEVICES);
    adgs_daisy_chain_write(s_mux_state);
}

void adgs_set_switch_safe(uint8_t device, uint8_t sw, bool closed)
{
    if (device >= ADGS_NUM_DEVICES || sw >= ADGS_NUM_SWITCHES) return;
    if (!s_daisy_chain_active) return;

    uint8_t group_mask = get_group_mask(sw);
    uint8_t new_state;

    if (closed) {
        // Opening: first open all switches in the same group
        uint8_t temp_state[ADGS_NUM_DEVICES];
        memcpy(temp_state, s_mux_state, ADGS_NUM_DEVICES);
        temp_state[device] &= ~group_mask;  // Open the entire group
        adgs_daisy_chain_write(temp_state);

        // Wait dead time
        delay_ms(ADGS_DEAD_TIME_MS);

        // Close the requested switch
        new_state = (s_mux_state[device] & ~group_mask) | (1 << sw);
    } else {
        // Simply open the switch (no dead time needed for opening)
        new_state = s_mux_state[device] & ~(1 << sw);
    }

    s_mux_state[device] = new_state;
    adgs_daisy_chain_write(s_mux_state);
}

uint8_t adgs_get_state(uint8_t device)
{
    if (device >= ADGS_NUM_DEVICES) return 0;
    return s_mux_state[device];
}

void adgs_get_all_states(uint8_t out[ADGS_NUM_DEVICES])
{
    memcpy(out, s_mux_state, ADGS_NUM_DEVICES);
}

void adgs_reset_all(void)
{
    memset(s_mux_state, 0, sizeof(s_mux_state));
    if (s_daisy_chain_active) {
        adgs_daisy_chain_write(s_mux_state);
    }
    ESP_LOGI(TAG, "All switches reset to open");
}
