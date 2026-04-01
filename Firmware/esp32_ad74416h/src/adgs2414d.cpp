// =============================================================================
// adgs2414d.cpp - ADGS2414D Octal SPST Switch Driver (4x daisy-chain)
//
// Uses the same SPI bus as AD74416H but with a different CS pin (GPIO 21).
// All 4 devices are in daisy-chain mode: one CS frame sends 4 bytes.
// =============================================================================

#include "adgs2414d.h"
#include "ad74416h_spi.h"
#include "config.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include <string.h>

static const char *TAG = "adgs2414d";

// Cached switch states for all 4 devices
static uint8_t s_mux_state[ADGS_NUM_DEVICES] = {};

// SPI device handle (separate from AD74416H, same bus, different CS)
static spi_device_handle_t s_spi_dev = NULL;

// Whether we're in daisy-chain mode
static bool s_mux_initialized = false;

// -----------------------------------------------------------------------------
// Low-level SPI helpers
// -----------------------------------------------------------------------------

// Send raw bytes on the SPI bus with manual MUX CS
// Cooperative SPI bus sharing with ADC poll task.
// The ADC task checks this flag and yields when set.
volatile bool g_spi_bus_request = false;
volatile bool g_spi_bus_granted = false;

static void spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    // Request bus access — ADC task will yield after its current transaction
    g_spi_bus_granted = false;
    g_spi_bus_request = true;

    // Wait for ADC task to grant us the bus (max 200ms)
    for (int i = 0; i < 200 && !g_spi_bus_granted; i++) {
        delay_ms(1);
    }
    if (!g_spi_bus_granted) {
        ESP_LOGE(TAG, "SPI bus request timeout (200ms) - aborting transfer");
        g_spi_bus_request = false;
        return;
    }

    gpio_set_level(PIN_MUX_CS, 0);
    delay_us(1);
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    spi_device_polling_transmit(s_spi_dev, &t);
    delay_us(1);
    gpio_set_level(PIN_MUX_CS, 1);

    // Release bus
    g_spi_bus_request = false;
    g_spi_bus_granted = false;
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
    s_mux_initialized = true;
    ESP_LOGI(TAG, "Daisy-chain mode entered");
}

// Send 4 bytes in daisy-chain mode (one byte per device)
// Byte order: first byte sent goes to last device in chain (U17),
// last byte sent goes to first device (U10).
// So we send: [dev3, dev2, dev1, dev0]
static void adgs_daisy_chain_write(const uint8_t states[ADGS_NUM_DEVICES])
{
    // Reverse order: device N-1 first (reaches end of chain)
    uint8_t tx[ADGS_NUM_DEVICES];
    for (int i = 0; i < ADGS_NUM_DEVICES; i++) {
        tx[i] = states[ADGS_NUM_DEVICES - 1 - i];
    }
    uint8_t rx[ADGS_NUM_DEVICES] = {};
    spi_transfer(tx, rx, ADGS_NUM_DEVICES);
}

// Write current switch states to hardware (handles both modes)
static void adgs_write_states(const uint8_t states[ADGS_NUM_DEVICES])
{
#if ADGS_NUM_DEVICES > 1
    adgs_daisy_chain_write(states);
#else
    adgs_address_mode_write(ADGS_REG_SW_DATA, states[0]);
#endif
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

    // Add our own SPI device on the bus (manual CS, no auto-CS pin)
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 1000000;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 1;

    esp_err_t ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPI device added, manual CS on GPIO%d", PIN_MUX_CS);

    // Reset all switches to open
    memset(s_mux_state, 0, sizeof(s_mux_state));

#if ADGS_NUM_DEVICES > 1
    // Enter daisy-chain mode for multi-device setup
    adgs_enter_daisy_chain();
    adgs_daisy_chain_write(s_mux_state);
#else
    // Single device: stay in address mode (no daisy-chain)
    s_mux_initialized = true;  // Flag still set so API works
    adgs_address_mode_write(ADGS_REG_SW_DATA, 0x00);
    ESP_LOGI(TAG, "Single device mode (address mode, no daisy-chain)");
#endif

    ESP_LOGI(TAG, "ADGS2414D mux matrix initialized (%d devices, CS=GPIO%d, LShift OE=GPIO%d)",
             ADGS_NUM_DEVICES, PIN_MUX_CS, PIN_LSHIFT_OE);
}

void adgs_set_all_raw(const uint8_t states[ADGS_MAIN_DEVICES])
{
    if (!s_mux_initialized) return;
    memcpy(s_mux_state, states, ADGS_MAIN_DEVICES);
    // Preserve self-test device (index 4) — don't touch it from main MUX path
    adgs_write_states(s_mux_state);
}

void adgs_set_all_safe(const uint8_t states[ADGS_MAIN_DEVICES])
{
    if (!s_mux_initialized) return;

#if ADGS_HAS_SELFTEST
    // Interlock: if caller tries to set U17 S2 while U23 is active, block it
    if ((states[U17_DEVICE_IDX] & U17_S2_MASK) && adgs_selftest_active()) {
        ESP_LOGE(TAG, "INTERLOCK: Cannot close U17 S2 while U23 self-test is active!");
        return;
    }
#endif

    // Step 1: Open main MUX switches (preserve self-test device)
    uint8_t temp[ADGS_NUM_DEVICES];
    memset(temp, 0, ADGS_MAIN_DEVICES);
#if ADGS_HAS_SELFTEST
    temp[ADGS_SELFTEST_DEV] = s_mux_state[ADGS_SELFTEST_DEV];  // preserve U23
#endif
    adgs_write_states(temp);

    // Step 2: Wait dead time
    delay_ms(ADGS_DEAD_TIME_MS);

    // Step 3: Set new main MUX state (preserve self-test device)
    memcpy(s_mux_state, states, ADGS_MAIN_DEVICES);
    adgs_write_states(s_mux_state);
}

void adgs_set_switch_safe(uint8_t device, uint8_t sw, bool closed)
{
    if (device >= ADGS_MAIN_DEVICES || sw >= ADGS_NUM_SWITCHES) return;
    if (!s_mux_initialized) return;

#if ADGS_HAS_SELFTEST
    // Interlock: block U17 S2 close if U23 is active
    if (device == U17_DEVICE_IDX && sw == 1 && closed && adgs_selftest_active()) {
        ESP_LOGE(TAG, "INTERLOCK: Cannot close U17 S2 while U23 self-test is active!");
        return;
    }
#endif

    uint8_t group_mask = get_group_mask(sw);
    uint8_t new_state;

    if (closed) {
        // Opening: first open all switches in the same group
        uint8_t temp_state[ADGS_NUM_DEVICES];
        memcpy(temp_state, s_mux_state, ADGS_NUM_DEVICES);
        temp_state[device] &= ~group_mask;  // Open the entire group
        adgs_write_states(temp_state);

        // Wait dead time
        delay_ms(ADGS_DEAD_TIME_MS);

        // Close the requested switch
        new_state = (s_mux_state[device] & ~group_mask) | (1 << sw);
    } else {
        // Simply open the switch (no dead time needed for opening)
        new_state = s_mux_state[device] & ~(1 << sw);
    }

    s_mux_state[device] = new_state;
    adgs_write_states(s_mux_state);
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
    if (s_mux_initialized) {
#if ADGS_NUM_DEVICES > 1
        adgs_daisy_chain_write(s_mux_state);
#else
        adgs_address_mode_write(ADGS_REG_SW_DATA, 0x00);
#endif
    }
    ESP_LOGI(TAG, "All switches reset to open");
}

uint8_t adgs_test_address_mode(uint8_t sw_data)
{
    // Exit daisy-chain mode first via soft reset
    // Soft reset: write 0xA3 then 0x05 to SOFT_RESETB register (0x0B)
    adgs_address_mode_write(ADGS_REG_SOFT_RESET, ADGS_SOFT_RESET_VAL1);
    delay_ms(1);
    adgs_address_mode_write(ADGS_REG_SOFT_RESET, ADGS_SOFT_RESET_VAL2);
    delay_ms(10);
    s_mux_initialized = false;
    ESP_LOGI(TAG, "Soft reset done, now in address mode");

    // Write switch data
    adgs_address_mode_write(ADGS_REG_SW_DATA, sw_data);
    ESP_LOGI(TAG, "Wrote SW_DATA = 0x%02X", sw_data);

    // Read back: send read command (0x80 | reg), data=0x00 (don't care)
    uint8_t tx[2] = { ADGS_CMD_READ | ADGS_REG_SW_DATA, 0x00 };
    uint8_t rx[2] = {0};
    spi_transfer(tx, rx, 2);
    ESP_LOGI(TAG, "Read SW_DATA: rx=[0x%02X, 0x%02X]", rx[0], rx[1]);

    return rx[1];  // Data is in second byte
}

void adgs_soft_reset(void)
{
    adgs_address_mode_write(ADGS_REG_SOFT_RESET, ADGS_SOFT_RESET_VAL1);
    delay_ms(1);
    adgs_address_mode_write(ADGS_REG_SOFT_RESET, ADGS_SOFT_RESET_VAL2);
    delay_ms(10);
    s_mux_initialized = false;
    memset(s_mux_state, 0, sizeof(s_mux_state));
    ESP_LOGI(TAG, "Soft reset complete");
}

// -----------------------------------------------------------------------------
// Self-Test MUX (U23) — device index ADGS_SELFTEST_DEV
// Only available when ADGS_HAS_SELFTEST == 1 (PCB mode)
// -----------------------------------------------------------------------------

#if ADGS_HAS_SELFTEST

bool adgs_set_selftest(uint8_t sw_byte)
{
    if (!s_mux_initialized) return false;

    // Safety interlock: U17 S2 must be open before ANY U23 switch can close
    if (sw_byte != 0 && adgs_u17_s2_active()) {
        ESP_LOGE(TAG, "INTERLOCK: Cannot activate U23 while U17 S2 (IO10 analog) is closed!");
        return false;
    }

    uint8_t old = s_mux_state[ADGS_SELFTEST_DEV];
    if (old == sw_byte) return true;  // no change

    if (sw_byte == 0) {
        // Opening all — just write directly
        s_mux_state[ADGS_SELFTEST_DEV] = 0;
        adgs_write_states(s_mux_state);
    } else {
        // Break-before-make: open U23 first, wait dead time, then close new switches
        s_mux_state[ADGS_SELFTEST_DEV] = 0;
        adgs_write_states(s_mux_state);
        delay_ms(ADGS_DEAD_TIME_MS);
        s_mux_state[ADGS_SELFTEST_DEV] = sw_byte;
        adgs_write_states(s_mux_state);
    }

    ESP_LOGD(TAG, "U23 selftest: 0x%02X -> 0x%02X", old, sw_byte);
    return true;
}

uint8_t adgs_get_selftest(void)
{
    return s_mux_state[ADGS_SELFTEST_DEV];
}

bool adgs_u17_s2_active(void)
{
    return (s_mux_state[U17_DEVICE_IDX] & U17_S2_MASK) != 0;
}

bool adgs_selftest_active(void)
{
    return s_mux_state[ADGS_SELFTEST_DEV] != 0;
}

#endif // ADGS_HAS_SELFTEST
