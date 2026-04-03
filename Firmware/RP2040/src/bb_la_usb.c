// =============================================================================
// bb_la_usb.c — Logic Analyzer USB bulk data streaming
//
// Uses TinyUSB vendor class instance 1 (instance 0 = CMSIS-DAP).
// Data flows: LA capture buffer → tud_vendor_n_write() → USB bulk IN → Host
//
// Gapless streaming: the host sends commands on the vendor OUT endpoint
// (EP 0x06) to start/stop continuous DMA→USB streaming. No ESP32 involvement
// for the data path — commands go directly from USB host to RP2040.
// =============================================================================

#include "bb_la_usb.h"
#include "bb_la.h"
#include "tusb.h"
#include "device/usbd.h"  // for usbd_edpt_xfer (low-level endpoint access)
#include "pico/stdlib.h"
#include <stdio.h>

#ifdef DEBUGPROBE_INTEGRATION
#include "FreeRTOS.h"
#include "task.h"
extern TaskHandle_t tud_taskhandle;  // defined in bb_main_integrated.c
#endif

// USB vendor OUT command bytes
#define LA_USB_CMD_STOP         0x00
#define LA_USB_CMD_START_STREAM 0x01

// Shared CDC TX ring buffer: bb_cmd_task writes here, usb_thread sends via tud_cdc_write
#define CDC_TX_BUF_SIZE  1024
static uint8_t s_cdc_tx_buf[CDC_TX_BUF_SIZE];
static volatile uint32_t s_cdc_tx_head = 0;  // written by bb_cmd_task
static volatile uint32_t s_cdc_tx_tail = 0;  // read by usb_thread

// Queue data for CDC TX (called from bb_cmd_task)
static uint32_t cdc_queue_write(const uint8_t *data, uint32_t len) {
    uint32_t head = s_cdc_tx_head;
    uint32_t tail = s_cdc_tx_tail;
    uint32_t free = (tail > head) ? (tail - head - 1) : (CDC_TX_BUF_SIZE - head + tail - 1);
    if (len > free) len = free;
    for (uint32_t i = 0; i < len; i++) {
        s_cdc_tx_buf[(head + i) % CDC_TX_BUF_SIZE] = data[i];
    }
    s_cdc_tx_head = (head + len) % CDC_TX_BUF_SIZE;
    return len;
}

void bb_la_usb_init(void)
{
    // Nothing to init — TinyUSB handles endpoint setup from descriptor
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

uint32_t bb_la_usb_write_raw(const uint8_t *buf, uint32_t total_bytes)
{
    // Queue data for CDC TX — usb_thread will actually send it
    uint32_t sent = 0;
    uint32_t stall = 0;
    while (sent < total_bytes) {
        uint32_t n = cdc_queue_write(buf + sent, total_bytes - sent);
        sent += n;
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
        stall = 0;
#ifdef DEBUGPROBE_INTEGRATION
        xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
    }
    return sent;
}

// Flush/reset the CDC TX ring buffer (call on stream stop)
void bb_la_cdc_flush_ring(void) {
    s_cdc_tx_head = 0;
    s_cdc_tx_tail = 0;
}

// Called from usb_thread (tud_task context) — actually sends queued CDC data
void bb_la_cdc_send_pending(void) {
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
    s_cdc_tx_tail = (tail + written) % CDC_TX_BUF_SIZE;
}

void bb_la_usb_poll_commands(void)
{
    // Read commands from CDC serial (vendor endpoints don't work for instance 1)
    if (!tud_cdc_available()) return;

    uint8_t cmd_buf[64];
    uint32_t n = tud_cdc_read(cmd_buf, sizeof(cmd_buf));
    int read_inst = -1;  // CDC path
    if (n == 0) return;

    // Process each command byte
    for (uint32_t i = 0; i < n; i++) {
        switch (cmd_buf[i]) {
        case LA_USB_CMD_START_STREAM:
            printf("[LA_USB] CMD: start stream\n");
            printf("[LA_USB] vendor0_mounted=%d vendor1_mounted=%d tud_mounted=%d\n",
                tud_vendor_n_mounted(0) ? 1 : 0,
                tud_vendor_n_mounted(1) ? 1 : 0,
                tud_mounted() ? 1 : 0);
            {
                // Test: write directly to EP 0x87 using low-level API (bypass mounted check)
                static uint8_t test_data[64];
                for (int j = 0; j < 64; j++) test_data[j] = (uint8_t)(j + 0xA0);
                // Also try via tud_vendor_n_write to both instances
                uint32_t w0 = tud_vendor_n_write(0, test_data, 8);
                tud_vendor_n_flush(0);
                uint32_t w1 = tud_vendor_n_write(1, test_data + 8, 8);
                tud_vendor_n_flush(1);
                printf("[LA_USB] write0=%lu write1=%lu\n", (unsigned long)w0, (unsigned long)w1);
#ifdef DEBUGPROBE_INTEGRATION
                xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
            }
            if (!bb_la_start_stream()) {
                printf("[LA_USB] start_stream failed\n");
            } else {
                printf("[LA_USB] start_stream OK\n");
            }
            break;

        case LA_USB_CMD_STOP:
            printf("[LA_USB] CMD: stop\n");
            bb_la_stop();
            break;

        case 0x42: {
            // Diagnostic: try ALL write methods, report which instance received data
            static uint8_t diag[16];
            diag[0] = 0x42;
            diag[1] = tud_mounted() ? 1 : 0;
            diag[2] = tud_vendor_n_mounted(0) ? 1 : 0;
            diag[3] = tud_vendor_n_mounted(1) ? 1 : 0;
            diag[4] = (uint8_t)read_inst;  // Which instance had the data
            // Try writing via instance that's mounted
            uint32_t w0 = tud_vendor_n_write(0, diag, 8);
            tud_vendor_n_flush(0);
            uint32_t w1 = tud_vendor_n_write(1, diag, 8);
            tud_vendor_n_flush(1);
            diag[5] = (uint8_t)w0;
            diag[6] = (uint8_t)w1;
            // Direct endpoint xfer to EP 0x87
            diag[7] = 0xDE;
            bool ex = usbd_edpt_xfer(0, 0x87, diag, 8);
            printf("[LA_USB] DIAG: tud=%d v0=%d v1=%d inst=%d w0=%lu w1=%lu edpt=%d\n",
                diag[1], diag[2], diag[3], read_inst,
                (unsigned long)w0, (unsigned long)w1, ex ? 1 : 0);
#ifdef DEBUGPROBE_INTEGRATION
            xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
            break;
        }

        default:
            printf("[LA_USB] CMD: unknown 0x%02X\n", cmd_buf[i]);
            break;
        }
    }
}
