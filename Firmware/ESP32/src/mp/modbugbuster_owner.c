// =============================================================================
// modbugbuster_owner.c — MicroPython IO-ownership bindings (PR-5).
//
// Public surface:
//
//   ctx = bugbuster.claim(slots, purpose="", lease_ms=0)
//   with ctx:
//       ...                          # io_owner_acquire on enter, release on exit
//
//   bugbuster.release(slots=None)   # immediate bare release (None = release all)
//
//   table = bugbuster.owner_status() # list of 16 dicts:
//       # { "slot": int, "kind": int, "session_id": int,
//       #   "token_fp32": int, "lease_until_ms": int }
//
// Slot numbers: 0..11 = IO1..IO12, 12..15 = CH0..CH3 (per plan §1).
//
// Error mapping (io_claim_result_t → OSError):
//   IO_HELD_BY_OTHER (1) → OSError(17)  # EEXIST — close to EPERM semantics
//   IO_NOT_OWNED (2)     → OSError(1)   # EPERM
//   IO_INVALID_SLOT (3)  → OSError(22)  # EINVAL
//   IO_OK (0)            → no error
//
// Auto-wrap: when a Channel method is called outside a claim context the
//   binding calls bugbuster_mp_io_acquire() with a 5 s one-shot lease.
//   This mirrors the firmware v4 backward-compat path (plan §3 / §6).
//   The active-claim depth is tracked via a module-static counter that is
//   safe to use here because all MP execution is single-threaded within the
//   FreeRTOS task (one VM at a time, no native threads from Python).
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/objlist.h"

#include "modbugbuster_bridge.h"

// ---------------------------------------------------------------------------
// Active-claim depth counter — incremented by Claim.__enter__, decremented
// by Claim.__exit__.  Zero means no explicit claim is held; auto-wrap kicks in.
// ---------------------------------------------------------------------------

static int s_claim_depth = 0;

bool bugbuster_mp_in_claim_context(void) { return s_claim_depth > 0; }

// ---------------------------------------------------------------------------
// FNV-1a 32-bit hash for purpose strings
// ---------------------------------------------------------------------------

static uint32_t fnv1a_32(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Error mapping helper
// ---------------------------------------------------------------------------

static void raise_from_result(int result)
{
    switch (result) {
        case 0:  return;                              // IO_OK
        case 1:  mp_raise_OSError(17); break;         // IO_HELD_BY_OTHER → EEXIST
        case 2:  mp_raise_OSError(MP_EPERM); break;   // IO_NOT_OWNED
        case 3:  mp_raise_OSError(MP_EINVAL); break;  // IO_INVALID_SLOT
        default: mp_raise_OSError(MP_EIO); break;
    }
}

// ---------------------------------------------------------------------------
// Claim object type
// ---------------------------------------------------------------------------

typedef struct _bugbuster_claim_obj_t {
    mp_obj_base_t base;

    // Slot list stored as a compact C array (max 16 slots)
    uint8_t  slots[16];
    uint8_t  n_slots;

    uint32_t lease_ms;
    uint32_t purpose_tag;
} bugbuster_claim_obj_t;

// bugbuster_claim_type defined below via MP_DEFINE_CONST_OBJ_TYPE;
// mp_obj_malloc() needs the pointer at call site so the type must be
// declared before use.  In C, MP_DEFINE_CONST_OBJ_TYPE emits a plain
// const struct — declare it extern here so the compiler knows the type
// when bugbuster_claim_fn references &bugbuster_claim_type.
extern const mp_obj_type_t bugbuster_claim_type;

// Claim.__enter__ — acquire the slots and return self
static mp_obj_t bugbuster_claim_enter(mp_obj_t self_in)
{
    bugbuster_claim_obj_t *self = (bugbuster_claim_obj_t *)MP_OBJ_TO_PTR(self_in);
    int rc = bugbuster_mp_io_acquire(self->slots, self->n_slots,
                                      self->lease_ms, self->purpose_tag);
    raise_from_result(rc);
    s_claim_depth++;
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bugbuster_claim_enter_obj, bugbuster_claim_enter);

// Claim.__exit__ — release the slots regardless of exception
static mp_obj_t bugbuster_claim_exit(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    bugbuster_claim_obj_t *self = (bugbuster_claim_obj_t *)MP_OBJ_TO_PTR(args[0]);
    bugbuster_mp_io_release(self->slots, self->n_slots);
    if (s_claim_depth > 0) s_claim_depth--;
    return mp_const_false;   // do not suppress exceptions
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(bugbuster_claim_exit_obj, 4, 4, bugbuster_claim_exit);

static const mp_rom_map_elem_t bugbuster_claim_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&bugbuster_claim_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),  MP_ROM_PTR(&bugbuster_claim_exit_obj) },
};
static MP_DEFINE_CONST_DICT(bugbuster_claim_locals_dict, bugbuster_claim_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    bugbuster_claim_type,
    MP_QSTR_Claim,
    MP_TYPE_FLAG_NONE,
    locals_dict, &bugbuster_claim_locals_dict
);

// ---------------------------------------------------------------------------
// Helper: parse a list/tuple of slot ints into a C uint8_t array.
// Returns the number of slots filled.  Raises ValueError on bad input.
// ---------------------------------------------------------------------------

static size_t parse_slots(mp_obj_t slots_obj, uint8_t *out, size_t max_out)
{
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(slots_obj, &n, &items);
    if (n == 0 || n > max_out) {
        mp_raise_ValueError(MP_ERROR_TEXT("slots must be a non-empty list of 1..16 ints"));
    }
    for (size_t i = 0; i < n; i++) {
        mp_int_t s = mp_obj_get_int(items[i]);
        if (s < 0 || s > 15) {
            mp_raise_ValueError(MP_ERROR_TEXT("slot must be 0..15"));
        }
        out[i] = (uint8_t)s;
    }
    return n;
}

// ---------------------------------------------------------------------------
// bugbuster.claim(slots, purpose="", lease_ms=0) -> Claim
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_claim_fn(size_t n_args, const mp_obj_t *pos_args,
                                    mp_map_t *kw_args)
{
    enum { ARG_slots, ARG_purpose, ARG_lease_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_slots,    MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_purpose,  MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_lease_ms, MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    bugbuster_claim_obj_t *obj = mp_obj_malloc(bugbuster_claim_obj_t,
                                                &bugbuster_claim_type);

    obj->n_slots    = (uint8_t)parse_slots(parsed[ARG_slots].u_obj, obj->slots, 16);
    obj->lease_ms   = (uint32_t)parsed[ARG_lease_ms].u_int;

    // purpose: accept str or None/missing
    const char *purpose_str = "";
    mp_obj_t p = parsed[ARG_purpose].u_obj;
    if (p != mp_const_none && mp_obj_is_str(p)) {
        purpose_str = mp_obj_str_get_str(p);
    }
    obj->purpose_tag = fnv1a_32(purpose_str);

    return MP_OBJ_FROM_PTR(obj);
}
MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_claim_fn_obj, 1, bugbuster_claim_fn);

// ---------------------------------------------------------------------------
// bugbuster.release(slots=None) — bare immediate release
// slots=None means "release all slots owned by this script session".
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_release_fn(size_t n_args, const mp_obj_t *pos_args,
                                      mp_map_t *kw_args)
{
    enum { ARG_slots };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_slots, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    mp_obj_t slots_obj = parsed[ARG_slots].u_obj;

    if (slots_obj == mp_const_none) {
        // release-all: pass NULL / n=0 to the bridge
        int rc = bugbuster_mp_io_release(NULL, 0);
        raise_from_result(rc);
    } else {
        uint8_t slots[16];
        size_t n = parse_slots(slots_obj, slots, 16);
        int rc = bugbuster_mp_io_release(slots, n);
        raise_from_result(rc);
    }
    if (s_claim_depth > 0) s_claim_depth--;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_release_fn_obj, 0, bugbuster_release_fn);

// ---------------------------------------------------------------------------
// bugbuster.owner_status() -> list[dict]
// Returns 16 dicts: { slot, kind, session_id, token_fp32, lease_until_ms }
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_owner_status_fn(void)
{
    uint8_t  kind_arr[16];
    uint8_t  sess_arr[16];
    uint32_t token_arr[16];
    uint32_t lease_arr[16];
    uint32_t purpose_arr[16];

    if (!bugbuster_mp_io_status(kind_arr, sess_arr, token_arr, lease_arr, purpose_arr, 16)) {
        mp_raise_OSError(MP_EIO);
    }

    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < 16; i++) {
        mp_obj_t d = mp_obj_new_dict(5);
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_slot),
                          mp_obj_new_int((mp_int_t)i));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_kind),
                          mp_obj_new_int(kind_arr[i]));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_session_id),
                          mp_obj_new_int(sess_arr[i]));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_token_fp32),
                          mp_obj_new_int_from_uint(token_arr[i]));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_lease_until_ms),
                          mp_obj_new_int_from_uint(lease_arr[i]));
        mp_obj_list_append(result, d);
    }
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(bugbuster_owner_status_fn_obj, bugbuster_owner_status_fn);

// ---------------------------------------------------------------------------
// Auto-wrap helper — called by Channel.set_function / set_voltage / set_do
// before executing the mutating operation when no explicit claim is held.
// Acquires a one-shot 5 s lease on channel slot (channel index + 12 for CH).
// Returns the auto-claim result (0=OK, non-zero = do not proceed).
// ---------------------------------------------------------------------------

int bugbuster_mp_auto_claim(uint8_t channel_slot, uint32_t purpose_tag)
{
    if (s_claim_depth > 0) return 0;  // inside explicit claim, no-op
    uint8_t slot = channel_slot;
    return bugbuster_mp_io_acquire(&slot, 1, 5000, purpose_tag);
}

// bugbuster_claim_type, bugbuster_claim_fn_obj, bugbuster_release_fn_obj,
// bugbuster_owner_status_fn_obj are all defined in this translation unit and
// declared extern in modbugbuster.c via the MP_DECLARE_* macros.
