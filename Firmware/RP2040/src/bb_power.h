#pragma once

// =============================================================================
// bb_power.h — Target power management (connector enables, current monitoring)
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     enabled;
    float    current_ma;
    bool     fault;
} ConnectorStatus;

void bb_power_init(void);
void bb_power_set(uint8_t connector, bool enable);
bool bb_power_get_enabled(uint8_t connector);
float bb_power_read_current(uint8_t connector);
bool bb_power_get_fault(uint8_t connector);
void bb_power_update(void);  // Poll ADC + fault pins
void bb_power_get_status(ConnectorStatus *a, ConnectorStatus *b);
