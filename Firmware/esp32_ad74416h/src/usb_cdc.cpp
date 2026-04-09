// =============================================================================
// usb_cdc.cpp - TinyUSB CDC composite device (CLI + UART bridges)
// =============================================================================

#include "usb_cdc.h"
#include "bbp.h"

#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"

static const char *TAG = "usb_cdc";

// Cached line coding from host for each bridge port
static usb_cdc_line_coding_t s_bridge_coding[CDC_BRIDGE_COUNT] = {};
static bool s_bridge_dtr[CDC_BRIDGE_COUNT] = {};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    // Data available - nothing to do here, polled by serial_available / bridge_read
    (void)itf;
    (void)event;
}

static void cdc_line_state_callback(int itf, cdcacm_event_t *event)
{
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "CDC %d line state: DTR=%d RTS=%d", itf, dtr, rts);

    if (itf == 0) {
        // CLI/BBP port: DO NOT exit binary mode on DTR changes.
        // macOS USB CDC drivers can glitch DTR during normal operation, and
        // exiting binary mode mid-session causes stream corruption when a
        // new handshake races with queued log/prompt text. A fresh handshake
        // will reset state cleanly if the host really did disconnect.
        (void)dtr;
    } else {
        // Track DTR for bridge ports
        int bridge_id = itf - 1;
        if (bridge_id >= 0 && bridge_id < CDC_BRIDGE_COUNT) {
            s_bridge_dtr[bridge_id] = dtr;
        }
    }
}

static void cdc_line_coding_callback(int itf, cdcacm_event_t *event)
{
    int bridge_id = itf - 1;
    if (bridge_id >= 0 && bridge_id < CDC_BRIDGE_COUNT) {
        cdc_line_coding_t const *coding = event->line_coding_changed_data.p_line_coding;
        s_bridge_coding[bridge_id].baudrate  = coding->bit_rate;
        s_bridge_coding[bridge_id].data_bits = coding->data_bits;
        s_bridge_coding[bridge_id].parity    = coding->parity;
        s_bridge_coding[bridge_id].stop_bits = coding->stop_bits;
        ESP_LOGI(TAG, "CDC %d line coding: %lu baud, %d-%d-%d",
                 itf, (unsigned long)coding->bit_rate,
                 coding->data_bits, coding->parity, coding->stop_bits);
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void usb_cdc_init(void)
{
    ESP_LOGI(TAG, "Initializing TinyUSB with %d CDC interfaces", CDC_TOTAL_COUNT);

    // Install TinyUSB driver with default descriptors
    // esp_tinyusb generates descriptors based on CONFIG_TINYUSB_CDC_COUNT
    tinyusb_config_t tusb_cfg = {};
    tusb_cfg.device_descriptor = NULL;         // Use default from menuconfig
    tusb_cfg.string_descriptor = NULL;         // Use default
    tusb_cfg.string_descriptor_count = 0;
    tusb_cfg.external_phy = false;
    tusb_cfg.configuration_descriptor = NULL;  // Use default (auto-generated for CDC_COUNT)

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB driver install failed: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize each CDC ACM interface
    for (int itf = 0; itf < CDC_TOTAL_COUNT; itf++) {
        tinyusb_config_cdcacm_t acm_cfg = {};
        acm_cfg.usb_dev = TINYUSB_USBDEV_0;
        acm_cfg.cdc_port = (tinyusb_cdcacm_itf_t)itf;
        acm_cfg.callback_rx = cdc_rx_callback;
        acm_cfg.callback_rx_wanted_char = NULL;
        acm_cfg.callback_line_state_changed = cdc_line_state_callback;
        acm_cfg.callback_line_coding_changed = cdc_line_coding_callback;

        ret = tusb_cdc_acm_init(&acm_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CDC ACM %d init failed: %s", itf, esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "CDC ACM %d initialized", itf);
        }
    }

    // Route stdout/stderr to CDC #0 for ESP_LOG and printf
    ret = esp_tusb_init_console(TINYUSB_CDC_ACM_0);
    if (ret != ESP_OK) {
        // Non-fatal: CLI will still work via direct read/write
        ESP_LOGW(TAG, "Console redirect to CDC0 failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "TinyUSB CDC ready (%d ports)", CDC_TOTAL_COUNT);
}

// ---------------------------------------------------------------------------
// CLI port (CDC #0)
// ---------------------------------------------------------------------------

bool usb_cdc_cli_connected(void)
{
    return tud_cdc_n_connected(0);
}

uint32_t usb_cdc_cli_available(void)
{
    return tud_cdc_n_available(0);
}

uint32_t usb_cdc_cli_read(uint8_t *buf, size_t len)
{
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, len, &rx_size);
    return (ret == ESP_OK) ? rx_size : 0;
}

uint32_t usb_cdc_cli_write(const uint8_t *buf, size_t len)
{
    size_t written = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t *)buf, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(10));
    return written;
}

void usb_cdc_cli_flush(void)
{
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(10));
}

// ---------------------------------------------------------------------------
// Bridge ports (CDC #1..N)
// ---------------------------------------------------------------------------

static inline tinyusb_cdcacm_itf_t bridge_itf(int bridge_id)
{
    return (tinyusb_cdcacm_itf_t)(bridge_id + 1);
}

bool usb_cdc_bridge_connected(int bridge_id)
{
    if (bridge_id < 0 || bridge_id >= CDC_BRIDGE_COUNT) return false;
    return tud_cdc_n_connected(bridge_id + 1);
}

uint32_t usb_cdc_bridge_available(int bridge_id)
{
    if (bridge_id < 0 || bridge_id >= CDC_BRIDGE_COUNT) return 0;
    return tud_cdc_n_available(bridge_id + 1);
}

uint32_t usb_cdc_bridge_read(int bridge_id, uint8_t *buf, size_t len)
{
    if (bridge_id < 0 || bridge_id >= CDC_BRIDGE_COUNT) return 0;
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(bridge_itf(bridge_id), buf, len, &rx_size);
    return (ret == ESP_OK) ? rx_size : 0;
}

uint32_t usb_cdc_bridge_write(int bridge_id, const uint8_t *buf, size_t len)
{
    if (bridge_id < 0 || bridge_id >= CDC_BRIDGE_COUNT) return 0;
    size_t written = tinyusb_cdcacm_write_queue(bridge_itf(bridge_id), (uint8_t *)buf, len);
    tinyusb_cdcacm_write_flush(bridge_itf(bridge_id), pdMS_TO_TICKS(5));
    return written;
}

void usb_cdc_bridge_flush(int bridge_id)
{
    if (bridge_id < 0 || bridge_id >= CDC_BRIDGE_COUNT) return;
    tinyusb_cdcacm_write_flush(bridge_itf(bridge_id), pdMS_TO_TICKS(5));
}

bool usb_cdc_bridge_dtr(int bridge_id)
{
    if (bridge_id < 0 || bridge_id >= CDC_BRIDGE_COUNT) return false;
    return s_bridge_dtr[bridge_id];
}

bool usb_cdc_bridge_get_line_coding(int bridge_id, usb_cdc_line_coding_t *coding)
{
    if (bridge_id < 0 || bridge_id >= CDC_BRIDGE_COUNT || !coding) return false;
    *coding = s_bridge_coding[bridge_id];
    return true;
}
