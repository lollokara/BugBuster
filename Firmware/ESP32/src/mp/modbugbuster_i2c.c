// =============================================================================
// modbugbuster_i2c.c — MicroPython bugbuster.I2C binding (Phase 4).
//
// API:
//   i2c = bugbuster.I2C(sda_io, scl_io, *, freq=400000, pullups='external',
//                       supply=3.3, vlogic=3.3)
//   i2c.scan([start=0x08, stop=0x77, skip_reserved=True, timeout_ms=50]) -> list
//   i2c.writeto(addr, buf [, timeout_ms=50])
//   i2c.readfrom(addr, n [, timeout_ms=50]) -> bytes
//   i2c.writeto_then_readfrom(addr, wr_buf, rd_n [, timeout_ms=50]) -> bytes
//
// sda_io / scl_io are BugBuster IO terminal numbers 1..12 (never raw GPIO).
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/objstr.h"
#include "py/stream.h"

#include "modbugbuster_bridge.h"

// ---------------------------------------------------------------------------
// Object type
// ---------------------------------------------------------------------------

typedef struct _bugbuster_i2c_obj_t {
    mp_obj_base_t base;
    uint8_t       sda_io;
    uint8_t       scl_io;
} bugbuster_i2c_obj_t;

// ---------------------------------------------------------------------------
// make_new
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_i2c_make_new(const mp_obj_type_t *type,
                                        size_t n_args, size_t n_kw,
                                        const mp_obj_t *args)
{
    enum { ARG_sda_io, ARG_scl_io, ARG_freq, ARG_pullups, ARG_supply, ARG_vlogic };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sda_io,  MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_scl_io,  MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_freq,    MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 400000} },
        { MP_QSTR_pullups, MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_external)} },
        { MP_QSTR_supply,  MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_vlogic,  MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };

    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args,
                               MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    mp_int_t sda_io = parsed[ARG_sda_io].u_int;
    mp_int_t scl_io = parsed[ARG_scl_io].u_int;
    if (sda_io < 1 || sda_io > 12) {
        mp_raise_ValueError(MP_ERROR_TEXT("sda_io must be 1..12"));
    }
    if (scl_io < 1 || scl_io > 12) {
        mp_raise_ValueError(MP_ERROR_TEXT("scl_io must be 1..12"));
    }
    if (sda_io == scl_io) {
        mp_raise_ValueError(MP_ERROR_TEXT("sda_io and scl_io must be different"));
    }

    uint32_t freq = (uint32_t)parsed[ARG_freq].u_int;

    // Parse pullups string: 'external' | 'internal' | 'off'
    const char *pullups_str = mp_obj_str_get_str(parsed[ARG_pullups].u_obj);
    bool internal_pullups = false;
    if (strcmp(pullups_str, "internal") == 0) {
        internal_pullups = true;
    } else if (strcmp(pullups_str, "external") == 0) {
        internal_pullups = false;
    } else if (strcmp(pullups_str, "off") == 0) {
        internal_pullups = false;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("pullups must be 'external', 'internal', or 'off'"));
    }

    float supply_v = (parsed[ARG_supply].u_obj == mp_const_none)
        ? 3.3f : (float)mp_obj_get_float(parsed[ARG_supply].u_obj);
    float vlogic_v = (parsed[ARG_vlogic].u_obj == mp_const_none)
        ? 3.3f : (float)mp_obj_get_float(parsed[ARG_vlogic].u_obj);

    if (supply_v > 5.0f) {
        mp_raise_ValueError(MP_ERROR_TEXT("supply > 5.0 V not allowed"));
    }

    char err[80] = {0};
    if (!bugbuster_mp_i2c_setup((uint8_t)sda_io, (uint8_t)scl_io,
                                 freq, internal_pullups,
                                 supply_v, vlogic_v,
                                 err, sizeof(err))) {
        mp_raise_ValueError(err[0] ? err : "I2C setup failed");
    }

    bugbuster_i2c_obj_t *self = mp_obj_malloc(bugbuster_i2c_obj_t, type);
    self->sda_io = (uint8_t)sda_io;
    self->scl_io = (uint8_t)scl_io;
    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// scan(start=0x08, stop=0x77, skip_reserved=True, timeout_ms=50) -> list
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_i2c_scan(size_t n_args, const mp_obj_t *pos_args,
                                    mp_map_t *kw_args)
{
    enum { ARG_start, ARG_stop, ARG_skip_reserved, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_start,         MP_ARG_INT, {.u_int = 0x08} },
        { MP_QSTR_stop,          MP_ARG_INT, {.u_int = 0x77} },
        { MP_QSTR_skip_reserved, MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_timeout_ms,    MP_ARG_INT, {.u_int = 50} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    uint8_t   start        = (uint8_t)parsed[ARG_start].u_int;
    uint8_t   stop         = (uint8_t)parsed[ARG_stop].u_int;
    bool      skip_res     = parsed[ARG_skip_reserved].u_bool;
    uint16_t  timeout_ms   = (uint16_t)parsed[ARG_timeout_ms].u_int;

    uint8_t  found[128];
    size_t   count = 0;
    if (!bugbuster_mp_i2c_scan(start, stop, skip_res, timeout_ms,
                                found, sizeof(found), &count)) {
        mp_raise_OSError(MP_EIO);
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count; i++) {
        mp_obj_list_append(list, mp_obj_new_int(found[i]));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_i2c_scan_obj, 1, bugbuster_i2c_scan);

// ---------------------------------------------------------------------------
// writeto(addr, buf [, timeout_ms=50])
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_i2c_writeto(size_t n_args, const mp_obj_t *pos_args,
                                       mp_map_t *kw_args)
{
    enum { ARG_addr, ARG_buf, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_addr,       MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_buf,        MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout_ms, MP_ARG_INT,                   {.u_int = 50} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    uint8_t  addr       = (uint8_t)parsed[ARG_addr].u_int;
    uint16_t timeout_ms = (uint16_t)parsed[ARG_timeout_ms].u_int;

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(parsed[ARG_buf].u_obj, &bufinfo, MP_BUFFER_READ);

    if (!bugbuster_mp_i2c_write(addr, (const uint8_t *)bufinfo.buf,
                                 bufinfo.len, timeout_ms)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_i2c_writeto_obj, 1, bugbuster_i2c_writeto);

// ---------------------------------------------------------------------------
// readfrom(addr, n [, timeout_ms=50]) -> bytes
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_i2c_readfrom(size_t n_args, const mp_obj_t *pos_args,
                                        mp_map_t *kw_args)
{
    enum { ARG_addr, ARG_n, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_addr,       MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_n,          MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_timeout_ms, MP_ARG_INT,                   {.u_int = 50} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    uint8_t  addr       = (uint8_t)parsed[ARG_addr].u_int;
    mp_int_t n          = parsed[ARG_n].u_int;
    uint16_t timeout_ms = (uint16_t)parsed[ARG_timeout_ms].u_int;

    if (n <= 0 || n > 512) {
        mp_raise_ValueError(MP_ERROR_TEXT("n must be 1..512"));
    }

    uint8_t buf[512];
    if (!bugbuster_mp_i2c_read(addr, buf, (size_t)n, timeout_ms)) {
        mp_raise_OSError(MP_ETIMEDOUT);
    }
    return mp_obj_new_bytes(buf, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_i2c_readfrom_obj, 1, bugbuster_i2c_readfrom);

// ---------------------------------------------------------------------------
// writeto_then_readfrom(addr, wr_buf, rd_n [, timeout_ms=50]) -> bytes
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_i2c_writeto_then_readfrom(size_t n_args,
                                                      const mp_obj_t *pos_args,
                                                      mp_map_t *kw_args)
{
    enum { ARG_addr, ARG_wr_buf, ARG_rd_n, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_addr,       MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_buf,        MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_rd_n,       MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_timeout_ms, MP_ARG_INT,                   {.u_int = 50} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    uint8_t  addr       = (uint8_t)parsed[ARG_addr].u_int;
    uint16_t timeout_ms = (uint16_t)parsed[ARG_timeout_ms].u_int;
    mp_int_t rd_n       = parsed[ARG_rd_n].u_int;

    mp_buffer_info_t wr_bufinfo;
    mp_get_buffer_raise(parsed[ARG_wr_buf].u_obj, &wr_bufinfo, MP_BUFFER_READ);

    if (rd_n <= 0 || rd_n > 512) {
        mp_raise_ValueError(MP_ERROR_TEXT("rd_n must be 1..512"));
    }

    uint8_t rd_buf[512];
    if (!bugbuster_mp_i2c_write_read(addr,
                                      (const uint8_t *)wr_bufinfo.buf, wr_bufinfo.len,
                                      rd_buf, (size_t)rd_n,
                                      timeout_ms)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_bytes(rd_buf, (size_t)rd_n);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_i2c_writeto_then_readfrom_obj, 1,
                                   bugbuster_i2c_writeto_then_readfrom);

// ---------------------------------------------------------------------------
// Type definition
// ---------------------------------------------------------------------------

static const mp_rom_map_elem_t bugbuster_i2c_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_scan),                  MP_ROM_PTR(&bugbuster_i2c_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeto),               MP_ROM_PTR(&bugbuster_i2c_writeto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readfrom),              MP_ROM_PTR(&bugbuster_i2c_readfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeto_then_readfrom), MP_ROM_PTR(&bugbuster_i2c_writeto_then_readfrom_obj) },
};
static MP_DEFINE_CONST_DICT(bugbuster_i2c_locals_dict, bugbuster_i2c_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    bugbuster_i2c_type,
    MP_QSTR_I2C,
    MP_TYPE_FLAG_NONE,
    make_new, bugbuster_i2c_make_new,
    locals_dict, &bugbuster_i2c_locals_dict
);
