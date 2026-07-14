// =============================================================================
// daq_config_registry.c — shared DAQ HAT settings registry implementation.
//
// Built into BOTH the ESP32-P4 and ESP32-C6 firmwares (see each src
// CMakeLists.txt, which adds ../../common to SRCS + INCLUDE_DIRS). Pure libc so
// it compiles identically on both targets and can be unit-tested on a host.
// =============================================================================

#include "daq_config_registry.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Enum option tables.
// ---------------------------------------------------------------------------
const uint32_t DAQ_SAMPLE_RATE_SPS[DAQ_SR_COUNT] = {
    10000u, 50000u, 100000u, 250000u, 1000000u,
};
const uint16_t DAQ_FFT_LENGTH_BINS[DAQ_FFT_LEN_COUNT] = {
    64u, 128u, 256u, 512u, 1024u, 2048u, 4096u,
};

static const char *const OPT_RANGE[]   = { "A", "mA", "uA" };
static const char *const OPT_SR[]      = { "10 ksps", "50 ksps", "100 ksps",
                                           "250 ksps", "1 Msps" };
static const char *const OPT_FFT_LEN[] = { "64", "128", "256", "512", "1024",
                                           "2048", "4096" };
static const char *const OPT_WINDOW[]  = { "Rect", "Hann", "Blackman-Harris" };
static const char *const OPT_FFTSRC[]  = { "Current", "Power" };
static const char *const OPT_NPX[]     = { "Off", "Solid", "Status", "Breathe",
                                           "Channel" };
static const char *const OPT_WIFI[]    = { "Access Point", "Station" };
static const char *const OPT_FILTER[]  = { "Wideband", "Sinc5", "Sinc3" };
static const char *const OPT_DECIM[]   = { "x32", "x64", "x128", "x256",
                                           "x512", "x1024" };

// ---------------------------------------------------------------------------
// Schema table — the canonical list of every configurable setting.
// ---------------------------------------------------------------------------
static const daq_setting_schema_t s_table[] = {
    // key                    type        flags                               min  max   step  def   label                 options
    // --- Acquisition ---
    { DAQ_K_AUTORANGING,     DAQ_T_BOOL, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   1,    1,    1,    "Autoranging",        NULL },
    { DAQ_K_RANGE_IDX,       DAQ_T_ENUM, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   2,    1,    1,    "Range",              OPT_RANGE },
    { DAQ_K_SAMPLE_RATE_IDX, DAQ_T_ENUM, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   4,    1,    2,    "Sample Rate",        OPT_SR },
    { DAQ_K_STREAMING,       DAQ_T_BOOL, DAQ_F_P4_LOCAL,                       0,   1,    1,    0,    "Streaming",          NULL },
    { DAQ_K_USB_DECIMATION,  DAQ_T_U16,  DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         1,   256,  1,    1,    "USB Decimation",     NULL },
    { DAQ_K_FILTER,          DAQ_T_ENUM, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   2,    1,    0,    "Filter",             OPT_FILTER },
    { DAQ_K_DECIMATION,      DAQ_T_ENUM, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   5,    1,    3,    "Decimation",         OPT_DECIM },
    { DAQ_K_REJECT_5060,     DAQ_T_BOOL, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   1,    1,    0,    "50/60Hz Reject",     NULL },
    // --- Source / SMU ---
    { DAQ_K_SOURCE_ENABLE,   DAQ_T_BOOL, DAQ_F_P4_LOCAL,                       0,   1,    1,    0,    "Source Enable",      NULL },
    { DAQ_K_DUT_VOLTAGE_MV,  DAQ_T_U16,  DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         1800,20000,100,  5000, "DUT Voltage",        NULL },
    { DAQ_K_DUT_ILIMIT_MA,   DAQ_T_U16,  DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         100, 2500, 100,  1000, "DUT Current Limit",  NULL },
    // --- DSP ---
    { DAQ_K_FFT_ENABLE,      DAQ_T_BOOL, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   1,    1,    0,    "FFT",                NULL },
    { DAQ_K_FFT_LENGTH,      DAQ_T_ENUM, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   6,    1,    4,    "FFT Length",         OPT_FFT_LEN },
    { DAQ_K_FFT_WINDOW,      DAQ_T_ENUM, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   2,    1,    1,    "FFT Window",         OPT_WINDOW },
    { DAQ_K_FFT_SOURCE,      DAQ_T_ENUM, DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   1,    1,    0,    "FFT Source",         OPT_FFTSRC },
    { DAQ_K_MULTIRES_TIERS,  DAQ_T_U8,   DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         1,   4,    1,    3,    "Zoom Tiers",         NULL },
    { DAQ_K_STATS_WINDOW_MS, DAQ_T_U16,  DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         10,  10000,10,   1000, "Stats Window",       NULL },
    // --- Display (C6) ---
    { DAQ_K_BRIGHTNESS_PCT,  DAQ_T_U8,   DAQ_F_C6_LOCAL|DAQ_F_PERSIST,         10,  100,  10,   100,  "Brightness",         NULL },
    { DAQ_K_DARK_MODE,       DAQ_T_BOOL, DAQ_F_C6_LOCAL|DAQ_F_PERSIST,         0,   1,    1,    0,    "Dark Mode",          NULL },
    // --- Neopixel (C6) ---
    { DAQ_K_NPX_MODE,        DAQ_T_ENUM, DAQ_F_C6_LOCAL|DAQ_F_PERSIST,         0,   4,    1,    4,    "LED Mode",           OPT_NPX },
    { DAQ_K_NPX_COLOR,       DAQ_T_U32,  DAQ_F_C6_LOCAL|DAQ_F_PERSIST,         0,   0xFFFFFF, 1, 0x00FF00, "LED Color",      NULL },
    { DAQ_K_NPX_BRIGHTNESS,  DAQ_T_U8,   DAQ_F_C6_LOCAL|DAQ_F_PERSIST,         0,   100,  5,    50,   "LED Brightness",     NULL },
    // --- WiFi (S3 mainboard radio; relayed by the P4, shown/edited on the C6) ---
    { DAQ_K_WIFI_ENABLE,     DAQ_T_BOOL, DAQ_F_S3_LOCAL|DAQ_F_PERSIST,         0,   1,    1,    0,    "WiFi",               NULL },
    { DAQ_K_WIFI_MODE,       DAQ_T_ENUM, DAQ_F_S3_LOCAL|DAQ_F_PERSIST,         0,   1,    1,    0,    "WiFi Mode",          OPT_WIFI },
    { DAQ_K_WIFI_SSID,       DAQ_T_STR,  DAQ_F_S3_LOCAL|DAQ_F_PERSIST,         0,   32,   0,    0,    "WiFi SSID",          NULL },
    { DAQ_K_WIFI_PASSWORD,   DAQ_T_STR,  DAQ_F_S3_LOCAL|DAQ_F_PERSIST|DAQ_F_SECRET, 0, 64, 0,   0,    "WiFi Password",      NULL },
    // --- System ---
    { DAQ_K_DEVICE_LABEL,    DAQ_T_STR,  DAQ_F_P4_LOCAL|DAQ_F_PERSIST,         0,   24,   0,    0,    "Device Label",       NULL },
};

#define S_TABLE_COUNT (sizeof(s_table) / sizeof(s_table[0]))

// ---------------------------------------------------------------------------
// Scalar size for a type tag (0 for variable / non-scalar).
// ---------------------------------------------------------------------------
static uint8_t type_size(uint8_t type)
{
    switch (type) {
    case DAQ_T_BOOL:
    case DAQ_T_U8:
    case DAQ_T_I8:
    case DAQ_T_ENUM: return 1;
    case DAQ_T_U16:
    case DAQ_T_I16:  return 2;
    case DAQ_T_U32:
    case DAQ_T_I32:
    case DAQ_T_F32:  return 4;
    default:         return 0; // DAQ_T_STR / DAQ_T_NONE are variable
    }
}

// ---------------------------------------------------------------------------
// TLV encode / parse.
// ---------------------------------------------------------------------------
int daq_tlv_encode(uint8_t *buf, size_t cap, uint16_t key, uint8_t type,
                   const void *val, uint8_t val_len)
{
    if (val_len > DAQ_TLV_MAX_VAL) return -1;
    if ((size_t)DAQ_TLV_HDR_LEN + val_len > cap) return -1;
    buf[0] = (uint8_t)(key & 0xFF);
    buf[1] = (uint8_t)(key >> 8);
    buf[2] = type;
    buf[3] = val_len;
    if (val_len && val) memcpy(&buf[4], val, val_len);
    return (int)(DAQ_TLV_HDR_LEN + val_len);
}

int daq_tlv_encode_i32(uint8_t *buf, size_t cap, uint16_t key, uint8_t type,
                       int32_t value)
{
    uint8_t sz = type_size(type);
    if (sz == 0 || sz > 4) return -1;
    uint8_t v[4];
    uint32_t u = (uint32_t)value;
    for (uint8_t i = 0; i < sz; i++) v[i] = (uint8_t)(u >> (8 * i));
    return daq_tlv_encode(buf, cap, key, type, v, sz);
}

int daq_tlv_parse(const uint8_t *buf, size_t len, uint16_t *key, uint8_t *type,
                  const uint8_t **val, uint8_t *val_len)
{
    if (len < DAQ_TLV_HDR_LEN) return -1;
    uint8_t vlen = buf[3];
    if ((size_t)DAQ_TLV_HDR_LEN + vlen > len) return -1;
    if (key)     *key = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    if (type)    *type = buf[2];
    if (val)     *val = (vlen ? &buf[4] : NULL);
    if (val_len) *val_len = vlen;
    return (int)(DAQ_TLV_HDR_LEN + vlen);
}

bool daq_tlv_value_i32(uint8_t type, const uint8_t *val, uint8_t val_len,
                       int32_t *out)
{
    uint8_t sz = type_size(type);
    if (sz == 0 || type == DAQ_T_F32 || sz != val_len || val == NULL) return false;
    uint32_t u = 0;
    for (uint8_t i = 0; i < sz; i++) u |= ((uint32_t)val[i]) << (8 * i);
    int32_t v;
    // Sign-extend signed sub-word types.
    if (type == DAQ_T_I8)  v = (int8_t)u;
    else if (type == DAQ_T_I16) v = (int16_t)u;
    else v = (int32_t)u;
    if (out) *out = v;
    return true;
}

// ---------------------------------------------------------------------------
// Schema access + clamp.
// ---------------------------------------------------------------------------
const daq_setting_schema_t *daq_config_schema(uint16_t key)
{
    for (size_t i = 0; i < S_TABLE_COUNT; i++) {
        if (s_table[i].key == key) return &s_table[i];
    }
    return NULL;
}

const daq_setting_schema_t *daq_config_table(size_t *count)
{
    if (count) *count = S_TABLE_COUNT;
    return s_table;
}

int32_t daq_config_clamp(uint16_t key, int32_t value)
{
    const daq_setting_schema_t *s = daq_config_schema(key);
    if (!s) return value;
    if (s->type == DAQ_T_STR || s->type == DAQ_T_F32) return value;
    if (value < s->min) return s->min;
    if (value > s->max) return s->max;
    return value;
}
