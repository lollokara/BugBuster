// =============================================================================
// uart_bridge.cpp - UART ↔ USB CDC bridge (multi-instance)
// =============================================================================

#include "uart_bridge.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "config.h"
#include "usb_cdc.h"

static const char *TAG = "uart_bridge";

// ---------------------------------------------------------------------------
// Per-bridge state
// ---------------------------------------------------------------------------

struct BridgeState {
    UartBridgeConfig config;
    bool             driver_installed;
    TaskHandle_t     task_handle;
};

static BridgeState s_bridges[CDC_BRIDGE_COUNT] = {};

// ---------------------------------------------------------------------------
// Excluded GPIO pins (in use or strapping)
// ---------------------------------------------------------------------------

static const int EXCLUDED_PINS[] = {
    0,                  // Strapping pin (boot)
    PIN_RESET,          // AD74416H RESET
    PIN_ADC_RDY,        // AD74416H ADC_RDY
    PIN_ALERT,          // AD74416H ALERT
    PIN_SDO,            // AD74416H SPI MISO
    PIN_SDI,            // AD74416H SPI MOSI
    PIN_SYNC,           // AD74416H SYNC/CS
    PIN_SCLK,           // AD74416H SPI SCLK
    PIN_MUX_CS,         // ADGS2414D CS
    PIN_I2C_SDA,        // I2C shared bus SDA
    PIN_I2C_SCL,        // I2C shared bus SCL
    19, 20,             // USB D-/D+
    45, 46,             // Strapping pins
    -1                  // Sentinel
};

static bool is_pin_excluded_for_bridge(int pin, int self_bridge_id)
{
    for (int i = 0; EXCLUDED_PINS[i] >= 0; i++) {
        if (EXCLUDED_PINS[i] == pin) return true;
    }
    // Also check if pin is used by another bridge
    for (int b = 0; b < CDC_BRIDGE_COUNT; b++) {
        if (b == self_bridge_id) continue;
        if (s_bridges[b].config.enabled) {
            if (s_bridges[b].config.tx_pin == pin || s_bridges[b].config.rx_pin == pin)
                return true;
        }
    }
    return false;
}

// Compatibility helper for existing call sites that validate "generic" pins
// outside the context of one configured bridge.
static bool is_pin_excluded(int pin)
{
    return is_pin_excluded_for_bridge(pin, -1);
}

// ---------------------------------------------------------------------------
// Default configs
// ---------------------------------------------------------------------------

static const UartBridgeConfig DEFAULT_CONFIGS[2] = {
    // Bridge 0 defaults to external IO lines (IO1/IO2 on the terminal map):
    // GPIO1 TX, GPIO2 RX.
    { .uart_num = 1, .tx_pin = 1, .rx_pin = 2, .baudrate = 921600,
      .data_bits = 8, .parity = 0, .stop_bits = 1, .enabled = true },
    { .uart_num = 2, .tx_pin = 47, .rx_pin = 48, .baudrate = 921600,
      .data_bits = 8, .parity = 0, .stop_bits = 1, .enabled = true },
};

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------

static void nvs_save_config(int id, const UartBridgeConfig *cfg)
{
    char ns[20];
    snprintf(ns, sizeof(ns), "uart_br_%d", id);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "uart_num", cfg->uart_num);
        nvs_set_i32(h, "tx_pin", cfg->tx_pin);
        nvs_set_i32(h, "rx_pin", cfg->rx_pin);
        nvs_set_u32(h, "baud", cfg->baudrate);
        nvs_set_u8(h, "data_bits", cfg->data_bits);
        nvs_set_u8(h, "parity", cfg->parity);
        nvs_set_u8(h, "stop_bits", cfg->stop_bits);
        nvs_set_u8(h, "enabled", cfg->enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Bridge %d config saved to NVS", id);
    }
}

static bool nvs_load_config(int id, UartBridgeConfig *cfg)
{
    char ns[20];
    snprintf(ns, sizeof(ns), "uart_br_%d", id);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;

    uint8_t u8; int32_t i32; uint32_t u32;
    bool ok = true;
    ok = ok && (nvs_get_u8(h, "uart_num", &u8) == ESP_OK); cfg->uart_num = u8;
    ok = ok && (nvs_get_i32(h, "tx_pin", &i32) == ESP_OK); cfg->tx_pin = (int)i32;
    ok = ok && (nvs_get_i32(h, "rx_pin", &i32) == ESP_OK); cfg->rx_pin = (int)i32;
    ok = ok && (nvs_get_u32(h, "baud", &u32) == ESP_OK);   cfg->baudrate = u32;
    ok = ok && (nvs_get_u8(h, "data_bits", &u8) == ESP_OK); cfg->data_bits = u8;
    ok = ok && (nvs_get_u8(h, "parity", &u8) == ESP_OK);    cfg->parity = u8;
    ok = ok && (nvs_get_u8(h, "stop_bits", &u8) == ESP_OK); cfg->stop_bits = u8;
    if (nvs_get_u8(h, "enabled", &u8) == ESP_OK) cfg->enabled = (u8 != 0);
    else cfg->enabled = true;

    nvs_close(h);
    return ok;
}

// ---------------------------------------------------------------------------
// UART driver management
// ---------------------------------------------------------------------------

static uart_word_length_t to_idf_data_bits(uint8_t bits)
{
    switch (bits) {
        case 5: return UART_DATA_5_BITS;
        case 6: return UART_DATA_6_BITS;
        case 7: return UART_DATA_7_BITS;
        default: return UART_DATA_8_BITS;
    }
}

static uart_parity_t to_idf_parity(uint8_t p)
{
    switch (p) {
        case 1: return UART_PARITY_ODD;
        case 2: return UART_PARITY_EVEN;
        default: return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t to_idf_stop_bits(uint8_t s)
{
    return (s == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static bool install_uart(int id)
{
    BridgeState &bs = s_bridges[id];
    const UartBridgeConfig &cfg = bs.config;

    if (bs.driver_installed) {
        uart_driver_delete((uart_port_t)cfg.uart_num);
        bs.driver_installed = false;
    }

    if (!cfg.enabled) return true;

    if (cfg.tx_pin < 0 || cfg.rx_pin < 0 || cfg.tx_pin == cfg.rx_pin ||
        is_pin_excluded_for_bridge(cfg.tx_pin, id) ||
        is_pin_excluded_for_bridge(cfg.rx_pin, id)) {
        ESP_LOGW(TAG, "Bridge %d: invalid/conflicting pins TX=%d RX=%d; trying defaults",
                 id, cfg.tx_pin, cfg.rx_pin);

        UartBridgeConfig fallback = DEFAULT_CONFIGS[id < 2 ? id : 0];
        if (fallback.tx_pin >= 0 && fallback.rx_pin >= 0 &&
            fallback.tx_pin != fallback.rx_pin &&
            !is_pin_excluded_for_bridge(fallback.tx_pin, id) &&
            !is_pin_excluded_for_bridge(fallback.rx_pin, id)) {
            bs.config = fallback;
            nvs_save_config(id, &bs.config);
            ESP_LOGI(TAG, "Bridge %d remapped to defaults: UART%d TX=%d RX=%d",
                     id, bs.config.uart_num, bs.config.tx_pin, bs.config.rx_pin);
        } else {
            ESP_LOGE(TAG, "Bridge %d: defaults also invalid; disabling bridge", id);
            bs.config.enabled = false;
            return true;
        }
    }

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate  = (int)cfg.baudrate;
    uart_cfg.data_bits  = to_idf_data_bits(cfg.data_bits);
    uart_cfg.parity     = to_idf_parity(cfg.parity);
    uart_cfg.stop_bits  = to_idf_stop_bits(cfg.stop_bits);
    uart_cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    uart_port_t port = (uart_port_t)cfg.uart_num;
    esp_err_t ret = uart_driver_install(port, 2048, 2048, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bridge %d: uart_driver_install(%d) failed: %s", id, cfg.uart_num, esp_err_to_name(ret));
        return false;
    }

    uart_param_config(port, &uart_cfg);
    uart_set_pin(port, cfg.tx_pin, cfg.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    bs.driver_installed = true;
    ESP_LOGI(TAG, "Bridge %d: UART%d TX=%d RX=%d %lu-%d%c%d",
             id, cfg.uart_num, cfg.tx_pin, cfg.rx_pin,
             (unsigned long)cfg.baudrate, cfg.data_bits,
             cfg.parity == 0 ? 'N' : (cfg.parity == 1 ? 'O' : 'E'),
             cfg.stop_bits);
    return true;
}

// ---------------------------------------------------------------------------
// Bridge task
// ---------------------------------------------------------------------------

static void bridge_task(void *pvParam)
{
    int id = (int)(intptr_t)pvParam;
    uint8_t buf[256];

    ESP_LOGI(TAG, "Bridge %d task started", id);

    for (;;) {
        BridgeState &bs = s_bridges[id];
        if (!bs.config.enabled || !bs.driver_installed) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uart_port_t port = (uart_port_t)bs.config.uart_num;

        // USB CDC -> UART TX
        uint32_t avail = usb_cdc_bridge_available(id);
        if (avail > 0) {
            uint32_t to_read = (avail > sizeof(buf)) ? sizeof(buf) : avail;
            uint32_t n = usb_cdc_bridge_read(id, buf, to_read);
            if (n > 0) {
                uart_write_bytes(port, buf, n);
            }
        }

        // UART RX -> USB CDC
        int len = uart_read_bytes(port, buf, sizeof(buf), 0);  // non-blocking
        if (len > 0) {
            usb_cdc_bridge_write(id, buf, len);
        }

        vTaskDelay(pdMS_TO_TICKS(1));  // 1ms poll
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void uart_bridge_init(void)
{
    // Ensure NVS is initialized
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    for (int id = 0; id < CDC_BRIDGE_COUNT; id++) {
        // Load from NVS or use defaults
        if (!nvs_load_config(id, &s_bridges[id].config)) {
            s_bridges[id].config = DEFAULT_CONFIGS[id < 2 ? id : 0];
            ESP_LOGI(TAG, "Bridge %d: using defaults", id);
        } else {
            ESP_LOGI(TAG, "Bridge %d: loaded from NVS", id);
        }

        install_uart(id);
    }
}

void uart_bridge_start(void)
{
    for (int id = 0; id < CDC_BRIDGE_COUNT; id++) {
        char name[16];
        snprintf(name, sizeof(name), "uartBr%d", id);
        xTaskCreatePinnedToCore(bridge_task, name, 4096,
                                (void *)(intptr_t)id, 2, &s_bridges[id].task_handle, 0);
    }
}

bool uart_bridge_get_config(int id, UartBridgeConfig *cfg)
{
    if (id < 0 || id >= CDC_BRIDGE_COUNT || !cfg) return false;
    *cfg = s_bridges[id].config;
    return true;
}

bool uart_bridge_set_config(int id, const UartBridgeConfig *cfg)
{
    if (id < 0 || id >= CDC_BRIDGE_COUNT || !cfg) return false;

    // Uninstall old UART if port changed
    if (s_bridges[id].driver_installed) {
        uart_driver_delete((uart_port_t)s_bridges[id].config.uart_num);
        s_bridges[id].driver_installed = false;
    }

    s_bridges[id].config = *cfg;
    nvs_save_config(id, cfg);
    install_uart(id);
    return true;
}

bool uart_bridge_is_connected(int id)
{
    return usb_cdc_bridge_dtr(id);
}

int uart_bridge_get_available_pins(int *out_pins, int max_pins)
{
    // ESP32-S3 GPIOs: 0-21, 35-48
    static const int ALL_GPIOS[] = {
        1, 2, 3, 4, 12, 13, 14, 15, 16, 17, 18, 21,
        35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 47, 48,
        -1
    };
    int count = 0;
    for (int i = 0; ALL_GPIOS[i] >= 0 && count < max_pins; i++) {
        if (!is_pin_excluded(ALL_GPIOS[i])) {
            out_pins[count++] = ALL_GPIOS[i];
        }
    }
    return count;
}
