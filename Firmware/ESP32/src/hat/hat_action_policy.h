// Timing policy for DAQ HAT config-registry requests relayed by the S3.
//
// Plain reads/writes (CONFIG_GET/SET/...) answer in a few ms and keep the tight
// 300 ms budget. CONFIG_ACTION (0x74) runs the action to completion on the P4
// before it replies: battery-sim LOAD / NEW / UNLOAD / DELETE / REOPEN touch
// many LittleFS files (one per simulated day) under the battsim lock, so a big
// run takes well over 300 ms. Such a request gets a long budget and must never
// be re-sent on timeout (the actions are not idempotent and the P4 is busy, not
// gone). Pure header so the host tests can compile it.
#pragma once
#include <stdint.h>

#define HAT_CMD_DAQ_CONFIG_ACTION   0x74u
#define HAT_REQ_DEFAULT_TIMEOUT_MS  300u
#define HAT_ACTION_TIMEOUT_MS       20000u

static inline uint32_t hat_request_timeout_ms(uint8_t cmd, uint32_t requested_ms)
{
    if (cmd == HAT_CMD_DAQ_CONFIG_ACTION && requested_ms < HAT_ACTION_TIMEOUT_MS)
        return HAT_ACTION_TIMEOUT_MS;
    return requested_ms;
}

static inline int hat_request_may_retry(uint8_t cmd)
{
    return cmd != HAT_CMD_DAQ_CONFIG_ACTION;
}
