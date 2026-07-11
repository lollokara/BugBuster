#include "buttons.h"
#include "config.h"

// On the production DAQ HAT the front-panel buttons are wired to the ESP32-P4,
// which debounces them and relays UP/DOWN/OK/BACK events to the C6 over DDP
// (see main.c: ddp_take_buttons()). On this PCB the legacy C6 button pins
// (IO5/6/7) ARE the display SPI SCLK/MOSI/DC — if the C6 reconfigures them as
// GPIO inputs it tears those lines away from the SPI peripheral, so there is no
// SPI output at all and the panel stays blank. The C6 must therefore NOT touch
// them. Define BUTTONS_LOCAL_GPIO only for legacy bring-up boards whose buttons
// were on the C6 and did not overlap the display pins.
#ifdef BUTTONS_LOCAL_GPIO

#include "driver/gpio.h"

#define DEBOUNCE_MS    25
#define REPEAT_DELAY   380   // hold this long before auto-repeat starts
#define REPEAT_RATE    110   // then one event every this many ms
#define LONG_PRESS_MS  550   // OK held this long => BACK

typedef struct {
    int      pin;
    bool     pressed;        // debounced state
    bool     raw_last;       // last raw sample
    uint32_t edge_ms;        // when raw last changed (for debounce)
    uint32_t press_ms;       // when the debounced press began
    uint32_t next_repeat_ms; // next auto-repeat due time
    bool     long_fired;     // OK long-press already emitted
} btn_t;

static btn_t s_up, s_down, s_ok;

static void btn_setup(btn_t *b, int pin)
{
    b->pin = pin;
    b->pressed = false;
    b->raw_last = false;
    b->edge_ms = 0;
    b->press_ms = 0;
    b->next_repeat_ms = 0;
    b->long_fired = false;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

void buttons_init(void)
{
    btn_setup(&s_up,   BTN_PIN_UP);
    btn_setup(&s_down, BTN_PIN_DOWN);
    btn_setup(&s_ok,   BTN_PIN_OK);
}

// Debounce a single button; returns true on a fresh press edge.
static bool debounce(btn_t *b, uint32_t now)
{
    bool raw = (gpio_get_level(b->pin) == 0);   // active low
    if (raw != b->raw_last) {
        b->raw_last = raw;
        b->edge_ms = now;
        return false;
    }
    if ((now - b->edge_ms) < DEBOUNCE_MS) return false;

    if (raw && !b->pressed) {
        b->pressed = true;
        b->press_ms = now;
        b->next_repeat_ms = now + REPEAT_DELAY;
        b->long_fired = false;
        return true;            // fresh press
    }
    if (!raw && b->pressed) {
        b->pressed = false;
    }
    return false;
}

// UP/DOWN: fire on press edge, then auto-repeat while held.
static uint32_t poll_repeat(btn_t *b, uint32_t now, uint32_t ev_bit)
{
    uint32_t ev = 0;
    if (debounce(b, now)) ev |= ev_bit;
    if (b->pressed && now >= b->next_repeat_ms) {
        ev |= ev_bit;
        b->next_repeat_ms = now + REPEAT_RATE;
    }
    return ev;
}

uint32_t buttons_poll(uint32_t now_ms)
{
    uint32_t ev = 0;

    ev |= poll_repeat(&s_up,   now_ms, BTN_EV_UP);
    ev |= poll_repeat(&s_down, now_ms, BTN_EV_DOWN);

    // OK: long-press => BACK (emitted once during the hold); a short press
    // emits OK on release.
    bool was_pressed = s_ok.pressed;
    bool fresh = debounce(&s_ok, now_ms);
    (void)fresh;
    if (s_ok.pressed) {
        if (!s_ok.long_fired && (now_ms - s_ok.press_ms) >= LONG_PRESS_MS) {
            s_ok.long_fired = true;
            ev |= BTN_EV_BACK;
        }
    } else if (was_pressed) {
        // released
        if (!s_ok.long_fired) ev |= BTN_EV_OK;
    }

    return ev;
}

#else  // !BUTTONS_LOCAL_GPIO

// Production build: button events are delivered by the P4 over DDP. These are
// no-ops so the C6 never reconfigures the display SPI pins (IO5/6/7).
void buttons_init(void) { }

uint32_t buttons_poll(uint32_t now_ms)
{
    (void)now_ms;
    return 0;
}

#endif  // BUTTONS_LOCAL_GPIO
