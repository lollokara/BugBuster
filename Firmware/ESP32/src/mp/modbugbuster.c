// =============================================================================
// modbugbuster.c — MicroPython bugbuster module skeleton.
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"

#include "ad74416h_regs.h"

extern const mp_obj_type_t bugbuster_channel_type;
extern const mp_obj_type_t bugbuster_i2c_type;
extern const mp_obj_type_t bugbuster_spi_type;

// V2-E — network function objects defined in modbugbuster_net.c
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_http_get_obj);
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_http_post_obj);
MP_DECLARE_CONST_FUN_OBJ_KW(bugbuster_mqtt_publish_obj);

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

static const mp_rom_map_elem_t bugbuster_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_bugbuster) },
    { MP_ROM_QSTR(MP_QSTR_sleep), MP_ROM_PTR(&bugbuster_sleep_obj) },
    { MP_ROM_QSTR(MP_QSTR_Channel), MP_ROM_PTR(&bugbuster_channel_type) },
    { MP_ROM_QSTR(MP_QSTR_I2C), MP_ROM_PTR(&bugbuster_i2c_type) },
    { MP_ROM_QSTR(MP_QSTR_SPI), MP_ROM_PTR(&bugbuster_spi_type) },
    // V2-E — network bindings
    { MP_ROM_QSTR(MP_QSTR_http_get),      MP_ROM_PTR(&bugbuster_http_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_http_post),     MP_ROM_PTR(&bugbuster_http_post_obj) },
    { MP_ROM_QSTR(MP_QSTR_mqtt_publish),  MP_ROM_PTR(&bugbuster_mqtt_publish_obj) },

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
