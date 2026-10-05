#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STANDBY_MAX_CLIENTS 8u
#define STANDBY_CLIENT_TTL_MS 45000u
#define STANDBY_STEP_TIMEOUT_MS 10000u

typedef enum {
    STANDBY_ACTIVE,
    STANDBY_PREPARING,
    STANDBY_ASLEEP,
    STANDBY_WAKING,
    STANDBY_FAULT_SAFE,
} StandbyState;

typedef enum {
    STANDBY_NO_STEP,
    STANDBY_QUIESCE,
    STANDBY_HAT_SLEEP,
    STANDBY_MUX_OFF,
    STANDBY_OUTPUTS_OFF,
    STANDBY_ANALOG_OFF,
    STANDBY_INDICATORS_OFF,
    STANDBY_WAKE_SAFE,
    STANDBY_ANALOG_ON,
    STANDBY_REINITIALIZE,
    STANDBY_HAT_WAKE,
    STANDBY_INDICATORS_ON,
} StandbyStep;

typedef struct {
    uint32_t id;
    uint32_t refreshed_ms;
} StandbyClient;

typedef struct {
    StandbyState state;
    StandbyStep step;
    StandbyStep failed_step;
    uint32_t generation;
    uint32_t timeout_ms;
    uint32_t idle_since_ms;
    uint32_t step_since_ms;
    uint32_t inhibitors;
    uint32_t completed_steps;
    bool pending;
    bool wake_requested;
    bool was_inhibited;
    bool sleep_now;   /* one-shot explicit request, consumed by the next tick */
    bool forced;      /* the running preparation was explicit: client leases do not cancel it */
    StandbyClient clients[STANDBY_MAX_CLIENTS];
} StandbyPolicy;

void standby_init(StandbyPolicy *policy, uint32_t now_ms, uint32_t timeout_seconds);
bool standby_timeout_valid(uint32_t seconds);
bool standby_set_timeout(StandbyPolicy *policy, uint32_t seconds, uint32_t now_ms);
bool standby_presence(StandbyPolicy *policy, uint32_t client_id, bool present, uint32_t now_ms);
uint8_t standby_client_count(const StandbyPolicy *policy);
void standby_activity(StandbyPolicy *policy, uint32_t now_ms);
/* Ask for sleep at the next tick, ignoring the timeout and client leases (real inhibitors still win). */
bool standby_request_sleep(StandbyPolicy *policy);
void standby_request_wake(StandbyPolicy *policy, uint32_t now_ms);
/* Peer found out of step while ACTIVE (stale HAT, failed boot): start a full wake
 * transaction so every participant is brought back to a known ACTIVE state. */
bool standby_resync(StandbyPolicy *policy, uint32_t now_ms);
bool standby_ready(const StandbyPolicy *policy);
StandbyStep standby_tick(StandbyPolicy *policy, uint32_t now_ms, uint32_t inhibitors);
bool standby_complete(StandbyPolicy *policy, uint32_t generation, StandbyStep step,
                      bool success, uint32_t now_ms);
/* Withdraw a preparation whose first step (QUIESCE) has not touched hardware yet. */
bool standby_cancel_prepare(StandbyPolicy *policy, uint32_t generation, uint32_t now_ms);
/* Time until the idle timer fires; 0 when it is not counting down. */
uint32_t standby_idle_remaining_ms(const StandbyPolicy *policy, uint32_t now_ms);

#ifdef __cplusplus
}
#endif