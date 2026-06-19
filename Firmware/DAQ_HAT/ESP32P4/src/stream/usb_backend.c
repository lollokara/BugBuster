// =============================================================================
// usb_backend.c — TinyUSB HS vendor-bulk backend for the measurement stream.
// =============================================================================

#include "usb_backend.h"
#include <string.h>
#include "esp_log.h"
#include "tinyusb.h"

static const char *TAG = "usb_backend";

#if defined(CFG_TUD_VENDOR) && (CFG_TUD_VENDOR > 0)

#include "tinyusb_default_config.h"

static usb_stream_t *s_stream;

// ---- USB descriptors --------------------------------------------------------
#define USB_VID            0x303A   // Espressif
#define USB_PID            0x4001   // vendor-class product id (dev stage)
#define EPNUM_VENDOR_OUT   0x01
#define EPNUM_VENDOR_IN    0x81
#define ITF_NUM_VENDOR     0
#define ITF_NUM_TOTAL      1
#define CONFIG_TOTAL_LEN   (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// High-speed config: 512-byte bulk endpoints.
static const uint8_t s_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 4, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 512),
};

// Full-speed config: 64-byte bulk endpoints (fallback when on an FS host).
static const uint8_t s_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 4, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0,
};
#endif

static const char *s_str_desc[] = {
    (const char[]){ 0x09, 0x04 },   // 0: English (US)
    "BugBuster",                    // 1: Manufacturer
    "BugBuster Power Analyzer",     // 2: Product
    "BBPA-0001",                    // 3: Serial
    "BugBuster Stream",             // 4: Vendor interface
};

// ---- TinyUSB vendor callbacks ----------------------------------------------
// NOTE: tud_mount_cb/tud_umount_cb are owned by esp_tinyusb; do not redefine
// them. Mount state is tracked via the device-event handler below and the
// stack's own tud_mounted() query.

// esp_tinyusb device-event handler (attach/detach/suspend/resume).
static void device_event_handler(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            ESP_LOGI(TAG, "USB attached");
            break;
        case TINYUSB_EVENT_DETACHED:
            ESP_LOGI(TAG, "USB detached");
            break;
        default:
            break;
    }
}

// Inbound bulk OUT -> feed the control parser.
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)itf;
    if (s_stream && buffer && bufsize) {
        usb_stream_on_rx(s_stream, buffer, bufsize);
    }
    // If buffering is enabled, also drain any FIFO remainder.
    uint8_t buf[256];
    uint32_t n;
    while ((n = tud_vendor_read(buf, sizeof(buf))) > 0) {
        if (s_stream) usb_stream_on_rx(s_stream, buf, n);
    }
}

// ---- Transport implementation ----------------------------------------------
static uint32_t backend_write(const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    if (!tud_mounted()) return 0;
    uint32_t wrote = tud_vendor_write(data, len);
    tud_vendor_write_flush();
    return wrote;
}

static uint32_t backend_writable(void *ctx)
{
    (void)ctx;
    if (!tud_mounted()) return 0;
    return tud_vendor_write_available();
}

esp_err_t usb_backend_start(usb_stream_t *stream)
{
    s_stream = stream;

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(device_event_handler);
    tusb_cfg.descriptor.device            = &s_device_desc;
    tusb_cfg.descriptor.string            = s_str_desc;
    tusb_cfg.descriptor.string_count      = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
    tusb_cfg.descriptor.full_speed_config = s_fs_config_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.port                         = TINYUSB_PORT_HIGH_SPEED_0;
    tusb_cfg.descriptor.high_speed_config = s_hs_config_desc;
    tusb_cfg.descriptor.qualifier         = &s_qualifier;
#else
    tusb_cfg.port                         = TINYUSB_PORT_FULL_SPEED_0;
#endif

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    usb_transport_t t = {
        .write    = backend_write,
        .writable = backend_writable,
        .ctx      = NULL,
    };
    usb_stream_set_transport(stream, &t);
    ESP_LOGI(TAG, "USB-HS vendor backend started");
    return ESP_OK;
}

bool usb_backend_mounted(void)
{
    return tud_mounted();
}

#else  // CFG_TUD_VENDOR == 0 — vendor class not compiled in.

esp_err_t usb_backend_start(usb_stream_t *stream)
{
    (void)stream;
    ESP_LOGW(TAG, "TinyUSB vendor class not enabled (CONFIG_TINYUSB_VENDOR_COUNT=0); "
                  "USB stream disabled");
    return ESP_ERR_NOT_SUPPORTED;
}

bool usb_backend_mounted(void) { return false; }

#endif
