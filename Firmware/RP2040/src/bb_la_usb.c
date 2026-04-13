// =============================================================================
// bb_la_usb.c — Logic Analyzer USB transport helpers
//
// Vendor bulk is the primary LA data path. One-shot capture readout uses a
// length-prefixed bulk dump; live streaming uses packetized vendor-bulk frames.
// The legacy CDC live path is kept only for compatibility/debugging.
//
// CONSOLIDATED USB OWNERSHIP:
// All tud_vendor_n_write calls for the LA interface MUST happen from the
// usb_thread (tud_task context). bb_cmd_task and other threads queue data
// via ring buffers or status flags.
// =============================================================================

#include "bb_la_usb.h"
#include "bb_la.h"
#include "tusb.h"
#include "pico/stdlib.h"

#include <string.h>
#include "hardware/sync.h"   // __dmb()

#ifdef DEBUGPROBE_INTEGRATION
#include "FreeRTOS.h"
#include "task.h"
extern TaskHandle_t tud_taskhandle;  // defined in bb_main_integrated.c
#endif

// USB vendor OUT command bytes
#define LA_USB_CMD_STOP         0x00
#define LA_USB_CMD_START_STREAM 0x01

// -----------------------------------------------------------------------------
// CDC Legacy Path (Compatibility)
// -----------------------------------------------------------------------------

#define CDC_TX_BUF_SIZE  4096
static uint8_t s_cdc_tx_buf[CDC_TX_BUF_SIZE];
static volatile uint32_t s_cdc_tx_head = 0;
static volatile uint32_t s_cdc_tx_tail = 0;
static uint8_t s_cdc_seq = 0;

static uint32_t cdc_queue_write_framed(const uint8_t *data, uint32_t len) {
    uint32_t head = s_cdc_tx_head;
    uint32_t tail = s_cdc_tx_tail;
    uint32_t packet_len = 2 + len;
    uint32_t free = (tail > head) ? (tail - head - 1) : (CDC_TX_BUF_SIZE - head + tail - 1);
    if (packet_len > free) return 0;

    s_cdc_tx_buf[head] = s_cdc_seq++;
    s_cdc_tx_buf[(head + 1) % CDC_TX_BUF_SIZE] = (uint8_t)len;
    for (uint32_t i = 0; i < len; i++) {
        s_cdc_tx_buf[(head + 2 + i) % CDC_TX_BUF_SIZE] = data[i];
    }
    __dmb();
    s_cdc_tx_head = (head + packet_len) % CDC_TX_BUF_SIZE;
    return len;
}

void bb_la_cdc_send_pending(void) {
    if (!tud_cdc_connected()) return;
    uint32_t head = s_cdc_tx_head;
    uint32_t tail = s_cdc_tx_tail;
    if (head == tail) return;
    uint32_t avail = (head >= tail) ? (head - tail) : (CDC_TX_BUF_SIZE - tail);
    uint32_t cdc_avail = tud_cdc_write_available();
    if (cdc_avail == 0) return;
    uint32_t chunk = (avail < cdc_avail) ? avail : cdc_avail;
    uint32_t written = tud_cdc_write(&s_cdc_tx_buf[tail], chunk);
    tud_cdc_write_flush();
    __dmb();
    s_cdc_tx_tail = (tail + written) % CDC_TX_BUF_SIZE;
}

void bb_la_cdc_flush_ring(void) {
    __dmb();
    s_cdc_tx_tail = 0;
    s_cdc_tx_head = 0;
    __dmb();
}

// -----------------------------------------------------------------------------
// Vendor Bulk Path (Primary)
// -----------------------------------------------------------------------------

// Small ring buffer for control markers (START, STOP, ERROR)
#define BULK_CTRL_BUF_SIZE 256
static uint8_t s_bulk_ctrl_buf[BULK_CTRL_BUF_SIZE];
static volatile uint32_t s_bulk_ctrl_head = 0;
static volatile uint32_t s_bulk_ctrl_tail = 0;

static uint8_t s_live_seq = 0;

// State for active large buffer transfer (live stream half or one-shot readout)
static struct {
    const uint8_t *buf;
    uint32_t total_len;
    uint32_t sent_len;
    bool is_live;
    bool active;
    bool header_sent;
} s_bulk_data;

void bb_la_usb_init(void) {
    s_cdc_seq = 0;
    s_live_seq = 0;
    memset(&s_bulk_data, 0, sizeof(s_bulk_data));
}

void bb_la_usb_live_reset_sequence(void) {
    s_live_seq = 0;
}

bool bb_la_usb_connected(void) {
    return tud_vendor_n_mounted(BB_LA_VENDOR_ITF);
}

void bb_la_usb_abort_bulk(void) {
    uint32_t status = save_and_disable_interrupts();
    s_bulk_data.active = false;
    s_bulk_data.buf = NULL;
    s_bulk_ctrl_tail = 0;
    s_bulk_ctrl_head = 0;
    restore_interrupts(status);
    // Do NOT call tud_vendor_n_flush() here — it must run from the USB task
    // (tud_task context). bb_la_usb_send_pending() flushes at its end, so
    // the next USB task iteration will flush whatever remains in the TX FIFO.
}

bool bb_la_usb_send_stream_marker(uint8_t packet_type, uint8_t info) {
    uint32_t head = s_bulk_ctrl_head;
    uint32_t tail = s_bulk_ctrl_tail;
    uint32_t free = (tail > head) ? (tail - head - 1) : (BULK_CTRL_BUF_SIZE - head + tail - 1);
    if (free < 4) return false;

    s_bulk_ctrl_buf[head] = packet_type;
    s_bulk_ctrl_buf[(head + 1) % BULK_CTRL_BUF_SIZE] = s_live_seq;
    s_bulk_ctrl_buf[(head + 2) % BULK_CTRL_BUF_SIZE] = 0; // len=0 for markers
    s_bulk_ctrl_buf[(head + 3) % BULK_CTRL_BUF_SIZE] = info;
    
    __dmb();
    s_bulk_ctrl_head = (head + 4) % BULK_CTRL_BUF_SIZE;

#ifdef DEBUGPROBE_INTEGRATION
    if (tud_taskhandle) xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
    return true;
}

void bb_la_usb_register_readout(const uint8_t *buf, uint32_t total_bytes) {
    uint32_t status = save_and_disable_interrupts();
    s_bulk_data.buf = buf;
    s_bulk_data.total_len = total_bytes;
    s_bulk_data.sent_len = 0;
    s_bulk_data.is_live = false;
    s_bulk_data.header_sent = false;
    s_bulk_data.active = true;
    restore_interrupts(status);
#ifdef DEBUGPROBE_INTEGRATION
    if (tud_taskhandle) xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
}

void bb_la_usb_send_pending(void) {
    if (!tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) return;

    // 1. Drain control markers (highest priority)
    uint32_t head = s_bulk_ctrl_head;
    uint32_t tail = s_bulk_ctrl_tail;
    while (head != tail) {
        uint32_t avail = tud_vendor_n_write_available(BB_LA_VENDOR_ITF);
        if (avail < 4) break;
        
        uint8_t pkt[4];
        for (int i=0; i<4; i++) pkt[i] = s_bulk_ctrl_buf[(tail + i) % BULK_CTRL_BUF_SIZE];
        tud_vendor_n_write(BB_LA_VENDOR_ITF, pkt, 4);
        tail = (tail + 4) % BULK_CTRL_BUF_SIZE;
        
        uint32_t status = save_and_disable_interrupts();
        s_bulk_ctrl_tail = tail;
        restore_interrupts(status);
    }

    // 2. If no control markers, check for data
    if (head == tail) {
        // If not already active, check if a streaming buffer is ready
        if (!s_bulk_data.active) {
            const uint8_t *stream_buf;
            uint32_t stream_len;
            uint32_t status = save_and_disable_interrupts();
            if (bb_la_stream_get_buffer(&stream_buf, &stream_len)) {
                s_bulk_data.buf = stream_buf;
                s_bulk_data.total_len = stream_len;
                s_bulk_data.sent_len = 0;
                s_bulk_data.is_live = true;
                s_bulk_data.active = true;
            }
            restore_interrupts(status);
        }

        if (s_bulk_data.active) {
            uint32_t usb_avail = tud_vendor_n_write_available(BB_LA_VENDOR_ITF);
            
            if (s_bulk_data.is_live) {
                // LIVE STREAM: Packetized [TYPE:1][SEQ:1][LEN:1][INFO:1][DATA:N]
                // Each packet is max 64 bytes total, so N=60.
                while (s_bulk_data.sent_len < s_bulk_data.total_len && usb_avail >= 64) {
                    uint8_t chunk = (uint8_t)(s_bulk_data.total_len - s_bulk_data.sent_len);
                    if (chunk > 60) chunk = 60;

                    uint8_t packet[64];
                    packet[0] = LA_USB_STREAM_PKT_DATA;
                    packet[1] = s_live_seq++;
                    packet[2] = chunk;
                    packet[3] = LA_USB_STREAM_INFO_NONE;
                    memcpy(&packet[4], s_bulk_data.buf + s_bulk_data.sent_len, chunk);

                    tud_vendor_n_write(BB_LA_VENDOR_ITF, packet, (uint32_t)(4 + chunk));
                    
                    uint32_t status = save_and_disable_interrupts();
                    s_bulk_data.sent_len += chunk;
                    restore_interrupts(status);

                    usb_avail = tud_vendor_n_write_available(BB_LA_VENDOR_ITF);
                }
            } else {
                // ONE-SHOT READOUT: Raw [LEN:4][DATA...]
                if (!s_bulk_data.header_sent) {
                    if (usb_avail >= 4) {
                        uint8_t header[4];
                        header[0] = (uint8_t)(s_bulk_data.total_len & 0xFF);
                        header[1] = (uint8_t)((s_bulk_data.total_len >> 8) & 0xFF);
                        header[2] = (uint8_t)((s_bulk_data.total_len >> 16) & 0xFF);
                        header[3] = (uint8_t)((s_bulk_data.total_len >> 24) & 0xFF);
                        tud_vendor_n_write(BB_LA_VENDOR_ITF, header, 4);
                        
                        uint32_t status = save_and_disable_interrupts();
                        s_bulk_data.header_sent = true;
                        restore_interrupts(status);
                        
                        usb_avail -= 4;
                    }
                }
                
                if (s_bulk_data.header_sent) {
                    uint32_t chunk = s_bulk_data.total_len - s_bulk_data.sent_len;
                    if (chunk > usb_avail) chunk = usb_avail;
                    if (chunk > 0) {
                        tud_vendor_n_write(BB_LA_VENDOR_ITF, s_bulk_data.buf + s_bulk_data.sent_len, chunk);
                        
                        uint32_t status = save_and_disable_interrupts();
                        s_bulk_data.sent_len += chunk;
                        restore_interrupts(status);
                    }
                }
            }

            if (s_bulk_data.sent_len >= s_bulk_data.total_len) {
                if (s_bulk_data.is_live) {
                    bb_la_stream_buffer_sent(s_bulk_data.buf);
                }
                uint32_t status = save_and_disable_interrupts();
                s_bulk_data.active = false;
                restore_interrupts(status);
            }
        }
    }
    
    tud_vendor_n_flush(BB_LA_VENDOR_ITF);
}

// Keep these for API compatibility but they are now either redirects or internal
uint32_t bb_la_usb_write(const uint8_t *data, uint32_t len) {
    (void)data; (void)len;
    // This function is no longer safe to call from outside usb_thread.
    // It is kept only as a stub or could be implemented as a blocking wait for the ring buffer.
    // For now, return 0 to indicate it shouldn't be used directly.
    return 0;
}

uint32_t bb_la_usb_stream_buffer(const uint8_t *buf, uint32_t total_bytes) {
    bb_la_usb_register_readout(buf, total_bytes);
    return total_bytes;
}

uint32_t bb_la_usb_write_live(const uint8_t *buf, uint32_t total_bytes) {
    // This is now handled by bb_la_usb_send_pending polling the streaming buffers.
    // We return total_bytes to satisfy bb_main.c's check, assuming handoff is success.
    (void)buf;
    return total_bytes;
}

uint32_t bb_la_usb_write_raw(const uint8_t *buf, uint32_t total_bytes) {
    return cdc_queue_write_framed(buf, total_bytes);
}

static void cdc_write_reply(const char *msg) {
    if (!tud_cdc_connected()) return;
    tud_cdc_write_str(msg);
    tud_cdc_write_flush();
}

static void handle_stream_command(uint8_t cmd, bool reply_on_cdc) {
    switch (cmd) {
    case LA_USB_CMD_START_STREAM:
        s_cdc_seq = 0;
        bb_la_usb_live_reset_sequence();
        if (!bb_la_start_stream()) {
            if (reply_on_cdc) {
                cdc_write_reply("ERR\n");
            } else {
                bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_ERROR, LA_USB_STREAM_INFO_START_REJECTED);
            }
            return;
        }
        if (reply_on_cdc) {
            cdc_write_reply("START\n");
        } else {
            bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_START, LA_USB_STREAM_INFO_NONE);
        }
        break;

    case LA_USB_CMD_STOP:
        bb_la_stop();
        bb_la_usb_abort_bulk();
        bb_la_cdc_flush_ring();
        if (tud_cdc_connected()) tud_cdc_write_clear();
        if (reply_on_cdc) {
            cdc_write_reply("STOP\n");
        } else {
            bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_STOP, LA_STREAM_STOP_HOST);
        }
        break;

    default:
        if (reply_on_cdc) cdc_write_reply("ERR\n");
        break;
    }
}

void bb_la_usb_poll_commands(void) {
    uint8_t cmd_buf[64];
    if (tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) {
        while (tud_vendor_n_available(BB_LA_VENDOR_ITF) > 0) {
            uint32_t n = tud_vendor_n_read(BB_LA_VENDOR_ITF, cmd_buf, sizeof(cmd_buf));
            for (uint32_t i = 0; i < n; i++) {
                handle_stream_command(cmd_buf[i], false);
            }
        }
    }
    while (tud_cdc_available()) {
        uint32_t n = tud_cdc_read(cmd_buf, sizeof(cmd_buf));
        for (uint32_t i = 0; i < n; i++) {
            handle_stream_command(cmd_buf[i], true);
        }
    }
}
