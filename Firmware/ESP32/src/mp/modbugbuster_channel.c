// =============================================================================
// modbugbuster_channel.c — MicroPython bugbuster.Channel binding.
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"

#include "ad74416h_regs.h"
#include "modbugbuster_bridge.h"

typedef struct _bugbuster_channel_obj_t {
    mp_obj_base_t base;
    uint8_t channel;
} bugbuster_channel_obj_t;

static bool bugbuster_is_valid_channel_function(mp_int_t func)
{
    switch (func) {
        case CH_FUNC_HIGH_IMP:
        case CH_FUNC_VOUT:
        case CH_FUNC_IOUT:
        case CH_FUNC_VIN:
        case CH_FUNC_IIN_EXT_PWR:
        case CH_FUNC_IIN_LOOP_PWR:
        case CH_FUNC_RES_MEAS:
        case CH_FUNC_DIN_LOGIC:
        case CH_FUNC_DIN_LOOP:
        case CH_FUNC_IOUT_HART:
        case CH_FUNC_IIN_EXT_PWR_HART:
        case CH_FUNC_IIN_LOOP_PWR_HART:
            return true;
        default:
            return false;
    }
}

static mp_obj_t bugbuster_channel_make_new(const mp_obj_type_t *type,
                                           size_t n_args,
                                           size_t n_kw,
                                           const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    mp_int_t channel = mp_obj_get_int(args[0]);
    if (channel < 0 || channel >= 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel must be 0..3"));
    }

    bugbuster_channel_obj_t *self = mp_obj_malloc(bugbuster_channel_obj_t, type);
    self->channel = (uint8_t)channel;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t bugbuster_channel_set_function(mp_obj_t self_in, mp_obj_t func_in)
{
    bugbuster_channel_obj_t *self = (bugbuster_channel_obj_t *)MP_OBJ_TO_PTR(self_in);
    mp_int_t func = mp_obj_get_int(func_in);
    if (!bugbuster_is_valid_channel_function(func)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid channel function"));
    }
    if (!bugbuster_mp_channel_set_function(self->channel, (int)func)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_channel_set_function_obj, bugbuster_channel_set_function);

static mp_obj_t bugbuster_channel_set_voltage(size_t n_args, const mp_obj_t *args)
{
    bugbuster_channel_obj_t *self = (bugbuster_channel_obj_t *)MP_OBJ_TO_PTR(args[0]);
    mp_float_t voltage = mp_obj_get_float(args[1]);
    bool bipolar = (n_args >= 3) ? mp_obj_is_true(args[2]) : false;

    if (!bugbuster_mp_channel_set_voltage(self->channel, (float)voltage, bipolar)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(bugbuster_channel_set_voltage_obj, 2, 3, bugbuster_channel_set_voltage);

static mp_obj_t bugbuster_channel_read_voltage(mp_obj_t self_in)
{
    bugbuster_channel_obj_t *self = (bugbuster_channel_obj_t *)MP_OBJ_TO_PTR(self_in);
    float value = 0.0f;

    if (!bugbuster_mp_channel_read_voltage(self->channel, &value)) {
        mp_raise_OSError(MP_ETIMEDOUT);
    }
    return mp_obj_new_float(value);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bugbuster_channel_read_voltage_obj, bugbuster_channel_read_voltage);

static mp_obj_t bugbuster_channel_set_do(mp_obj_t self_in, mp_obj_t value_in)
{
    bugbuster_channel_obj_t *self = (bugbuster_channel_obj_t *)MP_OBJ_TO_PTR(self_in);

    if (!bugbuster_mp_channel_set_do(self->channel, mp_obj_is_true(value_in))) {
        mp_raise_OSError(MP_EBUSY);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_channel_set_do_obj, bugbuster_channel_set_do);

static const mp_rom_map_elem_t bugbuster_channel_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_set_function), MP_ROM_PTR(&bugbuster_channel_set_function_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_voltage), MP_ROM_PTR(&bugbuster_channel_set_voltage_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_voltage), MP_ROM_PTR(&bugbuster_channel_read_voltage_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_do), MP_ROM_PTR(&bugbuster_channel_set_do_obj) },
};

static MP_DEFINE_CONST_DICT(bugbuster_channel_locals_dict, bugbuster_channel_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    bugbuster_channel_type,
    MP_QSTR_Channel,
    MP_TYPE_FLAG_NONE,
    make_new, bugbuster_channel_make_new,
    locals_dict, &bugbuster_channel_locals_dict
    );
