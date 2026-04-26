// =============================================================================
// modbugbuster_spi.c — MicroPython bugbuster.SPI binding (Phase 4).
//
// API:
//   spi = bugbuster.SPI(sck_io, *, mosi_io=None, miso_io=None, cs_io=None,
//                       freq=1_000_000, mode=0, supply=3.3, vlogic=3.3)
//   spi.transfer(buf) -> bytes    (full-duplex, tx_len == rx_len, max 512 B)
//
// sck_io / mosi_io / miso_io / cs_io are BugBuster IO terminal numbers 1..12
// (never raw GPIO).  Pass None (default) to omit an optional signal.
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"

#include "modbugbuster_bridge.h"

// ---------------------------------------------------------------------------
// Object type
// ---------------------------------------------------------------------------

typedef struct _bugbuster_spi_obj_t {
    mp_obj_base_t base;
    uint8_t       sck_io;
    uint8_t       mosi_io;   // 0 = unused
    uint8_t       miso_io;   // 0 = unused
    uint8_t       cs_io;     // 0 = unused
} bugbuster_spi_obj_t;

// ---------------------------------------------------------------------------
// make_new
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_spi_make_new(const mp_obj_type_t *type,
                                        size_t n_args, size_t n_kw,
                                        const mp_obj_t *args)
{
    enum { ARG_sck_io, ARG_mosi_io, ARG_miso_io, ARG_cs_io,
           ARG_freq, ARG_mode, ARG_supply, ARG_vlogic };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sck_io,  MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_mosi_io, MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_miso_io, MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_cs_io,   MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_freq,    MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 1000000} },
        { MP_QSTR_mode,    MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_supply,  MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_vlogic,  MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };

    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args,
                               MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    mp_int_t sck_io = parsed[ARG_sck_io].u_int;
    if (sck_io < 1 || sck_io > 12) {
        mp_raise_ValueError(MP_ERROR_TEXT("sck_io must be 1..12"));
    }

    // Helper to decode optional IO kwarg (None -> 0)
    #define DECODE_OPT_IO(idx, name) \
        ({ mp_obj_t _o = parsed[(idx)].u_obj; \
           mp_int_t _v = 0; \
           if (_o != mp_const_none) { \
               _v = mp_obj_get_int(_o); \
               if (_v < 1 || _v > 12) { \
                   mp_raise_ValueError(MP_ERROR_TEXT(name " must be 1..12 or None")); \
               } \
           } \
           _v; })

    mp_int_t mosi_io = DECODE_OPT_IO(ARG_mosi_io, "mosi_io");
    mp_int_t miso_io = DECODE_OPT_IO(ARG_miso_io, "miso_io");
    mp_int_t cs_io   = DECODE_OPT_IO(ARG_cs_io,   "cs_io");
    #undef DECODE_OPT_IO

    uint32_t freq    = (uint32_t)parsed[ARG_freq].u_int;
    uint8_t  mode    = (uint8_t)parsed[ARG_mode].u_int;
    if (mode > 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("mode must be 0..3"));
    }

    float supply_v = (parsed[ARG_supply].u_obj == mp_const_none)
        ? 3.3f : (float)mp_obj_get_float(parsed[ARG_supply].u_obj);
    float vlogic_v = (parsed[ARG_vlogic].u_obj == mp_const_none)
        ? 3.3f : (float)mp_obj_get_float(parsed[ARG_vlogic].u_obj);

    if (supply_v > 5.0f) {
        mp_raise_ValueError(MP_ERROR_TEXT("supply > 5.0 V not allowed"));
    }

    char err[80] = {0};
    if (!bugbuster_mp_spi_setup((uint8_t)sck_io,
                                 (uint8_t)mosi_io, (uint8_t)miso_io, (uint8_t)cs_io,
                                 freq, mode,
                                 supply_v, vlogic_v,
                                 err, sizeof(err))) {
        mp_raise_ValueError(err[0] ? err : "SPI setup failed");
    }

    bugbuster_spi_obj_t *self = mp_obj_malloc(bugbuster_spi_obj_t, type);
    self->sck_io  = (uint8_t)sck_io;
    self->mosi_io = (uint8_t)mosi_io;
    self->miso_io = (uint8_t)miso_io;
    self->cs_io   = (uint8_t)cs_io;
    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// transfer(buf) -> bytes
// Full-duplex; rx length == tx length; max 512 bytes.
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_spi_transfer(mp_obj_t self_in, mp_obj_t buf_in)
{
    (void)self_in;

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);

    if (bufinfo.len == 0 || bufinfo.len > 512) {
        mp_raise_ValueError(MP_ERROR_TEXT("transfer buf must be 1..512 bytes"));
    }

    uint8_t rx[512];
    size_t  rx_len = bufinfo.len;

    if (!bugbuster_mp_spi_transfer((const uint8_t *)bufinfo.buf, bufinfo.len,
                                    rx, &rx_len, 50)) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_bytes(rx, rx_len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bugbuster_spi_transfer_obj, bugbuster_spi_transfer);

// ---------------------------------------------------------------------------
// Type definition
// ---------------------------------------------------------------------------

static const mp_rom_map_elem_t bugbuster_spi_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_transfer), MP_ROM_PTR(&bugbuster_spi_transfer_obj) },
};
static MP_DEFINE_CONST_DICT(bugbuster_spi_locals_dict, bugbuster_spi_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    bugbuster_spi_type,
    MP_QSTR_SPI,
    MP_TYPE_FLAG_NONE,
    make_new, bugbuster_spi_make_new,
    locals_dict, &bugbuster_spi_locals_dict
);
