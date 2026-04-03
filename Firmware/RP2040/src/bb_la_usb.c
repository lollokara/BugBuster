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
#include "pico/stdlib.h"
#include <stdio.h>

#ifdef DEBUGPROBE_INTEGRATION
#include "FreeRTOS.h"
#include "task.h"
#endif

// USB vendor OUT command bytes
#define LA_USB_CMD_STOP         0x00
#define LA_USB_CMD_START_STREAM 0x01

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
            tud_task();
#ifdef DEBUGPROBE_INTEGRATION
            vTaskDelay(1);
#else
            sleep_us(100);
#endif
            stall_count++;
            if (stall_count > 10000) {  // ~1 second timeout
                break;  // Bail out — host isn't reading
            }
            continue;
        }
        stall_count = 0;
        uint32_t chunk = (len - written < avail) ? (len - written) : avail;
        uint32_t n = tud_vendor_n_write(BB_LA_VENDOR_ITF, data + written, chunk);
        written += n;
    }
    tud_vendor_n_flush(BB_LA_VENDOR_ITF);
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
    if (!tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) return 0;

    // Stream raw data in 64-byte chunks — no header, just packed samples
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

void bb_la_usb_poll_commands(void)
{
    if (!tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) return;

    uint32_t avail = tud_vendor_n_available(BB_LA_VENDOR_ITF);
    if (avail == 0) return;

    uint8_t cmd_buf[64];
    uint32_t n = tud_vendor_n_read(BB_LA_VENDOR_ITF, cmd_buf, sizeof(cmd_buf));
    if (n == 0) return;

    // Process each command byte
    for (uint32_t i = 0; i < n; i++) {
        switch (cmd_buf[i]) {
        case LA_USB_CMD_START_STREAM:
            printf("[LA_USB] CMD: start stream\n");
            // Diagnostic: test both vendor instances to find which one works
            printf("[LA_USB] vendor0_mounted=%d vendor1_mounted=%d tud_mounted=%d\n",
                tud_vendor_n_mounted(0) ? 1 : 0,
                tud_vendor_n_mounted(1) ? 1 : 0,
                tud_mounted() ? 1 : 0);
            {
                // Try writing to BOTH vendor instances
                uint8_t ack0[4] = {'V','0','O','K'};
                uint8_t ack1[4] = {'V','1','O','K'};
                uint32_t w0 = tud_vendor_n_write(0, ack0, 4);
                tud_vendor_n_flush(0);
                uint32_t w1 = tud_vendor_n_write(1, ack1, 4);
                tud_vendor_n_flush(1);
                printf("[LA_USB] write0=%lu write1=%lu\n", (unsigned long)w0, (unsigned long)w1);
            }
            if (!bb_la_start_stream()) {
                printf("[LA_USB] start_stream failed\n");
            }
            break;

        case LA_USB_CMD_STOP:
            printf("[LA_USB] CMD: stop\n");
            bb_la_stop();
            break;

        default:
            printf("[LA_USB] CMD: unknown 0x%02X\n", cmd_buf[i]);
            break;
        }
    }
}
