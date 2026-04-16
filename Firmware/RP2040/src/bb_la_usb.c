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
#include <stdarg.h>
#include <stdio.h>
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

// RLE scratch buffer for transport compression.
// Written only in send_pending() when s_bulk_data.active == false.
// send_pending() is non-reentrant (Core 0 / USB task only).
#define STREAM_RLE_SCRATCH_SIZE 9728u
static uint8_t s_rle_scratch[STREAM_RLE_SCRATCH_SIZE];

// Observability counters for RLE compression efficiency.
static uint32_t s_segments_compressed = 0;
static uint32_t s_segments_raw_fallback = 0;

// Segment-level RLE encoder: [value:8][count_minus_1:8] pairs.
// Returns compressed length on success (always < src_len), or 0 on fallback
// (compressed output would be >= src_len — caller sends raw instead).
static uint32_t bb_la_stream_rle_compress(
    const uint8_t *src, uint32_t src_len,
    uint8_t *dst, uint32_t dst_max)
{
    uint32_t i = 0, out = 0;
    while (i < src_len) {
        uint8_t val = src[i];
        uint32_t run = 1;
        while (i + run < src_len && src[i + run] == val && run < 256)
            run++;
        if (out + 2 > dst_max)
            return 0;  // early abort — fallback to raw
        dst[out++] = val;
        dst[out++] = (uint8_t)(run - 1);
        i += run;
    }
    return out;
}

// State for active large buffer transfer (live stream half or one-shot readout)
static struct {
    const uint8_t *buf;       // current send pointer (scratch or raw ring buffer)
    const uint8_t *ring_buf;  // original ring slot; NULL when already released
    uint32_t total_len;
    uint32_t sent_len;
    bool is_live;
    bool active;
    bool compressed;          // true when buf points to s_rle_scratch
    bool header_sent;
} s_bulk_data;

// Non-blocking CDC debug helper — drops output when CDC TX buffer < 64 bytes.
// MUST NOT be called from ISR context.
void bb_la_dbg(const char *fmt, ...) {
    if (!tud_cdc_connected()) return;
    if (tud_cdc_write_available() < 64) return;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        tud_cdc_write((const uint8_t *)buf, (uint32_t)n);
        tud_cdc_write_flush();
    }
}

// Set when abort_bulk() is called from any context; consumed in send_pending()
// (USB task) to safely call tud_vendor_n_write_clear without task-safety issues.
static volatile bool s_need_endpoint_rearm = false;
static volatile uint8_t s_rearm_request_count = 0;
static volatile uint8_t s_rearm_complete_count = 0;

// Deferred stop: when set, send_pending() emits PKT_STOP after the current
// data buffer is fully drained, guaranteeing clean packet alignment on the wire.
// Set by the STOP command handler or DMA-overrun path; consumed by send_pending().
static volatile bool    s_deferred_stop = false;
static volatile uint8_t s_deferred_stop_info = 0;

// Track whether we are in an active streaming session (set on START, cleared
// on STOP/error).  Unlike s_bulk_data.active this stays true between DMA
// half-buffer handoffs so the USB task keeps its tight polling loop running.
static volatile bool s_streaming_session = false;


// Retry counter for deferred stop emission.  If the IN endpoint is stuck
// (write_available returns 0), give up after this many send_pending() calls
// to prevent USB task starvation of other FreeRTOS tasks.
static volatile uint32_t s_deferred_stop_retries = 0;
#define DEFERRED_STOP_MAX_RETRIES  10000u  // ~1s at USB task rate

// Set when vendor-bulk STOP uses soft HW stop; consumed by send_pending()
// after PKT_STOP emission to call bb_la_stop() for full cleanup.
static volatile bool s_pending_hw_cleanup = false;


void bb_la_usb_init(void) {
    s_cdc_seq = 0;
    s_live_seq = 0;
    s_segments_compressed = 0;
    s_segments_raw_fallback = 0;
    memset(&s_bulk_data, 0, sizeof(s_bulk_data));
    s_bulk_ctrl_head = 0;
    s_bulk_ctrl_tail = 0;
    s_need_endpoint_rearm = false;
    s_rearm_request_count = 0;
    s_rearm_complete_count = 0;
    s_deferred_stop = false;
    s_deferred_stop_info = 0;
    s_streaming_session = false;
    s_pending_hw_cleanup = false;
    s_deferred_stop_retries = 0;
}

void bb_la_usb_live_reset_sequence(void) {
    s_live_seq = 0;
}

bool bb_la_usb_connected(void) {
    return tud_vendor_n_mounted(BB_LA_VENDOR_ITF);
}

#include "hardware/structs/usb.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/usb.h"

// Direct SIE buffer_control register manipulation for RP2040.
// This clears the AVAIL bit for the vendor bulk IN endpoint, releasing
// it from any stuck busy state without the buggy DCD abort_done spin.
static void rp2040_sie_endpoint_reset(uint8_t ep_addr) {
    uint8_t const ep_num = ep_addr & 0x0f;
    bool const is_in = (ep_addr & 0x80) != 0;

    // RP2040 DPRAM buffer_control layout (datasheet §4.1.2.5.2):
    //   EP0: DPRAM_BASE + 0x80 (IN) / 0x84 (OUT)
    //   EP1-15: DPRAM_BASE + 0x80 + 8*ep_num (IN) / + 4 (OUT)
    // BugBuster uses single-buffered bulk (buffer 0 only).
    uint32_t *buf_ctrl;
    if (ep_num == 0) {
        buf_ctrl = (uint32_t*)(USBCTRL_DPRAM_BASE + 0x80 + (is_in ? 0 : 4));
    } else {
        // RP2040 datasheet 4.1.2.5.2: 
        // 0x80 + 8*ep_num for IN (buffer 0), 0x84 + 8*ep_num for OUT (buffer 0)
        buf_ctrl = (uint32_t*)(USBCTRL_DPRAM_BASE + 0x80 + 8 * ep_num + (is_in ? 0 : 4));
    }

    // Clear everything: AVAIL, FULL, LAST, and the byte count.
    // This immediately releases the buffer from SIE control.
    *buf_ctrl = 0;
    __dmb();
}

void bb_la_usb_soft_reset(void) {
    s_streaming_session = false;
    uint32_t status = save_and_disable_interrupts();
    s_bulk_data.active = false;
    s_bulk_data.buf = NULL;
    s_bulk_data.ring_buf = NULL;
    s_bulk_data.compressed = false;
    s_bulk_data.header_sent = false;
    // Do NOT clear s_bulk_ctrl_{head,tail} — pending PKT_STOP markers
    // must drain naturally so the host's stop_stream() receives them.
    s_pending_hw_cleanup = false;
    s_deferred_stop = false;      // cancel any stale deferred stop
    s_deferred_stop_retries = 0;

    // Reset hardware IN endpoint to release stuck AVAIL bit.
    // Do NOT call TinyUSB stream functions here — soft_reset runs on
    // Core 1 (HAT UART handler) and races with tud_task on Core 0.
    // Only do direct SIE register writes; the full TinyUSB cleanup
    // (fifo_clear + rx_reprime) happens via s_need_endpoint_rearm on Core 0.
    rp2040_sie_endpoint_reset(0x87); // IN only — safe direct register write
    if (s_rearm_request_count < UINT8_MAX) s_rearm_request_count++;
    s_need_endpoint_rearm = true;    // defer TinyUSB cleanup to Core 0

    // Bump both counters together — no DCD abort needed, so
    // request and complete stay in sync.
    if (s_rearm_request_count < UINT8_MAX) s_rearm_request_count++;
    s_rearm_complete_count = s_rearm_request_count;
    restore_interrupts(status);
    // Wake the USB task so it picks up any pending control markers
    // (e.g., the PKT_STOP queued by HAT_CMD_LA_STOP after this call).
#ifdef DEBUGPROBE_INTEGRATION
    if (tud_taskhandle) xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
}

void bb_la_usb_abort_bulk(void) {
    taskENTER_CRITICAL();
    s_streaming_session = false;
    s_bulk_data.active = false;
    s_bulk_data.buf = NULL;
    s_bulk_data.ring_buf = NULL;
    s_bulk_data.compressed = false;
    s_bulk_data.header_sent = false;
    s_bulk_ctrl_tail = 0;
    s_bulk_ctrl_head = 0;
    s_pending_hw_cleanup = false;
    s_deferred_stop = false;      // cancel any stale deferred stop
    if (s_rearm_request_count < UINT8_MAX) s_rearm_request_count++;
    taskEXIT_CRITICAL();
    // Signal the USB task to re-arm the endpoint (tud_vendor_n_write_clear must
    // run from tud_task context — send_pending() will pick this up).
    if (tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) {
        s_need_endpoint_rearm = true;
    } else {
        // If not mounted, we can mark it complete immediately
        s_rearm_complete_count = s_rearm_request_count;
    }
#ifdef DEBUGPROBE_INTEGRATION
    // Wake the USB task so send_pending() processes the rearm promptly.
    if (tud_taskhandle) xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
}

void bb_la_usb_request_deferred_stop(uint8_t info) {
    s_deferred_stop_info = info;
    s_deferred_stop_retries = 0;
    __dmb();
    s_deferred_stop = true;
#ifdef DEBUGPROBE_INTEGRATION
    if (tud_taskhandle) xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
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
    taskENTER_CRITICAL();
    s_bulk_data.buf = buf;
    s_bulk_data.total_len = total_bytes;
    s_bulk_data.sent_len = 0;
    s_bulk_data.is_live = false;
    s_bulk_data.header_sent = false;
    s_bulk_data.active = true;
    taskEXIT_CRITICAL();
#ifdef DEBUGPROBE_INTEGRATION
    if (tud_taskhandle) xTaskNotify(tud_taskhandle, 0, eNoAction);
#endif
}

void bb_la_usb_send_pending(void) {
    if (!tud_vendor_n_mounted(BB_LA_VENDOR_ITF)) {
        // Can't do USB I/O, but clear pending rearm — there is nothing
        // to rearm on an unmounted interface and leaving the flag set
        // causes the preflight recovery poll to spin forever.
        if (s_need_endpoint_rearm) {
            s_need_endpoint_rearm = false;
            s_rearm_complete_count = s_rearm_request_count;
        }
        return;
    }

    // Re-arm endpoints if abort_bulk() was called from any task context.
    // Two-step: hardware SIE reset (AVAIL=0) then TinyUSB software state clear.
    if (s_need_endpoint_rearm) {
        s_need_endpoint_rearm = false;

        // Clear ALL stop-related flags to prevent a stale deferred stop
        // (read by Core 0 before Core 1's abort_bulk cleared it) from
        // firing the emergency path and re-setting the rearm flag or
        // triggering a spurious post-stream RX rearm.
        s_deferred_stop = false;
        s_deferred_stop_retries = 0;
        s_pending_hw_cleanup = false;

        // Step 1: clear hardware AVAIL bit on the IN endpoint only.
        // Only the IN (TX) endpoint can get stuck with AVAIL=1 after a DMA
        // abort; the OUT (RX) endpoint is not driven by DMA and must NOT be
        // reset here — clearing its AVAIL without re-priming leaves the host
        // unable to write (all writes time out).
        rp2040_sie_endpoint_reset(0x87);

        // Step 2: clear TinyUSB internal busy flag, TX FIFO, and re-prime.
        // tud_vendor_n_fifo_clear() calls usbd_edpt_clear_busy() (clears ep->active)
        // + tu_edpt_stream_clear() + tu_edpt_stream_write_xfer() without any
        // DCD abort spin — safe now that Step 1 already cleared the hardware.
        tud_vendor_n_fifo_clear(BB_LA_VENDOR_ITF);

        // Step 3: force-clear OUT endpoint hardware, then re-prime.
        // When the host releases the USB interface, macOS cancels pending OUT
        // transfers.  vendord_xfer_cb never fires, so TinyUSB's busy flag is
        // stale.  The SIE might still show AVAIL=1 from the old transfer, but
        // the host has abandoned it — the endpoint is effectively dead.
        // Clear the SIE register first (AVAIL→0), then rx_reprime detects
        // AVAIL=0 and does clear_busy + fresh re-prime.
        rp2040_sie_endpoint_reset(0x06);  // OUT: clear stale AVAIL
        tud_vendor_n_rx_reprime(BB_LA_VENDOR_ITF);  // re-prime cleanly

        s_rearm_complete_count = s_rearm_request_count;
    }

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
        s_deferred_stop_retries = 0;  // DATA MOVING
        
        uint32_t status = save_and_disable_interrupts();
        s_bulk_ctrl_tail = tail;
        restore_interrupts(status);
    }

    // 2. If no control markers, check for data
    if (head == tail) {
        // Continuous loop to drain ready buffers into USB FIFO.
        // LIMIT to 2 buffers per call to ensure poll_commands() and tud_task()
        // get a chance to process host commands (like STOP).
        int loops = 0;
        while (loops < 2) {
            if (!s_bulk_data.active) {
                const uint8_t *stream_buf;
                uint32_t stream_len;
                // bb_la_stream_get_buffer now pulls from the 4-buffer ring
                if (bb_la_stream_get_buffer(&stream_buf, &stream_len)) {
                    // Attempt segment-level RLE compression.
                    // INVARIANT: s_rle_scratch is written only here, when
                    // s_bulk_data.active == false.  send_pending() is non-reentrant
                    // (Core 0 / USB task only — see bb_main_integrated.c).
                    uint32_t comp_len = bb_la_stream_rle_compress(
                        stream_buf, stream_len, s_rle_scratch, stream_len);
                    s_bulk_data.ring_buf = stream_buf;  // save for raw-fallback release
                    if (comp_len > 0) {
                        // Compressed: release ring slot immediately, send scratch.
                        bb_la_stream_buffer_sent(stream_buf);
                        s_bulk_data.ring_buf = NULL;  // already released
                        s_bulk_data.buf = s_rle_scratch;
                        s_bulk_data.total_len = comp_len;
                        s_bulk_data.compressed = true;
                        s_segments_compressed++;
                    } else {
                        // Raw fallback: ring slot released after full USB drain.
                        s_bulk_data.buf = stream_buf;
                        s_bulk_data.total_len = stream_len;
                        s_bulk_data.compressed = false;
                        s_segments_raw_fallback++;
                    }
                    s_bulk_data.sent_len = 0;
                    s_bulk_data.is_live = true;
                    s_bulk_data.active = true;
                } else {
                    // No more buffers ready in the ring
                    break;
                }
            }

            if (s_bulk_data.active) {
                uint32_t usb_avail = tud_vendor_n_write_available(BB_LA_VENDOR_ITF);
                
                if (s_bulk_data.is_live) {
                    // LIVE STREAM: Packetized [TYPE:1][SEQ:1][LEN:1][INFO:1][DATA:N]
                    // Each packet is max 64 bytes total, so N=60.
                    while (s_bulk_data.sent_len < s_bulk_data.total_len && usb_avail >= 64) {
                        uint32_t chunk = s_bulk_data.total_len - s_bulk_data.sent_len;
                        if (chunk > 60) chunk = 60;

                        uint8_t packet[64];
                        packet[0] = LA_USB_STREAM_PKT_DATA;
                        packet[1] = s_live_seq++;
                        packet[2] = (uint8_t)chunk;
                        packet[3] = s_bulk_data.compressed ? LA_USB_STREAM_INFO_COMPRESSED : LA_USB_STREAM_INFO_NONE;
                        memcpy(&packet[4], s_bulk_data.buf + s_bulk_data.sent_len, chunk);

                        tud_vendor_n_write(BB_LA_VENDOR_ITF, packet, (uint32_t)(4 + chunk));
                        s_bulk_data.sent_len += chunk;
                        s_deferred_stop_retries = 0;  // DATA MOVING
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
                            s_bulk_data.header_sent = true;
                            s_deferred_stop_retries = 0;  // DATA MOVING
                            usb_avail -= 4;
                        }
                    }
                    
                    if (s_bulk_data.header_sent) {
                        uint32_t chunk = s_bulk_data.total_len - s_bulk_data.sent_len;
                        if (chunk > usb_avail) chunk = usb_avail;
                        if (chunk > 0) {
                            tud_vendor_n_write(BB_LA_VENDOR_ITF, s_bulk_data.buf + s_bulk_data.sent_len, chunk);
                            s_bulk_data.sent_len += chunk;
                            s_deferred_stop_retries = 0;  // DATA MOVING
                        }
                    }
                }

                if (s_bulk_data.sent_len >= s_bulk_data.total_len) {
                    if (s_bulk_data.is_live && s_bulk_data.ring_buf != NULL) {
                        // Raw fallback: ring slot not yet released, do it now.
                        bb_la_stream_buffer_sent(s_bulk_data.ring_buf);
                        s_bulk_data.ring_buf = NULL;
                    }
                    s_bulk_data.active = false;
                    loops++;
                    // Loop continues to check if NEXT ring buffer is ready
                } else {
                    // Current buffer didn't finish (USB FIFO full)
                    break;
                }
            } else {
                break;
            }
        }
    }

    // 3. Deferred stop: emit PKT_STOP only after the current data buffer is
    //    fully drained so every DATA packet on the wire is a complete 64-byte
    //    frame.  Avoids the partial-packet alignment bug that occurs when
    //    write_clear() truncates an in-flight transfer mid-packet.
    if (s_deferred_stop) {
        uint32_t avail = tud_vendor_n_write_available(BB_LA_VENDOR_ITF);
        if (!s_bulk_data.active && avail >= 4) {
            uint8_t stop_pkt[4] = {
                LA_USB_STREAM_PKT_STOP, s_live_seq, 0, s_deferred_stop_info
            };
            tud_vendor_n_write(BB_LA_VENDOR_ITF, stop_pkt, 4);
            s_deferred_stop = false;
            // Full cleanup after ring is fully drained
            if (s_pending_hw_cleanup) {
                s_pending_hw_cleanup = false;
                bb_la_stop();  // unload PIO, reset ring, state -> IDLE
            }
            __dmb();  // Ensure state=IDLE visible to Core 1 before clearing flag
            s_streaming_session = false;
        } else if (++s_deferred_stop_retries >= DEFERRED_STOP_MAX_RETRIES) {
            // IN endpoint stuck — can't send PKT_STOP in the normal aligned path.
            // Force an emergency PKT_STOP now: a slightly misaligned packet is
            // better than leaving the host blocked indefinitely (which causes the
            // 0x11 cascade on the next BBP session).
            s_deferred_stop = false;
            s_bulk_data.active = false; // Abort data drain
            {
                uint8_t stop_pkt[4] = {
                    LA_USB_STREAM_PKT_STOP, s_live_seq, 0, s_deferred_stop_info
                };
                tud_vendor_n_write(BB_LA_VENDOR_ITF, stop_pkt, 4);
                tud_vendor_n_flush(BB_LA_VENDOR_ITF);
            }
            // bb_la_stop() MUST run before clearing s_streaming_session.
            // Core 1 uses !s_streaming_session as the gate to call
            // bb_la_poll() and process BBP commands.  If s_streaming_session
            // is cleared while la_state is still STREAMING, Core 1 calls
            // bb_la_configure() which rejects it (state != IDLE/ERROR) and
            // sends HAT_RSP_ERROR over BB_UART → ESP32 loses UART sync → 0x11.
            if (s_pending_hw_cleanup) {
                s_pending_hw_cleanup = false;
                bb_la_stop();   // state → IDLE before Core 1 can observe flag
            }
            __dmb();  // Ensure state=IDLE visible to Core 1 before clearing flag
            s_streaming_session = false;   // clear AFTER state is IDLE
            // Re-arm the IN endpoint so the next session can start cleanly.
            s_need_endpoint_rearm = true;
        }
    }

    tud_vendor_n_flush(BB_LA_VENDOR_ITF);
}

bool bb_la_usb_is_streaming(void) {
    return s_streaming_session;
}

bool bb_la_usb_has_pending_data(void) {
    return s_bulk_data.active || (s_bulk_ctrl_head != s_bulk_ctrl_tail) || s_deferred_stop;
}

bool bb_la_usb_rearm_pending(void) {
    return s_need_endpoint_rearm;
}

uint8_t bb_la_usb_rearm_request_count(void) {
    return s_rearm_request_count;
}

uint8_t bb_la_usb_rearm_complete_count(void) {
    return s_rearm_complete_count;
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

void bb_la_usb_notify_task_from_isr(void)
{
#ifdef DEBUGPROBE_INTEGRATION
    if (tud_taskhandle) {
        BaseType_t higher = pdFALSE;
        xTaskNotifyFromISR(tud_taskhandle, 0, eNoAction, &higher);
        portYIELD_FROM_ISR(higher);
    }
#endif
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
        s_deferred_stop = false;   // cancel any pending deferred stop
        s_pending_hw_cleanup = false;
        bb_la_usb_live_reset_sequence();
        if (!bb_la_start_stream()) {
            if (reply_on_cdc) {
                cdc_write_reply("ERR\n");
            } else {
                bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_ERROR, LA_USB_STREAM_INFO_START_REJECTED);
            }
            return;
        }
        s_streaming_session = true;
        if (reply_on_cdc) {
            cdc_write_reply("START\n");
        } else {
            bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_START, LA_USB_STREAM_INFO_NONE);
        }
        break;

    case LA_USB_CMD_STOP:
        // Do NOT clear s_streaming_session here — leave the fast path
        // running so tud_task() + send_pending() keep draining the TX
        // FIFO and ring buffers naturally.  The deferred stop mechanism
        // emits PKT_STOP after all data is drained, and THEN clears
        // s_streaming_session to exit the fast path.
        bb_la_stop_streaming_hw();   // stops HW, preserves ring for drain
        bb_la_cdc_flush_ring();
        if (tud_cdc_connected()) tud_cdc_write_clear();
        s_pending_hw_cleanup = true;
        bb_la_usb_request_deferred_stop(LA_STREAM_STOP_HOST);
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
