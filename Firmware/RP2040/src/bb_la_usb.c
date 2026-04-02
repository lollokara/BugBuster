// =============================================================================
// bb_la_usb.c — Logic Analyzer USB bulk data streaming
//
// Uses TinyUSB vendor class instance 1 (instance 0 = CMSIS-DAP).
// Data flows: LA capture buffer → tud_vendor_n_write() → USB bulk IN → Host
// =============================================================================

#include "bb_la_usb.h"
#include "tusb.h"
#include "pico/stdlib.h"

#ifdef DEBUGPROBE_INTEGRATION
#include "FreeRTOS.h"
#include "task.h"
#endif

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
    while (written < len) {
        uint32_t avail = tud_vendor_n_write_available(BB_LA_VENDOR_ITF);
        if (avail == 0) {
            // Flush and wait for space
            tud_vendor_n_flush(BB_LA_VENDOR_ITF);
#ifdef DEBUGPROBE_INTEGRATION
            vTaskDelay(1);
#else
            sleep_us(100);
#endif
            continue;
        }
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
