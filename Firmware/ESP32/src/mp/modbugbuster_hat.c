// =============================================================================
// modbugbuster_hat.c — MicroPython HAT v2 bindings.
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/binary.h"

#include <string.h>

#include "modbugbuster_bridge.h"

static void raise_hat_error(void)
{
    mp_raise_OSError(MP_EIO);
}

static mp_obj_t dict_from_status(const bugbuster_mp_hat_status_t *st)
{
    mp_obj_t d = mp_obj_new_dict(21);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_detected), mp_obj_new_bool(st->detected));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_connected), mp_obj_new_bool(st->connected));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_type), mp_obj_new_int(st->type));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_detect_voltage), mp_obj_new_float(st->detect_voltage));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_fw_major), mp_obj_new_int(st->fw_major));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_fw_minor), mp_obj_new_int(st->fw_minor));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_config_confirmed), mp_obj_new_bool(st->config_confirmed));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_io_voltage_mv), mp_obj_new_int(st->io_voltage_mv));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_caps_valid), mp_obj_new_bool(st->caps_valid));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_la_route), mp_obj_new_int(st->la_route));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_dap_connected), mp_obj_new_bool(st->dap_connected));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_target_detected), mp_obj_new_bool(st->target_detected));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_target_dpidr), mp_obj_new_int_from_uint(st->target_dpidr));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_last_ok_ms), mp_obj_new_int_from_uint(st->last_ok_ms));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_last_timeout_ms), mp_obj_new_int_from_uint(st->last_timeout_ms));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_consecutive_timeouts), mp_obj_new_int(st->consecutive_timeouts));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_degraded), mp_obj_new_bool(st->degraded));

    mp_obj_t pins = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < 4; i++) mp_obj_list_append(pins, mp_obj_new_int(st->pin_config[i]));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_pin_config), pins);
    return d;
}

static mp_obj_t bugbuster_hat_status(void)
{
    bugbuster_mp_hat_status_t st = {0};
    if (!bugbuster_mp_hat_status(&st)) raise_hat_error();
    return dict_from_status(&st);
}
MP_DEFINE_CONST_FUN_OBJ_0(bugbuster_hat_status_obj, bugbuster_hat_status);

static mp_obj_t bugbuster_hat_caps(void)
{
    bugbuster_mp_hat_caps_t caps = {0};
    if (!bugbuster_mp_hat_caps(&caps)) raise_hat_error();
    mp_obj_t d = mp_obj_new_dict(9);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_hw_revision), mp_obj_new_int(caps.hw_revision));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_flags), mp_obj_new_int_from_uint(caps.flags));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rail_count), mp_obj_new_int(caps.rail_count));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_led_count), mp_obj_new_int(caps.led_count));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_shifted_io_count), mp_obj_new_int(caps.shifted_io_count));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_la_routes), mp_obj_new_int(caps.la_routes));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_fw_major), mp_obj_new_int(caps.fw_major));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_fw_minor), mp_obj_new_int(caps.fw_minor));
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_0(bugbuster_hat_caps_obj, bugbuster_hat_caps);

static mp_obj_t bugbuster_hat_rails(void)
{
    bugbuster_mp_hat_rail_t rails[3] = {0};
    size_t count = 0;
    if (!bugbuster_mp_hat_rails(rails, 3, &count)) raise_hat_error();
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count; i++) {
        mp_obj_t d = mp_obj_new_dict(6);
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rail_id), mp_obj_new_int(rails[i].rail_id));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_enabled), mp_obj_new_bool(rails[i].enabled));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_voltage_mv), mp_obj_new_int(rails[i].voltage_mv));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_current_ma), mp_obj_new_int(rails[i].current_ma));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_status), mp_obj_new_int(rails[i].status));
        mp_obj_list_append(list, d);
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(bugbuster_hat_rails_obj, bugbuster_hat_rails);

static mp_obj_t bugbuster_hat_set_rail_enable(mp_obj_t rail_in, mp_obj_t enable_in)
{
    mp_int_t rail = mp_obj_get_int(rail_in);
    if (rail < 0 || rail > 2) mp_raise_ValueError(MP_ERROR_TEXT("rail_id must be 0..2"));
    if (!bugbuster_mp_hat_set_rail_enable((uint8_t)rail, mp_obj_is_true(enable_in))) raise_hat_error();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_hat_set_rail_enable_obj, bugbuster_hat_set_rail_enable);

static mp_obj_t bugbuster_hat_set_rail_voltage(mp_obj_t rail_in, mp_obj_t mv_in)
{
    mp_int_t rail = mp_obj_get_int(rail_in);
    mp_int_t mv = mp_obj_get_int(mv_in);
    if (rail < 0 || rail > 2) mp_raise_ValueError(MP_ERROR_TEXT("rail_id must be 0..2"));
    if (mv < 0 || mv > 36000) mp_raise_ValueError(MP_ERROR_TEXT("voltage_mv must be 0..36000"));
    if (!bugbuster_mp_hat_set_rail_voltage((uint8_t)rail, (uint16_t)mv)) raise_hat_error();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_hat_set_rail_voltage_obj, bugbuster_hat_set_rail_voltage);

static mp_obj_t bugbuster_hat_led(mp_obj_t led_in, mp_obj_t color_in)
{
    mp_int_t led = mp_obj_get_int(led_in);
    mp_int_t color = mp_obj_get_int(color_in);
    if (led < 1 || led > 8) mp_raise_ValueError(MP_ERROR_TEXT("led_id must be 1..8"));
    if (color < 0 || color > 255) mp_raise_ValueError(MP_ERROR_TEXT("color_code must be 0..255"));
    if (!bugbuster_mp_hat_led((uint8_t)led, (uint8_t)color)) raise_hat_error();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_hat_led_obj, bugbuster_hat_led);

static mp_obj_t bugbuster_hat_io_bank(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_dirs, ARG_ups, ARG_dns, ARG_vals };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_dirs, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_ups,  MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_dns,  MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_vals, MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    int dirs = args[ARG_dirs].u_int;
    int ups = args[ARG_ups].u_int;
    int dns = args[ARG_dns].u_int;
    int vals = args[ARG_vals].u_int;
    if ((dirs | ups | dns | vals) & ~0xFF) mp_raise_ValueError(MP_ERROR_TEXT("masks must be 0..255"));
    if (!bugbuster_mp_hat_io_bank((uint8_t)dirs, (uint8_t)ups, (uint8_t)dns, (uint8_t)vals)) raise_hat_error();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_hat_io_bank_obj, 1, bugbuster_hat_io_bank);

static mp_obj_t bugbuster_hat_level_shift(mp_obj_t oe_in, mp_obj_t dir_in)
{
    bool oe_out = false;
    bool dir_out = false;
    if (!bugbuster_mp_hat_level_shift(mp_obj_is_true(oe_in), mp_obj_is_true(dir_in), &oe_out, &dir_out)) raise_hat_error();
    mp_obj_t d = mp_obj_new_dict(2);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_oe), mp_obj_new_bool(oe_out));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_dir), mp_obj_new_bool(dir_out));
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_hat_level_shift_obj, bugbuster_hat_level_shift);

static mp_obj_t bugbuster_hat_calibrate_start(mp_obj_t rail_in)
{
    mp_int_t rail = mp_obj_get_int(rail_in);
    if (rail < 0 || rail > 2) mp_raise_ValueError(MP_ERROR_TEXT("rail_id must be 0..2"));
    uint8_t status = 0;
    if (!bugbuster_mp_hat_calibrate_start((uint8_t)rail, &status)) raise_hat_error();
    return mp_obj_new_int(status);
}
MP_DEFINE_CONST_FUN_OBJ_1(bugbuster_hat_calibrate_start_obj, bugbuster_hat_calibrate_start);

static mp_obj_t bugbuster_hat_calibrate_status(void)
{
    bugbuster_mp_hat_cal_status_t st = {0};
    if (!bugbuster_mp_hat_calibrate_status(&st)) raise_hat_error();
    mp_obj_t d = mp_obj_new_dict(14);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_state), mp_obj_new_int(st.state));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_progress), mp_obj_new_int(st.progress));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rail_id), mp_obj_new_int(st.rail_id));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_last_error), mp_obj_new_int(st.last_error));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_persist_state), mp_obj_new_int(st.persist_state));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_stage), mp_obj_new_int(st.stage));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_point), mp_obj_new_int(st.point));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_code), mp_obj_new_int(st.code));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_measured_mv), mp_obj_new_int(st.measured_mv));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_min_mv), mp_obj_new_int(st.min_mv));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_max_mv), mp_obj_new_int(st.max_mv));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_max_gap_mv), mp_obj_new_int(st.max_gap_mv));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_max_error_mv), mp_obj_new_int(st.max_error_mv));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_validation_flags), mp_obj_new_int(st.validation_flags));
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_0(bugbuster_hat_calibrate_status_obj, bugbuster_hat_calibrate_status);

static mp_obj_t bugbuster_hat_calibrate_import(mp_obj_t rail_in, mp_obj_t points_in)
{
    mp_int_t rail = mp_obj_get_int(rail_in);
    if (rail < 0 || rail > 2) mp_raise_ValueError(MP_ERROR_TEXT("rail_id must be 0..2"));

    size_t n = 0;
    mp_obj_t *items = NULL;
    mp_obj_get_array(points_in, &n, &items);
    if (n < 2 || n > 6) mp_raise_ValueError(MP_ERROR_TEXT("points must contain 2..6 items"));

    uint8_t packed[30] = {0};
    for (size_t i = 0; i < n; i++) {
        size_t pair_n = 0;
        mp_obj_t *pair = NULL;
        mp_obj_get_array(items[i], &pair_n, &pair);
        if (pair_n != 2) mp_raise_ValueError(MP_ERROR_TEXT("point must be (dac_code, measured_v)"));
        mp_int_t code = mp_obj_get_int(pair[0]);
        if (code < -128 || code > 127) mp_raise_ValueError(MP_ERROR_TEXT("dac_code must be -128..127"));
        float measured = (float)mp_obj_get_float(pair[1]);
        packed[i * 5] = (uint8_t)(int8_t)code;
        memcpy(&packed[i * 5 + 1], &measured, sizeof(float));
    }

    if (!bugbuster_mp_hat_calibrate_import((uint8_t)rail, (uint8_t)n, packed, n * 5)) raise_hat_error();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_hat_calibrate_import_obj, bugbuster_hat_calibrate_import);

static mp_obj_t bugbuster_hat_setup_swd(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_target_voltage_mv, ARG_connector };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_target_voltage_mv, MP_ARG_INT, {.u_int = 3300} },
        { MP_QSTR_connector, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    int mv = args[ARG_target_voltage_mv].u_int;
    int connector = args[ARG_connector].u_int;
    if (mv < 1200 || mv > 5500) mp_raise_ValueError(MP_ERROR_TEXT("target_voltage_mv must be 1200..5500"));
    if (connector < 0 || connector > 1) mp_raise_ValueError(MP_ERROR_TEXT("connector must be 0 or 1"));
    if (!bugbuster_mp_hat_setup_swd((uint16_t)mv, (uint8_t)connector)) raise_hat_error();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_hat_setup_swd_obj, 0, bugbuster_hat_setup_swd);
