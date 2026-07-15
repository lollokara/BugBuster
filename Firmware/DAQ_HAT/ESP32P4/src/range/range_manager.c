// =============================================================================
// range_manager.c — current-range observation, override, FINE mux control and
// per-range calibration (verified-hardware model).
// =============================================================================

#include "range_manager.h"
#include <string.h>
#include "esp_log.h"
#include "config.h"

static const char *TAG = "range_mgr";

// Decode the two bypass-control lines into a range.
//   (bypass51, bypass2): (0,0)=HI, (1,0)=MID, (1,1)=LO, (0,1)=transition.
static current_range_t decode_bypass(int bypass51, int bypass2)
{
    if (!bypass51 && !bypass2) return RANGE_HI;
    if (bypass51 && !bypass2)  return RANGE_MID;
    if (bypass51 && bypass2)   return RANGE_LO;
    return RANGE_UNKNOWN;   // (0,1) — mid-transition
}

// Drive a single bypass pin: configure as output and set the level.
static void drive_pin(gpio_num_t pin, int level)
{
    if (pin == GPIO_NUM_NC) return;
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

// Return a bypass pin to high-Z input (observe mode / release override).
static void release_pin(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC) return;
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

// Apply a 2-bit mux address to the FINE mux address pins.
static void set_mux_addr(range_manager_t *rm, uint8_t addr)
{
    if (rm->mux_a0_pin != GPIO_NUM_NC) gpio_set_level(rm->mux_a0_pin, addr & 1);
    if (rm->mux_a1_pin != GPIO_NUM_NC) gpio_set_level(rm->mux_a1_pin, (addr >> 1) & 1);
}

esp_err_t range_manager_init(range_manager_t *rm)
{
    memset(rm, 0, sizeof(*rm));
    rm->bypass51_pin = RANGE_BYPASS51_PIN;
    rm->bypass2_pin  = RANGE_BYPASS2_PIN;
    rm->mux_a0_pin   = FINE_MUX_A0_PIN;
    rm->mux_a1_pin   = FINE_MUX_A1_PIN;
    rm->mux_en_pin   = MUX_EN_PIN;
    rm->current      = RANGE_UNKNOWN;
    rm->previous     = RANGE_UNKNOWN;
    rm->fine_mux_addr = 0xFF;   // unknown -> first set_fine_mux always writes

    // Default calibration from config constants.
    const float shunts[RANGE_COUNT] = { SHUNT_HI_OHM, SHUNT_MID_OHM, SHUNT_LO_OHM };
    for (int r = 0; r < RANGE_COUNT; ++r) {
        rm->cal[r].shunt_ohm = shunts[r];
        rm->cal[r].amp_gain  = ISENSE_AMP_GAIN;
        rm->cal[r].offset_v  = 0.0f;
        rm->cal[r].gain_corr = 1.0f;
    }

    // Bypass lines start as high-Z inputs (observe mode; analog loop drives).
    release_pin(rm->bypass51_pin);
    release_pin(rm->bypass2_pin);

    // FINE mux address pins as outputs; enable the mux.
    if (rm->mux_a0_pin != GPIO_NUM_NC) gpio_set_direction(rm->mux_a0_pin, GPIO_MODE_OUTPUT);
    if (rm->mux_a1_pin != GPIO_NUM_NC) gpio_set_direction(rm->mux_a1_pin, GPIO_MODE_OUTPUT);
    if (rm->mux_en_pin != GPIO_NUM_NC) {
        gpio_set_direction(rm->mux_en_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(rm->mux_en_pin, 1);   // enable U24 + U25
    }

    range_manager_poll(rm);
    ESP_LOGI(TAG, "range manager init, observed range = %s",
             range_manager_name(rm->current));
    return ESP_OK;
}

esp_err_t range_manager_set_fine_mux(range_manager_t *rm, current_range_t range)
{
    uint8_t addr;
    if (range == RANGE_HI) {
        addr = FINE_MUX_ADDR_HI;
    } else if (range == RANGE_MID) {
        addr = FINE_MUX_ADDR_MID;
    } else {
        return ESP_OK;   // LO is read by COARSE; mux is don't-care
    }
    if (rm->fine_mux_addr == addr) {
        return ESP_OK;   // already pointed there — skip the redundant GPIO write
    }
    set_mux_addr(rm, addr);
    rm->fine_mux_addr = addr;
    return ESP_OK;
}

current_range_t range_manager_poll(range_manager_t *rm)
{
    current_range_t r;
    if (rm->override_active) {
        // The P4 is driving the bypass lines and has right of way (the analog
        // latch is tapped through 1k, so a forced range holds). A push-pull
        // output pin does NOT reliably read back its own driven level, so
        // decoding gpio_get_level here would misread a forced MID/LO as HI and
        // strand the FINE mux on the wrong CSA (large phantom current). Trust
        // the forced range instead.
        r = rm->current;
    } else {
        // Autorange: lines are high-Z inputs; the latch drives them, so the
        // pin read is the true hardware-selected range.
        int b51 = (rm->bypass51_pin != GPIO_NUM_NC) ? gpio_get_level(rm->bypass51_pin) : 0;
        int b2  = (rm->bypass2_pin  != GPIO_NUM_NC) ? gpio_get_level(rm->bypass2_pin)  : 0;
        r = decode_bypass(b51, b2);
    }

    if (r != rm->current) {
        rm->previous = rm->current;
        rm->current  = r;
        rm->change_count++;
    }

    // Keep the FINE input mux pointed at the CSA for the active HI/MID range on
    // every poll (set_fine_mux skips redundant GPIO writes via the shadow, so
    // this is cheap in steady state). This tracks both forced and autoranged
    // HI/MID and self-heals any stale mux left by a prior force/release.
    if (r == RANGE_HI || r == RANGE_MID) {
        range_manager_set_fine_mux(rm, r);
    }
    return r;
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

esp_err_t range_manager_force(range_manager_t *rm, current_range_t range)
{
    if (range == RANGE_UNKNOWN) {
        // Release override: bypass lines back to high-Z, analog loop resumes.
        release_pin(rm->bypass51_pin);
        release_pin(rm->bypass2_pin);
        rm->override_active = false;
        return ESP_OK;
    }
    if (range >= RANGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    int b51 = 0, b2 = 0;
    switch (range) {
        case RANGE_HI:  b51 = 0; b2 = 0; break;
        case RANGE_MID: b51 = 1; b2 = 0; break;
        case RANGE_LO:  b51 = 1; b2 = 1; break;
        default: break;
    }
    drive_pin(rm->bypass51_pin, b51);
    drive_pin(rm->bypass2_pin, b2);
    range_manager_set_fine_mux(rm, range);
    rm->override_active = true;
    rm->previous = rm->current;
    rm->current  = range;
    return ESP_OK;
}

float range_manager_volts_to_amps(const range_manager_t *rm,
                                   current_range_t range, float v_adc)
{
    if (range >= RANGE_COUNT) {
        return 0.0f;
    }
    const range_cal_t *c = &rm->cal[range];
    float denom = c->shunt_ohm * c->amp_gain;
    if (denom == 0.0f) {
        return 0.0f;
    }
    return ((v_adc - c->offset_v) / denom) * c->gain_corr;
}

bool range_uses_coarse(current_range_t range)
{
    return range == RANGE_LO;
}

void range_manager_set_cal(range_manager_t *rm, current_range_t range,
                           const range_cal_t *cal)
{
    if (range < RANGE_COUNT) {
        rm->cal[range] = *cal;
    }
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
