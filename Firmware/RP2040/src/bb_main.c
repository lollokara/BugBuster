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
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#ifdef DEBUGPROBE_INTEGRATION
#include "FreeRTOS.h"
#include "task.h"
#endif

#include "bb_config.h"
#include "bb_protocol.h"
#include "bb_power.h"
#include "bb_hvpak.h"
#include "bb_pins.h"
#include "bb_swd.h"
#include "bb_la.h"
#include "bb_la_usb.h"

// Firmware version
#define BB_HAT_FW_MAJOR  1
#define BB_HAT_FW_MINOR  0

static HatFrameParser s_parser;

// Track previous LA state for detecting DONE transition
static LaState s_prev_la_state = LA_STATE_IDLE;

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
static uint32_t s_irq_assert_ms = 0;
static bool     s_irq_active = false;

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

static void send_response(uint8_t rsp_cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[36];
    size_t frame_len = hat_build_frame(frame, rsp_cmd, payload, len);
    uart_write_blocking(BB_UART, frame, frame_len);
}

static void send_ok(const uint8_t *payload, uint8_t len)
{
    send_response(HAT_RSP_OK, payload, len);
}

static void send_error(uint8_t error_code)
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
        bb_pins_set(pin, func);
        send_ok(NULL, 0);
    } else if (len == 4) {
        // All pins mode
        for (int i = 0; i < 4; i++) {
            if (payload[i] > HAT_FUNC_GPIO4) { send_error(HAT_ERR_INVALID_FUNC); return; }
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
    send_ok(NULL, 0);
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

    if (!bb_hvpak_set_voltage(mv)) {
        send_error(HAT_ERR_INVALID_FUNC);
        return;
    }
    send_ok(NULL, 0);
}

static void handle_get_io_voltage(void)
{
    uint16_t mv = bb_hvpak_get_voltage();
    uint8_t rsp[2] = { (uint8_t)(mv & 0xFF), (uint8_t)(mv >> 8) };
    send_ok(rsp, 2);
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
        bb_la_set_trigger(&trig);
        send_ok(NULL, 0);
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
        uint8_t rsp[14];
        size_t p = 0;
        rsp[p++] = (uint8_t)st.state;
        rsp[p++] = st.channels;
        memcpy(&rsp[p], &st.samples_captured, 4); p += 4;
        memcpy(&rsp[p], &st.total_samples, 4); p += 4;
        memcpy(&rsp[p], &st.actual_rate_hz, 4); p += 4;
        // Diagnostic: USB vendor mount status
        rsp[p++] = bb_la_usb_connected() ? 1 : 0;
        rsp[p++] = tud_mounted() ? 1 : 0;  // Overall USB mounted
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
        if (len > 900) len = 900;  // Max payload for data chunk
        uint8_t data[900];
        uint32_t actual = bb_la_read_data(offset, data, len);
        send_response(HAT_RSP_LA_DATA, data, (uint8_t)actual);
        break;
    }
    case HAT_CMD_LA_STOP:
        bb_la_stop();
        send_ok(NULL, 0);
        break;
    case HAT_CMD_LA_STREAM_START:
        if (!bb_la_start_stream()) { send_error(HAT_ERR_BUSY); }
        else { send_ok(NULL, 0); }
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
    uint8_t rsp[14];
    size_t p = 0;
    rsp[p++] = (uint8_t)st.state;
    rsp[p++] = st.channels;
    memcpy(&rsp[p], &st.samples_captured, 4); p += 4;
    memcpy(&rsp[p], &st.total_samples, 4); p += 4;
    memcpy(&rsp[p], &st.actual_rate_hz, 4); p += 4;
    send_response(HAT_RSP_LA_STATUS, rsp, (uint8_t)p);
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

    // Initialize subsystems
    bb_power_init();
    bb_hvpak_init();
    bb_pins_init();
    bb_swd_init();
    bb_la_init();

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

            // LA trigger check + DMA completion
            bb_la_poll();

            // Poll USB vendor OUT for direct stream commands (gapless path)
            bb_la_usb_poll_commands();

            // LA streaming: send completed buffer halves via USB (raw, no header)
            {
                const uint8_t *stream_buf;
                uint32_t stream_len;
                if (bb_la_stream_get_buffer(&stream_buf, &stream_len)) {
                    bb_la_usb_write_raw(stream_buf, stream_len);
                    bb_la_stream_buffer_sent();
                }
            }

            // Detect LA state transition to DONE and notify ESP32
            LaStatus la_st;
            bb_la_get_status(&la_st);
            if (la_st.state == LA_STATE_DONE && s_prev_la_state != LA_STATE_DONE) {
                bb_la_notify_done();  // Send unsolicited LA_STATUS frame
                bb_irq_pulse();       // Also assert IRQ
                // NOTE: no auto-push via USB — the host reads via gapless stream
                // or explicitly via HAT_CMD_LA_USB_SEND
            }
            s_prev_la_state = la_st.state;

            hat_parser_check_timeout(&s_parser, now);  // Reset parser on truncated frames
            bb_irq_poll();  // Manage IRQ pulse deassert
        }

        // Small sleep to avoid busy-loop when no UART data
#ifdef DEBUGPROBE_INTEGRATION
        vTaskDelay(1);  // Yield to other FreeRTOS tasks (1 tick = ~1ms)
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
