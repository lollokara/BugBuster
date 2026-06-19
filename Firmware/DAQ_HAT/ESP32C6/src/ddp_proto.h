#pragma once

// ===========================================================================
// DAQ HAT Display Protocol (DDP)  -  ESP32-P4  ->  ESP32-C6
// ===========================================================================
// The ESP32-P4 ("application processor") is the bus master. The ESP32-C6
// ("display co-processor") is the slave: it only renders what it is told and
// answers PING / GET_INFO. The framing intentionally mirrors the RP2040 HAT
// protocol (see Firmware/HAT_Protocol.md) so tooling and CRC code can be
// shared.
//
//   +------+-----+-----+------------------+------+
//   | SYNC | LEN | CMD |     PAYLOAD      | CRC  |
//   | 1B   | 1B  | 1B  |     0..32 B      | 1B   |
//   +------+-----+-----+------------------+------+
//
//   SYNC = 0xAA
//   LEN  = payload length (0..32)
//   CMD  = command id (master->slave) or response id (slave->master)
//   CRC  = CRC-8 (poly 0x07, init 0x00) over CMD + PAYLOAD
//   Multi-byte fields are little-endian.
// ===========================================================================

#include <stdint.h>
#include <stddef.h>

#define DDP_SYNC            0xAAu
#define DDP_MAX_PAYLOAD     32u
#define DDP_PROTO_VERSION   2u

// --- Commands (master -> slave), 0x01..0x7F --------------------------------
#define DDP_CMD_PING            0x01u  // -> RSP_OK
#define DDP_CMD_GET_INFO        0x02u  // -> RSP_INFO
#define DDP_CMD_SET_MEASUREMENT 0x10u  // float voltage_v, float current_a, u8 flags
#define DDP_CMD_SET_STATUS      0x11u  // u8 state, ASCII label (<=30 bytes)
#define DDP_CMD_SET_BACKLIGHT   0x12u  // u8 brightness 0..255
#define DDP_CMD_CLEAR           0x13u  // blank readouts / show "no data"
#define DDP_CMD_SET_DIAGNOSTICS 0x14u  // ddp_diag_t — full diagnostics snapshot

// --- Events (C6 -> P4), 0x60..0x7F -----------------------------------------
// The C6 emits these unsolicited when the user changes settings on-device, so
// the P4 can apply them. (The half-duplex link is otherwise master-polled.)
#define DDP_CMD_SET_CONFIG      0x60u  // ddp_config_t — current device settings

// --- Responses (slave -> master), 0x80..0xFF -------------------------------
#define DDP_RSP_OK              0x80u
#define DDP_RSP_INFO            0x82u  // u8 hat_type, u8 fw_major, u8 fw_minor, u8 proto
#define DDP_RSP_ERR             0xFFu  // u8 error code

// --- SET_MEASUREMENT flag bits ---------------------------------------------
#define DDP_FLAG_V_VALID        0x01u  // voltage field holds live data
#define DDP_FLAG_I_VALID        0x02u  // current field holds live data
#define DDP_FLAG_V_OVERRANGE    0x04u
#define DDP_FLAG_I_OVERRANGE    0x08u

// --- Link / connection state ------------------------------------------------
#define DDP_STATE_BOOT          0u
#define DDP_STATE_LIVE          1u   // receiving frames from P4
#define DDP_STATE_SIM           2u   // no link: locally generated demo data
#define DDP_STATE_FAULT         3u

// Payload of DDP_CMD_SET_MEASUREMENT (little-endian, 9 bytes).
typedef struct __attribute__((packed)) {
    float   voltage_v;   // volts
    float   current_a;   // amperes
    uint8_t flags;       // DDP_FLAG_*
} ddp_measurement_t;

// Payload of DDP_CMD_SET_DIAGNOSTICS (little-endian, 21 bytes). Compact
// fixed-point so the whole snapshot fits one frame. The P4 pushes this
// periodically; the C6 caches it for the Diagnostics menu.
typedef struct __attribute__((packed)) {
    int16_t  adc_temp_c10;     // ADC die temp, 0.1 C
    int16_t  vsrc_temp_c10;    // voltage-source temp, 0.1 C
    int16_t  esp_current_ma;   // ESP32 current readout, mA
    int16_t  esp_input_ma;     // ESP32 input current, mA
    uint16_t p4_free_stack_b;  // P4 min free stack, bytes
    uint16_t p4_free_mem_kb;   // P4 free heap, KB
    uint8_t  p4_tasks;         // P4 task count
    int16_t  mb_temp_c10;      // main board temp, 0.1 C
    uint16_t mb_pd_mv;         // USB-PD rail, mV
    uint16_t mb_dvcc_mv;       // DVCC rail, mV
    uint16_t mb_avcc_mv;       // AVCC rail, mV
} ddp_diag_t;

// Payload of DDP_CMD_SET_CONFIG (little-endian, 10 bytes). Mirrors the on-
// device settings the user can edit through the menu.
typedef struct __attribute__((packed)) {
    uint8_t  autoranging;      // 0/1
    uint8_t  range_idx;        // 0=A, 1=mA, 2=uA
    uint8_t  sample_rate_idx;  // 0..4
    uint8_t  reserved;
    uint16_t dut_current_ma;   // 100..2500
    uint16_t dut_voltage_mv;   // 1800..20000
    uint8_t  brightness_pct;   // 10..100
    uint8_t  dark_mode;        // 0/1
} ddp_config_t;

// CRC-8, poly 0x07, init 0x00 (matches RP2040 HAT protocol).
static inline uint8_t ddp_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}
