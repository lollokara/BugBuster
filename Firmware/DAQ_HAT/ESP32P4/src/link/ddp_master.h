#pragma once

// =============================================================================
// ddp_master.h — ESP32-P4 side of the DAQ HAT Display Protocol (DDP) link.
//
// The P4 is the bus master: it pushes measurement / diagnostics / backlight /
// button-event / config frames to the C6 display co-processor and receives the
// C6's unsolicited config events (menu edits) which it applies to the
// authoritative settings store. Transport = DAQ_UART (UART2), 921600 8N1.
//
// Framing + command set live in common/ddp_proto.h (shared with the C6).
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ddp_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    TaskHandle_t      task;
    volatile bool     running;
    SemaphoreHandle_t tx_lock;     // serialises send_frame across tasks
    uint32_t          rx_frames;
    uint32_t          crc_errors;
    bool              c6_present;  // a GET_INFO reply was seen
    uint8_t           c6_fw_major;
    uint8_t           c6_fw_minor;
    // Pending C6 "Main Board Settings" request (DDP_CMD_MB_REQUEST). Cached here
    // until the S3 polls for it (HATP_CMD_MB_POLL) and executes it. One-deep: a
    // newer request overwrites an unclaimed one.
    volatile bool     mb_req_pending;
    uint8_t           mb_req[32];
    uint8_t           mb_req_len;
    // DUT source calibration engine (smu_cal_t*) for DDP_CMD_CAL_CTRL. Bound by
    // daq_board after both the calibration engine and this link are up.
    void             *cal;
} ddp_master_t;

// Initialise UART2 + the TX mutex.
esp_err_t ddp_master_init(ddp_master_t *m);

// Launch the RX service task (handles C6 -> P4 config events + responses).
esp_err_t ddp_master_start(ddp_master_t *m, int task_core, int task_prio);

void ddp_master_stop(ddp_master_t *m);

// Stop the RX task AND release the UART driver, so another user (the C6 flasher)
// can take over UART2. Pair with ddp_master_init()+ddp_master_start() to resume.
void ddp_master_deinit(ddp_master_t *m);

// Low-level: send one DDP frame (SYNC/LEN/CMD/PAYLOAD/CRC). Thread-safe.
void ddp_master_send(ddp_master_t *m, uint8_t cmd, const uint8_t *payload,
                     uint8_t len);

// Consume the pending C6 mainboard request (DDP_CMD_MB_REQUEST) into buf for
// forwarding to the S3. Returns the length copied (<= cap), or 0 if none.
uint8_t ddp_master_take_mb_request(ddp_master_t *m, uint8_t *buf, uint8_t cap);

// Peek the pending request's type byte (DDP_MB_*) without consuming it; 0 if
// none pending. Used to keep serving lightweight power requests during
// acquisition while deferring heavier script requests.
uint8_t ddp_master_peek_mb_type(const ddp_master_t *m);

// --- Convenience senders (P4 -> C6) ----------------------------------------
void ddp_master_button_event(ddp_master_t *m, uint8_t events);   // DDP_BTN_*
void ddp_master_set_measurement(ddp_master_t *m, float v, float i, uint8_t flags);
void ddp_master_set_diagnostics(ddp_master_t *m, const ddp_diag_t *d);
void ddp_master_set_backlight(ddp_master_t *m, uint8_t level);
// Tell the C6 to enter (enable=true) or leave (enable=false) WiFi streaming
// mode: it stops rendering the normal readout/menu and shows a static "WiFi
// streaming" screen while the SDIO link is handed to ESP-Hosted.
void ddp_master_set_wifi_stream_mode(ddp_master_t *m, bool enable);
// 4 channel-status colour codes (front 4-connector) -> C6 neopixels (pairs).
void ddp_master_set_ch_leds(ddp_master_t *m, const uint8_t codes[4]);
// Push one or more pre-encoded TLVs so the C6 menu reflects S3-side edits.
void ddp_master_config_push(ddp_master_t *m, const uint8_t *tlvs, uint8_t len);

#ifdef __cplusplus
}
#endif
