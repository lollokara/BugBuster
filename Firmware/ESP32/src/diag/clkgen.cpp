// =============================================================================
// clkgen.cpp - Bench clock generator on any of the 12 BugBuster IOs.
//
// Two backends are supported:
//   LEDC  — LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, channel 0.  Range [10, 40 MHz].
//   MCPWM — v5 API (mcpwm_prelude.h), 160 MHz timer.  Range [10, 80 MHz].
//
// The chosen IO's GPIO-direct-drive ADGS2414D switch is closed on start and
// reopened on stop.  PIN_LSHIFT_OE is asserted high before any switch action
// and restored to its prior level on stop.
// =============================================================================

#include "clkgen.h"

#include "config.h"
#include "hal/adgs2414d.h"
#include "diag/selftest.h"

#include "driver/ledc.h"
#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"

#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// IO → GPIO / ADGS routing table (indexed by io-1, i.e. IO1 = index 0)
// Source of truth: MUX_GPIO_MAP in adgs2414d.h + DIO_PIN_MAP in dio.cpp.
// ---------------------------------------------------------------------------
static const struct {
    uint8_t gpio;
    uint8_t dev;
    uint8_t sw;
} IO_ROUTE[12] = {
    { 4,  0, 6 },  // IO1  → GPIO4,  U10 S7 (idx 6)
    { 2,  0, 4 },  // IO2  → GPIO2,  U10 S5 (idx 4)
    { 1,  0, 0 },  // IO3  → GPIO1,  U10 S1 (idx 0)
    { 7,  1, 6 },  // IO4  → GPIO7,  U11 S7 (idx 6)
    { 6,  1, 4 },  // IO5  → GPIO6,  U11 S5 (idx 4)
    { 5,  1, 0 },  // IO6  → GPIO5,  U11 S1 (idx 0)
    { 8,  3, 6 },  // IO7  → GPIO8,  U17 S7 (idx 6)
    { 9,  3, 4 },  // IO8  → GPIO9,  U17 S5 (idx 4)
    {10,  3, 0 },  // IO9  → GPIO10, U17 S1 (idx 0)
    {11,  2, 6 },  // IO10 → GPIO11, U16 S7 (idx 6)
    {12,  2, 4 },  // IO11 → GPIO12, U16 S5 (idx 4)
    {13,  2, 0 },  // IO12 → GPIO13, U16 S1 (idx 0)
};

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
typedef struct {
    bool          active;
    uint8_t       io;
    gpio_num_t    gpio;
    ClkSrc        src;
    uint32_t      req_hz;
    uint32_t      actual_hz;
    uint8_t       adgs_dev;
    uint8_t       adgs_sw;
    uint8_t       prior_lshift_oe;

    // MCPWM handles (valid when src==CLKSRC_MCPWM and active==true)
    mcpwm_timer_handle_t  mcpwm_timer;
    mcpwm_oper_handle_t   mcpwm_oper;
    mcpwm_cmpr_handle_t   mcpwm_cmpr;
    mcpwm_gen_handle_t    mcpwm_gen;
} ClkState;

static ClkState s_state;

// ---------------------------------------------------------------------------
// Helper: compute LEDC bit resolution so (hz << bits) <= 80_000_000.
// Returns the largest such value clamped to [1, 13].
// ---------------------------------------------------------------------------
static int pick_ledc_bits(uint32_t hz)
{
    int bits = 13;
    while (bits > 1 && ((uint64_t)hz << bits) > 80000000ULL) {
        bits--;
    }
    return bits;
}

// ---------------------------------------------------------------------------
// LEDC tear-down
// ---------------------------------------------------------------------------
static void ledc_teardown(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
    ledc_timer_rst(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
}

// ---------------------------------------------------------------------------
// MCPWM tear-down
// ---------------------------------------------------------------------------
static void mcpwm_teardown(void)
{
    if (s_state.mcpwm_timer) {
        mcpwm_timer_start_stop(s_state.mcpwm_timer, MCPWM_TIMER_START_STOP_FULL);
        mcpwm_timer_disable(s_state.mcpwm_timer);
    }
    if (s_state.mcpwm_gen)   { mcpwm_del_generator(s_state.mcpwm_gen);   s_state.mcpwm_gen   = NULL; }
    if (s_state.mcpwm_cmpr)  { mcpwm_del_comparator(s_state.mcpwm_cmpr); s_state.mcpwm_cmpr  = NULL; }
    if (s_state.mcpwm_oper)  { mcpwm_del_operator(s_state.mcpwm_oper);   s_state.mcpwm_oper  = NULL; }
    if (s_state.mcpwm_timer) { mcpwm_del_timer(s_state.mcpwm_timer);     s_state.mcpwm_timer = NULL; }
}

// ---------------------------------------------------------------------------
// clkgen_init — zero state, called once from main after dio_init().
// ---------------------------------------------------------------------------
void clkgen_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
}

// ---------------------------------------------------------------------------
// clkgen_start
// ---------------------------------------------------------------------------
bool clkgen_start(uint8_t io, ClkSrc src, uint32_t hz, char* err, size_t errlen)
{
    // 1. Already active?
    if (s_state.active) {
        snprintf(err, errlen, "already active, run 'clkout off' first");
        return false;
    }

    // 2. Validate io, src, hz range.
    if (io < 1 || io > 12) {
        snprintf(err, errlen, "IO must be 1..12");
        return false;
    }
    if (src != CLKSRC_LEDC && src != CLKSRC_MCPWM) {
        snprintf(err, errlen, "src must be ledc or mcpwm");
        return false;
    }
    if (src == CLKSRC_LEDC && (hz < 10 || hz > 40000000)) {
        snprintf(err, errlen, "LEDC range is 10..40000000 Hz");
        return false;
    }
    if (src == CLKSRC_MCPWM && (hz < 10 || hz > 80000000)) {
        snprintf(err, errlen, "MCPWM range is 10..80000000 Hz");
        return false;
    }

    // 3. Selftest interlock.
    if (selftest_is_busy()) {
        snprintf(err, errlen, "selftest busy");
        return false;
    }

    // 4. AD74416H contention check.
    // The GPIO-direct switch for IO<n> is at (dev, sw). The AD74416H side of
    // the same IO net is at switch index sw+1 (S2/S6/S8 vs S1/S5/S7).
    uint8_t dev = IO_ROUTE[io - 1].dev;
    uint8_t sw  = IO_ROUTE[io - 1].sw;
    uint8_t states[4];
    adgs_get_api_states(states);
    if (states[dev] & (1u << (sw + 1))) {
        snprintf(err, errlen,
                 "IO%u AD74416H side active; run 'func <ch> 0' first", io);
        return false;
    }

    // 5-6. Assert level shifter OE, saving prior state.
    uint8_t prior_oe = (uint8_t)gpio_get_level(PIN_LSHIFT_OE);
    pin_write(PIN_LSHIFT_OE, 1);

    // 7. Close the GPIO-direct-drive MUX switch (100 ms dead time inside driver).
    if (!adgs_set_api_switch_safe(dev, sw, true)) {
        pin_write(PIN_LSHIFT_OE, prior_oe);
        snprintf(err, errlen, "MUX switch failed (invalid device/switch)");
        return false;
    }

    // 8. Configure the clock peripheral.
    gpio_num_t gpin = (gpio_num_t)IO_ROUTE[io - 1].gpio;
    uint32_t actual_hz = 0;
    bool backend_ok = false;

    if (src == CLKSRC_LEDC) {
        int bits = pick_ledc_bits(hz);

        ledc_timer_config_t tcfg = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .duty_resolution = (ledc_timer_bit_t)bits,
            .timer_num       = LEDC_TIMER_0,
            .freq_hz         = hz,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        ledc_channel_config_t ccfg = {
            .gpio_num       = gpin,
            .speed_mode     = LEDC_LOW_SPEED_MODE,
            .channel        = LEDC_CHANNEL_0,
            .intr_type      = LEDC_INTR_DISABLE,
            .timer_sel      = LEDC_TIMER_0,
            .duty           = (uint32_t)(1u << (bits - 1)),
            .hpoint         = 0,
        };

        if (ledc_timer_config(&tcfg) == ESP_OK &&
            ledc_channel_config(&ccfg) == ESP_OK) {
            actual_hz  = ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
            backend_ok = true;
        }
    } else {
        // MCPWM v5 API
        uint32_t period_ticks = (uint32_t)(160000000.0 / (double)hz + 0.5);
        if (period_ticks < 2) period_ticks = 2;

        mcpwm_timer_config_t tcfg = {
            .group_id      = 0,
            .clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT,
            .resolution_hz = 160000000,
            .count_mode    = MCPWM_TIMER_COUNT_MODE_UP,
            .period_ticks  = period_ticks,
        };
        mcpwm_operator_config_t ocfg = { .group_id = 0 };
        mcpwm_comparator_config_t ccfg = {};
        ccfg.flags.update_cmp_on_tez = true;
        mcpwm_generator_config_t gcfg = { .gen_gpio_num = gpin };

        mcpwm_timer_handle_t  tmr  = NULL;
        mcpwm_oper_handle_t   oper = NULL;
        mcpwm_cmpr_handle_t   cmpr = NULL;
        mcpwm_gen_handle_t    gen  = NULL;

        if (mcpwm_new_timer(&tcfg, &tmr) == ESP_OK &&
            mcpwm_new_operator(&ocfg, &oper) == ESP_OK &&
            mcpwm_operator_connect_timer(oper, tmr) == ESP_OK &&
            mcpwm_new_comparator(oper, &ccfg, &cmpr) == ESP_OK &&
            mcpwm_comparator_set_compare_value(cmpr, period_ticks / 2) == ESP_OK &&
            mcpwm_new_generator(oper, &gcfg, &gen) == ESP_OK)
        {
            // High on timer empty (TEZ) going up, low on compare match.
            mcpwm_gen_timer_event_action_t tea_high =
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                             MCPWM_TIMER_EVENT_EMPTY,
                                             MCPWM_GEN_ACTION_HIGH);
            mcpwm_gen_compare_event_action_t cea_low =
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                               cmpr,
                                               MCPWM_GEN_ACTION_LOW);

            if (mcpwm_generator_set_action_on_timer_event(gen, tea_high) == ESP_OK &&
                mcpwm_generator_set_action_on_compare_event(gen, cea_low) == ESP_OK &&
                mcpwm_timer_enable(tmr) == ESP_OK &&
                mcpwm_timer_start_stop(tmr, MCPWM_TIMER_START_NO_STOP) == ESP_OK)
            {
                s_state.mcpwm_timer = tmr;
                s_state.mcpwm_oper  = oper;
                s_state.mcpwm_cmpr  = cmpr;
                s_state.mcpwm_gen   = gen;
                actual_hz  = 160000000u / period_ticks;
                backend_ok = true;
            } else {
                // Partial teardown of what was created.
                if (gen)  mcpwm_del_generator(gen);
                if (cmpr) mcpwm_del_comparator(cmpr);
                if (oper) mcpwm_del_operator(oper);
                if (tmr)  mcpwm_del_timer(tmr);
            }
        } else {
            if (gen)  mcpwm_del_generator(gen);
            if (cmpr) mcpwm_del_comparator(cmpr);
            if (oper) mcpwm_del_operator(oper);
            if (tmr)  mcpwm_del_timer(tmr);
        }
    }

    if (!backend_ok) {
        // Roll back MUX and OE.
        adgs_set_api_switch_safe(dev, sw, false);
        pin_write(PIN_LSHIFT_OE, prior_oe);
        snprintf(err, errlen, "peripheral configuration failed");
        return false;
    }

    // 9. Populate state.
    s_state.active         = true;
    s_state.io             = io;
    s_state.gpio           = gpin;
    s_state.src            = src;
    s_state.req_hz         = hz;
    s_state.actual_hz      = actual_hz;
    s_state.adgs_dev       = dev;
    s_state.adgs_sw        = sw;
    s_state.prior_lshift_oe = prior_oe;

    return true;
}

// ---------------------------------------------------------------------------
// clkgen_stop — idempotent.
// ---------------------------------------------------------------------------
bool clkgen_stop(void)
{
    if (!s_state.active) return true;

    // Tear down peripheral.
    if (s_state.src == CLKSRC_LEDC) {
        ledc_teardown();
    } else {
        mcpwm_teardown();
    }

    // Reset GPIO to floating input.
    gpio_reset_pin(s_state.gpio);
    gpio_set_direction(s_state.gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(s_state.gpio, GPIO_FLOATING);

    // Reopen MUX switch.
    adgs_set_api_switch_safe(s_state.adgs_dev, s_state.adgs_sw, false);

    // Restore level shifter OE.
    pin_write(PIN_LSHIFT_OE, s_state.prior_lshift_oe);

    memset(&s_state, 0, sizeof(s_state));
    return true;
}

// ---------------------------------------------------------------------------
// clkgen_status — fills caller's pointers; returns true if active.
// ---------------------------------------------------------------------------
bool clkgen_status(bool* active, uint8_t* io, int* gpio,
                   ClkSrc* src, uint32_t* req_hz, uint32_t* actual_hz)
{
    if (active)    *active    = s_state.active;
    if (io)        *io        = s_state.io;
    if (gpio)      *gpio      = (int)s_state.gpio;
    if (src)       *src       = s_state.src;
    if (req_hz)    *req_hz    = s_state.req_hz;
    if (actual_hz) *actual_hz = s_state.actual_hz;
    return s_state.active;
}
