#include "standby_policy.h"

#include <string.h>

static bool valid_timeout(uint32_t seconds)
{
    return seconds == 0 || seconds == 60 || seconds == 300 || seconds == 900;
}

bool standby_timeout_valid(uint32_t seconds)
{
    return valid_timeout(seconds);
}

static void next_generation(StandbyPolicy *policy)
{
    if (++policy->generation == 0) ++policy->generation;
}

static void begin_wake(StandbyPolicy *policy, uint32_t now_ms)
{
    next_generation(policy);
    policy->state = STANDBY_WAKING;
    policy->step = STANDBY_WAKE_SAFE;
    policy->completed_steps = 0;
    policy->pending = false;
    policy->wake_requested = false;
    policy->sleep_now = false;
    policy->forced = false;
    policy->idle_since_ms = now_ms;
}

static void fail_step(StandbyPolicy *policy)
{
    policy->failed_step = policy->step;
    policy->state = STANDBY_FAULT_SAFE;
    policy->pending = false;
    policy->step = STANDBY_NO_STEP;
    policy->wake_requested = false;
}

void standby_init(StandbyPolicy *policy, uint32_t now_ms, uint32_t timeout_seconds)
{
    memset(policy, 0, sizeof(*policy));
    policy->state = STANDBY_ACTIVE;
    policy->generation = 1;
    policy->timeout_ms = (valid_timeout(timeout_seconds) ? timeout_seconds : 300) * 1000;
    policy->idle_since_ms = now_ms;
}

bool standby_set_timeout(StandbyPolicy *policy, uint32_t seconds, uint32_t now_ms)
{
    if (!valid_timeout(seconds)) return false;
    policy->timeout_ms = seconds * 1000;
    standby_activity(policy, now_ms);
    return true;
}

uint8_t standby_client_count(const StandbyPolicy *policy)
{
    uint8_t count = 0;
    for (unsigned index = 0; index < STANDBY_MAX_CLIENTS; ++index)
        if (policy->clients[index].id != 0) ++count;
    return count;
}

void standby_request_wake(StandbyPolicy *policy, uint32_t now_ms)
{
    policy->idle_since_ms = now_ms;
    if (policy->state == STANDBY_PREPARING) {
        if (policy->pending) policy->wake_requested = true;
        else begin_wake(policy, now_ms);
    } else if (policy->state == STANDBY_ASLEEP || policy->state == STANDBY_FAULT_SAFE) {
        begin_wake(policy, now_ms);
    }
}

bool standby_resync(StandbyPolicy *policy, uint32_t now_ms)
{
    if (policy->state != STANDBY_ACTIVE) return false;
    begin_wake(policy, now_ms);
    return true;
}

void standby_activity(StandbyPolicy *policy, uint32_t now_ms)
{
    standby_request_wake(policy, now_ms);
}

bool standby_presence(StandbyPolicy *policy, uint32_t client_id, bool present, uint32_t now_ms)
{
    if (client_id == 0) return false;
    StandbyClient *empty = 0;
    for (unsigned index = 0; index < STANDBY_MAX_CLIENTS; ++index) {
        StandbyClient *client = &policy->clients[index];
        if (client->id == client_id) {
            if (!present) {
                client->id = 0;
                policy->idle_since_ms = now_ms;
            } else {
                client->refreshed_ms = now_ms;   /* a lease renewal is not new demand */
            }
            return true;
        }
        if (client->id == 0 && empty == 0) empty = client;
    }
    if (!present || empty == 0) return false;
    empty->id = client_id;
    empty->refreshed_ms = now_ms;
    policy->was_inhibited = true;
    standby_request_wake(policy, now_ms);
    return true;
}

bool standby_request_sleep(StandbyPolicy *policy)
{
    if (policy->state != STANDBY_ACTIVE) return false;
    policy->sleep_now = true;
    return true;
}

bool standby_ready(const StandbyPolicy *policy)
{
    return policy->state == STANDBY_ACTIVE;
}

bool standby_cancel_prepare(StandbyPolicy *policy, uint32_t generation, uint32_t now_ms)
{
    if (policy->state != STANDBY_PREPARING || !policy->pending ||
        policy->step != STANDBY_QUIESCE || policy->generation != generation)
        return false;
    policy->state = STANDBY_ACTIVE;
    policy->step = STANDBY_NO_STEP;
    policy->pending = false;
    policy->wake_requested = false;
    policy->forced = false;
    policy->completed_steps = 0;
    policy->idle_since_ms = now_ms;
    return true;
}

uint32_t standby_idle_remaining_ms(const StandbyPolicy *policy, uint32_t now_ms)
{
    if (policy->state != STANDBY_ACTIVE || policy->timeout_ms == 0 || policy->inhibitors != 0 ||
        standby_client_count(policy) != 0)
        return 0;
    uint32_t elapsed = now_ms - policy->idle_since_ms;
    return elapsed >= policy->timeout_ms ? 0 : policy->timeout_ms - elapsed;
}

StandbyStep standby_tick(StandbyPolicy *policy, uint32_t now_ms, uint32_t inhibitors)
{
    bool expired = false;
    for (unsigned index = 0; index < STANDBY_MAX_CLIENTS; ++index) {
        StandbyClient *client = &policy->clients[index];
        if (client->id != 0 && (uint32_t)(now_ms - client->refreshed_ms) >= STANDBY_CLIENT_TTL_MS) {
            client->id = 0;
            expired = true;
        }
    }
    policy->inhibitors = inhibitors;
    bool inhibited_real = inhibitors != 0;
    bool inhibited = inhibited_real || standby_client_count(policy) != 0;
    if (inhibited || policy->was_inhibited || expired) policy->idle_since_ms = now_ms;
    policy->was_inhibited = inhibited;

    if (policy->pending && (uint32_t)(now_ms - policy->step_since_ms) >= STANDBY_STEP_TIMEOUT_MS) {
        fail_step(policy);
        return STANDBY_NO_STEP;
    }
    if (policy->state == STANDBY_PREPARING && inhibited)
        standby_request_wake(policy, now_ms);
    bool explicit_sleep = policy->sleep_now;
    policy->sleep_now = false;
    bool due = policy->state == STANDBY_ACTIVE &&
               ((explicit_sleep && !inhibited) ||
                (!inhibited && policy->timeout_ms != 0 &&
                 (uint32_t)(now_ms - policy->idle_since_ms) >= policy->timeout_ms));
    if (due) {
        policy->forced = explicit_sleep;
        next_generation(policy);
        policy->state = STANDBY_PREPARING;
        policy->step = STANDBY_QUIESCE;
        policy->completed_steps = 0;
        policy->wake_requested = false;
    }
    if ((policy->state != STANDBY_PREPARING && policy->state != STANDBY_WAKING) || policy->pending)
        return STANDBY_NO_STEP;
    policy->pending = true;
    policy->step_since_ms = now_ms;
    return policy->step;
}

bool standby_complete(StandbyPolicy *policy, uint32_t generation, StandbyStep step,
                      bool success, uint32_t now_ms)
{
    if (!policy->pending || policy->generation != generation || policy->step != step) return false;
    if (!success) {
        fail_step(policy);
        return true;
    }
    policy->pending = false;
    policy->completed_steps |= (uint32_t)1 << step;
    if (policy->state == STANDBY_PREPARING && policy->wake_requested) {
        begin_wake(policy, now_ms);
    } else if (step == STANDBY_INDICATORS_OFF) {
        policy->state = STANDBY_ASLEEP;
        policy->step = STANDBY_NO_STEP;
    } else if (step == STANDBY_INDICATORS_ON) {
        policy->state = STANDBY_ACTIVE;
        policy->step = STANDBY_NO_STEP;
        policy->idle_since_ms = now_ms;
    } else {
        policy->step = (StandbyStep)(step + 1);
    }
    return true;
}