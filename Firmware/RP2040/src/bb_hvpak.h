#pragma once

// =============================================================================
// bb_hvpak.h — Renesas HVPAK / GreenPAK backend
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BB_HVPAK_PART_UNKNOWN = 0,
    BB_HVPAK_PART_SLG47104 = 1,
    BB_HVPAK_PART_SLG47115_E = 2,
} BbHvpakPart;

typedef enum {
    BB_HVPAK_ERR_NONE = 0,
    BB_HVPAK_ERR_NOT_INITIALIZED = 1,
    BB_HVPAK_ERR_NO_DEVICE = 2,
    BB_HVPAK_ERR_I2C_TIMEOUT = 3,
    BB_HVPAK_ERR_UNKNOWN_IDENTITY = 4,
    BB_HVPAK_ERR_UNSUPPORTED_VOLTAGE = 5,
    BB_HVPAK_ERR_WRITE_FAILED = 6,
    BB_HVPAK_ERR_INVALID_INDEX = 7,
    BB_HVPAK_ERR_UNSUPPORTED_CAPABILITY = 8,
    BB_HVPAK_ERR_INVALID_ARGUMENT = 9,
    BB_HVPAK_ERR_UNSAFE_REGISTER = 10,
} BbHvpakError;

typedef enum {
    BB_HVPAK_LUT2 = 0,
    BB_HVPAK_LUT3 = 1,
    BB_HVPAK_LUT4 = 2,
} BbHvpakLutKind;

typedef struct {
    uint32_t flags;
    uint8_t lut2_count;
    uint8_t lut3_count;
    uint8_t lut4_count;
    uint8_t pwm_count;
    uint8_t comparator_count;
    uint8_t bridge_count;
} BbHvpakCapabilities;

typedef struct {
    uint8_t kind;
    uint8_t index;
    uint8_t width_bits;
    uint16_t truth_table;
} BbHvpakLutConfig;

typedef struct {
    uint8_t output_mode[2];
    uint8_t ocp_retry[2];
    bool predriver_enabled;
    bool full_bridge_enabled;
    bool control_selection_ph_en;
    bool ocp_deglitch_enabled;
    bool uvlo_enabled;
} BbHvpakBridgeConfig;

typedef struct {
    uint8_t vref_mode;
    bool vref_powered;
    bool vref_power_from_matrix;
    bool vref_sink_12ua;
    uint8_t vref_input_selection;
    uint8_t current_sense_vref;
    bool current_sense_dynamic_from_pwm;
    uint8_t current_sense_gain;
    bool current_sense_invert;
    bool current_sense_enabled;
    uint8_t acmp0_gain;
    uint8_t acmp0_vref;
    uint8_t acmp1_gain;
    uint8_t acmp1_vref;
    bool has_acmp1;
} BbHvpakAnalogConfig;

typedef struct {
    uint8_t index;
    uint8_t initial_value;
    uint8_t current_value;
    bool resolution_7bit;
    bool out_plus_inverted;
    bool out_minus_inverted;
    bool async_powerdown;
    bool autostop_mode;
    bool boundary_osc_disable;
    bool phase_correct;
    uint8_t deadband;
    bool stop_mode;
    bool i2c_trigger;
    uint8_t duty_source;
    uint8_t period_clock_source;
    uint8_t duty_clock_source;
} BbHvpakPwmConfig;

#define BB_HVPAK_CAP_LUT2        (1u << 0)
#define BB_HVPAK_CAP_LUT3        (1u << 1)
#define BB_HVPAK_CAP_LUT4        (1u << 2)
#define BB_HVPAK_CAP_BRIDGE      (1u << 3)
#define BB_HVPAK_CAP_PWM0        (1u << 4)
#define BB_HVPAK_CAP_PWM1        (1u << 5)
#define BB_HVPAK_CAP_ANALOG      (1u << 6)
#define BB_HVPAK_CAP_ACMP1       (1u << 7)
#define BB_HVPAK_CAP_REG_RW      (1u << 8)

void bb_hvpak_init(void);
bool bb_hvpak_detect(void);
bool bb_hvpak_set_voltage(uint16_t mv);
uint16_t bb_hvpak_get_voltage(void);
uint16_t bb_hvpak_get_requested_voltage(void);
bool bb_hvpak_is_ready(void);
bool bb_hvpak_is_factory_virgin(void);
bool bb_hvpak_has_service_window(void);
uint8_t bb_hvpak_get_service_f5(void);
uint8_t bb_hvpak_get_service_fd(void);
uint8_t bb_hvpak_get_service_fe(void);
BbHvpakPart bb_hvpak_get_part(void);
uint8_t bb_hvpak_get_last_error(void);
const char *bb_hvpak_part_name(BbHvpakPart part);
const char *bb_hvpak_error_name(BbHvpakError error);

bool bb_hvpak_get_capabilities(BbHvpakCapabilities *out);
bool bb_hvpak_get_lut(uint8_t kind, uint8_t index, BbHvpakLutConfig *out);
bool bb_hvpak_set_lut(const BbHvpakLutConfig *cfg);
bool bb_hvpak_get_bridge(BbHvpakBridgeConfig *out);
bool bb_hvpak_set_bridge(const BbHvpakBridgeConfig *cfg);
bool bb_hvpak_get_analog(BbHvpakAnalogConfig *out);
bool bb_hvpak_set_analog(const BbHvpakAnalogConfig *cfg);
bool bb_hvpak_get_pwm(uint8_t index, BbHvpakPwmConfig *out);
bool bb_hvpak_set_pwm(const BbHvpakPwmConfig *cfg);
bool bb_hvpak_reg_read(uint8_t addr, uint8_t *value);
bool bb_hvpak_reg_write_masked(uint8_t addr, uint8_t mask, uint8_t value);
