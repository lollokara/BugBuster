// =============================================================================
// range_manager.c — software-controlled autorange state machine.
//
// The P4 receives edge events from two SR-latch Q outputs (FF_HI on GPIO37,
// FF_MID on GPIO38) and drives the bypass switches (GPIO52, GPIO53) entirely
// in firmware. See range_manager.h for the full design rationale.
//
// ISR safety:
//   All ISR functions are in IRAM (IRAM_ATTR).  They only write a single
//   volatile uint8_t flag and return immediately.  No SPI, no FreeRTOS APIs.
//   Both ISRs and range_manager_step() run on Core 0, so a plain volatile
//   is sufficient — ISRs can only preempt step(), never race with it.
// =============================================================================

#include "range_manager.h"
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "config.h"

static const char *TAG = "range_mgr";

// ---------------------------------------------------------------------------
// ISR handlers (IRAM, minimal work)
// ---------------------------------------------------------------------------

// Per-pin ISR context: pointer back to the range_manager_t plus which bit
// pair to set (UP or DN) depending on the current GPIO level.
typedef struct {
    range_manager_t *rm;
    uint8_t          bit_up;   // flag bit when pin reads HIGH (POSEDGE)
    uint8_t          bit_dn;   // flag bit when pin reads LOW  (NEGEDGE)
    gpio_num_t       pin;
} ff_isr_ctx_t;

static ff_isr_ctx_t s_ctx_hi;   // context for FF_HI  (GPIO37)
static ff_isr_ctx_t s_ctx_mid;  // context for FF_MID (GPIO38)

static void IRAM_ATTR ff_isr(void *arg)
{
    const ff_isr_ctx_t *ctx = (const ff_isr_ctx_t *)arg;
    if (gpio_get_level(ctx->pin)) {
        ctx->rm->isr_flags |= ctx->bit_up;
    } else {
        ctx->rm->isr_flags |= ctx->bit_dn;
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Drive a single bypass pin as a push-pull output at the given level.
static inline void drive_pin(gpio_num_t pin, int level)
{
    if (pin == GPIO_NUM_NC) return;
    gpio_set_level(pin, level);
}

// Apply a 2-bit mux address to the FINE mux address pins.
static void set_mux_addr(range_manager_t *rm, uint8_t addr)
{
    if (rm->mux_a0_pin != GPIO_NUM_NC) gpio_set_level(rm->mux_a0_pin, addr & 1);
    if (rm->mux_a1_pin != GPIO_NUM_NC) gpio_set_level(rm->mux_a1_pin, (addr >> 1) & 1);
}

// Set the bypass GPIOs and FINE mux to reflect a new range, then update the
// state.  Does NOT touch lock/confirm fields — caller manages those.
static void apply_range(range_manager_t *rm, current_range_t r)
{
    int b51 = 0, b2 = 0;
    switch (r) {
        case RANGE_HI:  b51 = 0; b2 = 0; break;   // 51 Ω in circuit
        case RANGE_MID: b51 = 1; b2 = 0; break;   // 51 Ω bypassed, 2 Ω in
        case RANGE_LO:  b51 = 1; b2 = 1; break;   // both bypassed, 50 mΩ
        default: return;                            // ignore UNKNOWN
    }
    drive_pin(rm->bypass51_pin, b51);
    drive_pin(rm->bypass2_pin,  b2);
    range_manager_set_fine_mux(rm, r);

    if (r != rm->current) {
        rm->previous = rm->current;
        rm->current  = r;
        rm->change_count++;
        ESP_LOGD(TAG, "range -> %s", range_manager_name(r));
    }
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

esp_err_t range_manager_init(range_manager_t *rm)
{
    memset(rm, 0, sizeof(*rm));
    rm->bypass51_pin  = RANGE_BYPASS51_PIN;
    rm->bypass2_pin   = RANGE_BYPASS2_PIN;
    rm->mux_a0_pin    = FINE_MUX_A0_PIN;
    rm->mux_a1_pin    = FINE_MUX_A1_PIN;
    rm->mux_en_pin    = MUX_EN_PIN;
    rm->current       = RANGE_UNKNOWN;
    rm->previous      = RANGE_UNKNOWN;
    rm->fine_mux_addr = 0xFF;

    // Default calibration from config constants.
    const float shunts[RANGE_COUNT] = { SHUNT_HI_OHM, SHUNT_MID_OHM, SHUNT_LO_OHM };
    for (int r = 0; r < RANGE_COUNT; ++r) {
        rm->cal[r].shunt_ohm = shunts[r];
        rm->cal[r].amp_gain  = ISENSE_AMP_GAIN;
        rm->cal[r].offset_v  = 0.0f;
        rm->cal[r].gain_corr = 1.0f;
    }

    // ---- Configure bypass outputs (push-pull, P4 is sole driver) ----
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << rm->bypass51_pin) | (1ULL << rm->bypass2_pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    // ---- Configure FINE mux address / enable pins as outputs ----
    if (rm->mux_a0_pin != GPIO_NUM_NC)
        gpio_set_direction(rm->mux_a0_pin, GPIO_MODE_OUTPUT);
    if (rm->mux_a1_pin != GPIO_NUM_NC)
        gpio_set_direction(rm->mux_a1_pin, GPIO_MODE_OUTPUT);
    if (rm->mux_en_pin != GPIO_NUM_NC) {
        gpio_set_direction(rm->mux_en_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(rm->mux_en_pin, 1);   // enable U24 + U25
    }

    // ---- Read initial range from FF GPIO levels (one-time boot read) ----
    // If GPIO37 and GPIO38 are both LOW: both latches are RESET → HI range.
    // If GPIO37 HIGH but GPIO38 LOW: HI latch SET → MID range.
    // If both HIGH: both SET → LO range.
    {
        gpio_set_direction(AR_FF_HI_PIN,  GPIO_MODE_INPUT);
        gpio_set_direction(AR_FF_MID_PIN, GPIO_MODE_INPUT);
        int hi  = gpio_get_level(AR_FF_HI_PIN);
        int mid = gpio_get_level(AR_FF_MID_PIN);
        current_range_t init_r;
        if (!hi && !mid)      init_r = RANGE_HI;
        else if (hi && !mid)  init_r = RANGE_MID;
        else                  init_r = RANGE_LO;
        apply_range(rm, init_r);
        ESP_LOGI(TAG, "initial range = %s (FF_HI=%d FF_MID=%d)",
                 range_manager_name(init_r), hi, mid);
    }

    // ---- Install GPIO ISRs for FF edge events ----
    // gpio_install_isr_service is idempotent; the capture task may call it
    // first on the same core — that is fine.
    gpio_install_isr_service(0);

    s_ctx_hi  = (ff_isr_ctx_t){ rm, AR_ISR_UP_HI,  AR_ISR_DN_HI,  AR_FF_HI_PIN  };
    s_ctx_mid = (ff_isr_ctx_t){ rm, AR_ISR_UP_MID,  AR_ISR_DN_MID, AR_FF_MID_PIN };

    gpio_set_intr_type(AR_FF_HI_PIN,  GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(AR_FF_MID_PIN, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(AR_FF_HI_PIN,  ff_isr, &s_ctx_hi);
    gpio_isr_handler_add(AR_FF_MID_PIN, ff_isr, &s_ctx_mid);

    ESP_LOGI(TAG, "range_manager_init done (ISRs installed on GPIO%d / GPIO%d)",
             AR_FF_HI_PIN, AR_FF_MID_PIN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Per-sample state machine
// ---------------------------------------------------------------------------

current_range_t range_manager_step(range_manager_t *rm)
{
    // Consume ISR flags atomically (ISR can only preempt here; no race).
    uint8_t flags = rm->isr_flags;
    rm->isr_flags = 0;

    // Manual override: run autorange logic but do not act on it.
    if (rm->override_active) {
        return rm->current;
    }

    current_range_t cur = rm->current;

    // ---- RANGE-UP: immediate, highest priority --------------------------------
    // POSEDGE on FF_HI while we are in HI → go to MID.
    if ((flags & AR_ISR_UP_HI) && cur == RANGE_HI) {
        apply_range(rm, RANGE_MID);
        rm->lock_remaining = AR_LOCK_SAMPLES;
        rm->pending_down   = false;
        rm->confirm_count  = 0;
        cur = rm->current;
    }
    // POSEDGE on FF_MID while we are in MID → go to LO.
    if ((flags & AR_ISR_UP_MID) && cur == RANGE_MID) {
        apply_range(rm, RANGE_LO);
        rm->lock_remaining = AR_LOCK_SAMPLES;
        rm->pending_down   = false;
        rm->confirm_count  = 0;
        cur = rm->current;
    }
    // Edge case: if we are in HI and FF_MID fires (stale latch state at boot),
    // honour it as an HI→MID→LO double-step.
    if ((flags & AR_ISR_UP_MID) && cur == RANGE_HI) {
        apply_range(rm, RANGE_MID);
        apply_range(rm, RANGE_LO);
        rm->lock_remaining = AR_LOCK_SAMPLES;
        rm->pending_down   = false;
        rm->confirm_count  = 0;
        cur = rm->current;
    }

    // ---- Lock-out timer -------------------------------------------------------
    if (rm->lock_remaining > 0) {
        rm->lock_remaining--;
        return cur;
    }

    // ---- RANGE-DOWN: deferred with confirmation -------------------------------
    // Poll the FF pin's LIVE LEVEL every step rather than relying on ISR edge
    // flags. Edge-only tracking has two failure modes with a bursty load:
    //   1. If the pin sits steady-low for a long stretch with no fresh edge,
    //      pending_down never (re-)arms once cancelled — it can get stuck in
    //      the coarser range indefinitely even during a long quiet period.
    //   2. Any burst that pops the pin back high (crossing the *reset*
    //      threshold, which is much lower than the original SET threshold)
    //      hits the "cancel" path and wipes confirm_count to 0 — frequent
    //      mild bursts then never let 8 consecutive quiet samples accumulate.
    // Level polling fixes (1) directly. For (2), decay confirm_count instead
    // of zeroing it (leaky bucket) so an occasional burst costs one sample of
    // progress rather than erasing it all, and it still converges downward
    // between bursts instead of getting permanently parked.
    gpio_num_t dn_pin = (cur == RANGE_MID) ? AR_FF_HI_PIN :
                        (cur == RANGE_LO)  ? AR_FF_MID_PIN : GPIO_NUM_NC;

    if (dn_pin != GPIO_NUM_NC) {
        if (!gpio_get_level(dn_pin)) {
            rm->pending_down = true;
            if (rm->confirm_count < AR_CONFIRM_SAMPLES) rm->confirm_count++;
        } else if (rm->confirm_count > 0) {
            rm->confirm_count--;
        } else {
            rm->pending_down = false;
        }
    }

    if (rm->pending_down && rm->confirm_count >= AR_CONFIRM_SAMPLES) {
        // Confirmed: switch to the finer range.
        rm->pending_down  = false;
        rm->confirm_count = 0;
        // After a downrange switch, use half the lock window so a brief
        // upward glitch doesn't trigger another full-length confirm cycle.
        rm->lock_remaining = AR_LOCK_SAMPLES / 2;
        if (cur == RANGE_MID) {
            apply_range(rm, RANGE_HI);
        } else if (cur == RANGE_LO) {
            apply_range(rm, RANGE_MID);
        }
        cur = rm->current;
    }

    return cur;
}

// ---------------------------------------------------------------------------
// Lightweight getter (backward compat; slow-path callers)
// ---------------------------------------------------------------------------

current_range_t range_manager_poll(range_manager_t *rm)
{
    // In the new model the P4 owns the range state; no GPIO read needed.
    // Callers that previously relied on poll() to observe the analog latch
    // should migrate to range_manager_step() in the fast path.  For the slow
    // path (daq_board_process_step, status reporting) a cached read is correct.
    return rm->current;
}

current_range_t range_manager_current(const range_manager_t *rm)
{
    return rm->current;
}

bool range_manager_changed(range_manager_t *rm)
{
    return rm->current != rm->previous;
}

bool range_manager_in_transition(const range_manager_t *rm)
{
    return rm->current == RANGE_UNKNOWN;
}

bool range_manager_settling(const range_manager_t *rm)
{
    return rm->lock_remaining > 0;
}

// ---------------------------------------------------------------------------
// Manual override (CLI, calibration routines)
// ---------------------------------------------------------------------------

esp_err_t range_manager_force(range_manager_t *rm, current_range_t range)
{
    if (range == RANGE_UNKNOWN) {
        // Release override: autorange resumes from the current bypass state.
        rm->override_active = false;
        rm->lock_remaining  = AR_LOCK_SAMPLES;   // brief lock so step settles
        rm->pending_down    = false;
        rm->confirm_count   = 0;
        ESP_LOGI(TAG, "range_manager_force: released (autorange resumed)");
        return ESP_OK;
    }
    if (range >= RANGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    apply_range(rm, range);
    rm->override_active = true;
    rm->pending_down    = false;
    rm->confirm_count   = 0;
    ESP_LOGI(TAG, "range_manager_force: forced to %s", range_manager_name(range));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// FINE mux
// ---------------------------------------------------------------------------

esp_err_t range_manager_set_fine_mux(range_manager_t *rm, current_range_t range)
{
    uint8_t addr;
    if (range == RANGE_HI) {
        addr = FINE_MUX_ADDR_HI;
    } else if (range == RANGE_MID) {
        addr = FINE_MUX_ADDR_MID;
    } else {
        return ESP_OK;   // LO is measured by COARSE; mux setting is don't-care
    }
    if (rm->fine_mux_addr == addr) {
        return ESP_OK;   // already correct — skip redundant GPIO write
    }
    set_mux_addr(rm, addr);
    rm->fine_mux_addr = addr;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Calibration & conversion
// ---------------------------------------------------------------------------

float range_manager_volts_to_amps(const range_manager_t *rm,
                                   current_range_t range, float v_adc)
{
    if (range >= RANGE_COUNT) return 0.0f;
    const range_cal_t *c = &rm->cal[range];
    float denom = c->shunt_ohm * c->amp_gain;
    if (denom == 0.0f) return 0.0f;
    return ((v_adc - c->offset_v) / denom) * c->gain_corr;
}

bool range_uses_coarse(current_range_t range)
{
    return range == RANGE_LO;
}

void range_manager_set_cal(range_manager_t *rm, current_range_t range,
                           const range_cal_t *cal)
{
    if (range < RANGE_COUNT) rm->cal[range] = *cal;
}

const range_cal_t *range_manager_get_cal(const range_manager_t *rm,
                                         current_range_t range)
{
    return (range < RANGE_COUNT) ? &rm->cal[range] : NULL;
}

float range_manager_shunt_ohm(const range_manager_t *rm, current_range_t range)
{
    return (range < RANGE_COUNT) ? rm->cal[range].shunt_ohm : 0.0f;
}

const char *range_manager_name(current_range_t range)
{
    switch (range) {
        case RANGE_HI:  return "HI(51R)";
        case RANGE_MID: return "MID(2R)";
        case RANGE_LO:  return "LO(50mR)";
        default:        return "UNKNOWN";
    }
}
