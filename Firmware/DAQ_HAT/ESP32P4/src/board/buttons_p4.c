// =============================================================================
// buttons_p4.c — ESP32-P4 front-panel button driver.
// =============================================================================

#include "buttons_p4.h"
#include "ddp_proto.h"
#include "config.h"
#include "driver/gpio.h"

// Timing (ms).
#define DEBOUNCE_MS     25
#define REPEAT_DELAY_MS 380     // hold time before auto-repeat begins
#define REPEAT_RATE_MS  110     // auto-repeat interval
#define LONGPRESS_MS    550     // OK hold -> BACK

typedef struct {
    gpio_num_t pin;
    bool       pressed;         // debounced state
    bool       raw;             // last raw read
    uint32_t   edge_ms;         // last raw transition time (debounce)
    uint32_t   press_ms;        // when the debounced press began
    uint32_t   next_repeat_ms;  // next auto-repeat fire time
    bool       longfired;       // OK long-press already emitted
} btn_t;

static btn_t s_up, s_down, s_ok;

static void btn_setup(btn_t *b, gpio_num_t pin)
{
    b->pin = pin;
    b->pressed = false;
    b->raw = false;
    b->edge_ms = 0;
    b->press_ms = 0;
    b->next_repeat_ms = 0;
    b->longfired = false;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

void buttons_p4_init(void)
{
    btn_setup(&s_up,   BTN_PIN_UP);
    btn_setup(&s_down, BTN_PIN_DOWN);
    btn_setup(&s_ok,   BTN_PIN_OK);
}

// Update one button's debounced state; returns true on a fresh press edge.
static bool debounce(btn_t *b, uint32_t now)
{
    bool raw = (gpio_get_level(b->pin) == 0);   // active-low
    if (raw != b->raw) {
        b->raw = raw;
        b->edge_ms = now;
        return false;
    }
    if ((now - b->edge_ms) < DEBOUNCE_MS) return false;
    if (raw && !b->pressed) {        // press confirmed
        b->pressed = true;
        b->press_ms = now;
        b->next_repeat_ms = now + REPEAT_DELAY_MS;
        b->longfired = false;
        return true;
    }
    if (!raw && b->pressed) {         // release confirmed
        b->pressed = false;
    }
    return false;
}

// UP/DOWN: emit on press edge + auto-repeat while held.
static uint8_t scroll_events(btn_t *b, uint8_t bit, uint32_t now)
{
    uint8_t ev = 0;
    if (debounce(b, now)) {
        ev |= bit;
    } else if (b->pressed && (int32_t)(now - b->next_repeat_ms) >= 0) {
        ev |= bit;
        b->next_repeat_ms = now + REPEAT_RATE_MS;
    }
    return ev;
}

uint8_t buttons_p4_poll(uint32_t now_ms)
{
    uint8_t ev = 0;

    ev |= scroll_events(&s_up,   DDP_BTN_UP,   now_ms);
    ev |= scroll_events(&s_down, DDP_BTN_DOWN, now_ms);

    // OK: long-press (>=LONGPRESS_MS) -> BACK once; short press -> OK on release.
    bool was_pressed = s_ok.pressed;
    debounce(&s_ok, now_ms);
    if (s_ok.pressed) {
        if (!s_ok.longfired && (now_ms - s_ok.press_ms) >= LONGPRESS_MS) {
            ev |= DDP_BTN_BACK;
            s_ok.longfired = true;
        }
    } else if (was_pressed && !s_ok.longfired) {
        // Released before the long-press threshold -> short OK.
        ev |= DDP_BTN_OK;
    }
    return ev;
}
