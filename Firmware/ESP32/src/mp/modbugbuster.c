// =============================================================================
// modbugbuster.c — MicroPython bugbuster module skeleton.
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"

#include <string.h>

#include "ad74416h_regs.h"
#include "modbugbuster_bridge.h"

extern const mp_obj_type_t bugbuster_channel_type;
extern const mp_obj_type_t bugbuster_i2c_type;
extern const mp_obj_type_t bugbuster_spi_type;
extern const mp_obj_type_t bugbuster_claim_type;

// V2-E — network function objects defined in modbugbuster_net.c
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_http_get_obj);
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_http_post_obj);
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_mqtt_publish_obj);

// PR-5 — IO-ownership function objects defined in modbugbuster_owner.c
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_claim_fn_obj);
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_release_fn_obj);
MP_DECLARE_CONST_FUN_OBJ_0(bugbuster_owner_status_fn_obj);

// HAT v2 function objects defined in modbugbuster_hat.c
MP_DECLARE_CONST_FUN_OBJ_0(bugbuster_hat_status_obj);
MP_DECLARE_CONST_FUN_OBJ_0(bugbuster_hat_caps_obj);
MP_DECLARE_CONST_FUN_OBJ_0(bugbuster_hat_rails_obj);
MP_DECLARE_CONST_FUN_OBJ_2(bugbuster_hat_set_rail_enable_obj);
MP_DECLARE_CONST_FUN_OBJ_2(bugbuster_hat_set_rail_voltage_obj);
MP_DECLARE_CONST_FUN_OBJ_2(bugbuster_hat_led_obj);
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_hat_io_bank_obj);
MP_DECLARE_CONST_FUN_OBJ_2(bugbuster_hat_level_shift_obj);
MP_DECLARE_CONST_FUN_OBJ_1(bugbuster_hat_calibrate_start_obj);
MP_DECLARE_CONST_FUN_OBJ_0(bugbuster_hat_calibrate_status_obj);
MP_DECLARE_CONST_FUN_OBJ_2(bugbuster_hat_calibrate_import_obj);
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_hat_setup_swd_obj);

static mp_obj_t bugbuster_sleep(mp_obj_t ms_in)
{
    mp_int_t ms = mp_obj_get_int(ms_in);
    if (ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("sleep ms must be >= 0"));
    }
    mp_hal_delay_ms((mp_uint_t)ms);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bugbuster_sleep_obj, bugbuster_sleep);

static mp_obj_t bugbuster_vadj_pd_warning(mp_obj_t rail_in, mp_obj_t voltage_in)
{
    mp_int_t rail = mp_obj_get_int(rail_in);
    mp_float_t voltage = mp_obj_get_float(voltage_in);
    if (rail != 1 && rail != 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("rail must be 1 or 2"));
    }
    char warning[384] = {0};
    if (bugbuster_mp_vadj_pd_warning((uint8_t)rail, (float)voltage, warning, sizeof(warning))) {
        return mp_obj_new_str(warning, strlen(warning));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_vadj_pd_warning_obj, bugbuster_vadj_pd_warning);

static const mp_rom_map_elem_t bugbuster_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_bugbuster) },
    { MP_ROM_QSTR(MP_QSTR_sleep), MP_ROM_PTR(&bugbuster_sleep_obj) },
    { MP_ROM_QSTR(MP_QSTR_vadj_pd_warning), MP_ROM_PTR(&bugbuster_vadj_pd_warning_obj) },
    { MP_ROM_QSTR(MP_QSTR_Channel), MP_ROM_PTR(&bugbuster_channel_type) },
    { MP_ROM_QSTR(MP_QSTR_I2C), MP_ROM_PTR(&bugbuster_i2c_type) },
    { MP_ROM_QSTR(MP_QSTR_SPI), MP_ROM_PTR(&bugbuster_spi_type) },
    // V2-E — network bindings
    { MP_ROM_QSTR(MP_QSTR_http_get),      MP_ROM_PTR(&bugbuster_http_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_http_post),     MP_ROM_PTR(&bugbuster_http_post_obj) },
    { MP_ROM_QSTR(MP_QSTR_mqtt_publish),  MP_ROM_PTR(&bugbuster_mqtt_publish_obj) },
    // PR-5 — IO-ownership bindings
    { MP_ROM_QSTR(MP_QSTR_claim),        MP_ROM_PTR(&bugbuster_claim_fn_obj) },
    { MP_ROM_QSTR(MP_QSTR_release),      MP_ROM_PTR(&bugbuster_release_fn_obj) },
    { MP_ROM_QSTR(MP_QSTR_owner_status), MP_ROM_PTR(&bugbuster_owner_status_fn_obj) },
    { MP_ROM_QSTR(MP_QSTR_Claim),        MP_ROM_PTR(&bugbuster_claim_type) },

    // HAT v2 bindings
    { MP_ROM_QSTR(MP_QSTR_hat_status),              MP_ROM_PTR(&bugbuster_hat_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_caps),                MP_ROM_PTR(&bugbuster_hat_caps_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_rails),               MP_ROM_PTR(&bugbuster_hat_rails_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_set_rail_enable),     MP_ROM_PTR(&bugbuster_hat_set_rail_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_set_rail_voltage),    MP_ROM_PTR(&bugbuster_hat_set_rail_voltage_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_led),                 MP_ROM_PTR(&bugbuster_hat_led_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_io_bank),             MP_ROM_PTR(&bugbuster_hat_io_bank_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_level_shift),         MP_ROM_PTR(&bugbuster_hat_level_shift_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_calibrate_start),     MP_ROM_PTR(&bugbuster_hat_calibrate_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_calibrate_status),    MP_ROM_PTR(&bugbuster_hat_calibrate_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_calibrate_import),    MP_ROM_PTR(&bugbuster_hat_calibrate_import_obj) },
    { MP_ROM_QSTR(MP_QSTR_hat_setup_swd),           MP_ROM_PTR(&bugbuster_hat_setup_swd_obj) },

    { MP_ROM_QSTR(MP_QSTR_HAT_RAIL_3V3_ADJ), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_HAT_RAIL_VADJ3),   MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_HAT_RAIL_VADJ4),   MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_HAT_LED_OFF),      MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_HAT_LED_RED),      MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_HAT_LED_GREEN),    MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_HAT_LED_BLUE),     MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_HAT_LED_YELLOW),   MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_HAT_LED_CYAN),     MP_ROM_INT(5) },
    { MP_ROM_QSTR(MP_QSTR_HAT_LED_MAGENTA),  MP_ROM_INT(6) },
    { MP_ROM_QSTR(MP_QSTR_HAT_LED_WHITE),    MP_ROM_INT(7) },

    { MP_ROM_QSTR(MP_QSTR_FUNC_HIGH_IMP), MP_ROM_INT(CH_FUNC_HIGH_IMP) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_VOUT), MP_ROM_INT(CH_FUNC_VOUT) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_IOUT), MP_ROM_INT(CH_FUNC_IOUT) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_VIN), MP_ROM_INT(CH_FUNC_VIN) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_IIN_EXT_PWR), MP_ROM_INT(CH_FUNC_IIN_EXT_PWR) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_IIN_LOOP_PWR), MP_ROM_INT(CH_FUNC_IIN_LOOP_PWR) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_RES_MEAS), MP_ROM_INT(CH_FUNC_RES_MEAS) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_DIN_LOGIC), MP_ROM_INT(CH_FUNC_DIN_LOGIC) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_DIN_LOOP), MP_ROM_INT(CH_FUNC_DIN_LOOP) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_IOUT_HART), MP_ROM_INT(CH_FUNC_IOUT_HART) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_IIN_EXT_PWR_HART), MP_ROM_INT(CH_FUNC_IIN_EXT_PWR_HART) },
    { MP_ROM_QSTR(MP_QSTR_FUNC_IIN_LOOP_PWR_HART), MP_ROM_INT(CH_FUNC_IIN_LOOP_PWR_HART) },
};

static MP_DEFINE_CONST_DICT(bugbuster_module_globals, bugbuster_module_globals_table);

const mp_obj_module_t mp_module_bugbuster = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&bugbuster_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_bugbuster, mp_module_bugbuster);
