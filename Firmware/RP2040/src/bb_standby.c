// =============================================================================
// bb_standby.c - LA HAT (RP2040) standby participant, Pico binding.
// See bb_standby.h and bb_standby_core.h for the contract.
//
// Power facts this relies on:
//   * The RP2040 runs, and the DS4424 trim DAC answers, with 3V3_ADJ OFF: that is the
//     boot state (bb_power_init() holds GPIO24 low, then bb_hat_v2_init() programs and
//     read-back-verifies the DS4424). So switching 3V3_ADJ off cannot take the controller,
//     the trim codes or the I2C bus down. The schematic labels agree (HAT schematic PDF):
//     the level shifters' B side (U8 TXS0108 / U9 SN74LVC8T245 VCCB) is 3V3_ADJ, their
//     A side and the RP2040 are "3V3".
//   * The WS2812 LEDs are on 5V_BUCK, not on a HAT rail: software can only drive
//     them dark, it cannot unpower them.
//   * There is no software-readable button on the HAT (BOOT/RST are hardware-only),
//     so a physical wake input does not exist; USB and S3 activity are the wake paths.
// =============================================================================

#include "bb_standby.h"
#include "bb_standby_core.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#include "board_bugbuster_hat_config.h"   // BB_DEBUGPROBE_CDC_UART_ENABLED (macros only)
#include "bb_config.h"
#include "bb_fw_update.h"
#include "bb_hat_v2.h"
#include "bb_la.h"
#include "bb_la_usb.h"
#include "bb_pins.h"
#include "bb_power.h"

#ifdef DEBUGPROBE_INTEGRATION
#include "tusb.h"
#include "DAP.h"
#endif

// Defined in bb_main.c.
extern void send_response(uint8_t rsp_cmd, const uint8_t *payload, uint8_t len);

_Static_assert(BB_SB_RAIL_3V3_ADJ == HAT_RAIL_3V3_ADJ, "rail ids must match bb_config.h");
_Static_assert(BB_SB_RAIL_VADJ3 == HAT_RAIL_VADJ3, "rail ids must match bb_config.h");
_Static_assert(BB_SB_RAIL_VADJ4 == HAT_RAIL_VADJ4, "rail ids must match bb_config.h");

static bb_sb_t s_sb;
static spin_lock_t *s_spin;
static uint32_t s_irq_state;          // written/read only by the current lock holder
static volatile bool s_ready;

// ---------------------------------------------------------------------------
// Environment for the core (lock, clock, live facts)
// ---------------------------------------------------------------------------
static void sb_lock(void *ctx)
{
    (void)ctx;
    const uint32_t saved = spin_lock_blocking(s_spin);
    s_irq_state = saved;
}

static void sb_unlock(void *ctx)
{
    (void)ctx;
    spin_unlock(s_spin, s_irq_state);
}

static uint32_t sb_now_ms(void *ctx)
{
    (void)ctx;
    return to_ms_since_boot(get_absolute_time());
}

// Facts come from the real owners, never from a cached flag of this module.
static void sb_sample(void *ctx, bb_sb_facts_t *f)
{
    (void)ctx;
    LaStatus la;
    bb_la_get_status(&la);
    f->la_armed = la.state == LA_STATE_ARMED;
    f->la_capturing = la.state == LA_STATE_CAPTURING;
    f->la_streaming = la.state == LA_STATE_STREAMING;
    f->la_usb_session = bb_la_usb_is_streaming();
    f->la_usb_pending = bb_la_usb_has_pending_data() || bb_la_usb_rearm_pending();

    uint8_t fw_state = 0, fw_err = 0;
    uint32_t written, size, expect, actual;
    bb_fw_update_get_status(&fw_state, &written, &size, &expect, &actual, &fw_err);
    f->fw_update = fw_state == BB_FW_UPDATE_RECEIVING || fw_state == BB_FW_UPDATE_READY ||
                   fw_state == BB_FW_UPDATE_COMMITTING;

    f->calibration = bb_hat_v2_busy();

#if defined(DEBUGPROBE_INTEGRATION) && BB_DEBUGPROBE_CDC_UART_ENABLED
    f->uart_bridge = tud_cdc_connected();
#else
    f->uart_bridge = false;   // the debugprobe CDC UART bridge is compiled out on this PCB
#endif
    f->bus = false;           // no external I2C/SPI/UART owner exists on the HAT
    f->cmd_pending = uart_is_readable(BB_UART);
}

static const bb_sb_env_t s_env = { NULL, sb_lock, sb_unlock, sb_now_ms, sb_sample };

// ---------------------------------------------------------------------------
// Guarded hardware actions
// ---------------------------------------------------------------------------
static const uint8_t k_io_bank[] = { 10, 11, 12, 13, 14, 15, 20, 21 };   // HS_IO0..7

static bool op_outputs_disable(void *ctx)
{
    (void)ctx;
    // OE first so no shifted output stays driven, then DIR back to the input default.
    gpio_put(BB_LEVEL_SHIFT_OE_PIN, 0);
    gpio_put(BB_LEVEL_SHIFT_DIR_PIN, 0);
    return !gpio_get(BB_LEVEL_SHIFT_OE_PIN) && !gpio_get(BB_LEVEL_SHIFT_DIR_PIN);
}

static bool op_pins_highz(void *ctx)
{
    (void)ctx;
    bb_pins_reset();   // EXP_EXT function shadow -> Disconnected, applied to the pins
    for (size_t i = 0; i < sizeof(k_io_bank); i++) {
        const uint pin = k_io_bank[i];
        gpio_init(pin);                    // SIO, input, output latch low
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
    }
    for (size_t i = 0; i < sizeof(k_io_bank); i++) {
        if (gpio_get_dir(k_io_bank[i]) == GPIO_OUT) return false;
    }
    return true;
}

static bool op_la_route_off(void *ctx)
{
    (void)ctx;
    // The HS route runs through the shared shifter, so closing it is OE low. An LA that
    // is still armed/capturing/streaming owns the route: refuse rather than cut it.
    LaStatus la;
    bb_la_get_status(&la);
    if (la.state == LA_STATE_ARMED || la.state == LA_STATE_CAPTURING ||
        la.state == LA_STATE_STREAMING) {
        return false;
    }
    gpio_put(BB_LEVEL_SHIFT_OE_PIN, 0);
    return !gpio_get(BB_LEVEL_SHIFT_OE_PIN);
}

static bool op_rail_off(void *ctx, uint8_t rail)
{
    (void)ctx;
    switch (rail) {
    case BB_SB_RAIL_VADJ3:
        bb_power_set(0, false);
        return !bb_power_get_enabled(0) && !gpio_get(BB_VADJ3_EN_PIN);
    case BB_SB_RAIL_VADJ4:
        bb_power_set(1, false);
        return !bb_power_get_enabled(1) && !gpio_get(BB_VADJ4_EN_PIN);
    case BB_SB_RAIL_3V3_ADJ:
        bb_power_set_3v3_adj(false);
        return !bb_power_get_3v3_adj_enabled() && !gpio_get(BB_3V3_ADJ_EN_PIN);
    default:
        return false;
    }
}

static bool op_verify_safe(void *ctx)
{
    (void)ctx;
    if (bb_power_get_enabled(0) || bb_power_get_enabled(1) || bb_power_get_3v3_adj_enabled()) return false;
    if (gpio_get(BB_VADJ3_EN_PIN) || gpio_get(BB_VADJ4_EN_PIN) || gpio_get(BB_3V3_ADJ_EN_PIN)) return false;
    if (gpio_get(BB_LEVEL_SHIFT_OE_PIN)) return false;
    for (size_t i = 0; i < sizeof(k_io_bank); i++) {
        if (gpio_get_dir(k_io_bank[i]) == GPIO_OUT) return false;
    }
    return true;
}

static bool op_restore_ready(void *ctx)
{
    (void)ctx;
    return bb_hat_v2_standby_restore();
}

static bool op_leds(void *ctx, bool dark)
{
    (void)ctx;
    bb_hat_v2_leds_set_dark(dark);
    return true;
}

static const bb_sb_ops_t s_ops = {
    NULL, op_outputs_disable, op_pins_highz, op_la_route_off,
    op_rail_off, op_verify_safe, op_restore_ready, op_leds,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void bb_standby_init(void)
{
    if (s_ready) return;
    s_spin = spin_lock_init(spin_lock_claim_unused(true));
    bb_sb_init(&s_sb);
    s_ready = true;
}

void bb_standby_handle_frame(const uint8_t *payload, uint8_t len)
{
    bb_standby_reply_t rp;
    if (!s_ready) {
        memset(&rp, 0, sizeof(rp));
        rp.schema = BB_STANDBY_SCHEMA;
        rp.failure = BB_SB_FAIL_ORDER;
    } else {
        bb_sb_service(&s_sb, &s_env, &s_ops, payload, len, &rp);
    }
    send_response(BB_HAT_RSP_STANDBY, (const uint8_t *)&rp, (uint8_t)sizeof(rp));
}

bool bb_standby_cmd_enter(uint8_t cmd, const uint8_t *payload, uint8_t len, bool *admitted)
{
    *admitted = false;
    if (!s_ready) return true;
    if (bb_sb_classify(cmd, payload, len) != BB_SB_CMD_WORK) return true;

    sb_lock(NULL);
    const bool ok = bb_sb_admit(&s_sb);
    sb_unlock(NULL);
    *admitted = ok;
    return ok;
}

void bb_standby_cmd_leave(void)
{
    if (!s_ready) return;
    sb_lock(NULL);
    bb_sb_leave(&s_sb);
    sb_unlock(NULL);
}

bool bb_standby_work_enter(void)
{
    if (!s_ready) return true;
    sb_lock(NULL);
    const bool ok = bb_sb_admit(&s_sb);
    sb_unlock(NULL);
    return ok;
}

void bb_standby_work_leave(void)
{
    bb_standby_cmd_leave();
}

bool bb_standby_poll(void)
{
    if (!s_ready) return false;
    bb_sb_facts_t f;
    memset(&f, 0, sizeof(f));
    sb_sample(NULL, &f);

    bb_sb_fx_t fx;
    sb_lock(NULL);
    bb_sb_tick(&s_sb, sb_now_ms(NULL), &f, &fx);
    sb_unlock(NULL);
    return fx.irq;
}

bool bb_standby_monitor_allowed(void)
{
    if (!s_ready) return true;
    sb_lock(NULL);
    const bool ok = bb_sb_monitor_allowed(&s_sb);
    sb_unlock(NULL);
    return ok;
}

void bb_standby_usb_unmounted(void)
{
    if (!s_ready) return;
    sb_lock(NULL);
    bb_sb_dap_host_gone(&s_sb);
    sb_unlock(NULL);
}

// ---------------------------------------------------------------------------
// CMSIS-DAP frames (linked in via -Wl,--wrap in CMakeLists.txt, so the debugprobe
// submodule stays untouched). DAP_Data.debug_port is the probe's own truth for
// "a host has an open session", so Connect/Disconnect are never guessed from bytes.
// ---------------------------------------------------------------------------
#ifdef DEBUGPROBE_INTEGRATION
extern uint32_t __real_DAP_ExecuteCommand(const uint8_t *request, uint8_t *response);
extern uint32_t __real_DAP_ProcessCommand(const uint8_t *request, uint8_t *response);

// DAP.h is included here without DAP_config.h, so only the first field's offset is safe to rely on.
_Static_assert(offsetof(DAP_Data_t, debug_port) == 0, "debug_port must be the first DAP_Data field");

static uint32_t dap_refuse(const uint8_t *request, uint8_t *response)
{
    // A well-formed refusal: DAP_Connect answers "no port", everything else is invalid.
    if (request[0] == ID_DAP_Connect) {
        response[0] = ID_DAP_Connect;
        response[1] = DAP_PORT_DISABLED;
        return (2U << 16) | 2U;
    }
    response[0] = ID_DAP_Invalid;
    return (1U << 16) | 1U;
}

static uint32_t dap_guarded(uint32_t (*real)(const uint8_t *, uint8_t *),
                            const uint8_t *request, uint8_t *response)
{
    if (!s_ready) return real(request, response);

    sb_lock(NULL);
    const bool ok = bb_sb_dap_enter(&s_sb, sb_now_ms(NULL));
    sb_unlock(NULL);
    if (!ok) return dap_refuse(request, response);

    const uint32_t r = real(request, response);

    sb_lock(NULL);
    bb_sb_dap_leave(&s_sb, sb_now_ms(NULL), DAP_Data.debug_port != DAP_PORT_DISABLED);
    sb_unlock(NULL);
    return r;
}

uint32_t __wrap_DAP_ExecuteCommand(const uint8_t *request, uint8_t *response)
{
    return dap_guarded(__real_DAP_ExecuteCommand, request, response);
}

uint32_t __wrap_DAP_ProcessCommand(const uint8_t *request, uint8_t *response)
{
    return dap_guarded(__real_DAP_ProcessCommand, request, response);
}
#endif
