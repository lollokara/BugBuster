// =============================================================================
// bb_la_usb.c — Logic Analyzer USB transport helpers
//
// Vendor bulk is the primary LA data path. One-shot capture readout uses a
// length-prefixed bulk dump; live streaming uses packetized vendor-bulk frames.
// The legacy CDC live path is kept only for compatibility/debugging.
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

// Shared CDC TX ring buffer: bb_cmd_task writes here, usb_thread sends via tud_cdc_write
#define CDC_TX_BUF_SIZE  4096
static uint8_t s_cdc_tx_buf[CDC_TX_BUF_SIZE];
static volatile uint32_t s_cdc_tx_head = 0;  // written by bb_cmd_task
static volatile uint32_t s_cdc_tx_tail = 0;  // read by usb_thread
static uint8_t s_cdc_seq = 0;
static uint8_t s_live_seq = 0;

// Queue data for CDC TX (called from bb_cmd_task)
// We wrap each chunk into a framed packet: [SEQ:1][LEN:1][DATA:N]
// MAX_CHUNK is 62 to fit in a 64-byte USB packet.
static uint32_t cdc_queue_write_framed(const uint8_t *data, uint32_t len) {
    uint32_t head = s_cdc_tx_head;
    uint32_t tail = s_cdc_tx_tail;
    
    // Each packet is 2 bytes header + data
    uint32_t packet_len = 2 + len;
    
    uint32_t free = (tail > head) ? (tail - head - 1) : (CDC_TX_BUF_SIZE - head + tail - 1);
    if (packet_len > free) return 0; // Not enough space for the whole frame

    s_cdc_tx_buf[head] = s_cdc_seq++;
    s_cdc_tx_buf[(head + 1) % CDC_TX_BUF_SIZE] = (uint8_t)len;
    
    for (uint32_t i = 0; i < len; i++) {
        s_cdc_tx_buf[(head + 2 + i) % CDC_TX_BUF_SIZE] = data[i];
    }
    
    __dmb();
    s_cdc_tx_head = (head + packet_len) % CDC_TX_BUF_SIZE;
    return len;
}

void bb_la_usb_init(void)
{
    // Nothing to init — TinyUSB handles endpoint setup from descriptor
    s_cdc_seq = 0;
    s_live_seq = 0;
}

void bb_la_usb_live_reset_sequence(void)
{
    s_live_seq = 0;
}

bool bb_la_usb_connected(void)
{
    return tud_vendor_n_mounted(BB_LA_VENDOR_ITF);
}

uint32_t bb_la_usb_write(const uint8_t *data, uint32_t len)
{
    if (!tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) return 0;

    uint32_t written = 0;
    uint32_t stall_count = 0;
    while (written < len) {
        uint32_t avail = tud_vendor_n_write_available(BB_LA_VENDOR_ITF);
        if (avail == 0) {
            tud_vendor_n_flush(BB_LA_VENDOR_ITF);
#ifdef DEBUGPROBE_INTEGRATION
            // Notify USB task to process the endpoint transfer
            xTaskNotify(tud_taskhandle, 0, eNoAction);
            vTaskDelay(1);
#else
            tud_task();
            sleep_us(100);
#endif
            stall_count++;
            if (stall_count > 10000) {  // ~1 second timeout
                break;
            }
            continue;
        }
        stall_count = 0;
        uint32_t chunk = (len - written < avail) ? (len - written) : avail;
        uint32_t n = tud_vendor_n_write(BB_LA_VENDOR_ITF, data + written, chunk);
        written += n;
    }
    tud_vendor_n_flush(BB_LA_VENDOR_ITF);
#ifdef DEBUGPROBE_INTEGRATION
    xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
    return written;
}

uint32_t bb_la_usb_stream_buffer(const uint8_t *buf, uint32_t total_bytes)
{
    if (!tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) return 0;

    // Flush any stale data from previous transfers
    tud_vendor_n_flush(BB_LA_VENDOR_ITF);
    tud_task();

    // Send a 4-byte header first: total length (LE)
    uint8_t header[4];
    header[0] = (uint8_t)(total_bytes & 0xFF);
    header[1] = (uint8_t)((total_bytes >> 8) & 0xFF);
    header[2] = (uint8_t)((total_bytes >> 16) & 0xFF);
    header[3] = (uint8_t)((total_bytes >> 24) & 0xFF);
    bb_la_usb_write(header, 4);

    // Stream data in 64-byte chunks (USB Full-Speed max packet)
    uint32_t sent = 0;
    while (sent < total_bytes) {
        uint32_t chunk = total_bytes - sent;
        if (chunk > 64) chunk = 64;
        uint32_t n = bb_la_usb_write(buf + sent, chunk);
        sent += n;
        if (n == 0) break;  // USB disconnected
    }

    return sent;
}

uint32_t bb_la_usb_write_live(const uint8_t *buf, uint32_t total_bytes)
{
    if (!tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) return 0;

    uint32_t sent = 0;
    while (sent < total_bytes) {
        uint8_t packet[64];
        uint8_t chunk = (uint8_t)((total_bytes - sent > 60) ? 60 : (total_bytes - sent));
        packet[0] = LA_USB_STREAM_PKT_DATA;
        packet[1] = s_live_seq++;
        packet[2] = chunk;
        packet[3] = LA_USB_STREAM_INFO_NONE;
        memcpy(&packet[4], buf + sent, chunk);

        uint32_t written = bb_la_usb_write(packet, (uint32_t)(4 + chunk));
        if (written != (uint32_t)(4 + chunk)) {
            break;
        }
        sent += chunk;
    }
    return sent;
}

bool bb_la_usb_send_stream_marker(uint8_t packet_type, uint8_t info)
{
    if (!tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) return false;

    uint8_t packet[4];
    packet[0] = packet_type;
    packet[1] = s_live_seq;
    packet[2] = 0;
    packet[3] = info;
    return bb_la_usb_write(packet, sizeof(packet)) == sizeof(packet);
}

uint32_t bb_la_usb_write_raw(const uint8_t *buf, uint32_t total_bytes)
{
    if (!tud_cdc_connected()) return 0;

    // Queue data for CDC TX in framed chunks
    uint32_t sent = 0;
    uint32_t stall = 0;
    while (sent < total_bytes) {
        uint32_t chunk = total_bytes - sent;
        if (chunk > 62) chunk = 62; // 64 - 2 bytes header

        uint32_t n = cdc_queue_write_framed(buf + sent, chunk);
        if (n == 0) {
            // Queue full — wait for usb_thread to drain
#ifdef DEBUGPROBE_INTEGRATION
            xTaskNotify(tud_taskhandle, 0, eNoAction);
            vTaskDelay(1);
#else
            sleep_us(100);
#endif
            stall++;
            if (stall > 5000) break;
            continue;
        }
        sent += n;
        stall = 0;
#ifdef DEBUGPROBE_INTEGRATION
        xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
    }
    return sent;
}

// Flush/reset the CDC TX ring buffer (call on stream stop).
// Must only be called when usb_thread is not actively sending (e.g. after stream stop).
void bb_la_cdc_flush_ring(void) {
    __dmb();
    s_cdc_tx_tail = 0;
    s_cdc_tx_head = 0;
    __dmb();
}

// Called from usb_thread (tud_task context) — actually sends queued CDC data
void bb_la_cdc_send_pending(void) {
    if (!tud_cdc_connected()) return;

    uint32_t head = s_cdc_tx_head;
    uint32_t tail = s_cdc_tx_tail;
    if (head == tail) return;  // nothing to send

    uint32_t avail = (head >= tail) ? (head - tail) : (CDC_TX_BUF_SIZE - tail);
    if (avail == 0) return;

    uint32_t cdc_avail = tud_cdc_write_available();
    if (cdc_avail == 0) return;

    uint32_t chunk = (avail < cdc_avail) ? avail : cdc_avail;
    uint32_t written = tud_cdc_write(&s_cdc_tx_buf[tail], chunk);
    tud_cdc_write_flush();
    __dmb();  // Ensure read is complete before updating tail
    s_cdc_tx_tail = (tail + written) % CDC_TX_BUF_SIZE;
}

static void cdc_write_reply(const char *msg)
{
    if (!tud_cdc_connected()) return;
    tud_cdc_write_str(msg);
    tud_cdc_write_flush();
}

static void handle_stream_command(uint8_t cmd, bool reply_on_cdc)
{
    switch (cmd) {
    case LA_USB_CMD_START_STREAM:
        s_cdc_seq = 0;
        bb_la_usb_live_reset_sequence();
        if (!bb_la_start_stream()) {
            if (reply_on_cdc) {
                cdc_write_reply("ERR\n");
            } else {
                bb_la_usb_send_stream_marker(
                    LA_USB_STREAM_PKT_ERROR,
                    LA_USB_STREAM_INFO_START_REJECTED
                );
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
        bb_la_cdc_flush_ring();
        if (tud_cdc_connected()) {
            tud_cdc_write_clear();
        }
        if (reply_on_cdc) {
            cdc_write_reply("STOP\n");
        } else {
            bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_STOP, LA_STREAM_STOP_HOST);
        }
        break;

    default:
        if (reply_on_cdc) {
            cdc_write_reply("ERR\n");
        }
        break;
    }
}

void bb_la_usb_poll_commands(void)
{
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
