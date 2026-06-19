#pragma once

// =============================================================================
// s3_link.h — ESP32-S3 mainboard link: HAT-protocol slave on the P4 DAQ board.
//
// Implements the existing BugBuster HAT protocol (Firmware/HAT_Protocol.md) so
// the S3 mainboard can drive the DAQ HAT exactly as it drove the RP2040 HAT:
//
//   Frame: [SYNC 0xAA][LEN][CMD][PAYLOAD 0..32][CRC8]   (CRC over CMD+PAYLOAD)
//   S3 = master (sends commands), P4 = slave (sends responses).
//   The P4 may assert the open-drain IRQ line LOW to request a poll, but never
//   transmits unsolicited frames.
//
// On boot the S3 detects the HAT, sends GET_INFO, reads hat_type =
// HAT_TYPE_DAQ_POWER, and dynamically loads the DAQ resource set (no hotswap).
//
// Standard commands handled here: PING, GET_INFO, GET_CAPS, RESET.
// DAQ-specific commands (0x50..0x5F) are dispatched to the board via a callback.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// Framing constants (match HAT_Protocol.md). The LEN field is a full byte, so
// the DAQ link raises the payload cap above the legacy 32-byte convention to
// carry OTA metadata (56 B) and stream firmware in larger chunks. Standard
// control commands still use small (<= 32 B) payloads for compatibility.
#define HATP_SYNC            0xAAu
#define HATP_MAX_PAYLOAD     240u
#define HATP_CRC_POLY        0x07u

// Largest firmware chunk per OTA_DATA frame: payload = offset(u32) + data.
#define HATP_OTA_CHUNK_MAX   (HATP_MAX_PAYLOAD - 4u)

// Standard commands (master -> slave).
#define HATP_CMD_PING        0x01u
#define HATP_CMD_GET_INFO    0x02u
#define HATP_CMD_RESET       0x05u
#define HATP_CMD_GET_CAPS    0x06u

// DAQ power-analyzer specific commands (vendor sub-range 0x50..0x5F).
#define HATP_CMD_DAQ_START       0x50u   // start acquisition (payload: optional)
#define HATP_CMD_DAQ_STOP        0x51u   // stop acquisition
#define HATP_CMD_DAQ_SET_SOURCE  0x52u   // payload: vdut(f32) ilimit(f32) en(u8)
#define HATP_CMD_DAQ_GET_STATUS  0x53u   // -> range/streaming/energy summary
#define HATP_CMD_DAQ_SYNC        0x54u   // sync epoch (pre/post acquisition)

// Version + OTA commands (vendor sub-range 0x60..0x6F).
#define HATP_CMD_GET_VERSION     0x60u   // -> fw version (u32 + string)
#define HATP_CMD_OTA_BEGIN       0x61u   // payload: ota_meta (size,ver,sha,prod)
#define HATP_CMD_OTA_DATA        0x62u   // payload: firmware bytes chunk
#define HATP_CMD_OTA_END         0x63u   // finalise + verify
#define HATP_CMD_OTA_ABORT       0x64u   // abort in-progress update
#define HATP_CMD_OTA_STATUS      0x65u   // -> ota progress/state
#define HATP_CMD_OTA_CONFIRM     0x66u   // confirm running image (cancel rollback)
#define HATP_CMD_OTA_ROLLBACK    0x67u   // revert to previous image (reboots)

// Responses (slave -> master).
#define HATP_RSP_OK          0x80u
#define HATP_RSP_ERROR       0x81u
#define HATP_RSP_INFO        0x82u
#define HATP_RSP_CAPS        0x87u
#define HATP_RSP_DAQ_STATUS  0x90u   // DAQ status payload
#define HATP_RSP_VERSION     0x91u   // version payload
#define HATP_RSP_OTA_STATUS  0x92u   // OTA status payload

// Firmware version reported in GET_INFO.
#define S3LINK_FW_MAJOR      1u
#define S3LINK_FW_MINOR      0u

// DAQ status payload (response to DAQ_GET_STATUS).
typedef struct __attribute__((packed)) {
    uint8_t  range;          // current_range_t
    uint8_t  streaming;      // bool
    uint8_t  source_enabled; // bool
    uint8_t  _pad;
    float    last_i;
    float    last_v;
    float    last_p;
    float    energy_mwh;
} s3link_daq_status_t;

// OTA_DATA payload: a firmware chunk at a given byte offset (for ordered,
// resumable streaming so the S3 never stages the whole image). The image is
// written straight to the P4 flash one chunk at a time.
typedef struct __attribute__((packed)) {
    uint32_t offset;         // byte offset of this chunk within the image
    // followed by up to HATP_OTA_CHUNK_MAX firmware bytes
} s3link_ota_data_hdr_t;

// Command callback: the board handles DAQ-specific commands and (optionally)
// fills a response payload. Return the number of response bytes written into
// @p resp (<= HATP_MAX_PAYLOAD), or a negative value to send RSP_ERROR.
//   cmd     : the HATP_CMD_DAQ_* code.
//   payload : received payload (may be NULL if len == 0).
//   len     : received payload length.
//   resp    : response payload buffer (HATP_MAX_PAYLOAD bytes).
typedef int (*s3link_cmd_cb_t)(uint8_t cmd, const uint8_t *payload, uint8_t len,
                               uint8_t *resp, void *user);

typedef struct {
    s3link_cmd_cb_t cmd_cb;
    void           *cmd_user;
    TaskHandle_t    task;
    volatile bool   running;
    uint32_t        rx_frames;
    uint32_t        crc_errors;
} s3_link_t;

/**
 * @brief Initialise UART1 + the IRQ line and register the DAQ command callback.
 */
esp_err_t s3_link_init(s3_link_t *s, s3link_cmd_cb_t cmd_cb, void *user);

/** @brief Launch the slave service task (parses frames, sends responses). */
esp_err_t s3_link_start(s3_link_t *s, int task_core, int task_prio);

/** @brief Stop the service task. */
void s3_link_stop(s3_link_t *s);

/** @brief Assert the IRQ line LOW to request a poll from the S3. */
void s3_link_assert_irq(s3_link_t *s);

/** @brief Release the IRQ line (high-Z; external pull-up restores HIGH). */
void s3_link_release_irq(s3_link_t *s);

/** @brief CRC-8 (poly 0x07, init 0x00) over CMD+PAYLOAD — exposed for tests. */
uint8_t s3_link_crc8(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
