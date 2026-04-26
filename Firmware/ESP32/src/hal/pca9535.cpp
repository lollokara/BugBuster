// =============================================================================
// pca9535.cpp - PCA9535AHF 16-bit I2C GPIO Expander Driver
// =============================================================================

#include "pca9535.h"
#include "i2c_bus.h"
#include "config.h"
#include "selftest.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "pca9535";

static PCA9535State s_state = {};

// Forward declarations
static void check_changes(uint8_t old_input0, uint8_t new_input0,
                           uint8_t old_input1, uint8_t new_input1);

static pca9535_fault_cb_t s_fault_cb = NULL;
static PcaFaultConfig s_fault_cfg = { .auto_disable_efuse = true, .log_events = true };
static bool s_change_detect_armed = false;  // Skip first update (no valid previous state)
static bool s_is_pcal9535a = false;         // True if PCAL9535A detected (enhanced regs respond)

static bool read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_bus_write_read(PCA9535_I2C_ADDR, &reg, 1, val, 1, 50);
}

static bool write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_bus_write(PCA9535_I2C_ADDR, buf, 2, 50);
}

// Write with read-back verification. Only compares bits in mask (output bits).
// Retries up to 3 times on mismatch.
static bool write_reg_verified(uint8_t reg, uint8_t val, uint8_t mask)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!write_reg(reg, val)) {
            ESP_LOGW(TAG, "Write reg 0x%02X failed (attempt %d)", reg, attempt + 1);
            delay_ms(1);
            continue;
        }

        uint8_t readback = 0;
        if (!read_reg(reg, &readback)) {
            ESP_LOGW(TAG, "Readback reg 0x%02X failed (attempt %d)", reg, attempt + 1);
            delay_ms(1);
            continue;
        }

        if ((readback & mask) == (val & mask)) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "Write-verify reg 0x%02X succeeded on retry %d", reg, attempt);
            }
            return true;
        }
        ESP_LOGW(TAG, "Write-verify mismatch reg 0x%02X: wrote=0x%02X read=0x%02X (mask=0x%02X, attempt %d)",
                 reg, val, readback, mask, attempt + 1);
        delay_ms(1);
    }
    ESP_LOGE(TAG, "Write-verify FAILED reg 0x%02X after 3 retries", reg);
    return false;
}

// Decode input registers into state booleans
static void decode_inputs(void)
{
    s_state.vadj1_pg = (s_state.input0 & PCA9535_VADJ1_PG) != 0;
    s_state.vadj2_pg = (s_state.input0 & PCA9535_VADJ2_PG) != 0;

    // E-Fuse faults are active LOW (fault when pin is low), but only meaningful
    // when the corresponding e-fuse output is enabled.
    // PCB silkscreen cross: physical P3 wired to EFUSE_EN_4, P4 to EFUSE_EN_3 — swap here so tools see natural efuse1..4 order.
    bool raw_fault[4] = {
        (s_state.input1 & PCA9535_EFUSE_FLT_1) == 0,
        (s_state.input1 & PCA9535_EFUSE_FLT_2) == 0,
        (s_state.input1 & PCA9535_EFUSE_FLT_4) == 0,
        (s_state.input1 & PCA9535_EFUSE_FLT_3) == 0,
    };
    for (int i = 0; i < 4; i++) {
        s_state.efuse_flt[i] = s_state.efuse_en[i] && raw_fault[i];
    }
}

// Decode output registers into state booleans
static void decode_outputs(void)
{
    // Expose LOGIC_EN via logic_pg for host API compatibility.
    s_state.logic_pg = (s_state.output0 & PCA9535_LOGIC_EN) != 0;
    s_state.vadj1_en  = (s_state.output0 & PCA9535_VADJ1_EN) != 0;
    s_state.vadj2_en  = (s_state.output0 & PCA9535_VADJ2_EN) != 0;
    s_state.en_15v    = (s_state.output0 & PCA9535_EN_15V_A) != 0;
    // Expose LOGIC_EN in en_mux as well so existing UI control index 3 can be
    // repurposed from legacy EN_MUX to LOGIC_EN without breaking the wire API.
    s_state.en_mux    = (s_state.output0 & PCA9535_LOGIC_EN) != 0;
    s_state.en_usb_hub = (s_state.output0 & PCA9535_EN_USB_HUB) != 0;

    s_state.efuse_en[0] = (s_state.output1 & PCA9535_EFUSE_EN_1) != 0;
    s_state.efuse_en[1] = (s_state.output1 & PCA9535_EFUSE_EN_2) != 0;
    s_state.efuse_en[2] = (s_state.output1 & PCA9535_EFUSE_EN_4) != 0;  // PCB cross: P3→EFUSE_EN_4 pin
    s_state.efuse_en[3] = (s_state.output1 & PCA9535_EFUSE_EN_3) != 0;  // PCB cross: P4→EFUSE_EN_3 pin
}

bool pca9535_init(void)
{
    memset(&s_state, 0, sizeof(s_state));

    if (!i2c_bus_ready()) {
        ESP_LOGW(TAG, "I2C bus not ready");
        return false;
    }

    s_state.present = i2c_bus_probe(PCA9535_I2C_ADDR);
    if (!s_state.present) {
        ESP_LOGW(TAG, "PCA9535 not found at 0x%02X", PCA9535_I2C_ADDR);
        return false;
    }

    // Part detection: PCAL9535A implements enhanced register window at 0x40+;
    // legacy PCA9535 either NACKs those addresses or wraps back to the base window.
    // Probe by writing a known pattern to INT_MASK0 (POR default 0xFF) and reading back.
    // If the write sticks, we have a PCAL9535A; otherwise assume legacy PCA9535.
    s_is_pcal9535a = false;
    {
        // Try writing 0xAA — distinct from POR 0xFF, not equal to base-window wrap values.
        uint8_t probe_buf[2] = { PCAL_REG_INT_MASK0, 0xAA };
        if (i2c_bus_write(PCA9535_I2C_ADDR, probe_buf, 2, 50)) {
            uint8_t reg = PCAL_REG_INT_MASK0;
            uint8_t readback = 0;
            if (i2c_bus_write_read(PCA9535_I2C_ADDR, &reg, 1, &readback, 1, 50) &&
                readback == 0xAA) {
                s_is_pcal9535a = true;
            }
        }
    }
    ESP_LOGI(TAG, "%s found at 0x%02X",
             s_is_pcal9535a ? "PCAL9535A detected" : "PCA9535 detected (legacy)",
             PCA9535_I2C_ADDR);

    // Configure pin directions (verified writes — critical for safe operation)
    // Port 0: config register - 1=input, 0=output
    // Inputs: P0.1 (VADJ1_PG), P0.4 (VADJ2_PG)
    // Outputs: P0.0, P0.2, P0.3, P0.5, P0.6, P0.7
    uint8_t config0 = PCA9535_PORT0_INPUT_MASK;  // 1=input for PG pins, 0=output for EN pins
    if (!write_reg_verified(PCA9535_REG_CONFIG0, config0, 0xFF)) {
        ESP_LOGE(TAG, "Failed to configure Port 0 direction");
        return false;
    }

    // Port 1: EN pins as output (even bits), FLT pins as input (odd bits)
    uint8_t config1 = PCA9535_PORT1_INPUT_MASK;  // 1=input for FLT, 0=output for EN
    if (!write_reg_verified(PCA9535_REG_CONFIG1, config1, 0xFF)) {
        ESP_LOGE(TAG, "Failed to configure Port 1 direction");
        return false;
    }

    // Boot defaults:
    // - Keep LOGIC_EN, EN_15V_A, and EN_USB_HUB asserted immediately after startup.
    // - Leave VADJ rails and e-fuse enables OFF by default.
    s_state.output0 = (uint8_t)(PCA9535_LOGIC_EN | PCA9535_EN_15V_A | PCA9535_EN_USB_HUB);
    s_state.output1 = 0x00;
    if (!write_reg_verified(PCA9535_REG_OUTPUT0, s_state.output0, PCA9535_PORT0_OUTPUT_MASK)) return false;
    if (!write_reg_verified(PCA9535_REG_OUTPUT1, s_state.output1, PCA9535_PORT1_OUTPUT_MASK)) return false;

    // No polarity inversion — verify to catch stuck registers
    if (!write_reg_verified(PCA9535_REG_POLAR0, 0x00, 0xFF)) {
        ESP_LOGE(TAG, "Polarity inversion Port 0 stuck non-zero!");
    }
    if (!write_reg_verified(PCA9535_REG_POLAR1, 0x00, 0xFF)) {
        ESP_LOGE(TAG, "Polarity inversion Port 1 stuck non-zero!");
    }

    // PCAL9535A-only: unmask the input bits we care about so INT fires.
    // Chip POR masks every interrupt (0x4A/0x4B = 0xFF) — on PCB we route INT → ESP32 GPIO4,
    // so we must explicitly clear mask bits for the signals we monitor.
    // Also latch the four e-fuse FLT inputs so transient trips are held until read.
    // Gated on BREADBOARD_MODE == 0 as well: breadboard build must not touch these regs
    // even if someone hand-swaps the part.
#if !BREADBOARD_MODE
    if (s_is_pcal9535a) {
        // Input latch: enable only on FLT bits (P1.1, P1.3, P1.5, P1.7).
        // Leave PG bits unlatched — they're steady-state and latching would
        // hide fast PG_LOST → PG_RESTORED cycles.
        if (!write_reg(PCAL_REG_INPUT_LATCH0, 0x00)) {
            ESP_LOGW(TAG, "Failed to write PCAL INPUT_LATCH0");
        }
        if (!write_reg(PCAL_REG_INPUT_LATCH1, PCA9535_PORT1_INPUT_MASK)) {
            ESP_LOGW(TAG, "Failed to write PCAL INPUT_LATCH1");
        }

        // Interrupt mask: bit=0 unmasks. Unmask inputs (PG + FLT); mask outputs.
        // Port 0: PG inputs at bits 0,1,4 — unmask those, mask everything else.
        uint8_t int_mask0 = (uint8_t)(~PCA9535_PORT0_INPUT_MASK);
        if (!write_reg(PCAL_REG_INT_MASK0, int_mask0)) {
            ESP_LOGW(TAG, "Failed to write PCAL INT_MASK0");
        }
        // Port 1: FLT inputs at bits 1,3,5,7 — unmask those.
        uint8_t int_mask1 = (uint8_t)(~PCA9535_PORT1_INPUT_MASK);
        if (!write_reg(PCAL_REG_INT_MASK1, int_mask1)) {
            ESP_LOGW(TAG, "Failed to write PCAL INT_MASK1");
        }

        ESP_LOGI(TAG, "PCAL9535A: INT unmasked (mask0=0x%02X mask1=0x%02X), FLT inputs latched",
                 int_mask0, int_mask1);
    }
#endif

    // Read initial inputs
    pca9535_update();

    ESP_LOGI(TAG, "PCA9535 configured: LOGIC_EN/EN_15V_A/EN_USB_HUB ON, others OFF");
    ESP_LOGI(TAG, "  LOGIC_EN=%d VADJ1_PG=%d VADJ2_PG=%d",
             s_state.logic_pg, s_state.vadj1_pg, s_state.vadj2_pg);

    return true;
}

bool pca9535_present(void)
{
    return s_state.present;
}

const PCA9535State* pca9535_get_state(void)
{
    return &s_state;
}

bool pca9535_update(void)
{
    if (!s_state.present) return false;

    uint8_t old_in0 = s_state.input0;
    uint8_t old_in1 = s_state.input1;

    bool ok = true;
    ok &= read_reg(PCA9535_REG_INPUT0, &s_state.input0);
    ok &= read_reg(PCA9535_REG_INPUT1, &s_state.input1);

    if (ok) {
        // Outputs first so decode_inputs can correctly gate FLT by efuse_en.
        decode_outputs();
        decode_inputs();
        check_changes(old_in0, s_state.input0, old_in1, s_state.input1);
        if (!s_change_detect_armed) {
            s_change_detect_armed = true;  // Arm after first successful read
        }
    }
    return ok;
}

bool pca9535_set_control(PcaControl ctrl, bool on)
{
    if (!s_state.present) return false;

    switch (ctrl) {
        case PCA_CTRL_VADJ1_EN:
            return pca9535_set_bit(0, 2, on);
        case PCA_CTRL_VADJ2_EN:
            return pca9535_set_bit(0, 3, on);
        case PCA_CTRL_15V_EN:
            return pca9535_set_bit(0, 5, on);
        case PCA_CTRL_MUX_EN:
            // Legacy slot reused as LOGIC_EN control on current hardware.
            return pca9535_set_bit(0, 0, on);
        case PCA_CTRL_USB_HUB_EN:
            return pca9535_set_bit(0, 7, on);
        case PCA_CTRL_EFUSE1_EN:
            return pca9535_set_bit(1, 0, on);
        case PCA_CTRL_EFUSE2_EN:
            return pca9535_set_bit(1, 2, on);
        case PCA_CTRL_EFUSE3_EN:
            return pca9535_set_bit(1, 6, on);  // PCB cross: logical EFUSE3 drives physical P3 (bit 6 = EFUSE_EN_4 pin)
        case PCA_CTRL_EFUSE4_EN:
            return pca9535_set_bit(1, 4, on);  // PCB cross: logical EFUSE4 drives physical P4 (bit 4 = EFUSE_EN_3 pin)
        default:
            return false;
    }
}

bool pca9535_get_status(PcaStatus status)
{
    switch (status) {
        case PCA_STATUS_LOGIC_PG:   return s_state.logic_pg;
        case PCA_STATUS_VADJ1_PG:   return s_state.vadj1_pg;
        case PCA_STATUS_VADJ2_PG:   return s_state.vadj2_pg;
        case PCA_STATUS_EFUSE1_FLT: return s_state.efuse_flt[0];
        case PCA_STATUS_EFUSE2_FLT: return s_state.efuse_flt[1];
        case PCA_STATUS_EFUSE3_FLT: return s_state.efuse_flt[2];
        case PCA_STATUS_EFUSE4_FLT: return s_state.efuse_flt[3];
        default: return false;
    }
}

bool pca9535_set_port(uint8_t port, uint8_t val)
{
    if (!s_state.present) return false;
    if (port > 1) return false;

    uint8_t reg = (port == 0) ? PCA9535_REG_OUTPUT0 : PCA9535_REG_OUTPUT1;
    // Only write bits that are configured as outputs
    uint8_t out_mask = (port == 0) ? PCA9535_PORT0_OUTPUT_MASK : PCA9535_PORT1_OUTPUT_MASK;
    uint8_t *cached = (port == 0) ? &s_state.output0 : &s_state.output1;

    *cached = (*cached & ~out_mask) | (val & out_mask);
    bool ok = write_reg_verified(reg, *cached, out_mask);
    if (ok) decode_outputs();
    return ok;
}

bool pca9535_read_port(uint8_t port, uint8_t *val)
{
    if (!s_state.present) return false;
    if (port > 1) return false;

    uint8_t reg = (port == 0) ? PCA9535_REG_INPUT0 : PCA9535_REG_INPUT1;
    return read_reg(reg, val);
}

bool pca9535_set_bit(uint8_t port, uint8_t bit, bool val)
{
    if (!s_state.present) return false;
    if (port > 1 || bit > 7) return false;

    uint8_t reg = (port == 0) ? PCA9535_REG_OUTPUT0 : PCA9535_REG_OUTPUT1;
    uint8_t *cached = (port == 0) ? &s_state.output0 : &s_state.output1;
    uint8_t mask = (1 << bit);

    if (val) {
        *cached |= mask;
    } else {
        *cached &= ~mask;
    }

    bool ok = write_reg_verified(reg, *cached, mask);
    if (ok) decode_outputs();
    return ok;
}

const char* pca9535_control_name(PcaControl ctrl)
{
    switch (ctrl) {
        case PCA_CTRL_VADJ1_EN:   return "VADJ1_EN";
        case PCA_CTRL_VADJ2_EN:   return "VADJ2_EN";
        case PCA_CTRL_15V_EN:     return "EN_15V_A";
        case PCA_CTRL_MUX_EN:     return "LOGIC_EN";
        case PCA_CTRL_USB_HUB_EN: return "EN_USB_HUB";
        case PCA_CTRL_EFUSE1_EN:  return "EFUSE_EN_1";
        case PCA_CTRL_EFUSE2_EN:  return "EFUSE_EN_2";
        case PCA_CTRL_EFUSE3_EN:  return "EFUSE_EN_3";
        case PCA_CTRL_EFUSE4_EN:  return "EFUSE_EN_4";
        default: return "UNKNOWN";
    }
}

const char* pca9535_status_name(PcaStatus status)
{
    switch (status) {
        case PCA_STATUS_LOGIC_PG:   return "LOGIC_EN";
        case PCA_STATUS_VADJ1_PG:   return "VADJ1_PG";
        case PCA_STATUS_VADJ2_PG:   return "VADJ2_PG";
        case PCA_STATUS_EFUSE1_FLT: return "EFUSE_FLT_1";
        case PCA_STATUS_EFUSE2_FLT: return "EFUSE_FLT_2";
        case PCA_STATUS_EFUSE3_FLT: return "EFUSE_FLT_3";
        case PCA_STATUS_EFUSE4_FLT: return "EFUSE_FLT_4";
        default: return "UNKNOWN";
    }
}

// --- Fault detection ---

static void check_changes(uint8_t old_input0, uint8_t new_input0,
                           uint8_t old_input1, uint8_t new_input1)
{
    if (!s_change_detect_armed) return;

    uint32_t now = millis_now();

    // Check Port 0 power-good changes (bits 1, 4)
    uint8_t pg_changed = (old_input0 ^ new_input0) & PCA9535_PORT0_INPUT_MASK;
    if (pg_changed) {
        // During active self-test calibration we intentionally toggle rails.
        // Suppress PG callbacks to keep ISR-path work minimal and avoid
        // feeding expected transitions into the generic fault path.
        bool suppress_pg_events = selftest_is_busy();
        struct { uint8_t mask; uint8_t ch; const char *name; } pg_pins[] = {
            { PCA9535_VADJ1_PG, 1, "VADJ1_PG" },
            { PCA9535_VADJ2_PG, 2, "VADJ2_PG" },
        };
        for (int i = 0; i < 2; i++) {
            if (pg_changed & pg_pins[i].mask) {
                bool restored = (new_input0 & pg_pins[i].mask) != 0;
                PcaFaultEvent evt = {
                    .type = restored ? PCA_FAULT_PG_RESTORED : PCA_FAULT_PG_LOST,
                    .channel = pg_pins[i].ch,
                    .timestamp_ms = now,
                };
                if (s_fault_cfg.log_events) {
                    ESP_LOGW(TAG, "%s %s", pg_pins[i].name, restored ? "RESTORED" : "LOST");
                }
                if (!suppress_pg_events && s_fault_cb) s_fault_cb(&evt);
            }
        }
    }

    // Check Port 1 e-fuse fault changes (odd bits: 1, 3, 5, 7)
    uint8_t flt_changed = (old_input1 ^ new_input1) & PCA9535_PORT1_INPUT_MASK;
    if (flt_changed) {
        for (int i = 0; i < 4; i++) {
            uint8_t flt_mask = (uint8_t)(PCA9535_EFUSE_FLT_1 << (i * 2));
            if (flt_changed & flt_mask) {
                bool faulted = (new_input1 & flt_mask) == 0;  // active low
                if (!s_state.efuse_en[i]) {
                    // Ignore fault transitions while the corresponding e-fuse is disabled.
                    // Some boards briefly assert FLT during output-enable transitions.
                    continue;
                }
                PcaFaultEvent evt = {
                    .type = faulted ? PCA_FAULT_EFUSE_TRIP : PCA_FAULT_EFUSE_CLEAR,
                    .channel = (uint8_t)i,
                    .timestamp_ms = now,
                };
                if (s_fault_cfg.log_events) {
                    ESP_LOGW(TAG, "EFUSE_%d %s", i + 1, faulted ? "FAULT — tripped!" : "CLEARED");
                }

                // Auto-disable faulted e-fuse
                if (faulted && s_fault_cfg.auto_disable_efuse) {
                    ESP_LOGW(TAG, "Auto-disabling EFUSE_%d", i + 1);
                    pca9535_set_control((PcaControl)(PCA_CTRL_EFUSE1_EN + i), false);
                }

                if (s_fault_cb) s_fault_cb(&evt);
            }
        }
    }
}

void pca9535_set_fault_config(const PcaFaultConfig *cfg)
{
    if (cfg) s_fault_cfg = *cfg;
}

void pca9535_register_fault_callback(pca9535_fault_cb_t cb)
{
    s_fault_cb = cb;
}

bool pca9535_any_fault_active(void)
{
    if (!s_state.present) return false;
    for (int i = 0; i < 4; i++) {
        if (s_state.efuse_flt[i]) return true;
    }
    return false;
}

// --- Interrupt-driven input monitoring ---

static TaskHandle_t s_isr_task = NULL;

static void IRAM_ATTR pca_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_isr_task) {
        vTaskNotifyGiveFromISR(s_isr_task, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void pca_isr_task(void* /*pvParameters*/)
{
    for (;;) {
        // Block until ISR fires (or timeout every 2s as fallback poll)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        if (s_state.present) {
            // PCAL9535A: read INT_STATUS first for attribution logging.
            // Not required for functional correctness — reading input regs in
            // pca9535_update() also clears INT — but the status bits tell us
            // exactly which pin fired.
            if (s_is_pcal9535a) {
                uint8_t st0 = 0, st1 = 0;
                if (read_reg(PCAL_REG_INT_STATUS0, &st0) &&
                    read_reg(PCAL_REG_INT_STATUS1, &st1)) {
                    if (st0 || st1) {
                        ESP_LOGD(TAG, "INT fired: status0=0x%02X status1=0x%02X", st0, st1);
                    }
                }
            }
            pca9535_update();
        }
    }
}

void pca9535_install_isr(void)
{
    if (PIN_MUX_INT == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "PCA9535 INT pin not configured (breadboard mode)");
        return;
    }
    if (!s_state.present) {
        ESP_LOGW(TAG, "PCA9535 not present, skipping ISR install");
        return;
    }

    // Create the deferred handler task (Core 0, low priority — I2C reads)
    BaseType_t t_ok = xTaskCreatePinnedToCore(pca_isr_task, "pcaISR", 4096, NULL, 2, &s_isr_task, 0);
    if (t_ok != pdPASS || s_isr_task == NULL) {
        ESP_LOGE(TAG, "Failed to create pcaISR task");
        return;
    }

    // Configure INT pin: active-low, falling edge
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << (uint64_t)PIN_MUX_INT);
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_conf.intr_type    = GPIO_INTR_NEGEDGE;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure INT GPIO%d: %s",
                 (int)PIN_MUX_INT, esp_err_to_name(err));
        return;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return;
    }

    err = gpio_isr_handler_add(PIN_MUX_INT, pca_isr_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed on GPIO%d: %s",
                 (int)PIN_MUX_INT, esp_err_to_name(err));
        return;
    }

    // Read initial state (clears any pending interrupt)
    pca9535_update();

    ESP_LOGI(TAG, "PCA9535 ISR installed on GPIO%d", PIN_MUX_INT);
}
