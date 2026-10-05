"""Host execution of the authoritative standby policy and transition ordering."""

from tests.firmware_host.fwhost import compile_and_run

POWER = "Firmware/ESP32/src/power"

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include "standby_policy.h"
#include "standby_wire.h"

static void finish(StandbyPolicy *policy, uint32_t now, StandbyStep step) {
    StandbyStep actual = standby_tick(policy, now, 0);
    if (actual != step) fprintf(stderr, "now=%u state=%d expected=%d actual=%d pending=%d\n",
                               now, policy->state, step, actual, policy->pending);
    assert(actual == step);
    assert(standby_complete(policy, policy->generation, step, true, now));
}

static void sleep_cycle(StandbyPolicy *policy, uint32_t now) {
    finish(policy, now, STANDBY_QUIESCE);
    assert(!standby_ready(policy));
    finish(policy, now, STANDBY_HAT_SLEEP);
    finish(policy, now, STANDBY_MUX_OFF);
    finish(policy, now, STANDBY_OUTPUTS_OFF);
    finish(policy, now, STANDBY_ANALOG_OFF);
    finish(policy, now, STANDBY_INDICATORS_OFF);
    assert(policy->state == STANDBY_ASLEEP);
    assert(!standby_ready(policy));
}

static void wake_cycle(StandbyPolicy *policy, uint32_t now) {
    standby_request_wake(policy, now);
    if (policy->pending) {
        assert(policy->step == STANDBY_WAKE_SAFE);
        assert(standby_complete(policy, policy->generation, STANDBY_WAKE_SAFE, true, now));
    } else {
        finish(policy, now, STANDBY_WAKE_SAFE);
    }
    finish(policy, now, STANDBY_ANALOG_ON);
    finish(policy, now, STANDBY_REINITIALIZE);
    finish(policy, now, STANDBY_HAT_WAKE);
    finish(policy, now, STANDBY_INDICATORS_ON);
    assert(standby_ready(policy));
}

int main(void) {
    StandbyPolicy policy;
    standby_init(&policy, 100, 42);
    assert(policy.timeout_ms == 300000);
    assert(!standby_set_timeout(&policy, 120, 100));
    const uint32_t choices[] = {60, 300, 900};
    for (unsigned index = 0; index < 3; ++index) {
        standby_init(&policy, 100, choices[index]);
        assert(standby_tick(&policy, 100 + choices[index]*1000 - 1, 0) == STANDBY_NO_STEP);
        sleep_cycle(&policy, 100 + choices[index]*1000);
        wake_cycle(&policy, 200 + choices[index]*1000);
    }
    standby_init(&policy, 0, 0);
    assert(standby_tick(&policy, 4000000, 0) == STANDBY_NO_STEP);
    puts("timeouts/default/off/boundary");

    for (unsigned index = 0; index < 32; ++index) {
        standby_init(&policy, 0, 60);
        assert(standby_tick(&policy, 60000, (uint32_t)1 << index) == STANDBY_NO_STEP);
        assert(standby_tick(&policy, 90000, 0) == STANDBY_NO_STEP);
        assert(standby_tick(&policy, 149999, 0) == STANDBY_NO_STEP);
        assert(standby_tick(&policy, 150000, 0) == STANDBY_QUIESCE);
    }
    puts("all-inhibitors/fresh-idle");

    standby_init(&policy, 0xfffffff0u, 60);
    assert(standby_tick(&policy, 0xfffffff0u + 59999u, 0) == STANDBY_NO_STEP);
    sleep_cycle(&policy, 0xfffffff0u + 60000u);
    wake_cycle(&policy, 70000);
    standby_activity(&policy, 80000);
    assert(standby_tick(&policy, 139999, 0) == STANDBY_NO_STEP);
    puts("wraparound/activity");

    // Activity that lands while a tick is blocked (the HAT exchange takes hundreds of ms) is stamped
    // AFTER the tick's own clock reading. That must read as "just now", never as a 49-day idle.
    standby_init(&policy, 0, 300);
    standby_activity(&policy, 5000);
    assert(standby_tick(&policy, 4990, 0) == STANDBY_NO_STEP);
    assert(policy.state == STANDBY_ACTIVE);
    assert(standby_idle_remaining_ms(&policy, 4990) == 300000);
    puts("future-activity-stamp");

    standby_init(&policy, 0, 60);
    assert(!standby_presence(&policy, 0, true, 0));
    for (uint32_t client = 1; client <= STANDBY_MAX_CLIENTS; ++client)
        assert(standby_presence(&policy, client, true, 0));
    assert(!standby_presence(&policy, STANDBY_MAX_CLIENTS + 1, true, 0));
    assert(standby_tick(&policy, STANDBY_CLIENT_TTL_MS - 1, 0) == STANDBY_NO_STEP);
    assert(standby_client_count(&policy) == STANDBY_MAX_CLIENTS);
    assert(standby_tick(&policy, STANDBY_CLIENT_TTL_MS, 0) == STANDBY_NO_STEP);
    assert(standby_client_count(&policy) == 0);
    sleep_cycle(&policy, STANDBY_CLIENT_TTL_MS + 60000);
    assert(standby_presence(&policy, 99, true, 110000));
    assert(policy.state == STANDBY_WAKING);
    assert(!standby_presence(&policy, 77, false, 110000));
    assert(standby_client_count(&policy) == 1);
    wake_cycle(&policy, 110000);
    puts("presence/expiry/capacity/stale-close/host-wake");

    standby_init(&policy, 0, 60);
    assert(standby_tick(&policy, 60000, 0) == STANDBY_QUIESCE);
    uint32_t generation = policy.generation;
    standby_request_wake(&policy, 60001);
    assert(policy.state == STANDBY_PREPARING);
    assert(standby_tick(&policy, 60001, 0) == STANDBY_NO_STEP);
    assert(standby_complete(&policy, generation, STANDBY_QUIESCE, true, 60002));
    assert(policy.state == STANDBY_WAKING);
    assert(policy.generation != generation);
    assert(!standby_complete(&policy, generation, STANDBY_MUX_OFF, true, 60002));
    wake_cycle(&policy, 60002);
    puts("preparation-cancel/late-ack/serialized-recovery");

    standby_init(&policy, 0, 60);
    finish(&policy, 60000, STANDBY_QUIESCE);
    assert(standby_tick(&policy, 60001, 1) == STANDBY_WAKE_SAFE);
    wake_cycle(&policy, 60002);
    puts("new-work-cancels-preparation");

    const StandbyStep failures[] = {STANDBY_QUIESCE, STANDBY_HAT_SLEEP,
        STANDBY_MUX_OFF, STANDBY_OUTPUTS_OFF, STANDBY_ANALOG_OFF,
        STANDBY_INDICATORS_OFF};
    for (unsigned index = 0; index < 6; ++index) {
        standby_init(&policy, 0, 60);
        for (unsigned prior = 0; prior < index; ++prior)
            finish(&policy, 60000, failures[prior]);
        assert(standby_tick(&policy, 60000, 0) == failures[index]);
        assert(standby_complete(&policy, policy.generation, failures[index], false, 60000));
        assert(policy.state == STANDBY_FAULT_SAFE && !standby_ready(&policy));
        assert(policy.failed_step == failures[index]);
        assert(standby_tick(&policy, 60001, 0) == STANDBY_NO_STEP);
    }
    standby_init(&policy, 0, 60);
    assert(standby_tick(&policy, 60000, 0) == STANDBY_QUIESCE);
    assert(standby_tick(&policy, 60000 + STANDBY_STEP_TIMEOUT_MS, 0) == STANDBY_NO_STEP);
    assert(policy.state == STANDBY_FAULT_SAFE);
    assert(!standby_complete(&policy, policy.generation, STANDBY_QUIESCE, true, 70000));
    puts("every-shutdown-failure/timeout");

    standby_init(&policy, 0, 60);
    uint32_t now = 60000;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        sleep_cycle(&policy, now);
        uint32_t before = policy.generation;
        standby_request_wake(&policy, now + 1);
        standby_request_wake(&policy, now + 2);
        assert(policy.generation == before + 1);
        wake_cycle(&policy, now + 3);
        now += 60003;
    }
    puts("repeated-transitions/idempotent-wake/output-off-order");
    return 0;
}
"""


def test_standby_policy_and_order():
    output = compile_and_run(
        MAIN,
        sources=[f"{POWER}/standby_policy.c"],
        include_dirs=[POWER, "Firmware/DAQ_HAT/common"],
        extra_flags=["-Werror"],
    )
    assert output.splitlines() == [
        "timeouts/default/off/boundary",
        "all-inhibitors/fresh-idle",
        "wraparound/activity",
        "future-activity-stamp",
        "presence/expiry/capacity/stale-close/host-wake",
        "preparation-cancel/late-ack/serialized-recovery",
        "new-work-cancels-preparation",
        "every-shutdown-failure/timeout",
        "repeated-transitions/idempotent-wake/output-off-order",
    ]


def test_explicit_sleep_does_not_override_connected_clients():
    output = compile_and_run(
        r"""
#include <assert.h>
#include <stdio.h>
#include "standby_policy.h"
int main(void) {
    StandbyPolicy policy;
    standby_init(&policy, 0, 60);
    assert(standby_presence(&policy, 123, true, 0));
    assert(standby_request_sleep(&policy));
    assert(standby_tick(&policy, 1, 0) == STANDBY_NO_STEP);
    assert(policy.state == STANDBY_ACTIVE);
    puts("connected client inhibits explicit sleep");
    return 0;
}
""",
        sources=[f"{POWER}/standby_policy.c"],
        include_dirs=[POWER],
        extra_flags=["-Werror"],
    )
    assert output.strip() == "connected client inhibits explicit sleep"


def test_resync_starts_a_full_wake_only_from_active():
    output = compile_and_run(
        r"""
#include <assert.h>
#include <stdio.h>
#include "standby_policy.h"
int main(void) {
    StandbyPolicy policy;
    standby_init(&policy, 0, 300);
    assert(standby_timeout_valid(0) && standby_timeout_valid(60) && standby_timeout_valid(300) &&
           standby_timeout_valid(900) && !standby_timeout_valid(120));
    uint32_t gen = policy.generation;
    assert(standby_resync(&policy, 10));                 /* ACTIVE: the wake chain starts */
    assert(policy.state == STANDBY_WAKING && policy.generation != gen && policy.step == STANDBY_WAKE_SAFE);
    assert(!standby_resync(&policy, 11));                /* already in a transaction: no second one */
    assert(standby_tick(&policy, 12, 0) == STANDBY_WAKE_SAFE);
    assert(!standby_resync(&policy, 13));
    puts("resync");
    return 0;
}
""",
        sources=[f"{POWER}/standby_policy.c"],
        include_dirs=[POWER],
        extra_flags=["-Werror"],
    )
    assert output.strip() == "resync"