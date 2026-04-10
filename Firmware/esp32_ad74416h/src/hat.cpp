// =============================================================================
// hat.cpp - HAT Expansion Board Driver
//
// Handles detection (GPIO47 ADC), UART communication (GPIO43/44, 115200 8N1),
// and EXP_EXT_1-4 pin configuration for attached HAT boards.
// PCB mode only.
// =============================================================================

#include "hat.h"
#include "config.h"
#include "bbp.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "hat";

// HAT support enabled in both breadboard and PCB modes.
// In breadboard mode: no ADC detect (assume HAT present), no IRQ.
// In PCB mode: full ADC detection + IRQ support.

static HatState s_state = {};
#if !HAT_NO_DETECT
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
#endif
static bool s_initialized = false;
static uint8_t s_last_error = 0;

// Dedicated LA-done IRQ (RP2040 GPIO28 → ESP32 PIN_HAT_LA_DONE_IRQ).
// Set from the GPIO ISR on falling edge, consumed by hat_la_done_consume().
static volatile bool s_la_done_pending = false;
// Tracks whether we've registered the global GPIO ISR service so we don't
// call gpio_install_isr_service() more than once.
static bool s_gpio_isr_service_installed = false;

static void IRAM_ATTR hat_la_done_isr(void *arg)
{
    (void)arg;
    s_la_done_pending = true;
}

// -----------------------------------------------------------------------------
// CRC-8 (polynomial 0x07, same as AD74416H SPI CRC)
// -----------------------------------------------------------------------------
static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
        }
    }
    return crc;
}

// -----------------------------------------------------------------------------
// UART Helpers
// -----------------------------------------------------------------------------

// Send a command frame to the HAT. Returns true if bytes were sent.
static bool hat_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t payload_len)
{
    if (payload_len > HAT_FRAME_MAX_LEN) return false;

    uint8_t frame[3 + HAT_FRAME_MAX_LEN + 1]; // SYNC + LEN + CMD + payload + CRC
    size_t pos = 0;

    frame[pos++] = HAT_FRAME_SYNC;
    frame[pos++] = payload_len;
    frame[pos++] = cmd;
    if (payload_len > 0 && payload) {
        memcpy(&frame[pos], payload, payload_len);
        pos += payload_len;
    }

    // CRC over CMD + payload
    frame[pos] = crc8(&frame[2], 1 + payload_len);
    pos++;

    int written = uart_write_bytes(HAT_UART_NUM, frame, pos);
    return written == (int)pos;
}

// Receive a response frame from the HAT.
// Blocks up to timeout_ms. Returns response CMD byte, fills payload/payload_len.
// Returns 0 on timeout or error.
static uint8_t hat_recv_frame(uint8_t *payload, uint8_t *payload_len, uint32_t timeout_ms)
{
    uint8_t buf[3 + HAT_FRAME_MAX_LEN + 1];
    size_t pos = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t deadline = start + pdMS_TO_TICKS(timeout_ms);

    // Wait for SYNC byte
    while (xTaskGetTickCount() < deadline) {
        uint8_t b;
        int n = uart_read_bytes(HAT_UART_NUM, &b, 1, pdMS_TO_TICKS(10));
        if (n == 1 && b == HAT_FRAME_SYNC) {
            buf[pos++] = b;
            break;
        }
    }
    if (pos == 0) return 0; // No SYNC received

    // Read LEN + CMD (2 bytes)
    {
        TickType_t remaining = deadline - xTaskGetTickCount();
        if ((int32_t)remaining <= 0) return 0;
        int n = uart_read_bytes(HAT_UART_NUM, &buf[pos], 2, remaining);
        if (n != 2) return 0;
        pos += 2;
    }

    uint8_t len = buf[1];
    uint8_t cmd = buf[2];

    if (len > HAT_FRAME_MAX_LEN) return 0; // Invalid

    // Read payload + CRC (len + 1 bytes)
    if (len + 1 > 0) {
        TickType_t remaining = deadline - xTaskGetTickCount();
        if ((int32_t)remaining <= 0) return 0;
        int n = uart_read_bytes(HAT_UART_NUM, &buf[pos], len + 1, remaining);
        if (n != (int)(len + 1)) return 0;
        pos += len + 1;
    }

    // Verify CRC over CMD + payload
    uint8_t expected_crc = crc8(&buf[2], 1 + len);
    uint8_t received_crc = buf[3 + len];
    if (expected_crc != received_crc) {
        ESP_LOGW(TAG, "CRC mismatch: expected 0x%02X, got 0x%02X", expected_crc, received_crc);
        return 0;
    }

    // Copy payload out
    if (payload && len > 0) {
        memcpy(payload, &buf[3], len);
    }
    if (payload_len) {
        *payload_len = len;
    }

    return cmd;
}

// Send command and wait for response. Returns response CMD, fills rsp_payload.
static uint8_t hat_command(uint8_t cmd, const uint8_t *payload, uint8_t payload_len,
                            uint8_t *rsp_payload, uint8_t *rsp_len, uint32_t timeout_ms)
{
    // Aggressively flush any stale data (e.g. unsolicited LA notifications)
    uart_flush_input(HAT_UART_NUM);
    {
        uint8_t drain[64];
        size_t avail = 0;
        uart_get_buffered_data_len(HAT_UART_NUM, &avail);
        while (avail > 0) {
            int n = uart_read_bytes(HAT_UART_NUM, drain, sizeof(drain), 0);
            if (n <= 0) break;
            uart_get_buffered_data_len(HAT_UART_NUM, &avail);
        }
    }

    s_last_error = 0;
    ESP_LOGD(TAG, "TX cmd=0x%02X len=%d", cmd, payload_len);

    if (!hat_send_frame(cmd, payload, payload_len)) {
        ESP_LOGW(TAG, "Failed to send command 0x%02X", cmd);
        return 0;
    }

    uint8_t rsp = hat_recv_frame(rsp_payload, rsp_len, timeout_ms);
    if (rsp == HAT_RSP_ERROR && rsp_payload && rsp_len && *rsp_len >= 1) {
        s_last_error = rsp_payload[0];
        ESP_LOGW(TAG, "HAT command 0x%02X failed with error 0x%02X", cmd, s_last_error);
    }
    ESP_LOGD(TAG, "RX rsp=0x%02X len=%d (for cmd=0x%02X)", rsp, rsp_len ? *rsp_len : 0, cmd);
    return rsp;
}

// -----------------------------------------------------------------------------
// ADC Detection
// -----------------------------------------------------------------------------

#if !HAT_NO_DETECT
static float hat_read_detect_voltage(void)
{
    if (!s_adc_handle) return -1.0f;

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, ADC_CHANNEL_6, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(err));
        return -1.0f;
    }

    // ESP32-S3 ADC: 12-bit, 0-3.3V (with default attenuation)
    // GPIO47 = ADC1_CH6 on ESP32-S3
    float voltage = (float)raw / 4095.0f * 3.3f;
    return voltage;
}

static HatType voltage_to_hat_type(float v)
{
    if (v < 0.0f) return HAT_TYPE_UNKNOWN;
    if (v > 2.5f) return HAT_TYPE_NONE;           // Pull-up only: no HAT (~3.3V)
    if (v > 1.2f && v < 2.1f) return HAT_TYPE_SWD_GPIO;  // 10k/10k divider (~1.65V)
    // Future HAT types would have additional voltage bands here:
    // if (v > 0.8f && v < 1.2f) return HAT_TYPE_ANALOG;   // 4.7k pull-down (~1.06V)
    // if (v > 2.1f && v < 2.5f) return HAT_TYPE_PROTOCOL;  // 22k pull-down (~2.27V)
    return HAT_TYPE_UNKNOWN;
}
#endif

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

bool hat_init(void)
{
    memset(&s_state, 0, sizeof(s_state));

    // Initialize ADC for detect pin (if available)
#if !HAT_NO_DETECT
    {
        adc_oneshot_unit_init_cfg_t adc_cfg = {
            .unit_id = ADC_UNIT_1,
        };
        esp_err_t err = adc_oneshot_new_unit(&adc_cfg, &s_adc_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
            // Continue without detect — will try UART ping instead
        } else {
            adc_oneshot_chan_cfg_t chan_cfg = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_6, &chan_cfg);
        }
    }
#else
    ESP_LOGI(TAG, "No detect pin (breadboard) — will probe via UART ping");
#endif

    // Initialize UART for HAT communication
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = HAT_UART_BAUD;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_param_config(HAT_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(HAT_UART_NUM, PIN_HAT_TX, PIN_HAT_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART pin config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_driver_install(HAT_UART_NUM, HAT_UART_BUF_SIZE, HAT_UART_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return false;
    }

    // Configure IRQ pin as open-drain input (shared line), if available
#if !HAT_NO_DETECT
    if ((int)PIN_HAT_IRQ >= 0) {
        gpio_config_t irq_cfg = {
            .pin_bit_mask = (1ULL << PIN_HAT_IRQ),
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&irq_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HAT IRQ pin config failed: %s (non-fatal)", esp_err_to_name(err));
        } else {
            gpio_set_level(PIN_HAT_IRQ, 1);
        }
    }
#endif

    // Configure the dedicated LA-done IRQ input from the RP2040 (active low,
    // falling-edge interrupt). Uses the internal pull-up since the RP2040
    // side drives push-pull only during the brief done pulse.
    if ((int)PIN_HAT_LA_DONE_IRQ >= 0) {
        gpio_config_t la_done_cfg = {
            .pin_bit_mask = (1ULL << PIN_HAT_LA_DONE_IRQ),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        err = gpio_config(&la_done_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LA-done IRQ pin config failed: %s (non-fatal)", esp_err_to_name(err));
        } else {
            if (!s_gpio_isr_service_installed) {
                esp_err_t isr_err = gpio_install_isr_service(0);
                // ESP_ERR_INVALID_STATE means it was already installed by
                // another subsystem — that's fine, we can still add a handler.
                if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
                    s_gpio_isr_service_installed = true;
                } else {
                    ESP_LOGW(TAG, "gpio_install_isr_service failed: %s",
                             esp_err_to_name(isr_err));
                }
            }
            if (s_gpio_isr_service_installed) {
                esp_err_t add_err = gpio_isr_handler_add(
                    PIN_HAT_LA_DONE_IRQ, hat_la_done_isr, NULL);
                if (add_err != ESP_OK) {
                    ESP_LOGW(TAG, "LA-done ISR handler add failed: %s",
                             esp_err_to_name(add_err));
                } else {
                    ESP_LOGI(TAG, "LA-done IRQ armed on GPIO%d (falling edge)",
                             (int)PIN_HAT_LA_DONE_IRQ);
                }
            }
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "HAT subsystem initialized (UART%d: GPIO%d TX, GPIO%d RX, %d baud)",
             HAT_UART_NUM, PIN_HAT_TX, PIN_HAT_RX, HAT_UART_BAUD);

    // Initial detection
    hat_detect();

    if (s_state.detected) {
        ESP_LOGI(TAG, "HAT detected: %s (%.2fV)", hat_type_name(s_state.type), s_state.detect_voltage);
        // Try to connect
        if (hat_connect()) {
            ESP_LOGI(TAG, "HAT connected: fw v%d.%d", s_state.fw_version_major, s_state.fw_version_minor);
        } else {
            ESP_LOGW(TAG, "HAT detected but UART connection failed");
        }
    } else {
        ESP_LOGI(TAG, "No HAT detected (%.2fV)", s_state.detect_voltage);
    }

    return true;
}

bool hat_detected(void)
{
    return s_state.detected;
}

const HatState* hat_get_state(void)
{
    return &s_state;
}

HatType hat_detect(void)
{
    // If no detect pin (breadboard mode), assume HAT might be present — probe via UART
#if HAT_NO_DETECT
    ESP_LOGI(TAG, "No detect pin — trying UART ping...");
    s_state.detect_voltage = 0.0f;
    s_state.type = HAT_TYPE_SWD_GPIO;  // Assume SWD/GPIO for breadboard test
    s_state.detected = true;            // Will be confirmed/denied by hat_connect()
    return s_state.type;
#else
    // Average multiple ADC readings for stability
    float sum = 0.0f;
    int valid = 0;
    for (int i = 0; i < 8; i++) {
        float v = hat_read_detect_voltage();
        if (v >= 0.0f) {
            sum += v;
            valid++;
        }
        delay_ms(2);
    }

    if (valid == 0) {
        s_state.detected = false;
        s_state.type = HAT_TYPE_UNKNOWN;
        s_state.detect_voltage = -1.0f;
        return HAT_TYPE_UNKNOWN;
    }

    float avg_v = sum / (float)valid;
    s_state.detect_voltage = avg_v;
    s_state.type = voltage_to_hat_type(avg_v);
    s_state.detected = (s_state.type != HAT_TYPE_NONE && s_state.type != HAT_TYPE_UNKNOWN);

    return s_state.type;
#endif  // HAT_NO_DETECT
}

bool hat_connect(void)
{
    if (!s_initialized || !s_state.detected) return false;

    // Send PING command
    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_PING, NULL, 0, rsp, &rsp_len, 200);

    if (cmd != HAT_RSP_OK) {
        s_state.connected = false;
        return false;
    }

    s_state.connected = true;
    s_state.last_ping_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    // Query HAT info
    cmd = hat_command(HAT_CMD_GET_INFO, NULL, 0, rsp, &rsp_len, 200);
    if (cmd == HAT_RSP_INFO && rsp_len >= 3) {
        s_state.type = (HatType)rsp[0];
        s_state.fw_version_major = rsp[1];
        s_state.fw_version_minor = rsp[2];
    }

    // Query current pin config
    hat_get_pin_config(s_state.pin_config);

    return true;
}

// Enum slots 1..4 are reserved (formerly SWDIO/SWCLK/TRACE1/TRACE2).
// SWD now lives on the dedicated 3-pin connector — these function codes
// are no longer assignable to EXP_EXT pins.
static inline bool hat_func_is_reserved(HatPinFunction func)
{
    return (uint8_t)func >= 1 && (uint8_t)func <= 4;
}

bool hat_set_pin(uint8_t ext_pin, HatPinFunction func)
{
    if (!s_state.connected) return false;
    if (ext_pin >= HAT_NUM_EXT_PINS) return false;
    if (func >= HAT_FUNC_COUNT) return false;
    if (hat_func_is_reserved(func)) {
        ESP_LOGW(TAG,
                 "EXP_EXT_%d: function code %u (SWDIO/SWCLK/TRACE) is reserved — "
                 "SWD now uses the dedicated connector, use hat_setup_swd() instead",
                 ext_pin + 1, (unsigned)func);
        s_last_error = HAT_ERR_INVALID_FUNC;
        return false;
    }

    uint8_t payload[2] = { ext_pin, (uint8_t)func };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_PIN_CONFIG, payload, 2, rsp, &rsp_len, 300);
    if (cmd == HAT_RSP_OK) {
        s_state.pin_config[ext_pin] = func;
        s_state.config_confirmed = true;
        ESP_LOGI(TAG, "EXP_EXT_%d → %s (confirmed)", ext_pin + 1, hat_func_name(func));
        return true;
    }

    ESP_LOGW(TAG, "EXP_EXT_%d config failed (rsp=0x%02X)", ext_pin + 1, cmd);
    s_state.config_confirmed = false;
    return false;
}

bool hat_set_all_pins(const HatPinFunction config[HAT_NUM_EXT_PINS])
{
    if (!s_state.connected) return false;

    uint8_t payload[HAT_NUM_EXT_PINS];
    for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
        if (config[i] >= HAT_FUNC_COUNT) return false;
        if (hat_func_is_reserved(config[i])) {
            ESP_LOGW(TAG,
                     "EXP_EXT_%d: function code %u reserved (SWD moved to dedicated connector)",
                     i + 1, (unsigned)config[i]);
            s_last_error = HAT_ERR_INVALID_FUNC;
            return false;
        }
        payload[i] = (uint8_t)config[i];
    }

    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_PIN_CONFIG, payload, HAT_NUM_EXT_PINS, rsp, &rsp_len, 300);
    if (cmd == HAT_RSP_OK) {
        for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
            s_state.pin_config[i] = config[i];
        }
        s_state.config_confirmed = true;
        ESP_LOGI(TAG, "All EXP_EXT pins configured (confirmed)");
        return true;
    }

    s_state.config_confirmed = false;
    return false;
}

bool hat_get_pin_config(HatPinFunction config[HAT_NUM_EXT_PINS])
{
    if (!s_state.connected) return false;

    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_PIN_CONFIG, NULL, 0, rsp, &rsp_len, 200);
    if (cmd == HAT_RSP_OK && rsp_len >= HAT_NUM_EXT_PINS) {
        for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
            config[i] = (HatPinFunction)rsp[i];
            s_state.pin_config[i] = config[i];
        }
        return true;
    }

    return false;
}

bool hat_reset(void)
{
    if (!s_state.connected) return false;

    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_RESET, NULL, 0, rsp, &rsp_len, 500);
    if (cmd == HAT_RSP_OK) {
        for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
            s_state.pin_config[i] = HAT_FUNC_DISCONNECTED;
        }
        s_state.config_confirmed = true;
        ESP_LOGI(TAG, "HAT reset (all pins disconnected)");
        return true;
    }

    return false;
}

const char* hat_func_name(HatPinFunction func)
{
    switch (func) {
        case HAT_FUNC_DISCONNECTED: return "Disconnected";
        case HAT_FUNC_RESERVED_1:
        case HAT_FUNC_RESERVED_2:
        case HAT_FUNC_RESERVED_3:
        case HAT_FUNC_RESERVED_4:   return "Reserved (deprecated)";
        case HAT_FUNC_GPIO1:        return "GPIO1";
        case HAT_FUNC_GPIO2:        return "GPIO2";
        case HAT_FUNC_GPIO3:        return "GPIO3";
        case HAT_FUNC_GPIO4:        return "GPIO4";
        default:                     return "Unknown";
    }
}

const char* hat_type_name(HatType type)
{
    switch (type) {
        case HAT_TYPE_NONE:      return "None";
        case HAT_TYPE_SWD_GPIO:  return "SWD/GPIO";
        case HAT_TYPE_UNKNOWN:   return "Unknown";
        default:                 return "Unknown";
    }
}

// =============================================================================
// Power Management
// =============================================================================

static bool hat_get_io_voltage(void)
{
    if (!s_state.connected) return false;

    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_IO_VOLTAGE, NULL, 0, rsp, &rsp_len, 200);
    if (cmd == HAT_RSP_OK && rsp_len >= 2) {
        uint16_t actual_mv = (uint16_t)rsp[0] | ((uint16_t)rsp[1] << 8);
        if (rsp_len >= 4) {
            actual_mv = (uint16_t)rsp[2] | ((uint16_t)rsp[3] << 8);
        }
        s_state.io_voltage_mv = actual_mv;
        if (rsp_len >= 5) {
            s_state.hvpak_part = rsp[4];
        }
        if (rsp_len >= 6) {
            s_state.hvpak_ready = rsp[5] != 0;
        }
        if (rsp_len >= 7) {
            s_state.hvpak_last_error = rsp[6];
        }
        return true;
    }
    return false;
}

bool hat_set_power(HatConnector conn, bool on)
{
    if (!s_state.connected) return false;
    if (conn > HAT_CONNECTOR_B) return false;

    uint8_t payload[2] = { (uint8_t)conn, (uint8_t)(on ? 1 : 0) };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_POWER, payload, 2, rsp, &rsp_len, 300);
    if (cmd == HAT_RSP_OK) {
        s_state.connector[conn].enabled = on;
        ESP_LOGI(TAG, "Connector %c power %s", 'A' + conn, on ? "ON" : "OFF");
        return true;
    }
    ESP_LOGW(TAG, "Connector %c power command failed", 'A' + conn);
    return false;
}

bool hat_get_power_status(void)
{
    if (!s_state.connected) return false;

    uint8_t rsp[16] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_POWER_STATUS, NULL, 0, rsp, &rsp_len, 200);
    if (cmd == HAT_RSP_POWER_STATUS && rsp_len >= 6) {
        // Connector A
        s_state.connector[0].enabled = rsp[0] != 0;
        memcpy(&s_state.connector[0].current_ma, &rsp[1], sizeof(float)); // bytes 1-4: float
        s_state.connector[0].fault = rsp[5] != 0;
        // Connector B (if present in response)
        if (rsp_len >= 12) {
            s_state.connector[1].enabled = rsp[6] != 0;
            memcpy(&s_state.connector[1].current_ma, &rsp[7], sizeof(float));
            s_state.connector[1].fault = rsp[11] != 0;
        }
        hat_get_io_voltage();
        return true;
    }
    return false;
}

bool hat_set_io_voltage(uint16_t mv)
{
    if (!s_state.connected) return false;
    if (mv < 1200 || mv > 5500) {
        ESP_LOGW(TAG, "I/O voltage %u mV out of range (1200-5500)", mv);
        return false;
    }

    uint8_t payload[2] = { (uint8_t)(mv & 0xFF), (uint8_t)(mv >> 8) };
    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_IO_VOLTAGE, payload, 2, rsp, &rsp_len, 300);
    if (cmd == HAT_RSP_OK && rsp_len >= 2) {
        uint16_t actual_mv = (uint16_t)rsp[0] | ((uint16_t)rsp[1] << 8);
        if (rsp_len >= 4) {
            actual_mv = (uint16_t)rsp[2] | ((uint16_t)rsp[3] << 8);
        }
        s_state.io_voltage_mv = actual_mv;
        if (rsp_len >= 5) {
            s_state.hvpak_part = rsp[4];
        }
        if (rsp_len >= 6) {
            s_state.hvpak_ready = rsp[5] != 0;
        }
        if (rsp_len >= 7) {
            s_state.hvpak_last_error = rsp[6];
        }
        ESP_LOGI(TAG, "I/O voltage set to %u mV (actual %u mV)", mv, actual_mv);
        return true;
    }
    return false;
}

bool hat_hvpak_request(uint8_t cmd, const uint8_t *payload, uint8_t payload_len,
                       uint8_t *rsp_payload, uint8_t *rsp_len, uint32_t timeout_ms)
{
    if (!s_state.connected) return false;
    return hat_command(cmd, payload, payload_len, rsp_payload, rsp_len, timeout_ms) == HAT_RSP_OK;
}

uint8_t hat_get_last_error(void)
{
    return s_last_error;
}

bool hat_setup_swd(uint16_t target_voltage_mv, HatConnector connector)
{
    if (!s_state.connected) return false;

    ESP_LOGI(TAG, "SWD quick-setup: %umV on connector %c", target_voltage_mv, 'A' + connector);

    // 1. Set HVPAK I/O voltage to match target
    if (!hat_set_io_voltage(target_voltage_mv)) {
        ESP_LOGE(TAG, "SWD setup: failed to set I/O voltage (hat err=0x%02X, hvpak err=0x%02X)",
                 s_last_error, s_state.hvpak_last_error);
        return false;
    }
    delay_ms(5);  // HVPAK stabilization

    // 2. Enable connector power
    if (!hat_set_power(connector, true)) {
        ESP_LOGE(TAG, "SWD setup: failed to enable connector power");
        return false;
    }
    delay_ms(50);  // Target power-up

    // 3. SWD routing is no longer done via EXP_EXT pin assignment.
    //    The new HAT PCB (2026-04-09) exposes a dedicated 3-pin SWD
    //    connector (SWDIO/SWCLK/TRACE) wired directly to the RP2040
    //    debugprobe pins. The debugprobe PIO is always running on those
    //    pins, so this function just sets voltage + power + leaves
    //    EXP_EXT alone.
    //    See .omc/specs/deep-interview-swd-exp-ext-cleanup-2026-04-09.md.

    ESP_LOGI(TAG, "SWD setup complete — dedicated SWD connector active, "
                  "connect debug tool to USB CMSIS-DAP");
    return true;
}

// =============================================================================
// SWD Management
// =============================================================================

bool hat_get_dap_status(void)
{
    if (!s_state.connected) return false;

    uint8_t rsp[16] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_DAP_STATUS, NULL, 0, rsp, &rsp_len, 200);
    if (cmd == HAT_RSP_DAP_STATUS && rsp_len >= 8) {
        s_state.dap_connected = rsp[0] != 0;
        s_state.target_detected = rsp[1] != 0;
        memcpy(&s_state.target_dpidr, &rsp[2], 4);
        // swd_clock_khz at bytes 6-7 (u16 LE) — stored for display but not in HatState currently
        return true;
    }
    return false;
}

bool hat_set_swd_clock(uint16_t khz)
{
    if (!s_state.connected) return false;

    uint8_t payload[2] = { (uint8_t)(khz & 0xFF), (uint8_t)(khz >> 8) };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_SWD_CLOCK, payload, 2, rsp, &rsp_len, 200);
    return cmd == HAT_RSP_OK;
}

// =============================================================================
// Logic Analyzer
// =============================================================================

bool hat_la_configure(uint8_t channels, uint32_t rate_hz, uint32_t depth)
{
    if (!s_state.connected) return false;

    uint8_t payload[9];
    payload[0] = channels;
    memcpy(&payload[1], &rate_hz, 4);
    memcpy(&payload[5], &depth, 4);

    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_CONFIG, payload, 9, rsp, &rsp_len, 300) == HAT_RSP_OK;
}

bool hat_la_set_trigger(uint8_t type, uint8_t channel)
{
    if (!s_state.connected) return false;
    uint8_t payload[2] = { type, channel };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_SET_TRIGGER, payload, 2, rsp, &rsp_len, 200) == HAT_RSP_OK;
}

bool hat_la_arm(void)
{
    if (!s_state.connected) return false;
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_ARM, NULL, 0, rsp, &rsp_len, 200) == HAT_RSP_OK;
}

bool hat_la_force(void)
{
    if (!s_state.connected) return false;
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_FORCE, NULL, 0, rsp, &rsp_len, 200) == HAT_RSP_OK;
}

bool hat_la_stop(void)
{
    if (!s_state.connected) return false;
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_STOP, NULL, 0, rsp, &rsp_len, 200) == HAT_RSP_OK;
}

bool hat_la_get_status(HatLaStatus *status)
{
    if (!s_state.connected || !status) return false;
    uint8_t rsp[26] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_LA_GET_STATUS, NULL, 0, rsp, &rsp_len, 200);
    if (cmd == HAT_RSP_LA_STATUS && rsp_len >= 14) {
        status->state = rsp[0];
        status->channels = rsp[1];
        memcpy(&status->samples_captured, &rsp[2], 4);
        memcpy(&status->total_samples, &rsp[6], 4);
        memcpy(&status->actual_rate_hz, &rsp[10], 4);
        if (rsp_len >= 16) {
            status->usb_connected = rsp[14];
            status->usb_mounted = rsp[15];
        }
        if (rsp_len >= 17) {
            status->stream_stop_reason = rsp[16];
        }
        if (rsp_len >= 21) {
            memcpy(&status->stream_overrun_count, &rsp[17], 4);
        }
        if (rsp_len >= 25) {
            memcpy(&status->stream_short_write_count, &rsp[21], 4);
        }
        return true;
    }
    return false;
}

uint8_t hat_la_read_data(uint32_t offset, uint8_t *buf, uint8_t len)
{
    if (!s_state.connected) return 0;
    if (len > 28) len = 28;

    uint8_t payload[6];
    memcpy(&payload[0], &offset, 4);
    payload[4] = (uint8_t)(len & 0xFF);
    payload[5] = (uint8_t)(len >> 8);

    uint8_t rsp[28] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_LA_READ_DATA, payload, 6, rsp, &rsp_len, 200);
    if (cmd == HAT_RSP_LA_DATA && rsp_len > 0) {
        memcpy(buf, rsp, rsp_len);
        return rsp_len;
    }
    return 0;
}

// =============================================================================
// Polling for unsolicited messages
// =============================================================================

// Simple non-blocking check for incoming UART frames
void hat_poll(void)
{
    if (!s_initialized || !s_state.connected) return;

    // Check if any bytes available on UART without blocking
    size_t buffered = 0;
    uart_get_buffered_data_len(HAT_UART_NUM, &buffered);
    if (buffered == 0) return;

    // Try to receive a frame with very short timeout
    uint8_t rsp[26] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_recv_frame(rsp, &rsp_len, 5);  // 5ms timeout

    if (cmd == 0) return;  // No valid frame

    // Handle unsolicited LA status (capture done notification)
    if (cmd == HAT_RSP_LA_STATUS && rsp_len >= 14) {
        uint8_t la_state = rsp[0];
        if (la_state == 3) {  // LA_STATE_DONE
            ESP_LOGI(TAG, "LA capture done (unsolicited notification)");

            // Forward as BBP event to host
            if (bbpIsActive()) {
                // Payload: [state, channels, samples_captured(u32), total_samples(u32), rate(u32)]
                bbpSendEvent(BBP_EVT_LA_DONE, rsp, rsp_len);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// LA-done IRQ (dedicated GPIO from RP2040 BB_LA_DONE_PIN)
// -----------------------------------------------------------------------------
bool hat_la_done_pending(void)
{
    return s_la_done_pending;
}

bool hat_la_done_consume(void)
{
    // Atomic-enough for this use: the ISR only sets the flag, the task side
    // only clears it, and we tolerate a single missed edge in the unlikely
    // race window. If that ever matters, promote to atomic_exchange.
    if (!s_la_done_pending) return false;
    s_la_done_pending = false;
    return true;
}

// end of hat.cpp
