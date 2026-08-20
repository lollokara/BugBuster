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
//   6       4     seq      (monotonic per stream). Contract: seq increments
//                 for every frame the device DECIDES to emit, even when that
//                 frame never reaches the wire (no transport, host not
//                 connected, TX back-pressure, or a short/failed write) --
//                 only the value written into a byte stream the host actually
//                 receives can skip. This means a gap between consecutive
//                 received seq values is always exactly the count of frames
//                 the device dropped in between, so host decoders can use it
//                 directly as a loss counter without a separate drop signal.
//   10      2     payload_len
//   12      N     payload
//   12+N    2     crc16-ccitt over bytes [2 .. 12+N) (header tail + payload)
//
// Device->PC data frames (type < 0x80) write 0x0000 in the CRC slot and do not
// compute a CRC — USB bulk transfers already carry hardware CRC/ACK integrity
// checking, so a software CRC on every high-rate WAVE_I/WAVE_V frame is pure
// overhead. PC->device control frames (type >= 0x80) still carry a real
// CRC-16/CCITT-FALSE and are verified on receipt.
//
// All multi-byte integers and floats are little-endian (native to the P4).
// =============================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_PROTO_MAGIC0     0xBBu
#define USB_PROTO_MAGIC1     0x50u
#define USB_PROTO_VERSION    2u

#define USB_FRAME_HEADER_LEN 12u   // bytes before payload
#define USB_FRAME_CRC_LEN    2u
#define USB_FRAME_OVERHEAD   (USB_FRAME_HEADER_LEN + USB_FRAME_CRC_LEN)
#define USB_MAX_PAYLOAD      16384u

// Record / frame types. 0x00..0x7F = device->PC data, 0x80+ = PC->device control.
typedef enum {
    USB_REC_WAVE_I   = 0x01,   // struct-of-arrays fused-current waveform block
    USB_REC_STATS    = 0x02,   // min/max/mean/rms/std for I, V, P
    USB_REC_ENERGY   = 0x03,   // energy + charge accumulators
    USB_REC_FFT      = 0x04,   // spectrum magnitude bins (Phase 5)
    USB_REC_MARKER   = 0x05,   // digital event marker (from S3, later)
    USB_REC_STATUS   = 0x06,   // device status / heartbeat
    USB_REC_WAVE_V   = 0x07,   // struct-of-arrays voltage waveform block

    USB_CMD_START        = 0x80,
    USB_CMD_STOP         = 0x81,
    USB_CMD_SET_RATE     = 0x82,   // payload: usb_cmd_rate_t
    USB_CMD_RANGE_LOCK   = 0x83,   // payload: u8 range (0xFF = auto)
    USB_CMD_RESET_ENERGY = 0x84,
    USB_CMD_RESET_STATS  = 0x85,
    USB_CMD_FFT_CONFIG   = 0x86,   // payload: usb_cmd_fft_t
    USB_CMD_SET_SOURCE   = 0x87,   // payload: usb_cmd_source_t (SMU)
    USB_CMD_ARM          = 0x88,   // payload: usb_cmd_arm_t (trigger pre-roll)
    USB_CMD_RANGE_CAL_START = 0x89, // payload: usb_cmd_range_cal_t (start cal)
    USB_CMD_RANGE_CAL_ACK   = 0x8A, // no payload (advance past PROMPT)
    USB_CMD_RANGE_CAL_ABORT = 0x8B, // no payload

    // Direct desktop -> P4 `staging` ingest (bypasses S3/WiFi entirely).
    USB_CMD_OTA_BEGIN       = 0x8C, // payload: ota_meta_t + target byte (RELAY_TARGET_C6/_S3)
    USB_CMD_OTA_DATA        = 0x8D, // payload: u32 offset + firmware bytes
    USB_CMD_OTA_END         = 0x8E, // no payload; finalizes + verifies staged image
    USB_CMD_OTA_ABORT       = 0x8F, // no payload
} usb_rec_type_t;

// ---- WAVE_I / WAVE_V records -------------------------------------------------
// Common 24-byte header for both SoA waveform records. WAVE_I payload
// continues with count * float i[] then count * uint8_t meta[]. WAVE_V
// payload continues with count * float v[] only (no meta array).
typedef struct __attribute__((packed)) {
    uint64_t start_index;    // sequence of the first sample in this block
    uint64_t timestamp_us;   // shared sync-epoch timestamp at slot 0
    uint32_t sample_rate;    // samples/second
    uint16_t count;          // number of samples following
    uint8_t  decimation;     // 1 = full rate; >1 = decimated view. WAVE_V: always 1
    uint8_t  _pad;
} usb_wave_hdr_t;            // 24 bytes

// meta[i] bit layout (WAVE_I only): bits 0-1 range, bits 2-3 source,
// bit 4 saturated, bit 5 settling.
#define USB_META_RANGE(m)    ((m) & 0x03)
#define USB_META_SOURCE(m)   (((m) >> 2) & 0x03)
#define USB_META_SATURATED   0x10u
#define USB_META_SETTLING    0x20u

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
    uint64_t sample_index;   // fused-sample index this marker aligns to
    uint64_t timestamp_us;   // shared sync-epoch timestamp
    uint8_t  channel;        // digital marker channel (S3 IO number, 1..12)
    uint8_t  edge;           // 0 = falling, 1 = rising
    uint8_t  kind;           // USB_MARK_KIND_* (flag / trigger)
    uint8_t  _pad;
} usb_marker_payload_t;      // 20 bytes

// MARKER kind codes (usb_marker_payload_t.kind).
#define USB_MARK_KIND_FLAG     0u   // informational event flag (vertical line)
#define USB_MARK_KIND_TRIGGER  1u   // acquisition trigger fired (defines t=0)

// ---- STATUS record ----------------------------------------------------------
// Extension v1 (bytes 20-27): input-rail sense (in_voltage, in_current).
// Extension v2 (bytes 28-35): FINE ADC health (adaq_ok_bits, fine_err_pct,
//   drop_fine, drop_coarse). Extension v3 (bytes 36-55): USB streaming
//   performance counters. Extension v4 (bytes 56-72): direct-USB relay/
//   staging ingest progress (see ota/relay_stage.h). Extension v5 (bytes
//   72-87): per-record-type TX/drop counters, split by WAVE_I vs WAVE_V.
//   Extension v6 (bytes 88-95): acquisition configuration readback (filter,
//   ADC decimation, stream decimation, actual ODR).
//   Older parsers silently ignore trailing bytes.
typedef struct __attribute__((packed)) {
    uint32_t sample_rate;     // 0
    uint32_t overflow_count;  // 4
    uint8_t  range;           // 8  — current_range_t
    uint8_t  streaming;       // 9  — bool
    uint8_t  range_locked;    // 10 — bool
    uint8_t  source_enabled;  // 11 — SMU on
    float    vdut_set;        // 12 — programmed V_DUT (V)
    float    ilimit_set;      // 16 — programmed current limit (A)
    // Extension v1 (bytes 20-27)
    float    in_voltage;      // 20 — SMU input voltage (V), 0 if N/A
    float    in_current;      // 24 — SMU input current (A), 0 if N/A
    // Extension v2 (bytes 28-35)
    uint8_t  adaq_ok_bits;      // 28 — bit0=FINE ok, bit1=COARSE ok, bit2=VOLT ok
    uint8_t  fine_err_pct;      // 29 — FINE STATUS_ERR % of last window (0-100)
    uint16_t drop_fine;         // 30 — FINE pairing-resync drops (saturates, see cal note)
    uint16_t drop_coarse;       // 32 — COARSE pairing-resync drops (saturates)
    uint8_t  fine_diag_sticky;  // 34 — OR of all MASTER_STATUS bits seen on FINE (ADAQ 0x2D)
    uint8_t  _pad;              // 35
    // Extension v3 (bytes 36-55): USB streaming performance counters.
    uint32_t frames_tx;         // 36 — total data frames transmitted
    uint32_t bytes_per_sec;     // 40 — EMA of TX throughput
    uint32_t fifo_drop_frames;  // 44 — frames dropped for back-pressure/no-transport
    uint32_t ring_high_water;   // 48 — max adaq ring fill seen (samples)
    uint32_t wave_i_index_lo;   // 52 — low 32 bits of live fused index
    // Extension v4 (bytes 56-75): direct-USB relay/staging ingest progress.
    // Sourced from relay_stage_get_status(); sha256 is intentionally omitted
    // (too large for a 10Hz frame) — desktop only needs progress/state here.
    uint8_t  relay_target;      // 56 — relay_target_t (0=none, 1=C6, 2=S3)
    uint8_t  relay_state;       // 57 — relay_state_t (IDLE/STAGING/STAGED/PUSHING/DONE/FAILED)
    uint16_t _pad2;             // 58
    uint32_t relay_image_size;  // 60 — total expected image bytes
    uint32_t relay_staged_bytes;// 64 — bytes written to staging partition so far
    uint32_t relay_pushed_bytes;// 68 — bytes pushed onward to target so far
    // Extension v5 (bytes 72-87): per-record-type TX accounting. Added to
    // diagnose asymmetric voltage loss -- the aggregate frames_tx/
    // fifo_drop_frames above cannot distinguish "WAVE_V was never built"
    // from "WAVE_V was built and dropped at the transport".
    uint32_t wave_i_frames;     // 72 — WAVE_I frames handed to the transport
    uint32_t wave_v_frames;     // 76 — WAVE_V frames handed to the transport
    uint32_t wave_i_drops;      // 80 — WAVE_I frames dropped (back-pressure/no transport)
    uint32_t wave_v_drops;      // 84 — WAVE_V frames dropped
    // --- extension v6 (offsets 92..99): acquisition configuration readback.
    // The device reports what it ACTUALLY applied, never what was requested:
    // the driver clamps filter/decimation combinations the part cannot hit,
    // and a UI that echoed its own request would silently misreport the rate.
    uint8_t  filter;        // 88  ADAQ_FILTER_*
    uint8_t  adc_dec;       // 89  ADAQ_DEC_*, or 0xFF when SINC3 programmable
    uint16_t stream_decim;  // 90  P4 stream decimation (>=1)
    uint32_t odr_mhz;       // 92  actual ODR, milli-SPS (ODR * 1000)
    // --- extension v7 (offsets 100..103): onboard board temperatures, 0.1 C.
    // The two AD7415s (U2 analog area, U28 power area). USB_TEMP_NA when the
    // sensor is absent or has not been polled yet.
    //
    // These are CACHED values refreshed by the ~1 Hz housekeeping poll
    // (diagnostics_push), never read here: this struct is filled on
    // daq_fast_task, and a blocking I2C transaction on the acquisition
    // producer would stall the capture. The ADAQ die sensors are deliberately
    // absent -- they share the converter and are only readable while
    // acquisition is stopped, i.e. never during a stream.
    //
    // Why on the wire at all: at SR rates the residual noise is dominated by
    // low-frequency drift, and thermal drift is the largest known contributor.
    // Without a temperature stamped on the same frames, a drifting sigma is
    // indistinguishable from converter 1/f.
    int16_t  t_board0_c10;  // 96   AD7415 U2  (analog area)
    int16_t  t_board1_c10;  // 98   AD7415 U28 (power area)
    // --- extension v8 (offsets 100..103): per-range current calibration validity.
    // Bit-packed flags: bit0=HI calibrated, bit1=MID calibrated, bit2=LO calibrated.
    // When a bit is 0, the corresponding range applies UNCALIBRATED conversion
    // (design shunt/gain constants, zero offset, unity gain correction) and may
    // carry significant offset/gain error. Host must flag captures taken while
    // any active range is uncalibrated as suspect. Sourced from smu_cal.h's
    // smu_range_cal_blob_t.have[] array (NVS-persisted interactive TUI meter cal).
    //
    // Appended, never inserted: every field above keeps its v2 offset so a host
    // built against an older header still parses everything it knows about.
    uint8_t  cal_have_rcal; // 100  per-range current cal validity (bits 0-2)
    uint8_t  _pad3[3];      // 101-103  reserved
} usb_status_payload_t;     // total: 104 bytes

#define USB_TEMP_NA  ((int16_t)0x7FFF)   // sensor absent / not yet polled

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

// ---- USB_CMD_RANGE_CAL_START payload --------------------------------------
// Operator supplies the two precision resistor values (Ω). Defaults (5600 / 56)
// are used if the values are zero.
typedef struct __attribute__((packed)) {
    float r_cal_a_ohm;   // Pass A resistor (HI/MID boundary), e.g. 5600.0
    float r_cal_b_ohm;   // Pass B resistor (MID/LO boundary),  e.g. 56.0
} usb_cmd_range_cal_t;

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF).
uint16_t usb_proto_crc16(const uint8_t *data, uint32_t len, uint16_t init);

#ifdef __cplusplus
}
#endif
