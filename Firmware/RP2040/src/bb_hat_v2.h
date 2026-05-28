#pragma once
#include <stdint.h>
#include <stdbool.h>

void bb_hat_v2_init(void);
void bb_hat_v2_handle_reset(void);

// Command handlers called from bb_main.c dispatch_command()
void handle_get_caps(void);
void handle_get_rail_status(void);
void handle_set_rail_enable(const uint8_t *payload, uint8_t len);
void handle_set_led_state(const uint8_t *payload, uint8_t len);
void handle_la_set_route(const uint8_t *payload, uint8_t len);
