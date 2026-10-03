// =============================================================================
// moddaq.c — MicroPython `daq` module: DAQ HAT VDUT + battery simulator
// (spec 2026-10-03 §3). Every call needs a connected DAQ HAT, else
// OSError(ENODEV). Wire formats live in daq_codec.c (host-tested).
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"

#include <string.h>

#include "daq_bridge.h"
#include "daq_codec.h"
#include "scripting.h"

#define BS_RSP_MAX 240

static void require_daq(void)
{
    if (!daq_mp_present()) mp_raise_OSError(MP_ENODEV);
}

static mp_obj_t new_float(float f) { return mp_obj_new_float((mp_float_t)f); }

static void dict_put(mp_obj_t d, qstr k, mp_obj_t v)
{
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(k), v);
}

static int64_t scaled(mp_obj_t v, mp_float_t scale)
{
    mp_float_t f = mp_obj_get_float(v) * scale;
    return (int64_t)(f < 0 ? f - (mp_float_t)0.5 : f + (mp_float_t)0.5);
}

// --- battery simulator transport -------------------------------------------
static int bs_req(const uint8_t *req, uint8_t len, uint8_t *rsp)
{
    int n = daq_mp_bs(req, len, rsp, BS_RSP_MAX);
    if (n == -2) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("battery simulator rejected the request"));
    if (n < 0) mp_raise_OSError(MP_EIO);
    return n;
}

static void bs_status(daqc_bs_status_t *st)
{
    uint8_t rsp[BS_RSP_MAX];
    uint8_t req = DAQC_BS_OP_STATUS;
    int n = bs_req(&req, 1, rsp);
    if (!daqc_bs_status_decode(rsp, (size_t)n, st)) mp_raise_OSError(MP_EIO);
}

static bool bs_meta(uint16_t run, daqc_bs_meta_t *m)
{
    uint8_t req[10], rsp[BS_RSP_MAX];
    daqc_read_request(req, run, DAQC_BS_FILE_META, 0, DAQC_META_SIZE);
    int n = daq_mp_bs(req, sizeof(req), rsp, BS_RSP_MAX);
    return n >= DAQC_META_SIZE && daqc_bs_meta_decode(rsp, (size_t)n, m);
}

static void raise_bs(const char *what)
{
    daqc_bs_status_t st;
    uint8_t rsp[BS_RSP_MAX];
    uint8_t req = DAQC_BS_OP_STATUS;
    int n = daq_mp_bs(&req, 1, rsp, BS_RSP_MAX);
    const char *why = (n > 0 && daqc_bs_status_decode(rsp, (size_t)n, &st))
                      ? daqc_error_name(st.last_error) : "no reply";
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s: %s"), what, why);
}

static void cfg_int(uint16_t key, int64_t v)
{
    uint8_t tlv[16];
    size_t n = daqc_tlv_int(tlv, sizeof(tlv), key, v);
    if (n == 0) mp_raise_ValueError(MP_ERROR_TEXT("battery parameter out of range"));
    if (!daq_mp_cfg_set(tlv, (uint8_t)n)) raise_bs("setting rejected");
}

static void action(uint8_t act, const char *what)
{
    if (!daq_mp_cfg_action(act)) raise_bs(what);
}

static void ensure_loaded(mp_int_t run_id)
{
    if (run_id < 1 || run_id > 65535) mp_raise_ValueError(MP_ERROR_TEXT("run_id must be 1-65535"));
    daqc_bs_status_t st;
    bs_status(&st);
    if (st.run_id == (uint16_t)run_id) return;
    cfg_int(DAQC_K_BS_RUN_SELECT, run_id);
    action(DAQC_ACT_BS_RUN_LOAD, "run load failed");
}

static int chem_arg(mp_obj_t v)
{
    if (mp_obj_is_str(v)) {
        int c = daqc_chem_from_name(mp_obj_str_get_str(v));
        if (c < 0) mp_raise_ValueError(MP_ERROR_TEXT("chem must be 'lipo', 'lifepo4', 'nimh' or 'lead'"));
        return c;
    }
    mp_int_t c = mp_obj_get_int(v);
    if (c < 0 || c > 3) mp_raise_ValueError(MP_ERROR_TEXT("chem must be 0-3"));
    return (int)c;
}

static void apply_param(qstr k, mp_obj_t v)
{
    switch (k) {
    case MP_QSTR_name: {
        size_t l;
        const char *s = mp_obj_str_get_data(v, &l);
        if (l > DAQC_NAME_MAX) mp_raise_ValueError(MP_ERROR_TEXT("name must be <= 23 bytes"));
        uint8_t tlv[32];
        size_t n = daqc_tlv_str(tlv, sizeof(tlv), DAQC_K_BS_NAME, s);
        if (n == 0 || !daq_mp_cfg_set(tlv, (uint8_t)n)) raise_bs("name rejected");
        break;
    }
    case MP_QSTR_chem:           cfg_int(DAQC_K_BS_CHEM, chem_arg(v)); break;
    case MP_QSTR_cells:          cfg_int(DAQC_K_BS_CELLS, mp_obj_get_int(v)); break;
    case MP_QSTR_capacity_mah:   cfg_int(DAQC_K_BS_CAPACITY_MAH, mp_obj_get_int(v)); break;
    case MP_QSTR_soc:            cfg_int(DAQC_K_BS_START_SOC, scaled(v, 10)); break;
    case MP_QSTR_cutoff_mv_cell: cfg_int(DAQC_K_BS_CUTOFF_MV, mp_obj_get_int(v)); break;
    case MP_QSTR_rint_uohm_cell: cfg_int(DAQC_K_BS_RINT_UOHM, mp_obj_get_int(v)); break;
    case MP_QSTR_peukert:        cfg_int(DAQC_K_BS_PEUKERT, scaled(v, 1000)); break;
    case MP_QSTR_dither:         cfg_int(DAQC_K_BS_DITHER, mp_obj_is_true(v)); break;
    case MP_QSTR_self_discharge:      // %/month; None/0/False disables
        if (v == mp_const_none || !mp_obj_is_true(v)) {
            cfg_int(DAQC_K_BS_SD_ENABLE, 0);
        } else {
            cfg_int(DAQC_K_BS_SD_PCT, scaled(v, 100));
            cfg_int(DAQC_K_BS_SD_ENABLE, 1);
        }
        break;
    case MP_QSTR_ext_load:            // µA; None/0 disables
        if (v == mp_const_none || !mp_obj_is_true(v)) {
            cfg_int(DAQC_K_BS_EXT_ENABLE, 0);
        } else {
            cfg_int(DAQC_K_BS_EXT_UA, mp_obj_get_int(v));
            cfg_int(DAQC_K_BS_EXT_ENABLE, 1);
        }
        break;
    default:
        mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("unknown battery parameter '%q'"), k);
    }
}

static mp_obj_t params_dict(const daqc_bs_params_t *p)
{
    mp_obj_t d = mp_obj_new_dict(10);
    dict_put(d, MP_QSTR_chem, mp_obj_new_str(daqc_chem_name(p->chem), strlen(daqc_chem_name(p->chem))));
    dict_put(d, MP_QSTR_cells, mp_obj_new_int(p->cells));
    dict_put(d, MP_QSTR_capacity_mah, mp_obj_new_int_from_uint(p->capacity_mah));
    dict_put(d, MP_QSTR_soc, new_float(p->start_soc_pct));
    dict_put(d, MP_QSTR_cutoff_mv_cell, mp_obj_new_int(p->cutoff_mv_cell));
    dict_put(d, MP_QSTR_rint_uohm_cell, mp_obj_new_int_from_uint(p->rint_uohm_cell));
    dict_put(d, MP_QSTR_peukert, new_float(p->peukert));
    dict_put(d, MP_QSTR_self_discharge, p->sd_enable ? new_float(p->sd_pct_month) : mp_const_none);
    dict_put(d, MP_QSTR_ext_load, p->ext_enable ? mp_obj_new_int_from_uint(p->ext_load_ua) : mp_const_none);
    dict_put(d, MP_QSTR_dither, mp_obj_new_bool(p->dither));
    return d;
}

// --- daq.* -------------------------------------------------------------------
static mp_obj_t daq_present(void)
{
    return mp_obj_new_bool(daq_mp_present());
}
static MP_DEFINE_CONST_FUN_OBJ_0(daq_present_obj, daq_present);

static mp_obj_t vdut_dict(const daq_mp_vdut_t *st)
{
    mp_obj_t d = mp_obj_new_dict(7);
    dict_put(d, MP_QSTR_present, mp_obj_new_bool(st->present));
    dict_put(d, MP_QSTR_enabled, mp_obj_new_bool(st->enabled));
    dict_put(d, MP_QSTR_fault, mp_obj_new_bool(st->fault));
    dict_put(d, MP_QSTR_volts, new_float(st->set_v));
    dict_put(d, MP_QSTR_amps_limit, new_float(st->ilimit_a));
    dict_put(d, MP_QSTR_v, new_float(st->meas_v));
    dict_put(d, MP_QSTR_i, new_float(st->meas_i));
    return d;
}

static mp_obj_t daq_vdut(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_enable, ARG_volts, ARG_amps_limit };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_enable,     MP_ARG_OBJ,                   {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_volts,      MP_ARG_KW_ONLY | MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_amps_limit, MP_ARG_KW_ONLY | MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);
    require_daq();

    daq_mp_vdut_t st;
    if (!daq_mp_vdut_status(&st)) mp_raise_OSError(MP_EIO);
    if (a[ARG_volts].u_obj != mp_const_none || a[ARG_amps_limit].u_obj != mp_const_none) {
        float v = a[ARG_volts].u_obj != mp_const_none ? (float)mp_obj_get_float(a[ARG_volts].u_obj) : st.set_v;
        float il = a[ARG_amps_limit].u_obj != mp_const_none ? (float)mp_obj_get_float(a[ARG_amps_limit].u_obj) : st.ilimit_a;
        int rc = daq_mp_vdut_setpoint(v, il);
        if (rc == 1) mp_raise_ValueError(MP_ERROR_TEXT("volts must be 1.76-19.94 and amps_limit 0.05-2.636"));
        if (rc != 0) mp_raise_OSError(MP_EIO);
    }
    if (a[ARG_enable].u_obj != mp_const_none) {
        if (!daq_mp_vdut_enable(mp_obj_is_true(a[ARG_enable].u_obj))) mp_raise_OSError(MP_EIO);
    }
    if (!daq_mp_vdut_status(&st)) mp_raise_OSError(MP_EIO);
    return vdut_dict(&st);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(daq_vdut_obj, 0, daq_vdut);

static mp_obj_t daq_read(void)
{
    require_daq();
    daq_mp_vdut_t st;
    if (!daq_mp_vdut_status(&st)) mp_raise_OSError(MP_EIO);
    mp_obj_t d = mp_obj_new_dict(4);
    dict_put(d, MP_QSTR_v, new_float(st.meas_v));
    dict_put(d, MP_QSTR_i, new_float(st.meas_i));
    dict_put(d, MP_QSTR_p, new_float(st.meas_v * st.meas_i));
    dict_put(d, MP_QSTR_enabled, mp_obj_new_bool(st.enabled));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(daq_read_obj, daq_read);

static mp_obj_t daq_samples(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_run_id, ARG_since_s, ARG_max };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_run_id,  MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_since_s, MP_ARG_INT,                   {.u_int = 0} },
        { MP_QSTR_max,     MP_ARG_INT,                   {.u_int = 600} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);
    require_daq();
    mp_int_t run = a[ARG_run_id].u_int, left = a[ARG_max].u_int;
    if (run < 1 || run > 65535) mp_raise_ValueError(MP_ERROR_TEXT("run_id must be 1-65535"));
    if (left < 1 || left > 3600) mp_raise_ValueError(MP_ERROR_TEXT("max must be 1-3600"));
    uint32_t since = a[ARG_since_s].u_int < 0 ? 0 : (uint32_t)a[ARG_since_s].u_int;

    mp_obj_t list = mp_obj_new_list(0, NULL);
    while (left > 0) {
        if (scripting_stop_requested()) mp_raise_msg(&mp_type_KeyboardInterrupt, MP_ERROR_TEXT("stopped"));
        uint8_t req[8], rsp[BS_RSP_MAX];
        daqc_s1_request(req, (uint16_t)run, since, (uint8_t)(left > DAQC_S1_MAX ? DAQC_S1_MAX : left));
        int n = daq_mp_bs(req, sizeof(req), rsp, BS_RSP_MAX);
        if (n == -2) mp_raise_OSError(MP_ENOENT);   // not the loaded run (or empty after a P4 reboot)
        if (n < 0) mp_raise_OSError(MP_EIO);
        daqc_s1_t recs[DAQC_S1_MAX];
        bool more = false;
        int k = daqc_s1_decode(rsp, (size_t)n, NULL, recs, DAQC_S1_MAX, &more);
        if (k < 0) mp_raise_OSError(MP_EIO);
        for (int i = 0; i < k; i++) {
            mp_obj_t t[5] = {
                mp_obj_new_int_from_uint(recs[i].t_s), new_float(recs[i].v), new_float(recs[i].i),
                new_float(recs[i].soc_pct), mp_obj_new_int(recs[i].flags),
            };
            mp_obj_list_append(list, mp_obj_new_tuple(5, t));
            since = recs[i].t_s;
            left--;
        }
        if (k == 0 || !more) break;
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(daq_samples_obj, 1, daq_samples);

// --- daq.run.* ---------------------------------------------------------------
static mp_obj_t daq_run_status(void)
{
    require_daq();
    daqc_bs_status_t st;
    bs_status(&st);
    mp_obj_t d = mp_obj_new_dict(12);
    dict_put(d, MP_QSTR_run_id, mp_obj_new_int(st.run_id));
    dict_put(d, MP_QSTR_state, mp_obj_new_str(daqc_state_name(st.state), strlen(daqc_state_name(st.state))));
    dict_put(d, MP_QSTR_soc, new_float(st.soc_pct));
    dict_put(d, MP_QSTR_v, new_float(st.v_meas));
    dict_put(d, MP_QSTR_i, new_float(st.i_meas));
    dict_put(d, MP_QSTR_elapsed_s, mp_obj_new_int_from_uint(st.elapsed_s));
    dict_put(d, MP_QSTR_remaining_s, st.remaining_s < 0 ? mp_const_none : mp_obj_new_int_from_ll(st.remaining_s));
    dict_put(d, MP_QSTR_energy_j, st.has_energy ? mp_obj_new_float((mp_float_t)st.e_dut_j) : mp_const_none);
    dict_put(d, MP_QSTR_error, mp_obj_new_str(daqc_error_name(st.last_error), strlen(daqc_error_name(st.last_error))));
    daqc_bs_meta_t m;
    if (st.run_id != 0 && bs_meta(st.run_id, &m)) {
        dict_put(d, MP_QSTR_name, mp_obj_new_str(m.name, strlen(m.name)));
        dict_put(d, MP_QSTR_params, params_dict(&m.params));
    } else {
        dict_put(d, MP_QSTR_name, mp_const_none);
        dict_put(d, MP_QSTR_params, mp_const_none);
    }
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(daq_run_status_obj, daq_run_status);

static mp_obj_t daq_run_list(void)
{
    require_daq();
    mp_obj_t list = mp_obj_new_list(0, NULL);
    uint16_t start = 0;
    for (;;) {
        uint8_t req[3], rsp[BS_RSP_MAX];
        daqc_list_request(req, start);
        int n = bs_req(req, sizeof(req), rsp);
        uint16_t total = 0, active = 0, ids[100];
        int cnt = 0;
        if (!daqc_list_decode(rsp, (size_t)n, &total, &active, ids, 100, &cnt)) mp_raise_OSError(MP_EIO);
        for (int i = 0; i < cnt; i++) {
            mp_obj_t d = mp_obj_new_dict(7);
            daqc_bs_meta_t m;
            bool have = bs_meta(ids[i], &m);
            dict_put(d, MP_QSTR_run_id, mp_obj_new_int(ids[i]));
            dict_put(d, MP_QSTR_name, have ? mp_obj_new_str(m.name, strlen(m.name)) : mp_const_none);
            dict_put(d, MP_QSTR_created, have ? mp_obj_new_int_from_uint(m.created_epoch) : mp_const_none);
            dict_put(d, MP_QSTR_chem, have ? mp_obj_new_str(daqc_chem_name(m.params.chem), strlen(daqc_chem_name(m.params.chem))) : mp_const_none);
            dict_put(d, MP_QSTR_cells, have ? mp_obj_new_int(m.params.cells) : mp_const_none);
            dict_put(d, MP_QSTR_capacity_mah, have ? mp_obj_new_int_from_uint(m.params.capacity_mah) : mp_const_none);
            dict_put(d, MP_QSTR_active, mp_obj_new_bool(ids[i] == active));
            mp_obj_list_append(list, d);
        }
        start = (uint16_t)(start + cnt);
        if (cnt == 0 || start >= total) break;
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(daq_run_list_obj, daq_run_list);

static mp_obj_t daq_run_new(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_name, ARG_chem, ARG_cells, ARG_capacity_mah, ARG_soc, ARG_self_discharge, ARG_ext_load };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_name,           MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_chem,           MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_cells,          MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_capacity_mah,   MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_soc,            MP_ARG_OBJ,                   {.u_rom_obj = MP_ROM_INT(100)} },
        { MP_QSTR_self_discharge, MP_ARG_OBJ,                   {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_ext_load,       MP_ARG_OBJ,                   {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);
    require_daq();
    apply_param(MP_QSTR_name, a[ARG_name].u_obj);
    apply_param(MP_QSTR_chem, a[ARG_chem].u_obj);
    apply_param(MP_QSTR_cells, a[ARG_cells].u_obj);
    apply_param(MP_QSTR_capacity_mah, a[ARG_capacity_mah].u_obj);
    apply_param(MP_QSTR_soc, a[ARG_soc].u_obj);
    apply_param(MP_QSTR_self_discharge, a[ARG_self_discharge].u_obj);
    apply_param(MP_QSTR_ext_load, a[ARG_ext_load].u_obj);
    action(DAQC_ACT_BS_RUN_NEW, "run create failed");
    daqc_bs_status_t st;
    bs_status(&st);
    return mp_obj_new_int(st.run_id);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(daq_run_new_obj, 4, daq_run_new);

static mp_obj_t daq_run_start(size_t n_args, const mp_obj_t *args)
{
    require_daq();
    if (n_args == 1 && args[0] != mp_const_none) ensure_loaded(mp_obj_get_int(args[0]));
    action(DAQC_ACT_BS_RUN_START, "run start failed");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(daq_run_start_obj, 0, 1, daq_run_start);

static mp_obj_t daq_run_pause(void)
{
    require_daq();
    action(DAQC_ACT_BS_RUN_PAUSE, "run pause failed");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(daq_run_pause_obj, daq_run_pause);

static mp_obj_t daq_run_stop(void)
{
    require_daq();
    action(DAQC_ACT_BS_RUN_STOP, "run stop failed");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(daq_run_stop_obj, daq_run_stop);

static mp_obj_t daq_run_reopen(size_t n_args, const mp_obj_t *args)
{
    require_daq();
    if (n_args == 1 && args[0] != mp_const_none) ensure_loaded(mp_obj_get_int(args[0]));
    action(DAQC_ACT_BS_RUN_REOPEN, "run reopen failed");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(daq_run_reopen_obj, 0, 1, daq_run_reopen);

static mp_obj_t daq_run_edit(size_t n_args, const mp_obj_t *args, mp_map_t *kw)
{
    require_daq();
    if (n_args > 1) mp_raise_TypeError(MP_ERROR_TEXT("edit takes one positional argument (run_id)"));
    mp_map_elem_t *rid = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(MP_QSTR_run_id), MP_MAP_LOOKUP);
    mp_obj_t run = n_args == 1 ? args[0] : (rid ? rid->value : mp_const_none);
    if (run != mp_const_none) ensure_loaded(mp_obj_get_int(run));
    for (size_t i = 0; i < kw->alloc; i++) {
        if (!mp_map_slot_is_filled(kw, i)) continue;
        qstr k = mp_obj_str_get_qstr(kw->table[i].key);
        if (k == MP_QSTR_run_id) continue;
        apply_param(k, kw->table[i].value);
    }
    return daq_run_status();
}
static MP_DEFINE_CONST_FUN_OBJ_KW(daq_run_edit_obj, 0, daq_run_edit);

static mp_obj_t daq_run_delete(mp_obj_t run_in)
{
    require_daq();
    mp_int_t run = mp_obj_get_int(run_in);
    if (run < 1 || run > 65535) mp_raise_ValueError(MP_ERROR_TEXT("run_id must be 1-65535"));
    cfg_int(DAQC_K_BS_RUN_SELECT, run);
    action(DAQC_ACT_BS_RUN_DELETE, "run delete failed");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(daq_run_delete_obj, daq_run_delete);

static const mp_rom_map_elem_t daq_run_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_run) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&daq_run_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_list),   MP_ROM_PTR(&daq_run_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_new),    MP_ROM_PTR(&daq_run_new_obj) },
    { MP_ROM_QSTR(MP_QSTR_start),  MP_ROM_PTR(&daq_run_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause),  MP_ROM_PTR(&daq_run_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),   MP_ROM_PTR(&daq_run_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_reopen), MP_ROM_PTR(&daq_run_reopen_obj) },
    { MP_ROM_QSTR(MP_QSTR_edit),   MP_ROM_PTR(&daq_run_edit_obj) },
    { MP_ROM_QSTR(MP_QSTR_delete), MP_ROM_PTR(&daq_run_delete_obj) },
};
static MP_DEFINE_CONST_DICT(daq_run_globals, daq_run_globals_table);

static const mp_obj_module_t daq_run_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&daq_run_globals,
};

static const mp_rom_map_elem_t daq_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_daq) },
    { MP_ROM_QSTR(MP_QSTR_present), MP_ROM_PTR(&daq_present_obj) },
    { MP_ROM_QSTR(MP_QSTR_vdut),    MP_ROM_PTR(&daq_vdut_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),    MP_ROM_PTR(&daq_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_samples), MP_ROM_PTR(&daq_samples_obj) },
    { MP_ROM_QSTR(MP_QSTR_run),     MP_ROM_PTR(&daq_run_module) },
};
static MP_DEFINE_CONST_DICT(daq_module_globals, daq_module_globals_table);

const mp_obj_module_t mp_module_daq = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&daq_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_daq, mp_module_daq);
