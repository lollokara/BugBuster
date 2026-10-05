#pragma once
// =============================================================================
// standby_hw.h - runtime of the S3 standby coordinator (binds standby_system.c
// to the real drivers; see standby_hw.cpp) and the hooks the rest of the
// firmware calls. Every gate here is OPEN until standby_runtime_init() has run,
// so boot order never blocks on it.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "standby_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the lock, load the persisted timeout and start the coordinator. */
void standby_runtime_init(void);
/* From the main-loop task, every iteration. All hardware steps run here. */
void standby_runtime_tick(void);
/* NULL until standby_runtime_init(). */
StandbySystem *standby_runtime(void);

/* Operation barrier (see standby_system.h). Before init: always OK. */
StandbyAdmit standby_hw_admit(void);
void standby_hw_leave(void);
/* Worker iteration bracket for anything that touches analog hardware. */
bool standby_hw_analog_enter(void);
void standby_hw_analog_leave(void);
/* SPI bus gate for the AD74416H / ADGS drivers: open when ACTIVE or when the
 * caller is the coordinator task. */
bool standby_hw_bus_gate_open(void);
/* True while the coordinator is deliberately changing rails (suppress the
 * resulting power-good alerts; real faults still report). */
bool standby_hw_power_transition(void);
/* Meaningful user activity that is not an operation (CLI keystroke, button). */
void standby_hw_activity(void);
/* Real boot milestone (BB_ST_BOOT_*) for the HAT loading screen. */
void standby_hw_boot_milestone(uint16_t completed, uint16_t failed, uint16_t skipped);
/* One HAT-mailbox policy request (C6 menu). 0xFFFF = query only.
 * Returns the effective timeout in seconds, or -1 for an invalid value. */
int standby_hw_mailbox_policy(uint32_t requested_seconds, uint8_t *state_out);
/* Status snapshot (zeroed ACTIVE snapshot before init). */
void standby_hw_status(StandbyStatus *out);
/* The USB (BBP) link opened (handshake) or closed. Called by the protocol owner (bbp.cpp)
 * at exactly those two points - never derived from a cable, DTR or an open port. */
void standby_hw_usb_session(bool open);
/* Judge the boot bring-up (PCA9535, converter SPI, switch matrix). Call once after it. */
void standby_hw_boot_check(void);
/* BBP_CMD_STANDBY with the transport the request really arrived on. Returns the reply
 * length, or -CmdError. */
int standby_hw_bbp(StandbySource src, const uint8_t *payload, size_t len, uint8_t *resp,
                   size_t *resp_len);

#ifdef __cplusplus
}

/* RAII bracket for the barrier: construct, check ok(), let it go out of scope. */
class StandbyWork {
public:
    StandbyWork() : m_rc(standby_hw_admit()), m_counted(true) {}
    /* counted=false: nothing is admitted or counted (ok() is true), e.g. a passive request. */
    explicit StandbyWork(bool counted)
        : m_rc(counted ? standby_hw_admit() : STANDBY_ADMIT_OK), m_counted(counted) {}
    ~StandbyWork() { if (m_counted && m_rc == STANDBY_ADMIT_OK) standby_hw_leave(); }
    bool ok() const { return m_rc == STANDBY_ADMIT_OK; }
    StandbyAdmit rc() const { return m_rc; }
private:
    StandbyAdmit m_rc;
    bool m_counted;
    StandbyWork(const StandbyWork &) = delete;
    StandbyWork &operator=(const StandbyWork &) = delete;
};
#endif
