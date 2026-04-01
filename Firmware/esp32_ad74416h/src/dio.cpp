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
// -----------------------------------------------------------------------------

#if BREADBOARD_MODE

// Breadboard mode: map to unused ESP32-S3 GPIOs that don't conflict with
// SPI (8-11), I2C (1,4), ADC control (5-7), MUX CS (12), OE (14), USB (19,20).
//
// IO_Block 1: IO 1=GPIO2,  IO 2=GPIO3,   IO 3=GPIO13
// IO_Block 2: IO 4=GPIO21, IO 5=GPIO35,  IO 6=GPIO36
// IO_Block 3: IO 7=GPIO37, IO 8=GPIO38,  IO 9=GPIO39
// IO_Block 4: IO10=GPIO40, IO11=GPIO47,  IO12=GPIO48

static const int8_t DIO_PIN_MAP[DIO_NUM_IOS] = {
     2,   3,  13,     // IO_Block 1: IO 1, 2, 3
    21,  35,  36,     // IO_Block 2: IO 4, 5, 6
    37,  38,  39,     // IO_Block 3: IO 7, 8, 9
    40,  47,  48,     // IO_Block 4: IO 10, 11, 12
};

#else  // PCB mode

// PCB mode: GPIOs from MUX_GPIO_MAP in adgs2414d.h.
// These connect to one side of the ADGS2414D analog switches; the other
// side goes to the physical terminal block pins.
//
// WARNING: IOs 4-6 share GPIOs with AD74416H control (RESET=5, RDY=6, ALERT=7)
// and IOs 9-12 share with SPI (SCLK=11, CS=10, SDI=9, SDO=8).
// The final PCB will reassign these — update this table when the PCB is done.
//
// IO_Block 1 (U10): IO 1=GPIO1,  IO 2=GPIO2,  IO 3=GPIO3
// IO_Block 2 (U11): IO 4=GPIO5,  IO 5=GPIO6,  IO 6=GPIO7
// IO_Block 3 (U16): IO 7=GPIO13, IO 8=GPIO12, IO 9=GPIO11
// IO_Block 4 (U17): IO10=GPIO10, IO11=GPIO9,  IO12=GPIO8

static const int8_t DIO_PIN_MAP[DIO_NUM_IOS] = {
     1,   2,   3,     // IO_Block 1: IO 1, 2, 3
     5,   6,   7,     // IO_Block 2: IO 4, 5, 6
    13,  12,  11,     // IO_Block 3: IO 7, 8, 9
    10,   9,   8,     // IO_Block 4: IO 10, 11, 12
};

#endif

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
    }
    ESP_LOGI(TAG, "DIO initialized (%d IOs, %s mode)",
             DIO_NUM_IOS, BREADBOARD_MODE ? "breadboard" : "PCB");
}

bool dio_configure(uint8_t io, uint8_t mode)
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

    switch (mode) {
    case DIO_MODE_DISABLED:
        gpio_reset_pin(gpin);
        // Set to input with no pull to avoid driving the pin
        gpio_set_direction(gpin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(gpin, GPIO_FLOATING);
        break;

    case DIO_MODE_INPUT:
        gpio_reset_pin(gpin);
        gpio_set_direction(gpin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(gpin, GPIO_FLOATING);
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
    ESP_LOGD(TAG, "IO %d (GPIO%d) -> %s",
             io, pin,
             mode == DIO_MODE_INPUT ? "INPUT" :
             mode == DIO_MODE_OUTPUT ? "OUTPUT" : "DISABLED");
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
