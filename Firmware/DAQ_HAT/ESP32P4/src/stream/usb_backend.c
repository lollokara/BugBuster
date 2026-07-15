// =============================================================================
// usb_backend.c — TinyUSB HS vendor-bulk backend for the measurement stream.
// =============================================================================

#include "usb_backend.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tinyusb.h"

// Force a virtual re-plug after this many consecutive unmounted poll ticks
// (poll runs at ~1 Hz). Kept generous so it never disrupts a normal
// enumeration (which completes in well under a second).
#define USB_REENUM_TIMEOUT_TICKS  5

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
    // 0x0210 (USB 2.1) so the host requests the BOS descriptor, which carries
    // the Microsoft OS 2.0 (WCID) platform capability that auto-binds WinUSB.
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    // Bumped 0x0100 -> 0x0101 so Windows re-queries the MS OS 2.0 descriptor:
    // the usbflags cache is keyed by VID+PID+bcdDevice, and a device that once
    // enumerated without WCID caches "no MS OS descriptor" until bcdDevice moves.
    .bcdDevice          = 0x0101,
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
    // Serial bumped 0001 -> 0002: Windows keys the devnode (and its cached
    // CONFIGFLAG_FAILEDINSTALL / "Code 28" verdict) by VID+PID+serial. The old
    // BBPA-0001 node had a failed-install flag from the pre-WCID firmware and
    // Windows never retries it; a new serial forces a fresh install that picks
    // up the MS OS 2.0 (WinUSB) descriptor.
    "BBPA-0004",                    // 3: Serial
    "BugBuster Stream",             // 4: Vendor interface
};

// ---- WCID / Microsoft OS 2.0 descriptor ------------------------------------
// A raw vendor-bulk interface has no Windows function driver, so Windows shows
// "Code 28" and the host-side libusb/WinUSB cannot open it without Zadig. The
// Microsoft OS 2.0 descriptor advertises the "WINUSB" compatible ID plus a
// DeviceInterfaceGUID, so Windows auto-loads WinUSB and binds it on plug-in.
// Requires bcdUSB >= 0x0210 (set above) so the host fetches the BOS descriptor.
#define VENDOR_REQUEST_MICROSOFT   0x20
#define MS_OS_20_DESC_LEN          0xA2   // 162 = 10 (header) + 20 (compat id) + 132 (reg property)

// BOS descriptor: one device capability (the MS OS 2.0 platform descriptor).
static const uint8_t s_desc_bos[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

// MS OS 2.0 descriptor set. This is a SINGLE-FUNCTION (non-composite) device
// (bDeviceClass=0, one vendor interface), so Windows does not load the composite
// parent driver (usbccgp) that would process a configuration/function subset.
// The WINUSB compatible ID and the DeviceInterfaceGUID must therefore be placed
// at the DEVICE level, directly under the set header — nesting them in a
// function subset (as composite-device examples do) makes Windows read the
// descriptor but never apply MS_COMP_WINUSB, leaving the device at "Code 28".
static const uint8_t s_desc_ms_os_20[] = {
    // Set header: length, type, windows version (8.1+), total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Device-level Compatible ID feature: length, type, compatible ID ("WINUSB"), sub-ID
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Device-level Registry property: length, type
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType=REG_MULTI_SZ, wPropertyNameLength
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    // DeviceInterfaceGUID "{A7B3C2D1-4E5F-4A6B-8C9D-1E2F3A4B5C6D}" (UTF-16LE, double-null)
    '{', 0x00, 'A', 0x00, '7', 0x00, 'B', 0x00, '3', 0x00, 'C', 0x00, '2', 0x00, 'D', 0x00, '1', 0x00, '-', 0x00,
    '4', 0x00, 'E', 0x00, '5', 0x00, 'F', 0x00, '-', 0x00, '4', 0x00, 'A', 0x00, '6', 0x00, 'B', 0x00, '-', 0x00,
    '8', 0x00, 'C', 0x00, '9', 0x00, 'D', 0x00, '-', 0x00, '1', 0x00, 'E', 0x00, '2', 0x00, 'F', 0x00, '3', 0x00,
    'A', 0x00, '4', 0x00, 'B', 0x00, '5', 0x00, 'C', 0x00, '6', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(s_desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor size mismatch");

// Invoked on GET BOS DESCRIPTOR. Weak in TinyUSB; esp_tinyusb does not define it.
uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_desc_bos;
}

// Vendor control transfers: serve the MS OS 2.0 descriptor set (wIndex == 7).
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;   // nothing to do on DATA/ACK

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_REQUEST_MICROSOFT && request->wIndex == 7) {
        uint16_t total_len;
        memcpy(&total_len, s_desc_ms_os_20 + 8, 2);
        return tud_control_xfer(rhport, request,
                                (void *)(uintptr_t)s_desc_ms_os_20, total_len);
    }
    return false;   // stall unsupported requests
}

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
// tud_vendor_rx_cb is a NOTIFICATION in esp_tinyusb: the `buffer` pointer
// points into the internal FIFO but the FIFO is NOT consumed until the app
// calls tud_vendor_read().  The old code processed `buffer` AND then drained
// with tud_vendor_read(), causing every command to be dispatched TWICE.
// Fix: ignore the callback `buffer` argument entirely and only consume data
// via tud_vendor_read(), which properly advances the FIFO read pointer so
// TinyUSB can accept the next bulk-OUT from the host.
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)itf;
    (void)buffer;
    (void)bufsize;
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
    // The TinyUSB device task defaults to core 1 (the esp_tinyusb multicore
    // default). But core 1 is our dedicated acquisition core: the DRDY-gated
    // capture task runs there at priority 20 and legitimately pins the core at
    // ~100% CPU. That starves the priority-5 usbd task, so DCD events (bus
    // reset, SETUP, ...) queue up until osal_queue_send() fails inside
    // queue_event() ("queue_event 382: ASSERT FAILED") and every USB event is
    // dropped — enumeration never completes and the HS device (PID 0x4001)
    // never appears. Pin the USB task to core 0 (the intended USB/DSP/links
    // core, per sdkconfig) and lift its priority above the fast-path consumer
    // (12) so it always preempts to drain the event queue promptly.
    tusb_cfg.task.xCoreID  = 0;
    tusb_cfg.task.priority = 13;
    // Default is 4096 bytes which is too small when CMD_SET_RATE fires
    // daq_board_stop_fast (vTaskDelay + SPI teardown) + run_fast inline.
    // The deferred ctrl task handles this now, but keep extra headroom.
    tusb_cfg.task.size     = 8192;
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

void usb_backend_poll(void)
{
    // Recovery for the "host never saw us" case only (e.g. it probed the HS
    // port before our USB stack was up and gave up). There is no VBUS-sense
    // line, so we infer host presence from the stack: tud_connected() is true
    // once the host has driven a bus reset / started enumerating, tud_mounted()
    // once configured. If EITHER is true we must NOT force a re-plug — a
    // driverless device (Windows "Code 28") legitimately enumerates but may
    // never get SET_CONFIGURATION, so mounted stays false; toggling the pull-up
    // there just produces a connect/disconnect loop. Only re-plug when the bus
    // is genuinely idle (no host activity at all) for the whole timeout.
    static uint8_t idle_ticks;
    if (tud_mounted() || tud_connected()) {
        idle_ticks = 0;
        return;
    }
    if (++idle_ticks >= USB_REENUM_TIMEOUT_TICKS) {
        idle_ticks = 0;
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(80));
        tud_connect();
        ESP_LOGD(TAG, "USB bus idle; forced re-enumeration");
    }
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
void usb_backend_poll(void) { }

#endif
