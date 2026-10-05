#pragma once

// =============================================================================
// buttons_p4.h — ESP32-P4 front-panel button driver (UP/DOWN/OK).
//
// The navigation buttons moved from the C6 to the P4 (new PCB rev). The P4
// debounces them and relays discrete events to the C6 over DDP, where they feed
// the existing menu state machine. Behaviour mirrors the old on-C6 driver:
// debounce, auto-repeat on UP/DOWN (hold to scroll / adjust), and an OK
// long-press that maps to BACK.
//
// Events use the DDP_BTN_* bit values (== C6 buttons.h BTN_EV_*) so the relayed
// bitmask can be fed straight into menu_update() on the C6.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configure the three button GPIOs (input, pull-up, active-low).
void buttons_p4_init(void);

// Poll once per loop with a monotonic millisecond timestamp. Returns a bitmask
// of DDP_BTN_* events that occurred since the last poll (0 = nothing).
uint8_t buttons_p4_poll(uint32_t now_ms);

// True while any button's debounced state is pressed. Valid after a poll. Used by
// the standby gate to consume a whole press/hold/release gesture.
bool buttons_p4_any_held(void);

// True while any pad is pulled low, including a press the debouncer has not
// confirmed yet. Valid after a poll.
bool buttons_p4_any_raw(void);

// The standby gate claimed the gesture in progress: every button that is down (or
// still debouncing) emits nothing - no repeat, no long-press BACK, no release-time OK -
// until it has been fully released.
void buttons_p4_discard_gesture(void);

#ifdef __cplusplus
}
#endif
