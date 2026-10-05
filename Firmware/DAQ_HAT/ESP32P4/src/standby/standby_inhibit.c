#include "standby_inhibit.h"

#include "standby_wire.h"

void sb_ctrl_track_init(sb_ctrl_track_t *t)
{
    t->standby_queued = 0;
    t->standby_busy = false;
    t->user_busy = false;
}

void sb_ctrl_posted(sb_ctrl_track_t *t, bool is_standby)
{
    if (is_standby) t->standby_queued++;
}

void sb_ctrl_taken(sb_ctrl_track_t *t, bool is_standby)
{
    if (is_standby) {
        if (t->standby_queued) t->standby_queued--;
        t->standby_busy = true;
    } else {
        t->user_busy = true;
    }
}

void sb_ctrl_done(sb_ctrl_track_t *t)
{
    t->standby_busy = false;
    t->user_busy = false;
}

uint32_t sb_ctrl_user_queued(const sb_ctrl_track_t *t, uint32_t queue_total)
{
    // The queue total can only be ahead of the standby count by user messages.
    return queue_total > t->standby_queued ? queue_total - t->standby_queued : 0u;
}

bool sb_ctrl_user_work(const sb_ctrl_track_t *t, uint32_t queue_total)
{
    return t->user_busy || sb_ctrl_user_queued(t, queue_total) != 0u;
}

uint32_t sb_inh_compute(const sb_inh_inputs_t *in, const sb_ctrl_track_t *t)
{
    uint32_t r = 0;
    if (in->client_stream) r |= BB_ST_INH_STREAM;
    if (in->trigger_armed) r |= BB_ST_INH_TRIGGER;
    if (in->battsim_owns_supply) r |= BB_ST_INH_BATTSIM;
    if (in->calibration) r |= BB_ST_INH_CALIBRATION;
    if (in->ota) r |= BB_ST_INH_OTA;
    if (in->c6_uart_owner || sb_ctrl_user_work(t, in->ctrl_queue_total)) r |= BB_ST_INH_WORK;
    return r;
}
