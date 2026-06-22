// =============================================================================
// daq_trigger.cpp — DAQ trigger / flag event engine (ESP32-S3 mainboard side).
//
// See daq_trigger.h for the model. Edge detection is purely level-based so the
// same logic serves digital GPIO levels, AD74416H DIN comparator bits, and
// analog-threshold crossings: each source resolves to a boolean "level" and an
// edge is a level transition that matches the IO's configured edge.
// =============================================================================

#include "daq_trigger.h"
#include "hat.h"
#include "esp_log.h"

static const char *TAG = "daq_trig";

// Per-IO runtime state (index 0 = IO 1 .. index 11 = IO 12).
typedef struct {
    daq_trig_io_cfg_t cfg;
    bool    have_level;   // a level has been sampled at least once
    bool    level;        // last logical level (digital high / analog above thr)
    bool    trig_latched; // this trigger IO has matched since the last arm (AND)
} trig_io_t;

static trig_io_t s_io[DIO_NUM_IOS];
static uint8_t   s_logic     = DAQ_TRIG_LOGIC_OR;
static bool      s_armed     = false;
static bool      s_fired     = false;
static uint32_t  s_pre_samples = 0;

static inline bool valid_io(uint8_t io) {
    return io >= DIO_FIRST_IO && io <= DIO_LAST_IO;
}
static inline int idx(uint8_t io) { return io - DIO_FIRST_IO; }

bool daq_trigger_io_is_analog_capable(uint8_t io) {
    // Position 3 of each connector block: IO 3, 6, 9, 12 -> AD74416H A..D.
    return (io == 3 || io == 6 || io == 9 || io == 12);
}

void daq_trigger_init(void) {
    for (int i = 0; i < DIO_NUM_IOS; i++) {
        s_io[i].cfg.role        = DAQ_TRIG_ROLE_OFF;
        s_io[i].cfg.edge        = DAQ_TRIG_EDGE_RISING;
        s_io[i].cfg.source      = DAQ_TRIG_SRC_DIGITAL;
        s_io[i].cfg._pad        = 0;
        s_io[i].cfg.threshold_v = 1.5f;
        s_io[i].have_level      = false;
        s_io[i].level           = false;
        s_io[i].trig_latched    = false;
    }
    s_logic       = DAQ_TRIG_LOGIC_OR;
    s_armed       = false;
    s_fired       = false;
    s_pre_samples = 0;
}

void daq_trigger_set_logic(uint8_t logic) {
    s_logic = (logic <= DAQ_TRIG_LOGIC_AND) ? logic : DAQ_TRIG_LOGIC_OR;
}
uint8_t daq_trigger_get_logic(void) { return s_logic; }

bool daq_trigger_set_io(uint8_t io, const daq_trig_io_cfg_t *cfg) {
    if (!valid_io(io) || cfg == nullptr) return false;
    trig_io_t *t = &s_io[idx(io)];
    t->cfg = *cfg;
    // Analog source is only meaningful on analog-capable IOs.
    if (t->cfg.source == DAQ_TRIG_SRC_ANALOG && !daq_trigger_io_is_analog_capable(io)) {
        t->cfg.source = DAQ_TRIG_SRC_DIGITAL;
    }
    // Reconfiguring an IO clears its edge history + latch.
    t->have_level   = false;
    t->trig_latched = false;
    ESP_LOGI(TAG, "IO%u role=%u edge=%u src=%u thr=%.3fV", io,
             t->cfg.role, t->cfg.edge, t->cfg.source, t->cfg.threshold_v);
    return true;
}

bool daq_trigger_get_io(uint8_t io, daq_trig_io_cfg_t *out) {
    if (!valid_io(io) || out == nullptr) return false;
    *out = s_io[idx(io)].cfg;
    return true;
}

// Count configured trigger IOs (for AND completion).
static uint8_t count_trigger_ios(void) {
    uint8_t n = 0;
    for (int i = 0; i < DIO_NUM_IOS; i++) {
        if (s_io[i].cfg.role == DAQ_TRIG_ROLE_TRIGGER) n++;
    }
    return n;
}

void daq_trigger_arm(bool armed, uint32_t pre_samples) {
    s_armed       = armed;
    s_fired       = false;
    s_pre_samples = pre_samples;
    for (int i = 0; i < DIO_NUM_IOS; i++) {
        s_io[i].trig_latched = false;
        s_io[i].have_level   = false;  // re-baseline so we never fire on a stale edge
    }
    hat_daq_send_arm(armed, s_logic, pre_samples);
    ESP_LOGI(TAG, "arm=%d logic=%u pre=%u trig_ios=%u",
             armed, s_logic, (unsigned)pre_samples, count_trigger_ios());
}

bool daq_trigger_is_armed(void)  { return s_armed; }
bool daq_trigger_has_fired(void) { return s_fired; }

// Does @p edge_rising satisfy the IO's configured edge?
static inline bool edge_matches(uint8_t cfg_edge, bool rising) {
    switch (cfg_edge) {
        case DAQ_TRIG_EDGE_RISING:  return rising;
        case DAQ_TRIG_EDGE_FALLING: return !rising;
        case DAQ_TRIG_EDGE_ANY:     return true;
        default:                    return false;
    }
}

// Evaluate the trigger group; fire (and emit a TRIGGER marker) if satisfied.
static void maybe_fire_trigger(uint8_t io, bool rising) {
    if (!s_armed || s_fired) return;

    if (s_logic == DAQ_TRIG_LOGIC_AND) {
        // All configured trigger IOs must have matched at least once.
        for (int i = 0; i < DIO_NUM_IOS; i++) {
            if (s_io[i].cfg.role == DAQ_TRIG_ROLE_TRIGGER && !s_io[i].trig_latched) {
                return;  // still waiting on others
            }
        }
    }
    // OR (or satisfied AND): fire now on this IO's edge.
    s_fired = true;
    hat_daq_send_mark(io, rising ? 1u : 0u, HAT_DAQ_MARK_KIND_TRIGGER);
    ESP_LOGI(TAG, "TRIGGER fired on IO%u (%s)", io, rising ? "rising" : "falling");
}

// Core per-IO level update shared by digital and analog sources.
static void update_io_level(uint8_t io, bool level) {
    trig_io_t *t = &s_io[idx(io)];
    if (t->cfg.role == DAQ_TRIG_ROLE_OFF) { t->have_level = false; return; }

    if (!t->have_level) {
        t->level      = level;
        t->have_level = true;
        return;  // baseline only — no edge on the first sample
    }
    if (level == t->level) return;  // no transition

    bool rising = (!t->level && level);
    t->level = level;

    if (!edge_matches(t->cfg.edge, rising)) return;

    if (t->cfg.role == DAQ_TRIG_ROLE_FLAG) {
        hat_daq_send_mark(io, rising ? 1u : 0u, HAT_DAQ_MARK_KIND_FLAG);
    } else if (t->cfg.role == DAQ_TRIG_ROLE_TRIGGER) {
        t->trig_latched = true;
        maybe_fire_trigger(io, rising);
    }
}

void daq_trigger_poll_digital(const DioState *all_dio) {
    if (all_dio == nullptr) return;
    for (int i = 0; i < DIO_NUM_IOS; i++) {
        uint8_t io = (uint8_t)(i + DIO_FIRST_IO);
        const trig_io_t *t = &s_io[i];
        if (t->cfg.role == DAQ_TRIG_ROLE_OFF) continue;
        if (t->cfg.source == DAQ_TRIG_SRC_ANALOG) continue;  // fed separately
        update_io_level(io, all_dio[i].input_level);
    }
}

void daq_trigger_feed_analog(uint8_t io, float volts) {
    if (!valid_io(io)) return;
    trig_io_t *t = &s_io[idx(io)];
    if (t->cfg.role == DAQ_TRIG_ROLE_OFF) return;
    if (t->cfg.source != DAQ_TRIG_SRC_ANALOG) return;
    update_io_level(io, volts >= t->cfg.threshold_v);
}
