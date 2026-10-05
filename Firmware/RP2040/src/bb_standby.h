#pragma once

// =============================================================================
// bb_standby.h - LA HAT (RP2040) standby participant, Pico binding.
//
// The pure state machine is bb_standby_core.c; this file binds it to the real
// owners (LA, DAP, power rails, GPIO, WS2812, flash update). The S3 owns the policy
// and drives HAT_CMD_STANDBY; this side only answers, refuses new work while it is
// powered down, and reports what is actually running.
//
// Threading: state changes happen under one spin lock (cmd task on Core 1, USB task on
// Core 0, DAP task on Core 1). Hardware actions run on the cmd task, outside the lock.
// Nothing here calls TinyUSB endpoint functions or bb_la_log().
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "standby_wire.h"

void bb_standby_init(void);

// One HAT_CMD_STANDBY frame (cmd task). Sends HAT_RSP_STANDBY.
void bb_standby_handle_frame(const uint8_t *payload, uint8_t len);

// Gate for a HAT UART command (cmd task). false = refuse with HAT_ERR_BUSY (the refusal
// already counted as activity and requested a wake). When *admitted is set the caller
// must call bb_standby_cmd_leave() after dispatching.
bool bb_standby_cmd_enter(uint8_t cmd, const uint8_t *payload, uint8_t len, bool *admitted);
void bb_standby_cmd_leave(void);

// The same gate for work that arrives on the USB thread (LA vendor-bulk START).
bool bb_standby_work_enter(void);
void bb_standby_work_leave(void);

// Periodic, from the cmd task. Returns true when the HAT IRQ should be pulsed.
bool bb_standby_poll(void);

// Periodic power-ADC monitoring is allowed only while ACTIVE.
bool bb_standby_monitor_allowed(void);

// tud_unmount_cb: the USB host is gone, so any logical DAP session ends with it.
void bb_standby_usb_unmounted(void);
