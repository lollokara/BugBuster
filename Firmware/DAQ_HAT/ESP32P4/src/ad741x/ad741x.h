#pragma once

// =============================================================================
// ad741x.h — AD7414 / AD7415 I2C temperature-sensor driver
//
// 10-bit temperature-to-digital, 0.25 C resolution, -40..+125 C.
// AD7414 adds an ALERT output with THIGH/TLOW limit registers; AD7415 is the
// cut-down variant (temperature + config only).
//
// Uses the ESP-IDF i2c_master (bus/device handle) API. The board layer owns the
// i2c_master_bus_handle_t and passes it in.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register pointer values (only the 2 LSBs are significant).
#define AD741X_REG_TEMP     0x00   // RO, 2 bytes
#define AD741X_REG_CONFIG   0x01   // RW, 1 byte
#define AD741X_REG_THIGH    0x02   // RW, AD7414 only
#define AD741X_REG_TLOW     0x03   // RW, AD7414 only

// Config register bits (AD7414 superset).
#define AD741X_CFG_PD            (1u << 7)  // full power-down
#define AD741X_CFG_FLTR          (1u << 6)  // 0 = bypass SDA/SCL filter
#define AD741X_CFG_ALERT_DISABLE (1u << 5)  // AD7414
#define AD741X_CFG_ALERT_POL_HI  (1u << 4)  // AD7414, 1 = active high
#define AD741X_CFG_ALERT_RESET   (1u << 3)  // AD7414, write 1
#define AD741X_CFG_ONESHOT       (1u << 2)  // write 1 -> single conversion

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t                 addr;        // 7-bit
    bool                    has_alert;   // true for AD7414
    bool                    present;
} ad741x_t;

/**
 * @brief Attach a sensor to an existing i2c_master bus and probe it.
 *
 * @param bus        i2c_master bus handle (board-owned).
 * @param addr       7-bit address.
 * @param scl_hz     SCL speed for this device.
 * @param has_alert  true if this is an AD7414 (ALERT + limits available).
 */
esp_err_t ad741x_attach(ad741x_t *s, i2c_master_bus_handle_t bus,
                        uint8_t addr, uint32_t scl_hz, bool has_alert);

/** @brief Read the temperature in degrees Celsius. */
esp_err_t ad741x_read_celsius(ad741x_t *s, float *celsius);

/** @brief Read the raw signed 10-bit temperature code. */
esp_err_t ad741x_read_raw(ad741x_t *s, int16_t *raw10);

/** @brief Write the configuration register. */
esp_err_t ad741x_write_config(ad741x_t *s, uint8_t config);

/** @brief Read the configuration register. */
esp_err_t ad741x_read_config(ad741x_t *s, uint8_t *config);

/** @brief Trigger a one-shot conversion (sets ONESHOT, keeps other bits). */
esp_err_t ad741x_oneshot(ad741x_t *s, uint8_t base_config);

/** @brief AD7414 only: set THIGH/TLOW limits (whole degrees, signed 8-bit). */
esp_err_t ad741x_set_limits(ad741x_t *s, int8_t t_high_c, int8_t t_low_c);

#ifdef __cplusplus
}
#endif
