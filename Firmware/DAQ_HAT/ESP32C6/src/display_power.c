#include "display_power.h"

bool dpower_sleep(const dpower_ops_t *o)
{
    bool ok = true;
    o->set_asleep(o->ctx, true);                 // 1. no new frame can start
    ok = o->wait_flush(o->ctx) && ok;            // 2. the frame in flight has finished
    ok = o->backlight_hard_off(o->ctx) && ok;    // 3. no light - cut even if step 2 timed out
    ok = o->panel_power(o->ctx, false) && ok;    // 4. the panel itself
    return ok;
}

bool dpower_wake(const dpower_ops_t *o)
{
    o->draw_logo(o->ctx);                        // 1. fresh buffer: nothing stale can show
    o->set_asleep(o->ctx, false);                // 2. flushing is allowed again
    // 3./4. the new frame must be IN panel RAM before anything lights up; if it is not,
    // stay dark (a stale UI flash is worse than a retry) and say so.
    if (!o->flush(o->ctx) || !o->wait_flush(o->ctx)) {
        o->set_asleep(o->ctx, true);
        return false;
    }
    if (!o->panel_power(o->ctx, true)) {         // 5. only now switch the panel on
        o->set_asleep(o->ctx, true);
        return false;                            //    (no light on a panel that did not confirm)
    }
    return o->backlight_restore(o->ctx);         // 6. light last, at the saved brightness
}
