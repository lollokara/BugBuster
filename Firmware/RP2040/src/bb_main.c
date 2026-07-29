// =============================================================================
// bb_main.c — BugBuster HAT main entry point
//
// This file provides the BugBuster command handler task that runs alongside
// the debugprobe firmware. When integrated with debugprobe, this becomes
// a FreeRTOS task created in debugprobe's main.c.
//
// For standalone testing (without debugprobe), this provides its own main().
// =============================================================================

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#ifdef DEBUGPROBE_INTEGRATION
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#endif

#include "bb_config.h"
#include "bb_protocol.h"
#include "bb_power.h"
#include "bb_pins.h"
#include "bb_swd.h"
#include "bb_la.h"
#include "bb_la_usb.h"
#include "bb_fw_update.h"
#include "tusb.h"
#include "bb_hat_v2.h"
#include "bb_hat_v2.h"

#ifdef DEBUGPROBE_INTEGRATION
extern TaskHandle_t tud_taskhandle;
#endif

// Firmware version — injected by CMakeLists.txt as compile definitions.
// The #ifndef guards serve as a fallback for any build system that does not
// set these via -D flags (e.g. IDE or standalone compilation).
// To bump the version, edit PROBE_VERSION in CMakeLists.txt only.
#ifndef BB_HAT_FW_MAJOR
#define BB_HAT_FW_MAJOR  5  /* 0 = sentinel: CMake -D flags were not provided */
#endif
#ifndef BB_HAT_FW_MINOR
#define BB_HAT_FW_MINOR  0  /* 0 = sentinel: CMake -D flags were not provided */
#endif
// L02: opt-in loud failure for builds that omit the CMake -D version flags.
// Define BB_HAT_REQUIRE_VERSION to turn the silent 0.0 sentinel into a hard
// build error.  The normal CMake / CI build does NOT define this macro, so
// the 0.0 fallback continues to work there without change.
#ifdef BB_HAT_REQUIRE_VERSION
#  if BB_HAT_FW_MAJOR == 0 && BB_HAT_FW_MINOR == 0
#    error "BB_HAT_FW_MAJOR/MINOR not set — rebuild via CMake (sets -DBB_HAT_FW_MAJOR=<N> -DBB_HAT_FW_MINOR=<N>) or remove BB_HAT_REQUIRE_VERSION"
#  endif
#endif

static HatFrameParser s_parser;

// Track previous LA state for detecting DONE transition
static LaState s_prev_la_state = LA_STATE_IDLE;

// Log relay: when enabled, bb_la_log() messages are sent via HAT UART to host
static volatile bool s_la_log_enabled = false;

#ifdef DEBUGPROBE_INTEGRATION
static SemaphoreHandle_t s_uart_tx_mutex = NULL;
#endif

// -----------------------------------------------------------------------------
// IRQ pin assertion (open-drain, active low, ~1ms pulse)
// Signals the ESP32 that an asynchronous event occurred.
// -----------------------------------------------------------------------------
static void bb_irq_assert(void)
{
    // Drive low (assert) by switching to output — output register is pre-loaded with 0
    gpio_set_dir(BB_IRQ_PIN, GPIO_OUT);
}

static void bb_irq_deassert(void)
{
    // Release to high-Z by switching back to input (pull-up restores high level)
    gpio_set_dir(BB_IRQ_PIN, GPIO_IN);
}

// IRQ pulse state machine (called from poll loop, non-blocking)
static volatile uint32_t s_irq_assert_ms = 0;
static volatile bool     s_irq_active = false;

static void bb_irq_pulse(void)
{
    if (!s_irq_active) {
        bb_irq_assert();
        s_irq_active = true;
        s_irq_assert_ms = to_ms_since_boot(get_absolute_time());
    }
}

static void bb_irq_poll(void)
{
    if (s_irq_active) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - s_irq_assert_ms >= 2) {  // 2ms pulse width
            bb_irq_deassert();
            s_irq_active = false;
        }
    }
}

// -----------------------------------------------------------------------------
// Send a response frame over UART
// -----------------------------------------------------------------------------

void send_response(uint8_t rsp_cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[36];
    size_t frame_len = hat_build_frame(frame, rsp_cmd, payload, len);
#ifdef DEBUGPROBE_INTEGRATION
    if (s_uart_tx_mutex && xSemaphoreTake(s_uart_tx_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uart_write_blocking(BB_UART, frame, frame_len);
        xSemaphoreGive(s_uart_tx_mutex);
        return;
    }
#endif
    uart_write_blocking(BB_UART, frame, frame_len);
}

void send_ok(const uint8_t *payload, uint8_t len)
{
    send_response(HAT_RSP_OK, payload, len);
}

void send_error(uint8_t error_code)
{
    send_response(HAT_RSP_ERROR, &error_code, 1);
}

// -----------------------------------------------------------------------------
// Command Handlers
// -----------------------------------------------------------------------------

static void handle_ping(void)
{
    send_ok(NULL, 0);
}

static void handle_get_info(void)
{
    uint8_t payload[3] = {
        HAT_TYPE_SWD_GPIO,
        BB_HAT_FW_MAJOR,
        BB_HAT_FW_MINOR,
    };
    send_response(HAT_RSP_INFO, payload, sizeof(payload));
}

static void handle_set_pin_config(const uint8_t *payload, uint8_t len)
{
    if (len == 2) {
        // Single pin mode
        uint8_t pin = payload[0];
        uint8_t func = payload[1];
        if (pin >= BB_NUM_EXT_PINS) { send_error(HAT_ERR_INVALID_PIN); return; }
        if (func > HAT_FUNC_GPIO4) { send_error(HAT_ERR_INVALID_FUNC); return; }
        // bb_pins_set() returns false if `func` is a reserved (deprecated)
        // slot like SWDIO/SWCLK/TRACE1/TRACE2 — those moved to the dedicated
        // 3-pin SWD connector and are no longer assignable here.
        if (!bb_pins_set(pin, func)) { send_error(HAT_ERR_INVALID_FUNC); return; }
        send_ok(NULL, 0);
    } else if (len == 4) {
        // All pins mode
        for (int i = 0; i < 4; i++) {
            uint8_t f = payload[i];
            if (f > HAT_FUNC_GPIO4) { send_error(HAT_ERR_INVALID_FUNC); return; }
            // Reject reserved slots 1-4 up-front so partial writes don't happen.
            if (f >= 1 && f <= 4) { send_error(HAT_ERR_INVALID_FUNC); return; }
        }
        bb_pins_set_all(payload);
        send_ok(NULL, 0);
    } else {
        send_error(HAT_ERR_FRAME);
    }
}

static void handle_get_pin_config(void)
{
    uint8_t funcs[4];
    bb_pins_get_all(funcs);
    send_ok(funcs, 4);
}

static void handle_reset(void)
{
    bb_pins_reset();
    bb_power_set(0, false);
    bb_power_set(1, false);
    bb_power_set_3v3_adj(false);
    bb_hat_v2_handle_reset();
    send_ok(NULL, 0);
}

uint16_t clamp_u16_from_float(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 65535.0f) return 65535;
    return (uint16_t)(v + 0.5f);
}

void append_rail_status(uint8_t *rsp, size_t *p, uint8_t rail_id,
                               bool enabled, uint16_t mv, uint16_t ma,
                               uint8_t status, uint16_t target_mv)
{
    rsp[(*p)++] = rail_id;
    rsp[(*p)++] = enabled ? 1 : 0;
    memcpy(&rsp[*p], &mv, sizeof(mv)); *p += sizeof(mv);
    memcpy(&rsp[*p], &ma, sizeof(ma)); *p += sizeof(ma);
    rsp[(*p)++] = status;
    memcpy(&rsp[*p], &target_mv, sizeof(target_mv)); *p += sizeof(target_mv);
}

static void handle_set_power(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_error(HAT_ERR_FRAME); return; }
    uint8_t conn = payload[0];
    bool enable = payload[1] != 0;
    if (conn > 1) { send_error(HAT_ERR_INVALID_PIN); return; }

    bb_power_set(conn, enable);
    send_ok(NULL, 0);
}

static void handle_get_power_status(void)
{
    bb_power_update();

    ConnectorStatus a, b;
    bb_power_get_status(&a, &b);

    // Response: [en_a(u8), current_a(f32), fault_a(u8), en_b(u8), current_b(f32), fault_b(u8)]
    uint8_t rsp[12];
    size_t pos = 0;
    rsp[pos++] = a.enabled ? 1 : 0;
    memcpy(&rsp[pos], &a.current_ma, sizeof(float)); pos += sizeof(float);
    rsp[pos++] = a.fault ? 1 : 0;
    rsp[pos++] = b.enabled ? 1 : 0;
    memcpy(&rsp[pos], &b.current_ma, sizeof(float)); pos += sizeof(float);
    rsp[pos++] = b.fault ? 1 : 0;

    send_response(HAT_RSP_POWER_STATUS, rsp, (uint8_t)pos);
}

static void handle_set_io_voltage(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_error(HAT_ERR_FRAME); return; }
    uint16_t mv = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);

    if (!bb_hat_v2_set_io_voltage(mv)) {
        send_error(HAT_ERR_INVALID_FUNC);
        return;
    }

    uint16_t applied = bb_hat_v2_get_io_voltage();
    uint8_t rsp[4] = {
        (uint8_t)(mv & 0xFF),
        (uint8_t)(mv >> 8),
        (uint8_t)(applied & 0xFF),
        (uint8_t)(applied >> 8),
    };
    send_ok(rsp, sizeof(rsp));
}

static void handle_get_io_voltage(void)
{
    uint16_t mv = bb_hat_v2_get_io_voltage();
    uint8_t rsp[4] = {
        (uint8_t)(mv & 0xFF),
        (uint8_t)(mv >> 8),
        (uint8_t)(mv & 0xFF),
        (uint8_t)(mv >> 8),
    };
    send_ok(rsp, sizeof(rsp));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void handle_fw_begin(const uint8_t *payload, uint8_t len)
{
    if (len < 8) { send_error(HAT_ERR_FRAME); return; }
    uint32_t image_size = le32(payload);
    uint32_t crc32 = le32(payload + 4);
    if (!bb_fw_update_begin(image_size, crc32)) {
        uint8_t state, err;
        uint32_t written, size, expect, actual;
        bb_fw_update_get_status(&state, &written, &size, &expect, &actual, &err);
        send_error(err ? err : HAT_ERR_BUSY);
        return;
    }
    send_ok(NULL, 0);
}

static void handle_fw_chunk(const uint8_t *payload, uint8_t len)
{
    if (len <= 4) { send_error(HAT_ERR_FRAME); return; }
    uint32_t offset = le32(payload);
    if (!bb_fw_update_chunk(offset, payload + 4, (uint8_t)(len - 4))) {
        uint8_t state, err;
        uint32_t written, size, expect, actual;
        bb_fw_update_get_status(&state, &written, &size, &expect, &actual, &err);
        send_error(err ? err : HAT_ERR_FRAME);
        return;
    }
    uint8_t rsp[4] = {
        (uint8_t)(offset + len - 4),
        (uint8_t)((offset + len - 4) >> 8),
        (uint8_t)((offset + len - 4) >> 16),
        (uint8_t)((offset + len - 4) >> 24),
    };
    send_ok(rsp, sizeof(rsp));
}

static void handle_fw_status(void)
{
    uint8_t state, err;
    uint32_t written, size, expect, actual;
    bb_fw_update_get_status(&state, &written, &size, &expect, &actual, &err);
    uint8_t rsp[18];
    size_t p = 0;
    rsp[p++] = state;
    rsp[p++] = err;
    memcpy(&rsp[p], &written, 4); p += 4;
    memcpy(&rsp[p], &size, 4); p += 4;
    memcpy(&rsp[p], &expect, 4); p += 4;
    memcpy(&rsp[p], &actual, 4); p += 4;
    send_ok(rsp, (uint8_t)p);
}

// -----------------------------------------------------------------------------
// Command Dispatcher
// -----------------------------------------------------------------------------

static void dispatch_command(const HatFrame *frame)
{
    // Log incoming commands (visible via USB stdio or probe_info)
    printf("[BB] CMD 0x%02X len=%d\n", frame->cmd, frame->payload_len);

    switch (frame->cmd) {
    // Core
    case HAT_CMD_PING:
        handle_ping();
        break;
    case HAT_CMD_GET_INFO:
        handle_get_info();
        break;
    case HAT_CMD_SET_PIN_CONFIG:
        handle_set_pin_config(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_GET_PIN_CONFIG:
        handle_get_pin_config();
        break;
    case HAT_CMD_RESET:
        handle_reset();
        break;
    case HAT_CMD_GET_CAPS:
        handle_get_caps();
        break;

    // Power management
    case HAT_CMD_SET_POWER:
        handle_set_power(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_GET_POWER_STATUS:
        handle_get_power_status();
        break;
    case HAT_CMD_SET_IO_VOLTAGE:
        handle_set_io_voltage(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_GET_IO_VOLTAGE:
        handle_get_io_voltage();
        break;
    // SWD management
    case HAT_CMD_GET_DAP_STATUS: {
        SwdStatus swd;
        bb_swd_get_status(&swd);
        uint8_t rsp[10];
        size_t p = 0;
        rsp[p++] = swd.dap_connected ? 1 : 0;
        rsp[p++] = swd.target_detected ? 1 : 0;
        memcpy(&rsp[p], &swd.dpidr, 4); p += 4;
        rsp[p++] = (uint8_t)(swd.swd_clock_khz & 0xFF);
        rsp[p++] = (uint8_t)((swd.swd_clock_khz >> 8) & 0xFF);
        send_response(HAT_RSP_DAP_STATUS, rsp, (uint8_t)p);
        break;
    }
    case HAT_CMD_GET_TARGET_INFO: {
        bb_swd_detect_target();
        SwdStatus swd;
        bb_swd_get_status(&swd);
        uint8_t rsp[6];
        size_t p = 0;
        rsp[p++] = swd.target_detected ? 1 : 0;
        memcpy(&rsp[p], &swd.dpidr, 4); p += 4;
        send_ok(rsp, (uint8_t)p);
        break;
    }
    case HAT_CMD_SET_SWD_CLOCK: {
        if (frame->payload_len < 2) { send_error(HAT_ERR_FRAME); break; }
        uint32_t khz = (uint32_t)frame->payload[0] | ((uint32_t)frame->payload[1] << 8);
        if (!bb_swd_set_clock(khz)) {
            send_error(HAT_ERR_INVALID_FUNC);
        } else {
            uint8_t rsp[2] = { frame->payload[0], frame->payload[1] };
            send_ok(rsp, 2);
        }
        break;
    }

    // Logic analyzer
    case HAT_CMD_LA_CONFIG: {
        if (frame->payload_len < 9) { send_error(HAT_ERR_FRAME); break; }
        LaConfig cfg;
        cfg.channels = frame->payload[0];
        cfg.sample_rate_hz = (uint32_t)frame->payload[1]
                           | ((uint32_t)frame->payload[2] << 8)
                           | ((uint32_t)frame->payload[3] << 16)
                           | ((uint32_t)frame->payload[4] << 24);
        cfg.depth_samples = (uint32_t)frame->payload[5]
                          | ((uint32_t)frame->payload[6] << 8)
                          | ((uint32_t)frame->payload[7] << 16)
                          | ((uint32_t)frame->payload[8] << 24);
        cfg.rle_enabled = (frame->payload_len >= 10) ? (frame->payload[9] != 0) : false;
        if (!bb_la_configure(&cfg)) {
            send_error(HAT_ERR_INVALID_FUNC);
        } else {
            send_ok(NULL, 0);
        }
        break;
    }
    case HAT_CMD_LA_SET_TRIGGER: {
        if (frame->payload_len < 2) { send_error(HAT_ERR_FRAME); break; }
        LaTrigger trig;
        trig.type = (LaTriggerType)frame->payload[0];
        trig.channel = frame->payload[1];
        if (trig.channel >= BB_LA_NUM_CHANNELS) { send_error(HAT_ERR_INVALID_PIN); break; }
        if (!bb_la_set_trigger(&trig)) {
            send_error(HAT_ERR_INVALID_FUNC);
        } else {
            send_ok(NULL, 0);
        }
        break;
    }
    case HAT_CMD_LA_ARM:
        if (!bb_la_arm()) { send_error(HAT_ERR_BUSY); }
        else { send_ok(NULL, 0); }
        break;
    case HAT_CMD_LA_FORCE:
        bb_la_force_trigger();
        send_ok(NULL, 0);
        break;
    case HAT_CMD_LA_GET_STATUS: {
        LaStatus st;
        bb_la_get_status(&st);
        // Buffer is 29 bytes: 28 existing + 1 ring_overflow byte appended at
        // the end for backward-compatible length extension.  Older parsers that
        // only read the first 28 bytes are unaffected.
        uint8_t rsp[29];
        size_t p = 0;
        rsp[p++] = (uint8_t)st.state;
        rsp[p++] = st.channels;
        memcpy(&rsp[p], &st.samples_captured, 4); p += 4;
        memcpy(&rsp[p], &st.total_samples, 4); p += 4;
        memcpy(&rsp[p], &st.actual_rate_hz, 4); p += 4;
        // Diagnostic: USB vendor mount status
        rsp[p++] = bb_la_usb_connected() ? 1 : 0;
        rsp[p++] = tud_mounted() ? 1 : 0;  // Overall USB mounted
        rsp[p++] = st.stream_stop_reason;
        memcpy(&rsp[p], &st.stream_overrun_count, 4); p += 4;
        memcpy(&rsp[p], &st.stream_short_write_count, 4); p += 4;
        rsp[p++] = bb_la_usb_rearm_pending() ? 1 : 0;
        rsp[p++] = bb_la_usb_rearm_request_count();
        rsp[p++] = bb_la_usb_rearm_complete_count();
        // Ring overflow flag: set when DMA producer overtook USB consumer.
        // Appended last so the response remains parseable by older host code
        // that only reads the first 28 bytes.  Cleared on next ARM (0x32).
        rsp[p++] = st.ring_overflow;
        send_response(HAT_RSP_LA_STATUS, rsp, (uint8_t)p);
        break;
    }
    case HAT_CMD_LA_READ_DATA: {
        if (frame->payload_len < 6) { send_error(HAT_ERR_FRAME); break; }
        uint32_t offset = (uint32_t)frame->payload[0]
                        | ((uint32_t)frame->payload[1] << 8)
                        | ((uint32_t)frame->payload[2] << 16)
                        | ((uint32_t)frame->payload[3] << 24);
        uint16_t len = (uint16_t)frame->payload[4] | ((uint16_t)frame->payload[5] << 8);
        if (len > 250) len = 250;  // Max payload for data chunk (limited by response framing)
        uint8_t data[250];
        uint32_t actual = bb_la_read_data(offset, data, len);
        send_response(HAT_RSP_LA_DATA, data, (uint8_t)actual);
        break;
    }
    case HAT_CMD_LA_STOP:
    {
        bb_la_log("[STOP] recv streaming=%d\n", (int)bb_la_usb_is_streaming());
        // Soft-reset USB state without DCD abort.  The host preflight
        // calls HAT_CMD_LA_USB_RESET once per session (which does a full
        // DCD write_clear), so the routine STOP path doesn't need it.
        bb_la_usb_soft_reset();
        bb_la_log("[STOP] soft_reset done\n");
        // Always queue PKT_STOP so any waiting host stream task is unblocked,
        // regardless of whether streaming was active.
        bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_STOP, LA_STREAM_STOP_HOST);
        bb_la_log("[STOP] pkt_stop queued\n");
        bb_la_stop();
        bb_la_log("[STOP] la_stop done\n");
        {
            // Sync edge detector with actual post-stop state.  Hardcoding
            // LA_STATE_IDLE here causes a spurious bb_la_notify_done() on the
            // next poll iteration when the LA ended in DONE (finite capture):
            // the notify frame races with the ESP32 waiting for RSP_OK from
            // the subsequent hat_la_usb_reset() command → BBP_ERR_TIMEOUT 0x11.
            LaStatus _st; bb_la_get_status(&_st);
            s_prev_la_state = _st.state;
        }
        bb_la_log("[STOP] sending RSP_OK\n");
        send_ok(NULL, 0);
        bb_la_log("[STOP] RSP_OK sent\n");
        break;
    }
    case HAT_CMD_LA_STREAM_START:
        bb_la_usb_live_reset_sequence();
        if (!bb_la_start_stream()) { send_error(HAT_ERR_BUSY); }
        else {
            // Must mirror handle_stream_command(START) by marking the session
            // active BEFORE queuing PKT_START.  Without this flag:
            //   - bb_la_usb_is_streaming() returns false during the stream
            //   - Core 1's guard lets bb_la_poll() run on the wrong core
            //   - usb_idle=true allows bb_la_notify_done() to send a spurious
            //     HAT UART frame → ESP32 BBP seq desync → 0x11 cascade
            bb_la_usb_set_streaming(true);
            send_ok(NULL, 0);
            bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_START, LA_USB_STREAM_INFO_NONE);
        }
        break;
    case HAT_CMD_LA_USB_SEND: {
        // Send capture buffer via USB bulk endpoint (fast readout)
        const uint8_t *cap_buf;
        uint32_t cap_len;
        if (bb_la_get_capture_buffer(&cap_buf, &cap_len)) {
            send_ok(NULL, 0);  // ACK first, then send data on USB bulk
            bb_la_usb_stream_buffer(cap_buf, cap_len);
        } else {
            send_error(HAT_ERR_BUSY);
        }
        break;
    }

    case HAT_CMD_LA_LOG_ENABLE: {
        if (frame->payload_len < 1) { send_error(HAT_ERR_FRAME); break; }
        s_la_log_enabled = frame->payload[0] != 0;
        send_ok(NULL, 0);
        break;
    }

    case HAT_CMD_LA_USB_RESET: {
        // Reinitialize the vendor bulk endpoint to a clean state.
        // No bb_la_log() here — log frames sent before send_ok() confuse
        // hat_command_internal on the ESP32 side (it sees the log frame as
        // the command response and returns non-OK → BBP_ERR_TIMEOUT).
        bb_la_stop();
        bb_la_usb_abort_bulk();
        {
            // Same race as HAT_CMD_LA_STOP: sync edge detector with actual state
            // to prevent spurious notify_done racing the next command's RSP_OK.
            LaStatus _st; bb_la_get_status(&_st);
            s_prev_la_state = _st.state;
        }
        send_ok(NULL, 0);
        break;
    }
    case HAT_CMD_LA_SET_ROUTE:
        handle_la_set_route(frame->payload, frame->payload_len);
        break;

    case HAT_CMD_GET_RAIL_STATUS:
        handle_get_rail_status();
        break;
    case HAT_CMD_SET_RAIL_ENABLE:
        handle_set_rail_enable(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_SET_LED_STATE:
        handle_set_led_state(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_CALIBRATE_START:
        handle_calibrate_start(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_CALIBRATE_STATUS:
        handle_calibrate_status();
        break;
    case HAT_CMD_CALIBRATE_IMPORT:
        handle_calibrate_import(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_CALIBRATE_EXPORT:
        handle_calibrate_export(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_SET_IO_BANK:
        handle_set_io_bank(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_SET_LEVEL_SHIFT:
        handle_set_level_shift(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_SET_RAIL_VOLTAGE:
        handle_set_rail_voltage(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_FW_BEGIN:
        handle_fw_begin(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_FW_CHUNK:
        handle_fw_chunk(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_FW_STATUS:
        handle_fw_status();
        break;
    case HAT_CMD_FW_COMMIT:
        send_ok(NULL, 0);
        sleep_ms(50);
        (void)bb_fw_update_commit_verified();
        break;


    default:
        send_error(HAT_ERR_INVALID_CMD);
        break;
    }
}

// -----------------------------------------------------------------------------
// Unsolicited notification: capture done
// Sends a RSP_LA_STATUS frame with state=DONE without a prior command.
// The ESP32 recognizes this as an event and forwards to the host.
// -----------------------------------------------------------------------------

static void bb_la_notify_done(void)
{
    LaStatus st;
    bb_la_get_status(&st);
    uint8_t rsp[15];
    size_t p = 0;
    rsp[p++] = (uint8_t)st.state;
    rsp[p++] = st.channels;
    memcpy(&rsp[p], &st.samples_captured, 4); p += 4;
    memcpy(&rsp[p], &st.total_samples, 4); p += 4;
    memcpy(&rsp[p], &st.actual_rate_hz, 4); p += 4;
    rsp[p++] = bb_la_usb_rearm_pending() ? 1 : 0;
    send_response(HAT_RSP_LA_STATUS, rsp, (uint8_t)p);
}

// -----------------------------------------------------------------------------
// Log relay: send a formatted message to the host via HAT UART
// Zero overhead when s_la_log_enabled is false.
// -----------------------------------------------------------------------------

void bb_la_log(const char *fmt, ...)
{
    if (!s_la_log_enabled) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > 200) n = 200;
        send_response(HAT_RSP_LA_LOG, (const uint8_t *)buf, (uint8_t)n);
    }
}

// -----------------------------------------------------------------------------
// BugBuster Command Task
//
// When integrated with debugprobe, this function runs as a FreeRTOS task:
//   xTaskCreate(bb_cmd_task, "bb_cmd", 2048, NULL, 1, NULL);
//
// For standalone testing, main() calls it directly.
// -----------------------------------------------------------------------------

void bb_cmd_task(void *params)
{
    (void)params;

    // Initialize UART for BugBuster command bus
    uart_init(BB_UART, BB_UART_BAUD);
    gpio_set_function(BB_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BB_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(BB_UART, true);

#ifdef DEBUGPROBE_INTEGRATION
    s_uart_tx_mutex = xSemaphoreCreateMutex();
#endif

    // Initialize subsystems
    bb_power_init();
    bb_pins_init();
    bb_swd_init();
    bb_la_init();
    bb_la_usb_init();
    bb_hat_v2_init();
    bb_fw_update_init();

    // Configure IRQ pin as open-drain output (shared line, active low).
    // Default state: high-Z (input with pull-up). To assert: set output low.
    gpio_init(BB_IRQ_PIN);
    gpio_set_dir(BB_IRQ_PIN, GPIO_IN);
    gpio_pull_up(BB_IRQ_PIN);
    gpio_put(BB_IRQ_PIN, 0);  // Pre-load output register low for when we switch to output

    // Initialize frame parser
    hat_parser_init(&s_parser);

    // Main command loop
    for (;;) {
        // Read available UART bytes
        while (uart_is_readable(BB_UART)) {
            uint8_t byte = uart_getc(BB_UART);
            if (hat_parser_feed(&s_parser, byte)) {
                HatFrame frame = hat_parser_get_frame(&s_parser);
                dispatch_command(&frame);
            }
        }

        // Periodic updates
        static uint32_t last_poll = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_poll >= 1) {
            last_poll = now;

            // Power monitoring — detect new faults and assert IRQ
            bb_power_update();
            ConnectorStatus pa, pb;
            bb_power_get_status(&pa, &pb);
            if (pa.fault || pb.fault) {
                bb_irq_pulse();  // Signal ESP32 asynchronously
            }

            // Note: bb_la_poll() is now called on Core 0 during streaming
            // to ensure low-latency DMA -> USB handoff.
            if (!bb_la_usb_is_streaming()) {
                bb_la_poll();
            }

            // Note: LA streaming (feeding buffers to USB) is now handled 
            // asynchronously by bb_la_usb_send_pending() in the usb_thread.

            // Detect LA state transition to DONE and notify ESP32
            LaStatus la_st;
            bb_la_get_status(&la_st);
            // Suppress unsolicited UART status frames while USB streaming is
            // active or still draining.  An unsolicited frame racing with a
            // BBP command-response exchange desyncs the ESP32 UART → 0x11.
            bool usb_idle = !bb_la_usb_is_streaming() && !bb_la_usb_has_pending_data();
            if (usb_idle) {
                if (la_st.state == LA_STATE_DONE && s_prev_la_state != LA_STATE_DONE) {
                    bb_la_notify_done();  // Send unsolicited LA_STATUS frame
                    bb_irq_pulse();       // Also assert IRQ
                    // NOTE: no auto-push via USB — the host reads via gapless stream
                    // or explicitly via HAT_CMD_LA_USB_SEND
                }
                if (la_st.state == LA_STATE_ERROR && s_prev_la_state != LA_STATE_ERROR) {
                    bb_la_usb_send_stream_marker(
                        LA_USB_STREAM_PKT_ERROR,
                        la_st.stream_stop_reason
                    );
                }
            }
            s_prev_la_state = la_st.state;

            hat_parser_check_timeout(&s_parser, now);  // Reset parser on truncated frames
            bb_irq_poll();  // Manage IRQ pulse deassert
        }

        // Small sleep to avoid busy-loop when no UART data
#ifdef DEBUGPROBE_INTEGRATION
        vTaskDelay(1);  // Yield to other FreeRTOS tasks (avoids UART FIFO overflow)
#else
        sleep_us(100);
#endif
    }
}

// =============================================================================
// Standalone main() — for testing without debugprobe
// When integrating with debugprobe, remove this and add bb_cmd_task as a
// FreeRTOS task in debugprobe's main.c instead.
// =============================================================================

#ifndef DEBUGPROBE_INTEGRATION

int main(void)
{
    stdio_init_all();

    // Status LED
    gpio_init(BB_LED_STATUS_PIN);
    gpio_set_dir(BB_LED_STATUS_PIN, GPIO_OUT);
    gpio_put(BB_LED_STATUS_PIN, 1);

    // Run command handler (never returns)
    bb_cmd_task(NULL);

    return 0;
}

#endif // DEBUGPROBE_INTEGRATION
