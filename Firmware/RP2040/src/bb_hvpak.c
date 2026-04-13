// =============================================================================
// bb_hvpak.c — Renesas HVPAK / GreenPAK backend
// =============================================================================

#include "bb_hvpak.h"
#include "bb_config.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/error.h"
#include "pico/stdlib.h"

#include <stddef.h>
#include <string.h>

typedef enum {
    BB_HVPAK_PRESET_1200 = 0,
    BB_HVPAK_PRESET_1800,
    BB_HVPAK_PRESET_2500,
    BB_HVPAK_PRESET_3300,
    BB_HVPAK_PRESET_5000,
    BB_HVPAK_PRESET_COUNT,
} BbHvpakPresetIndex;

typedef struct {
    uint16_t lsb;
    uint8_t width;
} HvpakField;

typedef struct {
    uint8_t addr;
    uint8_t bytes;
    uint8_t width_bits;
    uint8_t bit_offset;
} HvpakLutEntry;

typedef struct {
    uint16_t resolution;
    uint16_t out_plus_invert;
    uint16_t out_minus_invert;
    uint16_t sync;
    uint16_t autostop;
    uint16_t boundary_disable;
    uint16_t phase_correct;
    HvpakField deadband;
    uint16_t keep_stop;
    uint16_t i2c_trigger;
    HvpakField duty_source;
    HvpakField period_clock_source;
    HvpakField duty_clock_source;
    uint8_t initial_value_addr;
    uint8_t current_value_addr;
} HvpakPwmLayout;

typedef struct {
    BbHvpakPart part;
    const char *name;
    uint8_t identity;
    uint8_t command_codes[BB_HVPAK_PRESET_COUNT];
    BbHvpakCapabilities caps;
    const HvpakLutEntry *lut2;
    const HvpakLutEntry *lut3;
    const HvpakLutEntry *lut4;
    HvpakField bridge_output_mode[2];
    HvpakField bridge_ocp_retry[2];
    HvpakField bridge_predriver;
    HvpakField bridge_full_bridge;
    HvpakField bridge_control_sel;
    HvpakField bridge_ocp_deglitch;
    HvpakField bridge_uvlo;
    uint8_t bridge_uvlo_enable_value;
    HvpakField analog_vref_mode;
    HvpakField analog_vref_power;
    HvpakField analog_vref_pd_sel;
    HvpakField analog_vref_sink_current;
    HvpakField analog_vref_input_sel;
    HvpakField analog_current_sense_vref;
    HvpakField analog_current_sense_source;
    HvpakField analog_current_sense_gain;
    HvpakField analog_current_sense_invert;
    HvpakField analog_current_sense_enable;
    HvpakField analog_acmp0_gain;
    HvpakField analog_acmp0_vref;
    HvpakField analog_acmp1_gain;
    HvpakField analog_acmp1_vref;
    HvpakPwmLayout pwm[2];
} HvpakDescriptor;

typedef struct {
    bool initialized;
    bool detected;
    bool factory_virgin;
    bool service_window_ok;
    uint8_t service_f5;
    uint8_t service_fd;
    uint8_t service_fe;
    uint16_t requested_mv;
    uint16_t applied_mv;
    BbHvpakPart part;
    BbHvpakError last_error;
    const HvpakDescriptor *desc;
} HvpakState;

static const uint16_t k_supported_mv[BB_HVPAK_PRESET_COUNT] = {
    1200, 1800, 2500, 3300, 5000,
};

static int hvpak_read_raw(uint8_t reg, uint8_t *value);

static const HvpakLutEntry k_slg47104_lut2[] = {
    { .addr = 156, .bytes = 1, .width_bits = 4, .bit_offset = 0 },
    { .addr = 156, .bytes = 1, .width_bits = 4, .bit_offset = 4 },
    { .addr = 146, .bytes = 1, .width_bits = 4, .bit_offset = 0 },
};

static const HvpakLutEntry k_slg47104_lut3[] = {
    { .addr = 153, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 142, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 145, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 155, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 150, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 129, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 131, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
};

static const HvpakLutEntry k_slg47104_lut4[] = {
    { .addr = 151, .bytes = 2, .width_bits = 16, .bit_offset = 0 },
    { .addr = 125, .bytes = 2, .width_bits = 16, .bit_offset = 0 },
};

static const HvpakLutEntry k_slg47115e_lut2[] = {
    { .addr = 156, .bytes = 1, .width_bits = 4, .bit_offset = 0 },
    { .addr = 156, .bytes = 1, .width_bits = 4, .bit_offset = 4 },
    { .addr = 157, .bytes = 1, .width_bits = 4, .bit_offset = 0 },
    { .addr = 146, .bytes = 1, .width_bits = 4, .bit_offset = 0 },
};

static const HvpakLutEntry k_slg47115e_lut3[] = {
    { .addr = 153, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 142, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 143, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 144, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 145, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 155, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 150, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 129, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 131, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 133, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
    { .addr = 135, .bytes = 1, .width_bits = 8, .bit_offset = 0 },
};

static const HvpakLutEntry k_slg47115e_lut4[] = {
    { .addr = 151, .bytes = 2, .width_bits = 16, .bit_offset = 0 },
    { .addr = 125, .bytes = 2, .width_bits = 16, .bit_offset = 0 },
};

#define HVPAK_FIELD_INIT(bitv, widv) ((HvpakField){ .lsb = (bitv), .width = (widv) })
#define HVPAK_ONEBIT_FIELD(bitv) HVPAK_FIELD_INIT((bitv), 1)

static const HvpakDescriptor k_descriptors[] = {
    {
        .part = BB_HVPAK_PART_SLG47104,
        .name = "SLG47104",
        .identity = BB_HVPAK_ID_SLG47104,
        .command_codes = { 0x10, 0x11, 0x12, 0x13, 0x14 },
        .caps = {
            .flags = BB_HVPAK_CAP_LUT2 | BB_HVPAK_CAP_LUT3 | BB_HVPAK_CAP_LUT4 |
                     BB_HVPAK_CAP_BRIDGE | BB_HVPAK_CAP_PWM0 |
                     BB_HVPAK_CAP_ANALOG | BB_HVPAK_CAP_REG_RW,
            .lut2_count = 3,
            .lut3_count = 7,
            .lut4_count = 2,
            .pwm_count = 1,
            .comparator_count = 1,
            .bridge_count = 2,
        },
        .lut2 = k_slg47104_lut2,
        .lut3 = k_slg47104_lut3,
        .lut4 = k_slg47104_lut4,
        .bridge_output_mode = { HVPAK_FIELD_INIT(792, 2), HVPAK_FIELD_INIT(800, 2) },
        .bridge_ocp_retry = { HVPAK_FIELD_INIT(794, 3), HVPAK_FIELD_INIT(802, 3) },
        .bridge_predriver = HVPAK_FIELD_INIT(797, 1),
        .bridge_full_bridge = HVPAK_FIELD_INIT(798, 1),
        .bridge_control_sel = HVPAK_FIELD_INIT(876, 1),
        .bridge_ocp_deglitch = HVPAK_FIELD_INIT(875, 1),
        .bridge_uvlo = HVPAK_FIELD_INIT(864, 2),
        .bridge_uvlo_enable_value = 0x03,
        .analog_vref_mode = HVPAK_FIELD_INIT(640, 3),
        .analog_vref_power = HVPAK_FIELD_INIT(643, 1),
        .analog_vref_pd_sel = HVPAK_FIELD_INIT(644, 1),
        .analog_vref_sink_current = HVPAK_FIELD_INIT(645, 1),
        .analog_vref_input_sel = HVPAK_FIELD_INIT(646, 2),
        .analog_current_sense_vref = HVPAK_FIELD_INIT(704, 6),
        .analog_current_sense_source = HVPAK_FIELD_INIT(710, 1),
        .analog_current_sense_gain = HVPAK_FIELD_INIT(868, 1),
        .analog_current_sense_invert = HVPAK_FIELD_INIT(869, 1),
        .analog_current_sense_enable = HVPAK_FIELD_INIT(871, 1),
        .analog_acmp0_gain = HVPAK_FIELD_INIT(680, 2),
        .analog_acmp0_vref = HVPAK_FIELD_INIT(682, 6),
        .analog_acmp1_gain = HVPAK_FIELD_INIT(0, 0),
        .analog_acmp1_vref = HVPAK_FIELD_INIT(0, 0),
        .pwm = {
            {
                .resolution = 1472,
                .out_plus_invert = 1473,
                .out_minus_invert = 1474,
                .sync = 1475,
                .autostop = 1476,
                .boundary_disable = 1477,
                .phase_correct = 1478,
                .deadband = HVPAK_FIELD_INIT(1480, 2),
                .keep_stop = 1479,
                .i2c_trigger = 1297,
                .duty_source = HVPAK_FIELD_INIT(1482, 2),
                .period_clock_source = HVPAK_FIELD_INIT(1488, 4),
                .duty_clock_source = HVPAK_FIELD_INIT(1484, 2),
                .initial_value_addr = 163,
                .current_value_addr = 165,
            },
            { 0 },
        },
    },
    {
        .part = BB_HVPAK_PART_SLG47115_E,
        .name = "SLG47115-E",
        .identity = BB_HVPAK_ID_SLG47115_E,
        .command_codes = { 0x20, 0x21, 0x22, 0x23, 0x24 },
        .caps = {
            .flags = BB_HVPAK_CAP_LUT2 | BB_HVPAK_CAP_LUT3 | BB_HVPAK_CAP_LUT4 |
                     BB_HVPAK_CAP_BRIDGE | BB_HVPAK_CAP_PWM0 | BB_HVPAK_CAP_PWM1 |
                     BB_HVPAK_CAP_ANALOG | BB_HVPAK_CAP_ACMP1 | BB_HVPAK_CAP_REG_RW,
            .lut2_count = 4,
            .lut3_count = 11,
            .lut4_count = 2,
            .pwm_count = 2,
            .comparator_count = 2,
            .bridge_count = 2,
        },
        .lut2 = k_slg47115e_lut2,
        .lut3 = k_slg47115e_lut3,
        .lut4 = k_slg47115e_lut4,
        .bridge_output_mode = { HVPAK_FIELD_INIT(776, 2), HVPAK_FIELD_INIT(784, 2) },
        .bridge_ocp_retry = { HVPAK_FIELD_INIT(778, 3), HVPAK_FIELD_INIT(786, 3) },
        .bridge_predriver = HVPAK_FIELD_INIT(781, 1),
        .bridge_full_bridge = HVPAK_FIELD_INIT(782, 1),
        .bridge_control_sel = HVPAK_FIELD_INIT(874, 1),
        .bridge_ocp_deglitch = HVPAK_FIELD_INIT(873, 1),
        .bridge_uvlo = HVPAK_FIELD_INIT(864, 1),
        .bridge_uvlo_enable_value = 0x01,
        .analog_vref_mode = HVPAK_FIELD_INIT(640, 3),
        .analog_vref_power = HVPAK_FIELD_INIT(643, 1),
        .analog_vref_pd_sel = HVPAK_FIELD_INIT(644, 1),
        .analog_vref_sink_current = HVPAK_FIELD_INIT(645, 1),
        .analog_vref_input_sel = HVPAK_FIELD_INIT(646, 2),
        .analog_current_sense_vref = HVPAK_FIELD_INIT(696, 6),
        .analog_current_sense_source = HVPAK_FIELD_INIT(702, 1),
        .analog_current_sense_gain = HVPAK_FIELD_INIT(866, 1),
        .analog_current_sense_invert = HVPAK_FIELD_INIT(867, 1),
        .analog_current_sense_enable = HVPAK_FIELD_INIT(870, 1),
        .analog_acmp0_gain = HVPAK_FIELD_INIT(680, 2),
        .analog_acmp0_vref = HVPAK_FIELD_INIT(682, 6),
        .analog_acmp1_gain = HVPAK_FIELD_INIT(688, 2),
        .analog_acmp1_vref = HVPAK_FIELD_INIT(690, 6),
        .pwm = {
            {
                .resolution = 1298,
                .out_plus_invert = 1299,
                .out_minus_invert = 1300,
                .sync = 1301,
                .autostop = 1302,
                .boundary_disable = 1303,
                .phase_correct = 1460,
                .deadband = HVPAK_FIELD_INIT(1464, 2),
                .keep_stop = 1461,
                .i2c_trigger = 1296,
                .duty_source = HVPAK_FIELD_INIT(1466, 2),
                .period_clock_source = HVPAK_FIELD_INIT(1456, 4),
                .duty_clock_source = HVPAK_FIELD_INIT(1468, 2),
                .initial_value_addr = 161,
                .current_value_addr = 164,
            },
            {
                .resolution = 1472,
                .out_plus_invert = 1473,
                .out_minus_invert = 1474,
                .sync = 1475,
                .autostop = 1476,
                .boundary_disable = 1477,
                .phase_correct = 1478,
                .deadband = HVPAK_FIELD_INIT(1480, 2),
                .keep_stop = 1479,
                .i2c_trigger = 1297,
                .duty_source = HVPAK_FIELD_INIT(1482, 2),
                .period_clock_source = HVPAK_FIELD_INIT(1488, 4),
                .duty_clock_source = HVPAK_FIELD_INIT(1484, 2),
                .initial_value_addr = 163,
                .current_value_addr = 165,
            },
        },
    },
};

static HvpakState s_state = {
    .requested_mv = BB_HVPAK_DEFAULT_MV,
    .applied_mv = BB_HVPAK_DEFAULT_MV,
    .part = BB_HVPAK_PART_UNKNOWN,
    .last_error = BB_HVPAK_ERR_NOT_INITIALIZED,
};

static void hvpak_set_error(BbHvpakError error)
{
    s_state.last_error = error;
}

static const HvpakDescriptor *hvpak_find_descriptor(uint8_t identity)
{
    for (size_t i = 0; i < sizeof(k_descriptors) / sizeof(k_descriptors[0]); i++) {
        if (k_descriptors[i].identity == identity) {
            return &k_descriptors[i];
        }
    }
    return NULL;
}

static bool hvpak_probe_service_window(uint8_t *f5, uint8_t *fd, uint8_t *fe)
{
    uint8_t v_f5 = 0;
    uint8_t v_fd = 0;
    uint8_t v_fe = 0;
    int rc;

    rc = hvpak_read_raw(0xF5, &v_f5);
    if (rc != 1) return false;
    rc = hvpak_read_raw(0xFD, &v_fd);
    if (rc != 1) return false;
    rc = hvpak_read_raw(0xFE, &v_fe);
    if (rc != 1) return false;

    if (f5) *f5 = v_f5;
    if (fd) *fd = v_fd;
    if (fe) *fe = v_fe;
    return true;
}

static bool hvpak_require_ready(void)
{
    if (!s_state.initialized) {
        hvpak_set_error(BB_HVPAK_ERR_NOT_INITIALIZED);
        return false;
    }
    if (!s_state.detected && !bb_hvpak_detect()) {
        return false;
    }
    if (!s_state.desc) {
        hvpak_set_error(BB_HVPAK_ERR_UNKNOWN_IDENTITY);
        return false;
    }
    return true;
}

static int hvpak_write_raw(uint8_t reg, uint8_t value)
{
    uint8_t frame[2] = { reg, value };
    return i2c_write_timeout_us(
        BB_HVPAK_I2C, BB_HVPAK_I2C_ADDR, frame, sizeof(frame), false, BB_HVPAK_I2C_TIMEOUT_US
    );
}

static int hvpak_read_raw(uint8_t reg, uint8_t *value)
{
    int rc = i2c_write_timeout_us(
        BB_HVPAK_I2C, BB_HVPAK_I2C_ADDR, &reg, 1, true, BB_HVPAK_I2C_TIMEOUT_US
    );
    if (rc != 1) return rc;
    return i2c_read_timeout_us(
        BB_HVPAK_I2C, BB_HVPAK_I2C_ADDR, value, 1, false, BB_HVPAK_I2C_TIMEOUT_US
    );
}

static bool hvpak_read_byte(uint8_t reg, uint8_t *value)
{
    int rc = hvpak_read_raw(reg, value);
    if (rc == 1) return true;
    hvpak_set_error(rc == PICO_ERROR_TIMEOUT ? BB_HVPAK_ERR_I2C_TIMEOUT : BB_HVPAK_ERR_WRITE_FAILED);
    return false;
}

static bool hvpak_write_byte(uint8_t reg, uint8_t value)
{
    int rc = hvpak_write_raw(reg, value);
    if (rc == 2) return true;
    hvpak_set_error(rc == PICO_ERROR_TIMEOUT ? BB_HVPAK_ERR_I2C_TIMEOUT : BB_HVPAK_ERR_WRITE_FAILED);
    return false;
}

static bool hvpak_read_field(HvpakField field, uint8_t *value)
{
    uint8_t byte = 0;
    uint8_t shift = (uint8_t)(field.lsb & 0x7);
    uint8_t mask;
    if (field.width == 0) return false;
    if (shift + field.width > 8) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_ARGUMENT);
        return false;
    }
    if (!hvpak_read_byte((uint8_t)(field.lsb >> 3), &byte)) return false;
    mask = (uint8_t)(((1u << field.width) - 1u) << shift);
    *value = (uint8_t)((byte & mask) >> shift);
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

static bool hvpak_write_field(HvpakField field, uint8_t value)
{
    uint8_t shift = (uint8_t)(field.lsb & 0x7);
    if (field.width == 0) return false;
    if (field.width < 8 && value >= (1u << field.width)) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_ARGUMENT);
        return false;
    }
    if (!bb_hvpak_reg_write_masked((uint8_t)(field.lsb >> 3),
                                   (uint8_t)(((1u << field.width) - 1u) << shift),
                                   (uint8_t)(value << shift))) {
        return false;
    }
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

static bool hvpak_lut_table_io(const HvpakLutEntry *entry, uint16_t *truth, bool write)
{
    uint16_t value = 0;
    if (write && entry->bit_offset != 0) {
        uint8_t current = 0;
        uint8_t mask = (uint8_t)(((1u << entry->width_bits) - 1u) << entry->bit_offset);
        if (!hvpak_read_byte(entry->addr, &current)) return false;
        current = (uint8_t)((current & ~mask) | (((*truth) << entry->bit_offset) & mask));
        if (!hvpak_write_byte(entry->addr, current)) return false;
        hvpak_set_error(BB_HVPAK_ERR_NONE);
        return true;
    }

    for (uint8_t i = 0; i < entry->bytes; i++) {
        uint8_t byte = 0;
        if (write) {
            byte = (uint8_t)((*truth >> (8 * i)) & 0xFFu);
            if (!hvpak_write_byte((uint8_t)(entry->addr + i), byte)) return false;
        } else {
            if (!hvpak_read_byte((uint8_t)(entry->addr + i), &byte)) return false;
            value |= (uint16_t)(byte << (8 * i));
        }
    }
    if (entry->bit_offset != 0) {
        value = (uint16_t)((value >> entry->bit_offset) & ((1u << entry->width_bits) - 1u));
    }
    if (!write) {
        if (entry->width_bits < 16) value &= (uint16_t)((1u << entry->width_bits) - 1u);
        *truth = value;
    }
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

static const HvpakLutEntry *hvpak_lut_entry(uint8_t kind, uint8_t index)
{
    const HvpakDescriptor *d = s_state.desc;
    if (!d) return NULL;
    switch (kind) {
        case BB_HVPAK_LUT2:
            if (index < d->caps.lut2_count) return &d->lut2[index];
            break;
        case BB_HVPAK_LUT3:
            if (index < d->caps.lut3_count) return &d->lut3[index];
            break;
        case BB_HVPAK_LUT4:
            if (index < d->caps.lut4_count) return &d->lut4[index];
            break;
        default:
            break;
    }
    return NULL;
}

static BbHvpakPresetIndex hvpak_find_preset(uint16_t mv)
{
    for (size_t i = 0; i < BB_HVPAK_PRESET_COUNT; i++) {
        if (k_supported_mv[i] == mv) return (BbHvpakPresetIndex)i;
    }
    return BB_HVPAK_PRESET_COUNT;
}

static bool hvpak_addr_matches_field(uint8_t addr, HvpakField field)
{
    return field.width > 0 && addr == (uint8_t)(field.lsb >> 3);
}

static bool hvpak_is_runtime_safe_write_addr(uint8_t addr)
{
    const HvpakDescriptor *d = s_state.desc;
    if (!d) return false;

    for (uint8_t i = 0; i < d->caps.lut2_count; i++) {
        if (addr >= d->lut2[i].addr && addr < (uint8_t)(d->lut2[i].addr + d->lut2[i].bytes)) return true;
    }
    for (uint8_t i = 0; i < d->caps.lut3_count; i++) {
        if (addr >= d->lut3[i].addr && addr < (uint8_t)(d->lut3[i].addr + d->lut3[i].bytes)) return true;
    }
    for (uint8_t i = 0; i < d->caps.lut4_count; i++) {
        if (addr >= d->lut4[i].addr && addr < (uint8_t)(d->lut4[i].addr + d->lut4[i].bytes)) return true;
    }

    for (int i = 0; i < 2; i++) {
        if (hvpak_addr_matches_field(addr, d->bridge_output_mode[i]) ||
            hvpak_addr_matches_field(addr, d->bridge_ocp_retry[i])) return true;
    }

    if (hvpak_addr_matches_field(addr, d->bridge_predriver) ||
        hvpak_addr_matches_field(addr, d->bridge_full_bridge) ||
        hvpak_addr_matches_field(addr, d->bridge_control_sel) ||
        hvpak_addr_matches_field(addr, d->bridge_ocp_deglitch) ||
        hvpak_addr_matches_field(addr, d->bridge_uvlo) ||
        hvpak_addr_matches_field(addr, d->analog_vref_mode) ||
        hvpak_addr_matches_field(addr, d->analog_vref_power) ||
        hvpak_addr_matches_field(addr, d->analog_vref_pd_sel) ||
        hvpak_addr_matches_field(addr, d->analog_vref_sink_current) ||
        hvpak_addr_matches_field(addr, d->analog_vref_input_sel) ||
        hvpak_addr_matches_field(addr, d->analog_current_sense_vref) ||
        hvpak_addr_matches_field(addr, d->analog_current_sense_source) ||
        hvpak_addr_matches_field(addr, d->analog_current_sense_gain) ||
        hvpak_addr_matches_field(addr, d->analog_current_sense_invert) ||
        hvpak_addr_matches_field(addr, d->analog_current_sense_enable) ||
        hvpak_addr_matches_field(addr, d->analog_acmp0_gain) ||
        hvpak_addr_matches_field(addr, d->analog_acmp0_vref) ||
        hvpak_addr_matches_field(addr, d->analog_acmp1_gain) ||
        hvpak_addr_matches_field(addr, d->analog_acmp1_vref)) {
        return true;
    }

    for (uint8_t i = 0; i < d->caps.pwm_count; i++) {
        const HvpakPwmLayout *p = &d->pwm[i];
        if (addr == p->initial_value_addr ||
            hvpak_addr_matches_field(addr, p->deadband) ||
            hvpak_addr_matches_field(addr, p->duty_source) ||
            hvpak_addr_matches_field(addr, p->period_clock_source) ||
            hvpak_addr_matches_field(addr, p->duty_clock_source) ||
            addr == (uint8_t)(p->resolution >> 3) ||
            addr == (uint8_t)(p->out_plus_invert >> 3) ||
            addr == (uint8_t)(p->out_minus_invert >> 3) ||
            addr == (uint8_t)(p->sync >> 3) ||
            addr == (uint8_t)(p->autostop >> 3) ||
            addr == (uint8_t)(p->boundary_disable >> 3) ||
            addr == (uint8_t)(p->phase_correct >> 3) ||
            addr == (uint8_t)(p->keep_stop >> 3) ||
            addr == (uint8_t)(p->i2c_trigger >> 3)) {
            return true;
        }
    }

    return false;
}

void bb_hvpak_init(void)
{
    i2c_init(BB_HVPAK_I2C, BB_HVPAK_I2C_FREQ);
    gpio_set_function(BB_HVPAK_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BB_HVPAK_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BB_HVPAK_SDA_PIN);
    gpio_pull_up(BB_HVPAK_SCL_PIN);

    s_state.initialized = true;
    s_state.detected = false;
    s_state.factory_virgin = false;
    s_state.service_window_ok = false;
    s_state.service_f5 = 0;
    s_state.service_fd = 0;
    s_state.service_fe = 0;
    s_state.part = BB_HVPAK_PART_UNKNOWN;
    s_state.desc = NULL;
    s_state.requested_mv = BB_HVPAK_DEFAULT_MV;
    s_state.applied_mv = BB_HVPAK_DEFAULT_MV;
    hvpak_set_error(BB_HVPAK_ERR_NONE);

    (void)bb_hvpak_detect();
}

bool bb_hvpak_detect(void)
{
    uint8_t identity = 0;
    int rc;

    if (!s_state.initialized) {
        hvpak_set_error(BB_HVPAK_ERR_NOT_INITIALIZED);
        return false;
    }

    rc = hvpak_read_raw(BB_HVPAK_IDENTITY_REG, &identity);
    if (rc == PICO_ERROR_TIMEOUT) {
        s_state.detected = false;
        s_state.factory_virgin = false;
        s_state.part = BB_HVPAK_PART_UNKNOWN;
        s_state.desc = NULL;
        hvpak_set_error(BB_HVPAK_ERR_I2C_TIMEOUT);
        return false;
    }
    if (rc != 1) {
        s_state.detected = false;
        s_state.factory_virgin = false;
        s_state.part = BB_HVPAK_PART_UNKNOWN;
        s_state.desc = NULL;
        hvpak_set_error(BB_HVPAK_ERR_NO_DEVICE);
        return false;
    }

    s_state.desc = hvpak_find_descriptor(identity);
    if (!s_state.desc) {
        s_state.service_window_ok = hvpak_probe_service_window(
            &s_state.service_f5, &s_state.service_fd, &s_state.service_fe
        );
        s_state.factory_virgin = s_state.service_window_ok &&
                                 s_state.service_f5 == 0x00 &&
                                 s_state.service_fd == 0x00 &&
                                 s_state.service_fe == 0x00;
        s_state.detected = false;
        s_state.part = BB_HVPAK_PART_UNKNOWN;
        hvpak_set_error(BB_HVPAK_ERR_UNKNOWN_IDENTITY);
        return false;
    }

    s_state.detected = true;
    s_state.factory_virgin = false;
    s_state.service_window_ok = false;
    s_state.service_f5 = 0;
    s_state.service_fd = 0;
    s_state.service_fe = 0;
    s_state.part = s_state.desc->part;
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

bool bb_hvpak_set_voltage(uint16_t mv)
{
    BbHvpakPresetIndex preset;
    uint8_t command;
    int rc;

    s_state.requested_mv = mv;

    if (!hvpak_require_ready()) return false;
    if (mv < BB_HVPAK_MIN_MV || mv > BB_HVPAK_MAX_MV) {
        hvpak_set_error(BB_HVPAK_ERR_UNSUPPORTED_VOLTAGE);
        return false;
    }

    preset = hvpak_find_preset(mv);
    if (preset == BB_HVPAK_PRESET_COUNT || !s_state.desc) {
        hvpak_set_error(BB_HVPAK_ERR_UNSUPPORTED_VOLTAGE);
        return false;
    }

    command = s_state.desc->command_codes[preset];
    rc = hvpak_write_raw(BB_HVPAK_COMMAND_REG, command);
    if (rc == PICO_ERROR_TIMEOUT) {
        hvpak_set_error(BB_HVPAK_ERR_I2C_TIMEOUT);
        return false;
    }
    if (rc != 2) {
        hvpak_set_error(BB_HVPAK_ERR_WRITE_FAILED);
        return false;
    }

    s_state.applied_mv = mv;
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

uint16_t bb_hvpak_get_voltage(void) { return s_state.applied_mv; }
uint16_t bb_hvpak_get_requested_voltage(void) { return s_state.requested_mv; }
bool bb_hvpak_is_ready(void) { return s_state.initialized && s_state.detected && s_state.desc != NULL; }
bool bb_hvpak_is_factory_virgin(void) { return s_state.factory_virgin; }
bool bb_hvpak_has_service_window(void) { return s_state.service_window_ok; }
uint8_t bb_hvpak_get_service_f5(void) { return s_state.service_f5; }
uint8_t bb_hvpak_get_service_fd(void) { return s_state.service_fd; }
uint8_t bb_hvpak_get_service_fe(void) { return s_state.service_fe; }
BbHvpakPart bb_hvpak_get_part(void) { return s_state.part; }
uint8_t bb_hvpak_get_last_error(void) { return (uint8_t)s_state.last_error; }

const char *bb_hvpak_part_name(BbHvpakPart part)
{
    switch (part) {
        case BB_HVPAK_PART_SLG47104: return "SLG47104";
        case BB_HVPAK_PART_SLG47115_E: return "SLG47115-E";
        default: return "unknown";
    }
}

const char *bb_hvpak_error_name(BbHvpakError error)
{
    switch (error) {
        case BB_HVPAK_ERR_NONE: return "none";
        case BB_HVPAK_ERR_NOT_INITIALIZED: return "not_initialized";
        case BB_HVPAK_ERR_NO_DEVICE: return "no_device";
        case BB_HVPAK_ERR_I2C_TIMEOUT: return "i2c_timeout";
        case BB_HVPAK_ERR_UNKNOWN_IDENTITY: return "unknown_identity";
        case BB_HVPAK_ERR_UNSUPPORTED_VOLTAGE: return "unsupported_voltage";
        case BB_HVPAK_ERR_WRITE_FAILED: return "write_failed";
        case BB_HVPAK_ERR_INVALID_INDEX: return "invalid_index";
        case BB_HVPAK_ERR_UNSUPPORTED_CAPABILITY: return "unsupported_capability";
        case BB_HVPAK_ERR_INVALID_ARGUMENT: return "invalid_argument";
        case BB_HVPAK_ERR_UNSAFE_REGISTER: return "unsafe_register";
        default: return "unknown";
    }
}

bool bb_hvpak_get_capabilities(BbHvpakCapabilities *out)
{
    if (!out || !hvpak_require_ready()) return false;
    *out = s_state.desc->caps;
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

bool bb_hvpak_get_lut(uint8_t kind, uint8_t index, BbHvpakLutConfig *out)
{
    const HvpakLutEntry *entry;
    uint16_t truth = 0;
    if (!out || !hvpak_require_ready()) return false;
    entry = hvpak_lut_entry(kind, index);
    if (!entry) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_INDEX);
        return false;
    }
    if (!hvpak_lut_table_io(entry, &truth, false)) return false;
    out->kind = kind;
    out->index = index;
    out->width_bits = entry->width_bits;
    out->truth_table = truth;
    return true;
}

bool bb_hvpak_set_lut(const BbHvpakLutConfig *cfg)
{
    const HvpakLutEntry *entry;
    uint16_t truth;
    if (!cfg || !hvpak_require_ready()) return false;
    entry = hvpak_lut_entry(cfg->kind, cfg->index);
    if (!entry) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_INDEX);
        return false;
    }
    truth = cfg->truth_table;
    if (entry->width_bits < 16 && truth >= (1u << entry->width_bits)) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_ARGUMENT);
        return false;
    }
    return hvpak_lut_table_io(entry, &truth, true);
}

bool bb_hvpak_get_bridge(BbHvpakBridgeConfig *out)
{
    uint8_t value;
    if (!out || !hvpak_require_ready()) return false;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 2; i++) {
        if (!hvpak_read_field(s_state.desc->bridge_output_mode[i], &out->output_mode[i])) return false;
        if (!hvpak_read_field(s_state.desc->bridge_ocp_retry[i], &out->ocp_retry[i])) return false;
    }
    if (!hvpak_read_field(s_state.desc->bridge_predriver, &value)) return false;
    out->predriver_enabled = value != 0;
    if (!hvpak_read_field(s_state.desc->bridge_full_bridge, &value)) return false;
    out->full_bridge_enabled = value != 0;
    if (!hvpak_read_field(s_state.desc->bridge_control_sel, &value)) return false;
    out->control_selection_ph_en = value != 0;
    if (!hvpak_read_field(s_state.desc->bridge_ocp_deglitch, &value)) return false;
    out->ocp_deglitch_enabled = value != 0;
    if (!hvpak_read_field(s_state.desc->bridge_uvlo, &value)) return false;
    out->uvlo_enabled = value == s_state.desc->bridge_uvlo_enable_value;
    return true;
}

bool bb_hvpak_set_bridge(const BbHvpakBridgeConfig *cfg)
{
    if (!cfg || !hvpak_require_ready()) return false;
    uint8_t uvlo = cfg->uvlo_enabled ? s_state.desc->bridge_uvlo_enable_value : 0;
    for (int i = 0; i < 2; i++) {
        if (cfg->output_mode[i] > 3 || cfg->ocp_retry[i] > 7) {
            hvpak_set_error(BB_HVPAK_ERR_INVALID_ARGUMENT);
            return false;
        }
        if (!hvpak_write_field(s_state.desc->bridge_output_mode[i], cfg->output_mode[i])) return false;
        if (!hvpak_write_field(s_state.desc->bridge_ocp_retry[i], cfg->ocp_retry[i])) return false;
    }
    if (!hvpak_write_field(s_state.desc->bridge_predriver, cfg->predriver_enabled ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->bridge_full_bridge, cfg->full_bridge_enabled ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->bridge_control_sel, cfg->control_selection_ph_en ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->bridge_ocp_deglitch, cfg->ocp_deglitch_enabled ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->bridge_uvlo, uvlo)) return false;
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

bool bb_hvpak_get_analog(BbHvpakAnalogConfig *out)
{
    uint8_t value = 0;
    if (!out || !hvpak_require_ready()) return false;
    memset(out, 0, sizeof(*out));
    if (!hvpak_read_field(s_state.desc->analog_vref_mode, &out->vref_mode)) return false;
    if (!hvpak_read_field(s_state.desc->analog_vref_power, &value)) return false;
    out->vref_powered = value != 0;
    if (!hvpak_read_field(s_state.desc->analog_vref_pd_sel, &value)) return false;
    out->vref_power_from_matrix = value != 0;
    if (!hvpak_read_field(s_state.desc->analog_vref_sink_current, &value)) return false;
    out->vref_sink_12ua = value != 0;
    if (!hvpak_read_field(s_state.desc->analog_vref_input_sel, &out->vref_input_selection)) return false;
    if (!hvpak_read_field(s_state.desc->analog_current_sense_vref, &out->current_sense_vref)) return false;
    if (!hvpak_read_field(s_state.desc->analog_current_sense_source, &value)) return false;
    out->current_sense_dynamic_from_pwm = value != 0;
    if (!hvpak_read_field(s_state.desc->analog_current_sense_gain, &out->current_sense_gain)) return false;
    if (!hvpak_read_field(s_state.desc->analog_current_sense_invert, &value)) return false;
    out->current_sense_invert = value != 0;
    if (!hvpak_read_field(s_state.desc->analog_current_sense_enable, &value)) return false;
    out->current_sense_enabled = value != 0;
    if (!hvpak_read_field(s_state.desc->analog_acmp0_gain, &out->acmp0_gain)) return false;
    if (!hvpak_read_field(s_state.desc->analog_acmp0_vref, &out->acmp0_vref)) return false;
    out->has_acmp1 = (s_state.desc->caps.flags & BB_HVPAK_CAP_ACMP1) != 0;
    if (out->has_acmp1) {
        if (!hvpak_read_field(s_state.desc->analog_acmp1_gain, &out->acmp1_gain)) return false;
        if (!hvpak_read_field(s_state.desc->analog_acmp1_vref, &out->acmp1_vref)) return false;
    }
    return true;
}

bool bb_hvpak_set_analog(const BbHvpakAnalogConfig *cfg)
{
    if (!cfg || !hvpak_require_ready()) return false;
    if (cfg->vref_mode > 7 || cfg->vref_input_selection > 3 || cfg->current_sense_vref > 63 ||
        cfg->current_sense_gain > 1 || cfg->acmp0_gain > 3 || cfg->acmp0_vref > 63) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_ARGUMENT);
        return false;
    }
    if (cfg->has_acmp1 && ((s_state.desc->caps.flags & BB_HVPAK_CAP_ACMP1) == 0 || cfg->acmp1_gain > 3 || cfg->acmp1_vref > 63)) {
        hvpak_set_error(BB_HVPAK_ERR_UNSUPPORTED_CAPABILITY);
        return false;
    }
    if (!hvpak_write_field(s_state.desc->analog_vref_mode, cfg->vref_mode)) return false;
    if (!hvpak_write_field(s_state.desc->analog_vref_power, cfg->vref_powered ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->analog_vref_pd_sel, cfg->vref_power_from_matrix ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->analog_vref_sink_current, cfg->vref_sink_12ua ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->analog_vref_input_sel, cfg->vref_input_selection)) return false;
    if (!hvpak_write_field(s_state.desc->analog_current_sense_vref, cfg->current_sense_vref)) return false;
    if (!hvpak_write_field(s_state.desc->analog_current_sense_source, cfg->current_sense_dynamic_from_pwm ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->analog_current_sense_gain, cfg->current_sense_gain)) return false;
    if (!hvpak_write_field(s_state.desc->analog_current_sense_invert, cfg->current_sense_invert ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->analog_current_sense_enable, cfg->current_sense_enabled ? 1 : 0)) return false;
    if (!hvpak_write_field(s_state.desc->analog_acmp0_gain, cfg->acmp0_gain)) return false;
    if (!hvpak_write_field(s_state.desc->analog_acmp0_vref, cfg->acmp0_vref)) return false;
    if (cfg->has_acmp1 && (s_state.desc->caps.flags & BB_HVPAK_CAP_ACMP1)) {
        if (!hvpak_write_field(s_state.desc->analog_acmp1_gain, cfg->acmp1_gain)) return false;
        if (!hvpak_write_field(s_state.desc->analog_acmp1_vref, cfg->acmp1_vref)) return false;
    }
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

bool bb_hvpak_get_pwm(uint8_t index, BbHvpakPwmConfig *out)
{
    const HvpakPwmLayout *layout;
    uint8_t value = 0;
    if (!out || !hvpak_require_ready()) return false;
    if (index >= s_state.desc->caps.pwm_count) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_INDEX);
        return false;
    }
    layout = &s_state.desc->pwm[index];
    memset(out, 0, sizeof(*out));
    out->index = index;
    if (!hvpak_read_byte(layout->initial_value_addr, &out->initial_value)) return false;
    if (!hvpak_read_byte(layout->current_value_addr, &out->current_value)) return false;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->resolution), &value)) return false;
    out->resolution_7bit = value != 0;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->out_plus_invert), &value)) return false;
    out->out_plus_inverted = value != 0;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->out_minus_invert), &value)) return false;
    out->out_minus_inverted = value != 0;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->sync), &value)) return false;
    out->async_powerdown = value != 0;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->autostop), &value)) return false;
    out->autostop_mode = value != 0;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->boundary_disable), &value)) return false;
    out->boundary_osc_disable = value != 0;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->phase_correct), &value)) return false;
    out->phase_correct = value != 0;
    if (!hvpak_read_field(layout->deadband, &out->deadband)) return false;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->keep_stop), &value)) return false;
    out->stop_mode = value != 0;
    if (!hvpak_read_field(HVPAK_ONEBIT_FIELD(layout->i2c_trigger), &value)) return false;
    out->i2c_trigger = value != 0;
    if (!hvpak_read_field(layout->duty_source, &out->duty_source)) return false;
    if (!hvpak_read_field(layout->period_clock_source, &out->period_clock_source)) return false;
    if (!hvpak_read_field(layout->duty_clock_source, &out->duty_clock_source)) return false;
    return true;
}

bool bb_hvpak_set_pwm(const BbHvpakPwmConfig *cfg)
{
    const HvpakPwmLayout *layout;
    if (!cfg || !hvpak_require_ready()) return false;
    if (cfg->index >= s_state.desc->caps.pwm_count) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_INDEX);
        return false;
    }
    if (cfg->deadband > 3 || cfg->duty_source > 3 || cfg->period_clock_source > 11 || cfg->duty_clock_source > 3) {
        hvpak_set_error(BB_HVPAK_ERR_INVALID_ARGUMENT);
        return false;
    }
    layout = &s_state.desc->pwm[cfg->index];
    if (!hvpak_write_byte(layout->initial_value_addr, cfg->initial_value)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->resolution), cfg->resolution_7bit ? 1 : 0)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->out_plus_invert), cfg->out_plus_inverted ? 1 : 0)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->out_minus_invert), cfg->out_minus_inverted ? 1 : 0)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->sync), cfg->async_powerdown ? 1 : 0)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->autostop), cfg->autostop_mode ? 1 : 0)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->boundary_disable), cfg->boundary_osc_disable ? 1 : 0)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->phase_correct), cfg->phase_correct ? 1 : 0)) return false;
    if (!hvpak_write_field(layout->deadband, cfg->deadband)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->keep_stop), cfg->stop_mode ? 1 : 0)) return false;
    if (!hvpak_write_field(layout->duty_source, cfg->duty_source)) return false;
    if (!hvpak_write_field(layout->period_clock_source, cfg->period_clock_source)) return false;
    if (!hvpak_write_field(layout->duty_clock_source, cfg->duty_clock_source)) return false;
    if (!hvpak_write_field(HVPAK_ONEBIT_FIELD(layout->i2c_trigger), cfg->i2c_trigger ? 1 : 0)) return false;
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}

bool bb_hvpak_reg_read(uint8_t addr, uint8_t *value)
{
    if (!value || !hvpak_require_ready()) return false;
    return hvpak_read_byte(addr, value);
}

bool bb_hvpak_reg_write_masked(uint8_t addr, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    uint8_t next;
    if (!hvpak_require_ready()) return false;
    if (addr == BB_HVPAK_IDENTITY_REG || addr == BB_HVPAK_COMMAND_REG || addr == 0xF5 || addr == 0xF6) {
        hvpak_set_error(BB_HVPAK_ERR_UNSAFE_REGISTER);
        return false;
    }
    if (!hvpak_is_runtime_safe_write_addr(addr)) {
        hvpak_set_error(BB_HVPAK_ERR_UNSAFE_REGISTER);
        return false;
    }
    if (!hvpak_read_byte(addr, &current)) return false;
    next = (uint8_t)((current & ~mask) | (value & mask));
    if (!hvpak_write_byte(addr, next)) return false;
    hvpak_set_error(BB_HVPAK_ERR_NONE);
    return true;
}
