#pragma once

// =============================================================================
// daq_config_registry.h — canonical DAQ HAT settings registry (shared P4 + C6)
//
// Single source of truth for EVERY configurable setting on the DAQ HAT. The
// ESP32-P4 owns the authoritative value store; the ESP32-C6 (display/menu) and
// the ESP32-S3 mainboard (desktop / mobile / web / MCP front-ends) both read and
// write settings through the same key-addressed TLV protocol defined here.
//
//   * Each setting has a stable 16-bit key (group<<8 | index).
//   * Values are carried as TLV: [key u16 LE][type u8][len u8][value ...].
//   * A schema table describes type/min/max/step/options/flags/label so any UI
//     (C6 menu, desktop, web) can self-describe without a hand-maintained copy.
//
// This header is included by BOTH firmwares; the implementation lives in
// daq_config_registry.c (added to both component SRCS). Keep it dependency-free
// (only libc) so it compiles identically on the P4 (RISC-V) and C6 (RISC-V).
//
// Wire mapping (defined in the link headers, not here):
//   S3 <-> P4 : HAT protocol cmds CONFIG_GET/SET/GET_ALL/SCHEMA/ACTION 0x70..0x74
//   C6 <-> P4 : DDP cmds CONFIG_SET_TLV / CONFIG_PUSH / CONFIG_ACTION
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bump when the key set or wire encoding changes incompatibly.
#define DAQ_CFG_REGISTRY_VERSION 1u

// -----------------------------------------------------------------------------
// Value type tags (the TLV "type" byte).
// -----------------------------------------------------------------------------
typedef enum {
    DAQ_T_NONE = 0,
    DAQ_T_BOOL = 1,   // 1 byte, 0/1
    DAQ_T_U8   = 2,
    DAQ_T_I8   = 3,
    DAQ_T_U16  = 4,   // little-endian
    DAQ_T_I16  = 5,
    DAQ_T_U32  = 6,
    DAQ_T_I32  = 7,
    DAQ_T_F32  = 8,
    DAQ_T_ENUM = 9,   // stored as u8 option index
    DAQ_T_STR  = 10,  // UTF-8, not NUL-terminated on the wire (len in TLV)
} daq_type_t;

// -----------------------------------------------------------------------------
// Setting groups (high byte of the key).
// -----------------------------------------------------------------------------
#define DAQ_GRP_ACQ   0x01u   // acquisition / ranging
#define DAQ_GRP_SMU   0x02u   // source / current limit
#define DAQ_GRP_DSP   0x03u   // FFT / multires / stats
#define DAQ_GRP_DISP  0x04u   // C6 display
#define DAQ_GRP_NPX   0x05u   // C6 neopixels
#define DAQ_GRP_WIFI  0x06u   // S3 mainboard WiFi (relayed; the C6 has no radio)
#define DAQ_GRP_SYS   0x07u   // system / misc

#define DAQ_KEY(grp, idx)  ((uint16_t)(((uint16_t)(grp) << 8) | (uint8_t)(idx)))
#define DAQ_KEY_GROUP(key) ((uint8_t)((key) >> 8))

// -----------------------------------------------------------------------------
// Setting keys (stable; never renumber — append only).
// -----------------------------------------------------------------------------
typedef enum {
    // --- Acquisition / ranging ---
    DAQ_K_AUTORANGING     = DAQ_KEY(DAQ_GRP_ACQ, 0x01),  // bool
    DAQ_K_RANGE_IDX       = DAQ_KEY(DAQ_GRP_ACQ, 0x02),  // enum 0=A,1=mA,2=uA
    DAQ_K_SAMPLE_RATE_IDX = DAQ_KEY(DAQ_GRP_ACQ, 0x03),  // enum 0..4
    DAQ_K_STREAMING       = DAQ_KEY(DAQ_GRP_ACQ, 0x04),  // bool
    DAQ_K_USB_DECIMATION  = DAQ_KEY(DAQ_GRP_ACQ, 0x05),  // u16 1..256
    DAQ_K_FILTER          = DAQ_KEY(DAQ_GRP_ACQ, 0x06),  // enum wideband/sinc5/sinc3 (FINE+COARSE)
    DAQ_K_DECIMATION      = DAQ_KEY(DAQ_GRP_ACQ, 0x07),  // enum x32..x1024
    DAQ_K_REJECT_5060     = DAQ_KEY(DAQ_GRP_ACQ, 0x08),  // bool (Sinc3 50/60 Hz reject)
    DAQ_K_SR_MODE         = DAQ_KEY(DAQ_GRP_ACQ, 0x09),  // bool (super-resolution)

    // --- Source / SMU ---
    DAQ_K_SOURCE_ENABLE   = DAQ_KEY(DAQ_GRP_SMU, 0x01),  // bool
    DAQ_K_DUT_VOLTAGE_MV  = DAQ_KEY(DAQ_GRP_SMU, 0x02),  // u16 1800..20000
    DAQ_K_DUT_ILIMIT_MA   = DAQ_KEY(DAQ_GRP_SMU, 0x03),  // u16 100..2500

    // --- DSP ---
    DAQ_K_FFT_ENABLE      = DAQ_KEY(DAQ_GRP_DSP, 0x01),  // bool
    DAQ_K_FFT_LENGTH      = DAQ_KEY(DAQ_GRP_DSP, 0x02),  // enum 64..4096
    DAQ_K_FFT_WINDOW      = DAQ_KEY(DAQ_GRP_DSP, 0x03),  // enum rect/hann/bh
    DAQ_K_FFT_SOURCE      = DAQ_KEY(DAQ_GRP_DSP, 0x04),  // enum current/power
    DAQ_K_MULTIRES_TIERS  = DAQ_KEY(DAQ_GRP_DSP, 0x05),  // u8 1..4
    DAQ_K_STATS_WINDOW_MS = DAQ_KEY(DAQ_GRP_DSP, 0x06),  // u16 10..10000

    // --- Display (C6) ---
    DAQ_K_BRIGHTNESS_PCT  = DAQ_KEY(DAQ_GRP_DISP, 0x01), // u8 10..100
    DAQ_K_DARK_MODE       = DAQ_KEY(DAQ_GRP_DISP, 0x02), // bool

    // --- Neopixel (C6, 8x WS2812 on IO15) ---
    DAQ_K_NPX_MODE        = DAQ_KEY(DAQ_GRP_NPX, 0x01),  // enum off/solid/status/breathe
    DAQ_K_NPX_COLOR       = DAQ_KEY(DAQ_GRP_NPX, 0x02),  // u32 0x00RRGGBB
    DAQ_K_NPX_BRIGHTNESS  = DAQ_KEY(DAQ_GRP_NPX, 0x03),  // u8 0..100

    // --- WiFi (ESP32-S3 mainboard radio; relayed via P4, shown on C6) ---
    DAQ_K_WIFI_ENABLE     = DAQ_KEY(DAQ_GRP_WIFI, 0x01), // bool
    DAQ_K_WIFI_MODE       = DAQ_KEY(DAQ_GRP_WIFI, 0x02), // enum 0=AP,1=STA
    DAQ_K_WIFI_SSID       = DAQ_KEY(DAQ_GRP_WIFI, 0x03), // str <=32
    DAQ_K_WIFI_PASSWORD   = DAQ_KEY(DAQ_GRP_WIFI, 0x04), // str <=64 (secret)

    // --- System ---
    DAQ_K_DEVICE_LABEL    = DAQ_KEY(DAQ_GRP_SYS, 0x01),  // str <=24
} daq_key_t;

// -----------------------------------------------------------------------------
// CONFIG_ACTION ids (stateless one-shot operations, not stored values).
// -----------------------------------------------------------------------------
typedef enum {
    DAQ_ACT_ENERGY_RESET  = 1,
    DAQ_ACT_CHARGE_RESET  = 2,
    DAQ_ACT_FACTORY_RESET = 3,
} daq_action_t;

// -----------------------------------------------------------------------------
// Enum option indices (kept here so P4 + C6 agree without magic numbers).
// -----------------------------------------------------------------------------
enum { DAQ_RANGE_A = 0, DAQ_RANGE_MA = 1, DAQ_RANGE_UA = 2, DAQ_RANGE_COUNT };
enum { DAQ_SR_10K = 0, DAQ_SR_50K, DAQ_SR_100K, DAQ_SR_250K, DAQ_SR_1M, DAQ_SR_COUNT };
enum { DAQ_FFT_64 = 0, DAQ_FFT_128, DAQ_FFT_256, DAQ_FFT_512, DAQ_FFT_1024,
       DAQ_FFT_2048, DAQ_FFT_4096, DAQ_FFT_LEN_COUNT };
enum { DAQ_WIN_RECT = 0, DAQ_WIN_HANN, DAQ_WIN_BLACKMAN_HARRIS, DAQ_WIN_COUNT };
enum { DAQ_FFTSRC_CURRENT = 0, DAQ_FFTSRC_POWER, DAQ_FFTSRC_COUNT };
enum { DAQ_NPX_OFF = 0, DAQ_NPX_SOLID, DAQ_NPX_STATUS, DAQ_NPX_BREATHE,
       DAQ_NPX_CHANNEL, DAQ_NPX_MODE_COUNT };
enum { DAQ_WIFI_AP = 0, DAQ_WIFI_STA = 1, DAQ_WIFI_MODE_COUNT };
enum { DAQ_FILT_WIDEBAND = 0, DAQ_FILT_SINC5, DAQ_FILT_SINC3, DAQ_FILT_COUNT };
enum { DAQ_DEC_32 = 0, DAQ_DEC_64, DAQ_DEC_128, DAQ_DEC_256, DAQ_DEC_512,
       DAQ_DEC_1024, DAQ_DEC_COUNT };

// -----------------------------------------------------------------------------
// Super-Resolution mode (DAQ_K_SR_MODE).
//
// Trades bandwidth for resolution: the ADAQ7769-1 runs Sinc3 at maximum
// decimation (lowest noise bandwidth the part offers) and the P4 then applies a
// windowed-sinc FIR low-pass before decimating to the rates below. Averaging N
// samples buys ~sqrt(N) noise reduction; the FIR supplies the anti-alias
// filtering that keeps the stage-1 noise from folding back into the passband.
//
// Every surface (P4, C6, S3, desktop, iOS) must agree on these rates, so they
// live here rather than being duplicated per-surface.
// -----------------------------------------------------------------------------
#define DAQ_SR_ADC_DECIM      1024u   // ADAQ Sinc3 decimation in SR mode
#define DAQ_SR_CURRENT_SPS    1000u   // fused-current output rate
#define DAQ_SR_VOLTAGE_SPS     500u   // V_DUT output rate

// Sample-rate index -> samples per second.
extern const uint32_t DAQ_SAMPLE_RATE_SPS[DAQ_SR_COUNT];
// FFT length index -> bins.
extern const uint16_t DAQ_FFT_LENGTH_BINS[DAQ_FFT_LEN_COUNT];

// -----------------------------------------------------------------------------
// Schema descriptor for one setting.
// -----------------------------------------------------------------------------
#define DAQ_F_READONLY  0x01u   // status / diagnostics, not settable
#define DAQ_F_C6_LOCAL  0x02u   // owned/applied by the C6 (display/npx)
#define DAQ_F_P4_LOCAL  0x04u   // owned/applied by the P4 (acq/smu/dsp)
#define DAQ_F_PERSIST   0x08u   // persisted to NVS
#define DAQ_F_SECRET    0x10u   // do not echo value in GET_ALL (e.g. password)
#define DAQ_F_S3_LOCAL  0x20u   // owned/applied by the S3 mainboard (wifi); P4 relays

typedef struct {
    uint16_t           key;
    uint8_t            type;      // daq_type_t
    uint8_t            flags;     // DAQ_F_*
    int32_t            min;       // numeric/enum min (enum: 0)
    int32_t            max;       // numeric/enum max (enum: option_count-1)
    int32_t            step;      // editor step (numeric)
    int32_t            def;       // default value (numeric/enum/bool)
    const char        *label;     // human-readable
    const char *const *options;   // enum option labels [max+1], else NULL
} daq_setting_schema_t;

// -----------------------------------------------------------------------------
// TLV helpers. Frame: [key u16 LE][type u8][len u8][value(len) ...].
// -----------------------------------------------------------------------------
#define DAQ_TLV_HDR_LEN 4u
#define DAQ_TLV_MAX_VAL 64u   // longest value (wifi password) fits

// Encode one TLV into @buf (capacity @cap). Returns total bytes written, or
// -1 on overflow / invalid length.
int daq_tlv_encode(uint8_t *buf, size_t cap, uint16_t key, uint8_t type,
                   const void *val, uint8_t val_len);

// Convenience encoders for scalar values; return bytes written or -1.
int daq_tlv_encode_i32(uint8_t *buf, size_t cap, uint16_t key, uint8_t type,
                       int32_t value);

// Parse one TLV at @buf (@len bytes available). Fills @key/@type and points
// @val at the value bytes (in-place, no copy) with @val_len. Returns bytes
// consumed, or -1 if the buffer is too short / malformed.
int daq_tlv_parse(const uint8_t *buf, size_t len, uint16_t *key, uint8_t *type,
                  const uint8_t **val, uint8_t *val_len);

// Decode a TLV scalar value into an int32 (bool/u8/i8/u16/i16/u32/i32/enum).
// Returns true on success; false for non-scalar types or size mismatch.
bool daq_tlv_value_i32(uint8_t type, const uint8_t *val, uint8_t val_len,
                       int32_t *out);

// -----------------------------------------------------------------------------
// Schema access + validation.
// -----------------------------------------------------------------------------
// Look up the schema for @key, or NULL if unknown.
const daq_setting_schema_t *daq_config_schema(uint16_t key);
// Whole table for iteration (GET_ALL / menu build). @count is set to length.
const daq_setting_schema_t *daq_config_table(size_t *count);
// Clamp a numeric/enum value to the schema's [min,max]. Unknown key -> value.
int32_t daq_config_clamp(uint16_t key, int32_t value);

#ifdef __cplusplus
}
#endif
