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
#endif

#include "bb_config.h"
#include "bb_protocol.h"
#include "bb_power.h"
#include "bb_hvpak.h"
#include "bb_pins.h"
#include "bb_swd.h"
#include "bb_la.h"
#include "bb_la_usb.h"
#include "tusb.h"

#ifdef DEBUGPROBE_INTEGRATION
extern TaskHandle_t tud_taskhandle;
#endif

// Firmware version
#define BB_HAT_FW_MAJOR  1
#define BB_HAT_FW_MINOR  1

static HatFrameParser s_parser;

// Track previous LA state for detecting DONE transition
static LaState s_prev_la_state = LA_STATE_IDLE;

// Log relay: when enabled, bb_la_log() messages are sent via HAT UART to host
static volatile bool s_la_log_enabled = false;

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
        switch (bb_hvpak_get_last_error()) {
            case BB_HVPAK_ERR_NO_DEVICE:
                send_error(HAT_ERR_HVPAK_NO_DEVICE);
                break;
            case BB_HVPAK_ERR_I2C_TIMEOUT:
                send_error(HAT_ERR_HVPAK_TIMEOUT);
                break;
            case BB_HVPAK_ERR_UNKNOWN_IDENTITY:
                send_error(HAT_ERR_HVPAK_UNKNOWN_IDENTITY);
                break;
            case BB_HVPAK_ERR_UNSUPPORTED_VOLTAGE:
                send_error(HAT_ERR_HVPAK_UNSUPPORTED_VOLT);
                break;
            case BB_HVPAK_ERR_WRITE_FAILED:
                send_error(HAT_ERR_HVPAK_WRITE_FAILED);
                break;
            default:
                send_error(HAT_ERR_INVALID_FUNC);
                break;
        }
        return;
    }

    uint16_t requested_mv = bb_hvpak_get_requested_voltage();
    uint16_t applied_mv = bb_hvpak_get_voltage();
    uint8_t rsp[7] = {
        (uint8_t)(requested_mv & 0xFF),
        (uint8_t)(requested_mv >> 8),
        (uint8_t)(applied_mv & 0xFF),
        (uint8_t)(applied_mv >> 8),
        (uint8_t)bb_hvpak_get_part(),
        (uint8_t)(bb_hvpak_is_ready() ? 1 : 0),
        bb_hvpak_get_last_error(),
    };
    send_ok(rsp, sizeof(rsp));
}

static void handle_get_io_voltage(void)
{
    uint16_t requested_mv = bb_hvpak_get_requested_voltage();
    uint16_t applied_mv = bb_hvpak_get_voltage();
    uint8_t rsp[7] = {
        (uint8_t)(requested_mv & 0xFF),
        (uint8_t)(requested_mv >> 8),
        (uint8_t)(applied_mv & 0xFF),
        (uint8_t)(applied_mv >> 8),
        (uint8_t)bb_hvpak_get_part(),
        (uint8_t)(bb_hvpak_is_ready() ? 1 : 0),
        bb_hvpak_get_last_error(),
    };
    send_ok(rsp, sizeof(rsp));
}

static void send_hvpak_error_from_state(void)
{
    switch (bb_hvpak_get_last_error()) {
        case BB_HVPAK_ERR_NO_DEVICE:
            send_error(HAT_ERR_HVPAK_NO_DEVICE);
            break;
        case BB_HVPAK_ERR_I2C_TIMEOUT:
            send_error(HAT_ERR_HVPAK_TIMEOUT);
            break;
        case BB_HVPAK_ERR_UNKNOWN_IDENTITY:
            send_error(HAT_ERR_HVPAK_UNKNOWN_IDENTITY);
            break;
        case BB_HVPAK_ERR_UNSUPPORTED_VOLTAGE:
            send_error(HAT_ERR_HVPAK_UNSUPPORTED_VOLT);
            break;
        case BB_HVPAK_ERR_WRITE_FAILED:
            send_error(HAT_ERR_HVPAK_WRITE_FAILED);
            break;
        case BB_HVPAK_ERR_INVALID_INDEX:
            send_error(HAT_ERR_HVPAK_INVALID_INDEX);
            break;
        case BB_HVPAK_ERR_UNSUPPORTED_CAPABILITY:
            send_error(HAT_ERR_HVPAK_UNSUPPORTED_CAP);
            break;
        case BB_HVPAK_ERR_INVALID_ARGUMENT:
            send_error(HAT_ERR_HVPAK_INVALID_ARG);
            break;
        case BB_HVPAK_ERR_UNSAFE_REGISTER:
            send_error(HAT_ERR_HVPAK_UNSAFE_REG);
            break;
        default:
            send_error(HAT_ERR_INVALID_FUNC);
            break;
    }
}

static void handle_get_hvpak_info(void)
{
    uint16_t requested_mv = bb_hvpak_get_requested_voltage();
    uint16_t applied_mv = bb_hvpak_get_voltage();
    uint8_t rsp[12] = {
        (uint8_t)bb_hvpak_get_part(),
        (uint8_t)(bb_hvpak_is_ready() ? 1 : 0),
        bb_hvpak_get_last_error(),
        (uint8_t)(bb_hvpak_is_factory_virgin() ? 1 : 0),
        (uint8_t)(bb_hvpak_has_service_window() ? 1 : 0),
        (uint8_t)(requested_mv & 0xFF),
        (uint8_t)(requested_mv >> 8),
        (uint8_t)(applied_mv & 0xFF),
        (uint8_t)(applied_mv >> 8),
        bb_hvpak_get_service_f5(),
        bb_hvpak_get_service_fd(),
        bb_hvpak_get_service_fe(),
    };
    send_ok(rsp, sizeof(rsp));
}

static void handle_get_hvpak_caps(void)
{
    BbHvpakCapabilities caps;
    uint8_t rsp[11];
    size_t pos = 0;

    if (!bb_hvpak_get_capabilities(&caps)) {
        send_hvpak_error_from_state();
        return;
    }

    memcpy(&rsp[pos], &caps.flags, sizeof(caps.flags)); pos += sizeof(caps.flags);
    rsp[pos++] = caps.lut2_count;
    rsp[pos++] = caps.lut3_count;
    rsp[pos++] = caps.lut4_count;
    rsp[pos++] = caps.pwm_count;
    rsp[pos++] = caps.comparator_count;
    rsp[pos++] = caps.bridge_count;
    send_ok(rsp, (uint8_t)pos);
}

static void handle_get_hvpak_lut(const uint8_t *payload, uint8_t len)
{
    BbHvpakLutConfig cfg;
    uint8_t rsp[5];

    if (len < 2) { send_error(HAT_ERR_FRAME); return; }
    if (!bb_hvpak_get_lut(payload[0], payload[1], &cfg)) {
        send_hvpak_error_from_state();
        return;
    }

    rsp[0] = cfg.kind;
    rsp[1] = cfg.index;
    rsp[2] = cfg.width_bits;
    rsp[3] = (uint8_t)(cfg.truth_table & 0xFF);
    rsp[4] = (uint8_t)(cfg.truth_table >> 8);
    send_ok(rsp, sizeof(rsp));
}

static void handle_set_hvpak_lut(const uint8_t *payload, uint8_t len)
{
    BbHvpakLutConfig cfg;
    if (len < 4) { send_error(HAT_ERR_FRAME); return; }
    cfg.kind = payload[0];
    cfg.index = payload[1];
    cfg.truth_table = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
    if (!bb_hvpak_set_lut(&cfg)) {
        send_hvpak_error_from_state();
        return;
    }
    handle_get_hvpak_lut(payload, 2);
}

static void handle_get_hvpak_bridge(void)
{
    BbHvpakBridgeConfig cfg;
    uint8_t rsp[9];
    if (!bb_hvpak_get_bridge(&cfg)) {
        send_hvpak_error_from_state();
        return;
    }
    rsp[0] = cfg.output_mode[0];
    rsp[1] = cfg.ocp_retry[0];
    rsp[2] = cfg.output_mode[1];
    rsp[3] = cfg.ocp_retry[1];
    rsp[4] = cfg.predriver_enabled ? 1 : 0;
    rsp[5] = cfg.full_bridge_enabled ? 1 : 0;
    rsp[6] = cfg.control_selection_ph_en ? 1 : 0;
    rsp[7] = cfg.ocp_deglitch_enabled ? 1 : 0;
    rsp[8] = cfg.uvlo_enabled ? 1 : 0;
    send_ok(rsp, sizeof(rsp));
}

static void handle_set_hvpak_bridge(const uint8_t *payload, uint8_t len)
{
    BbHvpakBridgeConfig cfg;
    if (len < 9) { send_error(HAT_ERR_FRAME); return; }
    cfg.output_mode[0] = payload[0];
    cfg.ocp_retry[0] = payload[1];
    cfg.output_mode[1] = payload[2];
    cfg.ocp_retry[1] = payload[3];
    cfg.predriver_enabled = payload[4] != 0;
    cfg.full_bridge_enabled = payload[5] != 0;
    cfg.control_selection_ph_en = payload[6] != 0;
    cfg.ocp_deglitch_enabled = payload[7] != 0;
    cfg.uvlo_enabled = payload[8] != 0;
    if (!bb_hvpak_set_bridge(&cfg)) {
        send_hvpak_error_from_state();
        return;
    }
    handle_get_hvpak_bridge();
}

static void handle_get_hvpak_analog(void)
{
    BbHvpakAnalogConfig cfg;
    uint8_t rsp[15];
    if (!bb_hvpak_get_analog(&cfg)) {
        send_hvpak_error_from_state();
        return;
    }
    rsp[0] = cfg.vref_mode;
    rsp[1] = cfg.vref_powered ? 1 : 0;
    rsp[2] = cfg.vref_power_from_matrix ? 1 : 0;
    rsp[3] = cfg.vref_sink_12ua ? 1 : 0;
    rsp[4] = cfg.vref_input_selection;
    rsp[5] = cfg.current_sense_vref;
    rsp[6] = cfg.current_sense_dynamic_from_pwm ? 1 : 0;
    rsp[7] = cfg.current_sense_gain;
    rsp[8] = cfg.current_sense_invert ? 1 : 0;
    rsp[9] = cfg.current_sense_enabled ? 1 : 0;
    rsp[10] = cfg.acmp0_gain;
    rsp[11] = cfg.acmp0_vref;
    rsp[12] = cfg.has_acmp1 ? 1 : 0;
    rsp[13] = cfg.acmp1_gain;
    rsp[14] = cfg.acmp1_vref;
    send_ok(rsp, sizeof(rsp));
}

static void handle_set_hvpak_analog(const uint8_t *payload, uint8_t len)
{
    BbHvpakAnalogConfig cfg;
    if (len < 15) { send_error(HAT_ERR_FRAME); return; }
    cfg.vref_mode = payload[0];
    cfg.vref_powered = payload[1] != 0;
    cfg.vref_power_from_matrix = payload[2] != 0;
    cfg.vref_sink_12ua = payload[3] != 0;
    cfg.vref_input_selection = payload[4];
    cfg.current_sense_vref = payload[5];
    cfg.current_sense_dynamic_from_pwm = payload[6] != 0;
    cfg.current_sense_gain = payload[7];
    cfg.current_sense_invert = payload[8] != 0;
    cfg.current_sense_enabled = payload[9] != 0;
    cfg.acmp0_gain = payload[10];
    cfg.acmp0_vref = payload[11];
    cfg.has_acmp1 = payload[12] != 0;
    cfg.acmp1_gain = payload[13];
    cfg.acmp1_vref = payload[14];
    if (!bb_hvpak_set_analog(&cfg)) {
        send_hvpak_error_from_state();
        return;
    }
    handle_get_hvpak_analog();
}

static void handle_get_hvpak_pwm(const uint8_t *payload, uint8_t len)
{
    BbHvpakPwmConfig cfg;
    uint8_t rsp[17];
    if (len < 1) { send_error(HAT_ERR_FRAME); return; }
    if (!bb_hvpak_get_pwm(payload[0], &cfg)) {
        send_hvpak_error_from_state();
        return;
    }
    rsp[0] = cfg.index;
    rsp[1] = cfg.initial_value;
    rsp[2] = cfg.current_value;
    rsp[3] = cfg.resolution_7bit ? 1 : 0;
    rsp[4] = cfg.out_plus_inverted ? 1 : 0;
    rsp[5] = cfg.out_minus_inverted ? 1 : 0;
    rsp[6] = cfg.async_powerdown ? 1 : 0;
    rsp[7] = cfg.autostop_mode ? 1 : 0;
    rsp[8] = cfg.boundary_osc_disable ? 1 : 0;
    rsp[9] = cfg.phase_correct ? 1 : 0;
    rsp[10] = cfg.deadband;
    rsp[11] = cfg.stop_mode ? 1 : 0;
    rsp[12] = cfg.i2c_trigger ? 1 : 0;
    rsp[13] = cfg.duty_source;
    rsp[14] = cfg.period_clock_source;
    rsp[15] = cfg.duty_clock_source;
    rsp[16] = bb_hvpak_get_last_error();
    send_ok(rsp, sizeof(rsp));
}

static void handle_set_hvpak_pwm(const uint8_t *payload, uint8_t len)
{
    BbHvpakPwmConfig cfg;
    if (len < 16) { send_error(HAT_ERR_FRAME); return; }
    memset(&cfg, 0, sizeof(cfg));
    cfg.index = payload[0];
    cfg.initial_value = payload[1];
    cfg.resolution_7bit = payload[3] != 0;
    cfg.out_plus_inverted = payload[4] != 0;
    cfg.out_minus_inverted = payload[5] != 0;
    cfg.async_powerdown = payload[6] != 0;
    cfg.autostop_mode = payload[7] != 0;
    cfg.boundary_osc_disable = payload[8] != 0;
    cfg.phase_correct = payload[9] != 0;
    cfg.deadband = payload[10];
    cfg.stop_mode = payload[11] != 0;
    cfg.i2c_trigger = payload[12] != 0;
    cfg.duty_source = payload[13];
    cfg.period_clock_source = payload[14];
    cfg.duty_clock_source = payload[15];
    if (!bb_hvpak_set_pwm(&cfg)) {
        send_hvpak_error_from_state();
        return;
    }
    handle_get_hvpak_pwm(payload, 1);
}

static void handle_hvpak_reg_read(const uint8_t *payload, uint8_t len)
{
    uint8_t value = 0;
    uint8_t rsp[2];
    if (len < 1) { send_error(HAT_ERR_FRAME); return; }
    if (!bb_hvpak_reg_read(payload[0], &value)) {
        send_hvpak_error_from_state();
        return;
    }
    rsp[0] = payload[0];
    rsp[1] = value;
    send_ok(rsp, sizeof(rsp));
}

static void handle_hvpak_reg_write_masked(const uint8_t *payload, uint8_t len)
{
    uint8_t rsp[4];
    uint8_t actual = 0;
    if (len < 3) { send_error(HAT_ERR_FRAME); return; }
    if (!bb_hvpak_reg_write_masked(payload[0], payload[1], payload[2])) {
        send_hvpak_error_from_state();
        return;
    }
    if (!bb_hvpak_reg_read(payload[0], &actual)) {
        send_hvpak_error_from_state();
        return;
    }
    rsp[0] = payload[0];
    rsp[1] = payload[1];
    rsp[2] = payload[2];
    rsp[3] = actual;
    send_ok(rsp, sizeof(rsp));
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
    case HAT_CMD_GET_HVPAK_INFO:
        handle_get_hvpak_info();
        break;
    case HAT_CMD_GET_HVPAK_CAPS:
        handle_get_hvpak_caps();
        break;
    case HAT_CMD_GET_HVPAK_LUT:
        handle_get_hvpak_lut(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_SET_HVPAK_LUT:
        handle_set_hvpak_lut(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_GET_HVPAK_BRIDGE:
        handle_get_hvpak_bridge();
        break;
    case HAT_CMD_SET_HVPAK_BRIDGE:
        handle_set_hvpak_bridge(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_GET_HVPAK_ANALOG:
        handle_get_hvpak_analog();
        break;
    case HAT_CMD_SET_HVPAK_ANALOG:
        handle_set_hvpak_analog(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_GET_HVPAK_PWM:
        handle_get_hvpak_pwm(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_SET_HVPAK_PWM:
        handle_set_hvpak_pwm(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_HVPAK_REG_READ:
        handle_hvpak_reg_read(frame->payload, frame->payload_len);
        break;
    case HAT_CMD_HVPAK_REG_WRITE_MASKED:
        handle_hvpak_reg_write_masked(frame->payload, frame->payload_len);
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
        uint8_t rsp[28];
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
        // Soft-reset USB state without DCD abort.  The host preflight
        // calls HAT_CMD_LA_USB_RESET once per session (which does a full
        // DCD write_clear), so the routine STOP path doesn't need it.
        bb_la_usb_soft_reset();
        // Always queue PKT_STOP so any waiting host stream task is unblocked,
        // regardless of whether streaming was active.
        bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_STOP, LA_STREAM_STOP_HOST);
        bb_la_stop();
        s_prev_la_state = LA_STATE_IDLE;  // reset edge detector — prevent stale notify-done
        send_ok(NULL, 0);
        break;
    }
    case HAT_CMD_LA_STREAM_START:
        bb_la_usb_live_reset_sequence();
        if (!bb_la_start_stream()) { send_error(HAT_ERR_BUSY); }
        else {
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
        s_prev_la_state = LA_STATE_IDLE;  // reset edge detector — prevent stale notify-done
        send_ok(NULL, 0);
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

    // Initialize subsystems
    bb_power_init();
    bb_hvpak_init();
    bb_pins_init();
    bb_swd_init();
    bb_la_init();
    bb_la_usb_init();

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
