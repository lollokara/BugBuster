// =============================================================================
// bb_usb_descriptors.c — USB descriptors: debugprobe + BugBuster LA bulk
//
// Replaces debugprobe's usb_descriptors.c to add a second vendor interface
// for streaming logic analyzer data at full USB speed (~1.2 MB/s).
//
// Interface layout:
//   0: CMSIS-DAP v2 (vendor bulk) — EP 0x04 OUT, 0x85 IN
//   1: CDC-ACM UART bridge — EP 0x81 notif, 0x02 OUT, 0x83 IN
//   2: CDC-ACM data (paired with iface 1)
//   3: BugBuster LA data (vendor bulk) — EP 0x06 OUT, 0x87 IN
// =============================================================================

#include "tusb.h"
#include "get_serial.h"
#include "probe_config.h"

// Device Descriptor
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,
    .idProduct          = 0x000c,
    .bcdDevice          = 0x0300,  // Version 03.00 = BugBuster HAT
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// Interface numbers
enum {
    ITF_NUM_PROBE,       // 0: CMSIS-DAP
    ITF_NUM_CDC_COM,     // 1: CDC notification
    ITF_NUM_CDC_DATA,    // 2: CDC data
    ITF_NUM_BB_LA,       // 3: BugBuster LA bulk
    ITF_NUM_TOTAL
};

// Endpoint numbers
#define CDC_NOTIFICATION_EP  0x81
#define CDC_DATA_OUT_EP      0x02
#define CDC_DATA_IN_EP       0x83
#define DAP_OUT_EP           0x04
#define DAP_IN_EP            0x85
#define BB_LA_OUT_EP         0x06   // Host → RP2040 (commands/ack, rarely used)
#define BB_LA_IN_EP          0x87   // RP2040 → Host (LA data stream)

// Total config descriptor length
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN)

static uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE)
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) {
    (void)itf;
    return desc_hid_report;
}

uint8_t desc_configuration[] = {
    // Config descriptor
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),

    // Interface 0: CMSIS-DAP v2 (vendor bulk)
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_PROBE, 5, DAP_OUT_EP, DAP_IN_EP, 64),

    // Interface 1+2: CDC-ACM UART bridge
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_COM, 6, CDC_NOTIFICATION_EP, 64, CDC_DATA_OUT_EP, CDC_DATA_IN_EP, 64),

    // Interface 3: BugBuster LA data (vendor bulk)
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_BB_LA, 7, BB_LA_OUT_EP, BB_LA_IN_EP, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    // Hack in CAP_BREAK support for CDC (same as debugprobe).
    // The CDC bmCapabilities byte is at a fixed offset from the start of
    // the CDC descriptor: config header (9) + ACM functional desc offset.
    // CDC starts after TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN.
    // Within TUD_CDC_DESCRIPTOR: interface (9) + header (5) + call_mgmt (5)
    // + ACM functional: byte 3 is bmCapabilities → offset = 9+5+5+2 = 21
    {
        const int cdc_start = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN;
        const int bmCap_offset = cdc_start + 9 + 5 + 5 + 2;  // ACM bmCapabilities
        desc_configuration[bmCap_offset] = 0x06;  // SET_LINE_CODING + SEND_BREAK
    }
    return desc_configuration;
}

// String descriptors
char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},          // 0: English
    "BugBuster",                          // 1: Manufacturer
    "BugBuster HAT (CMSIS-DAP + LA)",    // 2: Product
    usb_serial,                           // 3: Serial
    "CMSIS-DAP v1 Interface",             // 4: HID
    "CMSIS-DAP v2 Interface",             // 5: DAP vendor
    "CDC-ACM UART Interface",             // 6: CDC
    "BugBuster Logic Analyzer",           // 7: LA vendor
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
            return NULL;
        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return _desc_str;
}

// BOS + MS OS 2.0 descriptors (same as debugprobe for WinUSB)
#define MS_OS_20_DESC_LEN 0xB2
#define BOS_TOTAL_LEN     (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1)
};

uint8_t const desc_ms_os_20[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_PROBE, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    '{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00, '3', 0x00, 'B', 0x00, '5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00,
    '2', 0x00, '9', 0x00, '3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, '6', 0x00, '6', 0x00, '3', 0x00, '-', 0x00,
    'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00, '-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00,
    '6', 0x00, '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00, '6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

uint8_t const *tud_descriptor_bos_cb(void) {
    return desc_bos;
}
