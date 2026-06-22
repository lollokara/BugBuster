#pragma once

// =============================================================================
// usb_proto.h — wire protocol for the power-analyzer USB-HS measurement stream.
//
// Frames are pushed device -> PC over the ESP32-P4 USB-HS vendor-bulk endpoint.
// Control commands flow PC -> device. The protocol is transport-agnostic: this
// header only defines the byte layout; usb_stream.* does the framing and
// usb_backend_*.c binds it to TinyUSB.
//
// Frame layout (little-endian):
//   offset  size  field
//   0       2     magic = 0xBB 0x50
//   2       1     version (USB_PROTO_VERSION)
//   3       1     type    (usb_rec_type_t)
//   4       1     flags
//   5       1     reserved (0)
//   6       4     seq      (monotonic per stream)
//   10      2     payload_len
//   12      N     payload
//   12+N    2     crc16-ccitt over bytes [2 .. 12+N) (header tail + payload)
//
// All multi-byte integers and floats are little-endian (native to the P4).
// =============================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_PROTO_MAGIC0     0xBBu
#define USB_PROTO_MAGIC1     0x50u
#define USB_PROTO_VERSION    1u

#define USB_FRAME_HEADER_LEN 12u   // bytes before payload
#define USB_FRAME_CRC_LEN    2u
#define USB_FRAME_OVERHEAD   (USB_FRAME_HEADER_LEN + USB_FRAME_CRC_LEN)
#define USB_MAX_PAYLOAD      4096u

// Record / frame types. 0x00..0x7F = device->PC data, 0x80+ = PC->device control.
typedef enum {
    USB_REC_WAVEFORM = 0x01,   // block of fused samples
    USB_REC_STATS    = 0x02,   // min/max/mean/rms/std for I, V, P
    USB_REC_ENERGY   = 0x03,   // energy + charge accumulators
    USB_REC_FFT      = 0x04,   // spectrum magnitude bins (Phase 5)
    USB_REC_MARKER   = 0x05,   // digital event marker (from S3, later)
    USB_REC_STATUS   = 0x06,   // device status / heartbeat

    USB_CMD_START        = 0x80,
    USB_CMD_STOP         = 0x81,
    USB_CMD_SET_RATE     = 0x82,   // payload: usb_cmd_rate_t
    USB_CMD_RANGE_LOCK   = 0x83,   // payload: u8 range (0xFF = auto)
    USB_CMD_RESET_ENERGY = 0x84,
    USB_CMD_RESET_STATS  = 0x85,
    USB_CMD_FFT_CONFIG   = 0x86,   // payload: usb_cmd_fft_t
    USB_CMD_SET_SOURCE   = 0x87,   // payload: usb_cmd_source_t (SMU)
    USB_CMD_ARM          = 0x88,   // payload: usb_cmd_arm_t (trigger pre-roll)
} usb_rec_type_t;

// ---- WAVEFORM record --------------------------------------------------------
// One fused sample. 16 bytes, naturally aligned.
typedef struct __attribute__((packed)) {
    float    i;        // fused current (A)
    float    v;        // DUT voltage (V)
    float    p;        // power (W)
    uint8_t  range;    // current_range_t
    uint8_t  source;   // fuse_source_t
    uint8_t  flags;    // bit0 saturated
    uint8_t  _pad;
} usb_wave_sample_t;

// WAVEFORM payload = header + count * usb_wave_sample_t.
typedef struct __attribute__((packed)) {
    uint32_t start_seq;     // sequence of the first sample in this block
    uint32_t sample_rate;   // samples/second (fused current ODR)
    uint16_t count;         // number of samples following
    uint8_t  decimation;    // 1 = full rate; >1 = decimated waveform view
    uint8_t  _pad;
} usb_wave_header_t;

// ---- STATS record -----------------------------------------------------------
typedef struct __attribute__((packed)) {
    float    min, max, mean, rms, std;
    uint32_t count;
} usb_stat_block_t;

typedef struct __attribute__((packed)) {
    usb_stat_block_t i;
    usb_stat_block_t v;
    usb_stat_block_t p;
} usb_stats_payload_t;

// ---- ENERGY record ----------------------------------------------------------
typedef struct __attribute__((packed)) {
    double   energy_mwh;
    double   energy_j;
    double   charge_mah;
    double   charge_c;
    double   elapsed_s;
    float    last_i;
    float    last_v;
    float    last_p;
} usb_energy_payload_t;

// ---- FFT record (Phase 5) ---------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t sample_rate;
    uint16_t nbins;
    uint8_t  source;     // 0 = current, 1 = power
    uint8_t  window;     // window id
    // followed by nbins * float magnitude
} usb_fft_header_t;

// ---- MARKER record ----------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t sample_index;   // fused-sample index this marker aligns to
    uint64_t timestamp_us;   // shared sync-epoch timestamp
    uint8_t  channel;        // digital marker channel (S3 IO number, 1..12)
    uint8_t  edge;           // 0 = falling, 1 = rising
    uint8_t  kind;           // USB_MARK_KIND_* (flag / trigger)
    uint8_t  _pad;
} usb_marker_payload_t;

// MARKER kind codes (usb_marker_payload_t.kind).
#define USB_MARK_KIND_FLAG     0u   // informational event flag (vertical line)
#define USB_MARK_KIND_TRIGGER  1u   // acquisition trigger fired (defines t=0)

// ---- STATUS record ----------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t sample_rate;
    uint32_t overflow_count;
    uint8_t  range;
    uint8_t  streaming;       // bool
    uint8_t  range_locked;    // bool
    uint8_t  source_enabled;  // SMU on
    float    vdut_set;        // programmed V_DUT (V)
    float    ilimit_set;      // programmed current limit (A)
} usb_status_payload_t;

// ---- Control command payloads ----------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t current_sps;   // requested current sample rate
    uint32_t voltage_sps;   // requested voltage sample rate
    uint8_t  decimation;    // waveform decimation for the stream
    uint8_t  _pad[3];
} usb_cmd_rate_t;

typedef struct __attribute__((packed)) {
    uint16_t nbins;         // FFT length / 2
    uint8_t  source;        // 0 = current, 1 = power
    uint8_t  window;        // window id
    uint8_t  enabled;       // continuous FFT on/off
    uint8_t  _pad[3];
} usb_cmd_fft_t;

typedef struct __attribute__((packed)) {
    float    vdut;          // requested V_DUT (V), 0 = off
    float    ilimit;        // requested current limit (A)
    uint8_t  enable;        // SMU output enable
    uint8_t  _pad[3];
} usb_cmd_source_t;

// Trigger / pre-roll arming. The S3 owns the IO event logic; the P4 streams
// continuously and the PC keeps the pre-trigger window, so this just records
// the requested pre-roll depth and arms the trigger latch. A trigger MARKER
// (kind = USB_MARK_KIND_TRIGGER) defines t=0 for the captured window.
typedef struct __attribute__((packed)) {
    uint8_t  armed;         // 1 = arm trigger latch, 0 = free-run/disarm
    uint8_t  trig_logic;    // 0 = none, 1 = OR, 2 = AND (informational)
    uint16_t _pad;
    uint32_t pre_samples;   // requested pre-trigger depth (fused samples)
} usb_cmd_arm_t;

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF).
uint16_t usb_proto_crc16(const uint8_t *data, uint32_t len, uint16_t init);

#ifdef __cplusplus
}
#endif
