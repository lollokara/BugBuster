#include "standby_rails_core.h"

void sb_rails_init(sb_rails_t *r, bool powered)
{
    r->cut = false;
    r->dirty = false;
    r->powered = powered;
    r->mux_open = false;
    r->mux_verified = false;
    r->fail_mask = 0;
}

static void note(sb_rails_t *r, bool ok, uint32_t bit)
{
    if (!ok) r->fail_mask |= bit;
}

static void rails_down(sb_rails_t *r, const sb_rails_ops_t *o)
{
    note(r, o->rail_set(o->ctx, SB_RAIL_24V, false), SB_RF_RAIL_24V);
    o->delay_us(o->ctx, SB_RAIL_GAP_US);
    note(r, o->rail_set(o->ctx, SB_RAIL_26V, false), SB_RF_RAIL_26V);
    o->delay_us(o->ctx, SB_RAIL_GAP_US);
    note(r, o->rail_set(o->ctx, SB_RAIL_3V3, false), SB_RF_RAIL_3V3);
}

static void isolate(sb_rails_t *r, const sb_rails_ops_t *o)
{
    note(r, o->adc_gate(o->ctx, SB_GATE_CLOSED), SB_RF_GATE);
    note(r, o->adaq_reset(o->ctx, true), SB_RF_RESET);
    note(r, o->bus_release(o->ctx), SB_RF_BUS);
}

bool sb_rails_mux_off(sb_rails_t *r, const sb_rails_ops_t *o)
{
    r->fail_mask = 0;
    r->mux_open = true;          // from the first write on, the routes are held
    r->mux_verified = false;
    note(r, o->mux_disconnect(o->ctx), SB_RF_MUX);
    note(r, o->bypass_park(o->ctx), SB_RF_BYPASS);
    note(r, o->mux_verify(o->ctx), SB_RF_MUX_VERIFY);
    r->mux_verified = r->fail_mask == 0u;
    return r->mux_verified;
}

bool sb_rails_mux_connect(sb_rails_t *r, const sb_rails_ops_t *o)
{
    if (!r->mux_open) return true;               // never disconnected: already the board's state
    r->fail_mask = 0;
    if (!r->powered || r->cut) {
        r->fail_mask |= SB_RF_NOT_POWERED;       // the converters are not back: nothing to measure with
        return false;
    }
    if (!o->mux_connect(o->ctx)) {
        r->fail_mask |= SB_RF_MUX;
        note(r, o->mux_disconnect(o->ctx), SB_RF_MUX);   // a half-closed route is worse than none
        note(r, o->bypass_park(o->ctx), SB_RF_BYPASS);
        r->mux_verified = o->mux_verify(o->ctx);
        return false;
    }
    r->mux_open = false;
    r->mux_verified = false;
    return true;
}

bool sb_rails_off(sb_rails_t *r, const sb_rails_ops_t *o)
{
    r->fail_mask = 0;
    // GUARD: not one rail, converter or pin moves unless the analog path is proven
    // disconnected right now. A stage-3 result alone is not enough - it is re-read.
    if (!r->mux_verified || !o->mux_verify(o->ctx)) {
        r->mux_verified = false;
        r->fail_mask |= SB_RF_MUX_GUARD;
        return false;
    }
    r->cut = true;        // from here the converters may lose their configuration
    r->dirty = true;

    isolate(r, o);
    rails_down(r, o);

    bool low = false;
    for (uint32_t ms = 0; ms < SB_PG_OFF_MS; ++ms) {
        const int pg = o->pg_read(o->ctx);
        if (pg < 0) { r->fail_mask |= SB_RF_PG_READ; low = true; break; }   // unknown: reported, not retried
        if (pg == 0) { low = true; break; }
        o->delay_us(o->ctx, 1000u);
    }
    if (!low) r->fail_mask |= SB_RF_PG_STUCK;

    r->powered = false;
    r->dirty = r->fail_mask != 0u;
    return r->fail_mask == 0u;
}

static bool fail_on(sb_rails_t *r, const sb_rails_ops_t *o)
{
    // A failed power-on never leaves half the rails up.
    isolate(r, o);
    rails_down(r, o);
    r->powered = false;
    r->dirty = true;
    return false;
}

static bool superseded(sb_rails_t *r, const sb_rails_ops_t *o)
{
    if (!o->cancelled || !o->cancelled(o->ctx)) return false;
    r->fail_mask |= SB_RF_CANCELLED;
    return true;
}

bool sb_rails_on(sb_rails_t *r, const sb_rails_ops_t *o)
{
    if (r->powered && !r->dirty) return true;   // already up and consistent: a repeated stage is a no-op
    r->fail_mask = 0;

    if (r->dirty) {
        // An earlier sequence stopped half way, so the rails are not in a state we
        // can vouch for: take everything down, let it discharge, then start over.
        isolate(r, o);
        rails_down(r, o);
        for (uint32_t ms = 0; ms < SB_RAIL_DRAIN_MS; ++ms) o->delay_us(o->ctx, 1000u);
        r->fail_mask = 0;
    }
    r->dirty = true;
    note(r, o->adaq_reset(o->ctx, true), SB_RF_RESET);       // *RST low before any supply rises

    if (superseded(r, o)) return fail_on(r, o);
    if (!o->rail_set(o->ctx, SB_RAIL_3V3, true)) {
        r->fail_mask |= SB_RF_RAIL_3V3;
        return fail_on(r, o);
    }
    bool good = false;
    for (uint32_t ms = 0; ms < SB_PG_ON_MS; ++ms) {
        const int pg = o->pg_read(o->ctx);
        if (pg < 0) { r->fail_mask |= SB_RF_PG_READ; return fail_on(r, o); }
        if (pg > 0) { good = true; break; }
        o->delay_us(o->ctx, 1000u);
    }
    if (!good) {
        r->fail_mask |= SB_RF_PG_TIMEOUT;
        return fail_on(r, o);
    }

    o->delay_us(o->ctx, 1000u);
    if (superseded(r, o)) return fail_on(r, o);
    if (!o->rail_set(o->ctx, SB_RAIL_26V, true)) {
        r->fail_mask |= SB_RF_RAIL_26V;
        return fail_on(r, o);
    }
    o->delay_us(o->ctx, 2000u);
    if (superseded(r, o)) return fail_on(r, o);
    if (!o->rail_set(o->ctx, SB_RAIL_24V, true)) {
        r->fail_mask |= SB_RF_RAIL_24V;
        return fail_on(r, o);
    }
    for (uint32_t ms = 0; ms < SB_RAIL_SETTLE_MS; ++ms) o->delay_us(o->ctx, 1000u);

    if (superseded(r, o)) return fail_on(r, o);
    if (!o->bus_restore(o->ctx)) {
        r->fail_mask |= SB_RF_BUS;
        return fail_on(r, o);
    }
    if (!o->adaq_pulse(o->ctx)) {
        r->fail_mask |= SB_RF_PULSE;
        return fail_on(r, o);
    }
    r->powered = true;
    r->dirty = false;       // rails are up and consistent; r->cut stays until the config is restored
    return true;
}
