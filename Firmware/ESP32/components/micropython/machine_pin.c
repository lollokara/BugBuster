// Custom minimal machine_pin.c for BugBuster (Phase 1)
#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/virtpin.h"
#include "mphalport.h"

// Forward declarations for standard ESP-IDF GPIO functions
void gpio_pullup_en(int gpio_num);
void gpio_pullup_dis(int gpio_num);
void gpio_pulldown_en(int gpio_num);
void gpio_pulldown_dis(int gpio_num);

typedef struct _machine_pin_obj_t {
    mp_obj_base_t base;
    int pin_id;
} machine_pin_obj_t;

extern const mp_obj_type_t machine_pin_type;

static void machine_pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "Pin(%d)", self->pin_id);
}

static mp_obj_t machine_pin_obj_init_helper(machine_pin_obj_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode, ARG_pull, ARG_value };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        { MP_QSTR_pull, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        { MP_QSTR_value, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int pin_id = self->pin_id;

    // Select the pad for digital GPIO
    esp_rom_gpio_pad_select_gpio(pin_id);

    // Set value if provided (do this before configuring mode)
    if (args[ARG_value].u_obj != mp_const_none) {
        gpio_set_level(pin_id, mp_obj_is_true(args[ARG_value].u_obj));
    }

    // Configure pull-up / pull-down
    if (args[ARG_pull].u_obj != mp_const_none) {
        int pull = mp_obj_get_int(args[ARG_pull].u_obj);
        if (pull == 1) { // Pin.PULL_UP
            gpio_pullup_en(pin_id);
            gpio_pulldown_dis(pin_id);
        } else if (pull == 2) { // Pin.PULL_DOWN
            gpio_pulldown_en(pin_id);
            gpio_pullup_dis(pin_id);
        } else if (pull == 0) { // Disabled
            gpio_pullup_dis(pin_id);
            gpio_pulldown_dis(pin_id);
        }
    }

    // Configure mode (input, output, open drain)
    if (args[ARG_mode].u_obj != mp_const_none) {
        int mode = mp_obj_get_int(args[ARG_mode].u_obj);
        gpio_set_direction(pin_id, mode);
    }

    return mp_const_none;
}

mp_obj_t mp_pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)type;
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    int pin_id = mp_obj_get_int(args[0]);
    if (pin_id < 0 || pin_id > 48) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
    }

    machine_pin_obj_t *self = mp_obj_malloc(machine_pin_obj_t, &machine_pin_type);
    self->pin_id = pin_id;

    if (n_args > 1 || n_kw > 0) {
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
        machine_pin_obj_init_helper(self, n_args - 1, args + 1, &kw_args);
    }

    return MP_OBJ_FROM_PTR(self);
}

gpio_num_t machine_pin_get_id(mp_obj_t pin_in) {
    if (mp_obj_is_type(pin_in, &machine_pin_type)) {
        machine_pin_obj_t *self = MP_OBJ_TO_PTR(pin_in);
        return self->pin_id;
    } else if (mp_obj_is_int(pin_in)) {
        return mp_obj_get_int(pin_in);
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
    }
}

static mp_obj_t machine_pin_value(size_t n_args, const mp_obj_t *args) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) {
        // Read level
        return MP_OBJ_NEW_SMALL_INT(gpio_get_level(self->pin_id));
    } else {
        // Write level
        gpio_set_level(self->pin_id, mp_obj_is_true(args[1]));
        return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_value_obj, 1, 2, machine_pin_value);

static mp_obj_t machine_pin_on(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    gpio_set_level(self->pin_id, 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_on_obj, machine_pin_on);

static mp_obj_t machine_pin_off(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    gpio_set_level(self->pin_id, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_off_obj, machine_pin_off);

static mp_obj_t machine_pin_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return machine_pin_obj_init_helper(MP_OBJ_TO_PTR(args[0]), n_args - 1, args + 1, kw_args);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_init_obj, 1, machine_pin_init);

static mp_uint_t machine_pin_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)errcode;
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_PIN_READ:
            return gpio_get_level(self->pin_id);
        case MP_PIN_WRITE:
            gpio_set_level(self->pin_id, arg);
            return 0;
        case MP_PIN_INPUT:
            gpio_set_direction(self->pin_id, GPIO_MODE_INPUT);
            return 0;
        case MP_PIN_OUTPUT:
            gpio_set_direction(self->pin_id, GPIO_MODE_INPUT_OUTPUT);
            return 0;
    }
    return -1;
}

static const mp_pin_p_t machine_pin_pin_p = {
    .ioctl = machine_pin_ioctl,
};

static const mp_rom_map_elem_t machine_pin_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_pin_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&machine_pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&machine_pin_on_obj) },
    { MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&machine_pin_off_obj) },

    // Constants
    { MP_ROM_QSTR(MP_QSTR_IN), MP_ROM_INT(GPIO_MODE_INPUT) },
    { MP_ROM_QSTR(MP_QSTR_OUT), MP_ROM_INT(GPIO_MODE_INPUT_OUTPUT) },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN), MP_ROM_INT(GPIO_MODE_INPUT_OUTPUT_OD) },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN), MP_ROM_INT(2) },
};
static MP_DEFINE_CONST_DICT(machine_pin_locals_dict, machine_pin_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, mp_pin_make_new,
    print, machine_pin_print,
    call, machine_pin_value,
    protocol, &machine_pin_pin_p,
    locals_dict, &machine_pin_locals_dict
);
