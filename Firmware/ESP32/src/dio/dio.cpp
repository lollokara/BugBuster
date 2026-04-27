// =============================================================================
// dio.cpp - Digital IO (DIO) — ESP32 GPIO-based digital input/output
//
// See dio.h for module overview and IO numbering.
// =============================================================================

#include "dio.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "dio";

// -----------------------------------------------------------------------------
// GPIO pin mapping tables (index 0 = IO 1, index 11 = IO 12)
// PCB schematic assignments. Each GPIO is routed to one side of an ADGS2414D
// analog switch; the other side goes to the physical terminal-block pin.
//
// IO_Block 1 (U10): IO 1=GPIO4,  IO 2=GPIO2,  IO 3=GPIO1   (IO3 is analog-capable)
// IO_Block 2 (U11): IO 4=GPIO7,  IO 5=GPIO6,  IO 6=GPIO5   (IO6 is analog-capable)
// IO_Block 3 (U17): IO 7=GPIO8,  IO 8=GPIO9,  IO 9=GPIO10  (IO9 is analog-capable)
// IO_Block 4 (U16): IO10=GPIO11, IO11=GPIO12, IO12=GPIO13  (IO12 is analog-capable)
// -----------------------------------------------------------------------------

static const int8_t DIO_PIN_MAP[DIO_NUM_IOS] = {
     4,   2,   1,     // IO_Block 1: IO 1, 2, 3
     7,   6,   5,     // IO_Block 2: IO 4, 5, 6
     8,   9,  10,     // IO_Block 3: IO 7, 8, 9
    11,  12,  13,     // IO_Block 4: IO 10, 11, 12
};

// Runtime state for each IO
static DioState s_io[DIO_NUM_IOS];

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static inline bool valid_io(uint8_t io) {
    return io >= DIO_FIRST_IO && io <= DIO_LAST_IO;
}

static inline int idx(uint8_t io) {
    return io - DIO_FIRST_IO;  // IO 1 -> index 0
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void dio_init(void)
{
    for (int i = 0; i < DIO_NUM_IOS; i++) {
        s_io[i].gpio_num     = DIO_PIN_MAP[i];
        s_io[i].mode         = DIO_MODE_DISABLED;
        s_io[i].output_level = false;
        s_io[i].input_level  = false;
        s_io[i].pulldown     = false;
    }
    ESP_LOGI(TAG, "DIO initialized (%d IOs, PCB mode)", DIO_NUM_IOS);
}

bool dio_configure(uint8_t io, uint8_t mode)
{
    return dio_configure_ext(io, mode, false);
}

bool dio_configure_ext(uint8_t io, uint8_t mode, bool pulldown)
{
    if (!valid_io(io)) {
        ESP_LOGE(TAG, "Invalid IO %d (must be %d–%d)", io, DIO_FIRST_IO, DIO_LAST_IO);
        return false;
    }

    int i = idx(io);
    int8_t pin = s_io[i].gpio_num;
    if (pin < 0) {
        ESP_LOGE(TAG, "IO %d has no GPIO mapping", io);
        return false;
    }

    gpio_num_t gpin = (gpio_num_t)pin;
    gpio_pull_mode_t pull = pulldown ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING;

    switch (mode) {
    case DIO_MODE_DISABLED:
        gpio_reset_pin(gpin);
        gpio_set_direction(gpin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(gpin, pull);
        break;

    case DIO_MODE_INPUT:
        gpio_reset_pin(gpin);
        gpio_set_direction(gpin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(gpin, pull);
        break;

    case DIO_MODE_OUTPUT:
        gpio_reset_pin(gpin);
        gpio_set_direction(gpin, GPIO_MODE_OUTPUT);
        gpio_set_level(gpin, 0);
        s_io[i].output_level = false;
        break;

    default:
        ESP_LOGE(TAG, "IO %d: invalid mode %d", io, mode);
        return false;
    }

    s_io[i].mode = mode;
    s_io[i].pulldown = pulldown;
    ESP_LOGD(TAG, "IO %d (GPIO%d) -> %s (pulldown=%s)",
             io, pin,
             mode == DIO_MODE_INPUT ? "INPUT" :
             mode == DIO_MODE_OUTPUT ? "OUTPUT" : "DISABLED",
             pulldown ? "YES" : "NO");
    return true;
}

bool dio_write(uint8_t io, bool level)
{
    if (!valid_io(io)) return false;
    int i = idx(io);

    if (s_io[i].mode != DIO_MODE_OUTPUT) {
        ESP_LOGE(TAG, "IO %d not configured as OUTPUT (mode=%d)", io, s_io[i].mode);
        return false;
    }

    gpio_set_level((gpio_num_t)s_io[i].gpio_num, level ? 1 : 0);
    s_io[i].output_level = level;
    return true;
}

bool dio_read(uint8_t io)
{
    if (!valid_io(io)) return false;
    int i = idx(io);

    if (s_io[i].mode != DIO_MODE_INPUT) {
        ESP_LOGW(TAG, "IO %d not configured as INPUT (mode=%d)", io, s_io[i].mode);
        return false;
    }

    int level = gpio_get_level((gpio_num_t)s_io[i].gpio_num);
    s_io[i].input_level = (level != 0);
    return s_io[i].input_level;
}

bool dio_get_state(uint8_t io, DioState *out)
{
    if (!valid_io(io) || !out) return false;
    *out = s_io[idx(io)];
    return true;
}

const DioState* dio_get_all(void)
{
    return s_io;
}

void dio_poll_inputs(void)
{
    for (int i = 0; i < DIO_NUM_IOS; i++) {
        if (s_io[i].mode == DIO_MODE_INPUT && s_io[i].gpio_num >= 0) {
            s_io[i].input_level = (gpio_get_level((gpio_num_t)s_io[i].gpio_num) != 0);
        }
    }
}
