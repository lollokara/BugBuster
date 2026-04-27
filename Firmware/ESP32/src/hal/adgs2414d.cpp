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

// Cached switch states for all devices
static uint8_t s_mux_state[ADGS_NUM_DEVICES] = {};

// Public 4-byte API shadow. On PCB builds this mirrors the 4 populated main
// devices; on breadboard builds only byte 0 is backed by hardware.
static uint8_t s_api_main_state[ADGS_API_MAIN_DEVICES] = {};

// SPI device handle (separate from AD74416H, same bus, different CS)
static spi_device_handle_t s_spi_dev = NULL;

// Whether we're in daisy-chain mode
static bool s_mux_initialized = false;

// Fault flag: set when write-verify fails after ADGS_MAX_RETRIES
static bool s_mux_faulted = false;

// Readback capability for single-device breadboard mode.
// If the first non-zero SW_DATA write is visible on the hardware but SDO keeps
// returning 0x00 with no ERR_FLAGS set, verification is downgraded to write-only
// so the driver does not falsely roll the MUX state back to all-open.
static bool s_readback_available = true;
static bool s_readback_checked = false;

static void sync_api_main_from_physical(void)
{
    for (uint8_t i = 0; i < ADGS_MAIN_DEVICES && i < ADGS_API_MAIN_DEVICES; i++) {
        s_api_main_state[i] = s_mux_state[i];
    }
}


// -----------------------------------------------------------------------------
// Low-level SPI helpers
// -----------------------------------------------------------------------------

// Send raw bytes on the SPI bus with manual MUX CS.
// Cooperative SPI bus sharing with ADC poll task via FreeRTOS semaphore.
// The ADC task gives the semaphore between transactions; requesters take it.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Global SPI bus semaphore — created in adgs_init(), taken by MUX/CMD callers,
// given back by ADC task between its polling cycles.
SemaphoreHandle_t g_spi_bus_mutex = NULL;

static void spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    // Acquire SPI bus (max 200ms wait)
    if (g_spi_bus_mutex == NULL ||
        xSemaphoreTakeRecursive(g_spi_bus_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(TAG, "SPI bus acquire timeout (200ms) - aborting transfer");
        return;
    }

    // CS is managed by the SPI hardware (spics_io_num = PIN_MUX_CS).
    // The hardware asserts CS only AFTER reconfiguring SCLK polarity for this device
    // (Mode 0, CPOL=0). This prevents the spurious falling SCLK edge that occurs when
    // CS is asserted manually while the bus is still in Mode 2 (CPOL=1) from a previous
    // AD74416H transaction — which would give the ADGS 17 SCLK cycles instead of 16,
    // triggering SCLK_ERR and silently blocking register writes.
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    spi_device_polling_transmit(s_spi_dev, &t);

    // Release bus
    xSemaphoreGiveRecursive(g_spi_bus_mutex);
}

// Send a 16-bit address-mode command to all devices (before daisy-chain)
static void adgs_address_mode_write(uint8_t reg, uint8_t data)
{
    uint8_t tx[2] = { (uint8_t)(ADGS_CMD_WRITE | (reg & 0x7F)), data };
    uint8_t rx[2] = {0};
    spi_transfer(tx, rx, 2);
}

#if ADGS_NUM_DEVICES > 1
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

// Send switch bytes in daisy-chain mode.
// The first byte sent reaches the last device in the chain.
static void adgs_daisy_chain_write(const uint8_t states[ADGS_NUM_DEVICES])
{
    uint8_t tx[ADGS_NUM_DEVICES];
    for (int i = 0; i < ADGS_NUM_DEVICES; i++) {
        tx[i] = states[ADGS_NUM_DEVICES - 1 - i];
    }
    uint8_t rx[ADGS_NUM_DEVICES] = {};
    spi_transfer(tx, rx, ADGS_NUM_DEVICES);
}
#endif

// Read a register in address mode (single device)
static uint8_t adgs_address_mode_read(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(ADGS_CMD_READ | (reg & 0x7F)), 0x00 };
    uint8_t rx[2] = {0};
    spi_transfer(tx, rx, 2);
    ESP_LOGD(TAG, "ADGS read reg 0x%02X: tx=[%02X %02X] rx=[%02X %02X] → 0x%02X",
             reg, tx[0], tx[1], rx[0], rx[1], rx[1]);
    return rx[1];  // data is in second byte
}

#if ADGS_NUM_DEVICES > 1
// Read back switch states in daisy-chain mode.
// Perform a "dummy" write of the current cached state and capture SDO.
// In daisy-chain mode, SDO outputs the previous switch data register contents.
static void adgs_daisy_chain_readback(uint8_t out[ADGS_NUM_DEVICES])
{
    uint8_t tx[ADGS_NUM_DEVICES];
    for (int i = 0; i < ADGS_NUM_DEVICES; i++) {
        tx[i] = s_mux_state[ADGS_NUM_DEVICES - 1 - i];
    }
    uint8_t rx[ADGS_NUM_DEVICES] = {};
    spi_transfer(tx, rx, ADGS_NUM_DEVICES);

    for (int i = 0; i < ADGS_NUM_DEVICES; i++) {
        out[i] = rx[ADGS_NUM_DEVICES - 1 - i];
    }
}
#endif

// Write states and verify by readback. Returns true on match.
static bool adgs_write_and_verify(const uint8_t states[ADGS_NUM_DEVICES])
{
#if ADGS_NUM_DEVICES > 1
    adgs_daisy_chain_write(states);
    delay_us(10);

    // Readback verify
    uint8_t readback[ADGS_NUM_DEVICES] = {};
    adgs_daisy_chain_readback(readback);

    for (int i = 0; i < ADGS_NUM_DEVICES; i++) {
        if (readback[i] != states[i]) {
            ESP_LOGW(TAG, "Readback mismatch: device %d wrote=0x%02X read=0x%02X",
                     i, states[i], readback[i]);
            return false;
        }
    }
    return true;
#else
    adgs_address_mode_write(ADGS_REG_SW_DATA, states[0]);
    delay_ms(1);  // ensure register write is committed

    if (!s_readback_available) {
        return true;
    }

    uint8_t rb = adgs_address_mode_read(ADGS_REG_SW_DATA);
    if (rb == states[0]) {
        if (states[0] != 0x00) {
            s_readback_checked = true;
        }
        ESP_LOGI(TAG, "Write-verify: wrote=0x%02X readback=0x%02X OK",
                 states[0], rb);
        return true;
    }

    uint8_t err_flags = adgs_address_mode_read(ADGS_REG_ERR_FLAGS);
    if (states[0] != 0x00 && !s_readback_checked && rb == 0x00 && err_flags == 0x00) {
        s_readback_checked = true;
        s_readback_available = false;
        ESP_LOGW(TAG,
                 "SDO readback appears unavailable/unreliable in breadboard mode "
                 "(wrote=0x%02X readback=0x%02X ERR_FLAGS=0x%02X). "
                 "Continuing in write-only mode.",
                 states[0], rb, err_flags);
        return true;
    }

    if (states[0] != 0x00) {
        s_readback_checked = true;
    }
    ESP_LOGI(TAG, "Write-verify: wrote=0x%02X readback=0x%02X %s",
             states[0], rb, (rb == states[0]) ? "OK" : "MISMATCH");
    if (err_flags) {
        ESP_LOGW(TAG, "ERR_FLAGS=0x%02X (CRC=%d SCLK=%d RW=%d)",
                 err_flags,
                 !!(err_flags & ADGS_ERR_CRC_FLAG),
                 !!(err_flags & ADGS_ERR_SCLK_FLAG),
                 !!(err_flags & ADGS_ERR_RW_FLAG));
    }
    return false;
#endif
}

// Write switch states with verification and retry.
// Sets fault flag if all retries fail.
static void adgs_write_states(const uint8_t states[ADGS_NUM_DEVICES])
{
    for (int attempt = 0; attempt < ADGS_MAX_RETRIES; attempt++) {
        if (adgs_write_and_verify(states)) {
            if (attempt > 0) {
                ESP_LOGI(TAG, "MUX write succeeded on retry %d", attempt);
            }
            s_mux_faulted = false;
            return;  // success
        }
        ESP_LOGW(TAG, "MUX write-verify failed (attempt %d/%d)", attempt + 1, ADGS_MAX_RETRIES);
        delay_ms(5);
    }

    // All retries exhausted. In address mode we can try a datasheet-defined
    // software reset, but in daisy-chain mode all commands target SW_DATA and
    // the datasheet requires a hardware reset to exit the chain.
    ESP_LOGW(TAG, "MUX write-verify failed after %d retries.", ADGS_MAX_RETRIES);

#if ADGS_NUM_DEVICES > 1
    ESP_LOGE(TAG,
             "ADGS2414D is in daisy-chain mode. The datasheet says all commands "
             "target SW_DATA in this mode and a hardware reset is required to exit it, "
             "so software-reset recovery is unavailable.");
#else
    // Address mode: soft reset and retry
    adgs_address_mode_write(ADGS_REG_SOFT_RESET, ADGS_SOFT_RESET_VAL1);
    delay_ms(1);
    adgs_address_mode_write(ADGS_REG_SOFT_RESET, ADGS_SOFT_RESET_VAL2);
    delay_ms(10);
    s_mux_initialized = true;

    // Check error flags after reset
    uint8_t err_flags = adgs_address_mode_read(ADGS_REG_ERR_FLAGS);
    if (err_flags) {
        ESP_LOGW(TAG, "Error flags after reset: 0x%02X (CRC=%d SCLK=%d RW=%d)",
                 err_flags,
                 !!(err_flags & ADGS_ERR_CRC_FLAG),
                 !!(err_flags & ADGS_ERR_SCLK_FLAG),
                 !!(err_flags & ADGS_ERR_RW_FLAG));
        // Clear them
        uint8_t clr[2] = { ADGS_ERR_CLEAR_HI, ADGS_ERR_CLEAR_LO };
        uint8_t clr_rx[2] = {};
        spi_transfer(clr, clr_rx, 2);
    }

    // One final attempt after reset
    if (adgs_write_and_verify(states)) {
        ESP_LOGI(TAG, "MUX recovered after software reset!");
        s_mux_faulted = false;
        return;
    }
#endif

    // Recovery failed — declare MUX FAULT
    ESP_LOGE(TAG, "MUX FAULT: recovery failed! Opening all switches for safety.");
    s_mux_faulted = true;

    // Safety: force all switches open (best effort, no verification)
#if ADGS_NUM_DEVICES > 1
    uint8_t zeros[ADGS_NUM_DEVICES] = {};
    adgs_daisy_chain_write(zeros);
#else
    adgs_address_mode_write(ADGS_REG_SW_DATA, 0x00);
#endif
    memset(s_mux_state, 0, sizeof(s_mux_state));
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

bool adgs_init(void)
{
    // Create SPI bus mutex if not already created
    if (g_spi_bus_mutex == NULL) {
        g_spi_bus_mutex = xSemaphoreCreateRecursiveMutex();
        assert(g_spi_bus_mutex != NULL);
    }

    // Enable level shifters
    gpio_reset_pin(PIN_LSHIFT_OE);
    gpio_set_direction(PIN_LSHIFT_OE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LSHIFT_OE, 1);  // OE active high

    // Add SPI device with hardware CS (spics_io_num = PIN_MUX_CS).
    // Hardware CS ensures the bus polarity is reconfigured to Mode 0 (SCLK idle LOW)
    // BEFORE CS is asserted — preventing a spurious falling SCLK edge that would occur
    // if CS were asserted manually while the bus was still in Mode 2 (SCLK idle HIGH)
    // from the AD74416H. Without this fix the ADGS counts 17 SCLK cycles per frame,
    // triggering SCLK_ERR_FLAG and silently discarding every register write.
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 1000000;
    devcfg.mode = 0;
    devcfg.spics_io_num = PIN_MUX_CS;  // Hardware CS — not manual GPIO
    devcfg.queue_size = 1;

    if (s_spi_dev == NULL) {
        esp_err_t ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
            return false;
        }
        ESP_LOGI(TAG, "SPI device added, hardware CS on GPIO%d", PIN_MUX_CS);
    }

    // Reset all switches to open
    memset(s_mux_state, 0, sizeof(s_mux_state));
    memset(s_api_main_state, 0, sizeof(s_api_main_state));

    s_mux_faulted = false;
    s_readback_available = true;
    s_readback_checked = false;

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
    return true;
}

void adgs_set_all_raw(const uint8_t states[ADGS_MAIN_DEVICES])
{
    if (!s_mux_initialized) return;
    memcpy(s_mux_state, states, ADGS_MAIN_DEVICES);
    sync_api_main_from_physical();
    // Preserve self-test device (index 4) — don't touch it from main MUX path
    adgs_write_states(s_mux_state);
}

void adgs_set_all_safe(const uint8_t states[ADGS_MAIN_DEVICES])
{
    if (!s_mux_initialized) return;

#if ADGS_HAS_SELFTEST
    // Interlock: if caller tries to set U16 S3 while U23 is active, block it
    if ((states[U16_DEVICE_IDX] & U16_S3_MASK) && adgs_selftest_active()) {
        ESP_LOGE(TAG, "INTERLOCK: Cannot close U16 S3 while U23 self-test is active!");
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
    sync_api_main_from_physical();
}

void adgs_set_switch_safe(uint8_t device, uint8_t sw, bool closed)
{
    if (device >= ADGS_MAIN_DEVICES || sw >= ADGS_NUM_SWITCHES) return;
    if (!s_mux_initialized) return;

#if ADGS_HAS_SELFTEST
    // Interlock: block U16 S3 close if U23 is active
    if (device == U16_DEVICE_IDX && sw == 2 && closed && adgs_selftest_active()) {
        ESP_LOGE(TAG, "INTERLOCK: Cannot close U16 S3 while U23 self-test is active!");
        return;
    }
#endif

    uint8_t group_mask = get_group_mask(sw);
    uint8_t new_state;

    if (closed) {
        // Closing a switch: break-before-make — first open all switches in the
        // same group to prevent momentary shorts, then close the requested one.
        uint8_t temp_state[ADGS_NUM_DEVICES];
        memcpy(temp_state, s_mux_state, ADGS_NUM_DEVICES);
        temp_state[device] &= ~group_mask;  // Open the entire group
        adgs_write_states(temp_state);

        // Wait dead time before closing
        delay_ms(ADGS_DEAD_TIME_MS);

        // Close the requested switch
        new_state = (s_mux_state[device] & ~group_mask) | (1 << sw);
    } else {
        // Opening a switch: no dead time needed
        new_state = s_mux_state[device] & ~(1 << sw);
    }

    s_mux_state[device] = new_state;
    adgs_write_states(s_mux_state);
    sync_api_main_from_physical();
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

void adgs_get_api_states(uint8_t out[ADGS_API_MAIN_DEVICES])
{
    sync_api_main_from_physical();
    memcpy(out, s_api_main_state, ADGS_API_MAIN_DEVICES);
}

void adgs_set_api_all_safe(const uint8_t states[ADGS_API_MAIN_DEVICES])
{
    memcpy(s_api_main_state, states, ADGS_API_MAIN_DEVICES);

    uint8_t physical[ADGS_MAIN_DEVICES];
    for (uint8_t i = 0; i < ADGS_MAIN_DEVICES; i++) {
        physical[i] = states[i];
    }
    adgs_set_all_safe(physical);
    sync_api_main_from_physical();
}

bool adgs_set_api_switch_safe(uint8_t device, uint8_t sw, bool closed)
{
    if (device >= ADGS_API_MAIN_DEVICES || sw >= ADGS_NUM_SWITCHES) {
        return false;
    }

    if (device < ADGS_MAIN_DEVICES) {
        adgs_set_switch_safe(device, sw, closed);
        sync_api_main_from_physical();
        return true;
    }

    uint8_t group_mask = get_group_mask(sw);
    if (closed) {
        s_api_main_state[device] = (s_api_main_state[device] & ~group_mask) | (1u << sw);
    } else {
        s_api_main_state[device] &= ~(1u << sw);
    }
    return true;
}

void adgs_reset_all(void)
{
    memset(s_mux_state, 0, sizeof(s_mux_state));
    memset(s_api_main_state, 0, sizeof(s_api_main_state));
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
#if ADGS_NUM_DEVICES > 1
    ESP_LOGE(TAG,
             "Address-mode test unavailable in daisy-chain builds. "
             "The datasheet requires a hardware reset to exit daisy-chain mode.");
    (void)sw_data;
    return 0xFF;
#else
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
#endif
}

bool adgs_readback_verify(uint8_t out[ADGS_NUM_DEVICES])
{
    if (!s_mux_initialized) return false;

#if ADGS_NUM_DEVICES > 1
    adgs_daisy_chain_readback(out);
#else
    out[0] = adgs_address_mode_read(ADGS_REG_SW_DATA);
#endif

    // Compare with cached state
    for (int i = 0; i < ADGS_NUM_DEVICES; i++) {
        if (out[i] != s_mux_state[i]) return false;
    }
    return true;
}

uint8_t adgs_read_error_flags(void)
{
#if ADGS_NUM_DEVICES == 1
    // Only works in address mode (single device)
    return adgs_address_mode_read(ADGS_REG_ERR_FLAGS);
#else
    // In daisy-chain mode, can't read individual device registers.
    // Would need to exit DC mode to read — not safe during operation.
    return 0;
#endif
}

void adgs_clear_error_flags(void)
{
#if ADGS_NUM_DEVICES == 1
    uint8_t tx[2] = { ADGS_ERR_CLEAR_HI, ADGS_ERR_CLEAR_LO };
    uint8_t rx[2] = {0};
    spi_transfer(tx, rx, 2);
    ESP_LOGI(TAG, "Error flags cleared");
#endif
}

bool adgs_is_faulted(void)
{
    return s_mux_faulted;
}

void adgs_soft_reset(void)
{
#if ADGS_NUM_DEVICES > 1
    ESP_LOGE(TAG,
             "Software reset unavailable in daisy-chain mode. "
             "The datasheet requires a hardware reset to exit daisy-chain mode.");
    return;
#else
    adgs_address_mode_write(ADGS_REG_SOFT_RESET, ADGS_SOFT_RESET_VAL1);
    delay_ms(1);
    adgs_address_mode_write(ADGS_REG_SOFT_RESET, ADGS_SOFT_RESET_VAL2);
    // ADGS2414D datasheet requires 120 µs minimum POR delay after reset.
    // Using 10 ms here provides >80x margin for reliable initialization.
    delay_ms(10);
    s_mux_initialized = false;
    memset(s_mux_state, 0, sizeof(s_mux_state));
    s_mux_faulted = false;
    s_readback_available = true;
    s_readback_checked = false;
    ESP_LOGI(TAG, "Soft reset complete");
#endif
}

// -----------------------------------------------------------------------------
// Self-Test MUX (U23) — device index ADGS_SELFTEST_DEV
// Only available when ADGS_HAS_SELFTEST == 1 (PCB mode)
// -----------------------------------------------------------------------------

#if ADGS_HAS_SELFTEST

bool adgs_set_selftest(uint8_t sw_byte)
{
    if (!s_mux_initialized) return false;

    // Safety interlock: U16 S3 must be open before ANY U23 switch can close
    if (sw_byte != 0 && adgs_u16_s3_active()) {
        ESP_LOGE(TAG, "INTERLOCK: Cannot activate U23 while U16 S3 (IO 12 analog) is closed!");
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

bool adgs_u16_s3_active(void)
{
    return (s_mux_state[U16_DEVICE_IDX] & U16_S3_MASK) != 0;
}

bool adgs_selftest_active(void)
{
    return s_mux_state[ADGS_SELFTEST_DEV] != 0;
}

#endif // ADGS_HAS_SELFTEST
