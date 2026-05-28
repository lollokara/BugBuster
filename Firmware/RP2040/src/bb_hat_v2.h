#pragma once

// =============================================================================
// bb_hat_v2.h — BugBuster HAT v2 Feature Handlers
// =============================================================================

#include "bb_protocol.h"
#include <stdint.h>
#include <stdbool.h>

void bb_hat_v2_init(void);
void bb_hat_v2_handle_reset(void);

void handle_get_caps(void);
void handle_set_rail_enable(const uint8_t *payload, uint8_t len);
void handle_get_rail_status(void);
void handle_set_led_state(const uint8_t *payload, uint8_t len);
void handle_la_set_route(const uint8_t *payload, uint8_t len);
void handle_calibrate_start(const uint8_t *payload, uint8_t len);
void handle_calibrate_status(void);
void handle_calibrate_import(const uint8_t *payload, uint8_t len);
void handle_set_io_bank(const uint8_t *payload, uint8_t len);
void handle_set_level_shift(const uint8_t *payload, uint8_t len);
void handle_set_rail_voltage(const uint8_t *payload, uint8_t len);

bool bb_hat_v2_set_io_voltage(uint16_t mv);
uint16_t bb_hat_v2_get_io_voltage(void);
bool bb_hat_v2_set_rail_voltage(uint8_t rail_id, uint16_t mv);
