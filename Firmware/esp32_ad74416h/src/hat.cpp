// =============================================================================
// hat.cpp - HAT Expansion Board Driver
//
// Handles detection (GPIO47 ADC), UART communication (GPIO43/44, 115200 8N1),
// and EXP_EXT_1-4 pin configuration for attached HAT boards.
// PCB mode only.
// =============================================================================

#include "hat.h"
#include "config.h"
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
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static bool s_initialized = false;

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
    // Flush RX buffer before sending
    uart_flush_input(HAT_UART_NUM);

    if (!hat_send_frame(cmd, payload, payload_len)) {
        ESP_LOGW(TAG, "Failed to send command 0x%02X", cmd);
        return 0;
    }

    return hat_recv_frame(rsp_payload, rsp_len, timeout_ms);
}

// -----------------------------------------------------------------------------
// ADC Detection
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

bool hat_init(void)
{
    memset(&s_state, 0, sizeof(s_state));

    // Initialize ADC for detect pin (GPIO47 = ADC1_CH6 on ESP32-S3)
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&adc_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,   // Full 0-3.3V range
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_6, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return false;
    }

    // Initialize UART for HAT communication
    uart_config_t uart_cfg = {
        .baud_rate = HAT_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(HAT_UART_NUM, &uart_cfg);
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

    // Configure IRQ pin as open-drain input (shared line)
    gpio_config_t irq_cfg = {
        .pin_bit_mask = (1ULL << PIN_HAT_IRQ),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE, // Will enable later if needed
    };
    gpio_config(&irq_cfg);
    gpio_set_level(PIN_HAT_IRQ, 1); // Release (high = idle for open-drain)

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

bool hat_set_pin(uint8_t ext_pin, HatPinFunction func)
{
    if (!s_state.connected) return false;
    if (ext_pin >= HAT_NUM_EXT_PINS) return false;
    if (func >= HAT_FUNC_COUNT) return false;

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
        case HAT_FUNC_SWDIO:        return "SWDIO";
        case HAT_FUNC_SWCLK:        return "SWCLK";
        case HAT_FUNC_TRACE1:       return "TRACE1";
        case HAT_FUNC_TRACE2:       return "TRACE2";
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
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_IO_VOLTAGE, payload, 2, rsp, &rsp_len, 300);
    if (cmd == HAT_RSP_OK) {
        s_state.io_voltage_mv = mv;
        ESP_LOGI(TAG, "I/O voltage set to %u mV", mv);
        return true;
    }
    return false;
}

bool hat_setup_swd(uint16_t target_voltage_mv, HatConnector connector)
{
    if (!s_state.connected) return false;

    ESP_LOGI(TAG, "SWD quick-setup: %umV on connector %c", target_voltage_mv, 'A' + connector);

    // 1. Set HVPAK I/O voltage to match target
    if (!hat_set_io_voltage(target_voltage_mv)) {
        ESP_LOGE(TAG, "SWD setup: failed to set I/O voltage");
        return false;
    }
    delay_ms(5);  // HVPAK stabilization

    // 2. Enable connector power
    if (!hat_set_power(connector, true)) {
        ESP_LOGE(TAG, "SWD setup: failed to enable connector power");
        return false;
    }
    delay_ms(50);  // Target power-up

    // 3. Route EXP_EXT pins for SWD
    // Connector A: EXT1=SWDIO, EXT2=SWCLK (leave EXT3/4 as-is)
    // Connector B: EXT3=SWDIO, EXT4=SWCLK (leave EXT1/2 as-is)
    if (connector == HAT_CONNECTOR_A) {
        hat_set_pin(0, HAT_FUNC_SWDIO);
        hat_set_pin(1, HAT_FUNC_SWCLK);
    } else {
        hat_set_pin(2, HAT_FUNC_SWDIO);
        hat_set_pin(3, HAT_FUNC_SWCLK);
    }

    ESP_LOGI(TAG, "SWD setup complete — connect debug tool to USB CMSIS-DAP");
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
    uint8_t rsp[16] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_LA_GET_STATUS, NULL, 0, rsp, &rsp_len, 200);
    if (cmd == HAT_RSP_LA_STATUS && rsp_len >= 14) {
        status->state = rsp[0];
        status->channels = rsp[1];
        memcpy(&status->samples_captured, &rsp[2], 4);
        memcpy(&status->total_samples, &rsp[6], 4);
        memcpy(&status->actual_rate_hz, &rsp[10], 4);
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

#endif // BREADBOARD_MODE
