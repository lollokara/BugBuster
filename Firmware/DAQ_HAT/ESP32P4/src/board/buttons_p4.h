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

#ifdef __cplusplus
extern "C" {
#endif

// Configure the three button GPIOs (input, pull-up, active-low).
void buttons_p4_init(void);

// Poll once per loop with a monotonic millisecond timestamp. Returns a bitmask
// of DDP_BTN_* events that occurred since the last poll (0 = nothing).
uint8_t buttons_p4_poll(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
