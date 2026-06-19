#pragma once

// =============================================================================
// ds4424_p4.h — DS4424 4-channel I2C IDAC (ESP32-P4 port)
//
// Ported from Firmware/ESP32/src/hal/ds4424.{cpp,h}. This pass provides the
// low-level raw-code interface (the part that is board-independent). The
// voltage<->code calibration math from the ESP32 driver can be layered on top
// once the P4 board's regulator feedback network is finalised.
//
// Data byte format (per-channel register 0xF8..0xFB):
//   bit7  = sign  (1 = source current, 0 = sink current)
//   bit6:0 = magnitude (0..127)
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS4424_NUM_CHANNELS  4
#define DS4424_REG_OUT0      0xF8
#define DS4424_REG_OUT1      0xF9
#define DS4424_REG_OUT2      0xFA
#define DS4424_REG_OUT3      0xFB

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t                 addr;
    bool                    present;
    int8_t                  code[DS4424_NUM_CHANNELS];
} ds4424_t;

/** @brief Attach the IDAC to an existing i2c_master bus and probe it. */
esp_err_t ds4424_attach(ds4424_t *d, i2c_master_bus_handle_t bus,
                        uint8_t addr, uint32_t scl_hz);

/** @brief Set raw DAC code for a channel (-127..+127). 0 = midpoint. */
esp_err_t ds4424_set_code(ds4424_t *d, uint8_t ch, int8_t code);

/** @brief Read back the raw DAC code for a channel. */
esp_err_t ds4424_get_code(ds4424_t *d, uint8_t ch, int8_t *code);

#ifdef __cplusplus
}
#endif
