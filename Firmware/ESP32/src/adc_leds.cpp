// =============================================================================
// adc_leds.cpp - AD74416H on-chip GPIO status LED driver
//
// GPIO mapping (AD74416H pins 0..5 = A..F):
//   GPIO 0 (A) — green, channel fault OK
//   GPIO 1 (B) — red,   channel fault ERROR
//   GPIO 2 (C) — green, selftest/cal OK
//   GPIO 3 (D) — red,   selftest/cal ERROR
//   GPIO 4 (E) — green, supply/reference OK
//   GPIO 5 (F) — red,   supply/reference ERROR
//
// LED state logic (per pair):
//   green=ON, red=OFF  → OK
//   green=OFF, red=ON  → FAULT
//   green=OFF, red=OFF → UNINIT (not yet initialized / ADC disabled)
// =============================================================================

#include "adc_leds.h"
#include "ad74416h.h"
#include "ad74416h_regs.h"
#include "selftest.h"
#include "tasks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "adc_leds";

// Minimum tick interval in FreeRTOS ticks (~200 ms debounce)
#define ADC_LEDS_MIN_INTERVAL_MS    200

// GPIO indices
#define GPIO_A  0   // green: channel OK
#define GPIO_B  1   // red:   channel fault
#define GPIO_C  2   // green: selftest OK
#define GPIO_D  3   // red:   selftest fault
#define GPIO_E  4   // green: supply OK
#define GPIO_F  5   // red:   supply fault

// LED group state
typedef enum {
    LED_STATE_UNINIT = 0,   // both off
    LED_STATE_OK,           // green on, red off
    LED_STATE_FAULT,        // green off, red on
} LedState;

// Internal state
static bool      s_initialized     = false;
static bool      s_manual_override = false;
static TickType_t s_last_tick      = 0;

static LedState  s_channel_state   = LED_STATE_UNINIT;
static LedState  s_selftest_state  = LED_STATE_UNINIT;
static LedState  s_supply_state    = LED_STATE_UNINIT;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void drive_pair(uint8_t gpio_green, uint8_t gpio_red, LedState state)
{
    AD74416H *dev = tasks_get_device();
    if (!dev) return;

    switch (state) {
        case LED_STATE_OK:
            dev->setGpioOutput(gpio_green, true);
            dev->setGpioOutput(gpio_red,   false);
            break;
        case LED_STATE_FAULT:
            dev->setGpioOutput(gpio_green, false);
            dev->setGpioOutput(gpio_red,   true);
            break;
        case LED_STATE_UNINIT:
        default:
            dev->setGpioOutput(gpio_green, false);
            dev->setGpioOutput(gpio_red,   false);
            break;
    }
}

static void apply_all(void)
{
    drive_pair(GPIO_A, GPIO_B, s_channel_state);
    drive_pair(GPIO_C, GPIO_D, s_selftest_state);
    drive_pair(GPIO_E, GPIO_F, s_supply_state);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void adc_leds_init(void)
{
    AD74416H *dev = tasks_get_device();
    if (!dev) {
        ESP_LOGW(TAG, "adc_leds_init: device not ready");
        return;
    }

    // Configure all 6 AD74416H GPIOs as push-pull outputs, pulldown disabled
    for (uint8_t g = 0; g < 6; g++) {
        dev->configureGpio(g, GPIO_SEL_OUTPUT, false);
    }

    // Drive all LEDs off (UNINIT state)
    for (uint8_t g = 0; g < 6; g++) {
        dev->setGpioOutput(g, false);
    }

    s_channel_state   = LED_STATE_UNINIT;
    s_selftest_state  = LED_STATE_UNINIT;
    s_supply_state    = LED_STATE_UNINIT;
    s_manual_override = false; // Run automatically
    s_last_tick       = 0;
    s_initialized     = true;

    ESP_LOGI(TAG, "AD74416H status LEDs initialized (GPIO A..F)");
}

void adc_leds_tick(void)
{
    if (!s_initialized || s_manual_override) return;

    // Throttle: only update every ADC_LEDS_MIN_INTERVAL_MS
    TickType_t now = xTaskGetTickCount();
    if (s_last_tick != 0 &&
        (now - s_last_tick) < pdMS_TO_TICKS(ADC_LEDS_MIN_INTERVAL_MS)) {
        return;
    }
    s_last_tick = now;

    AD74416H *dev = tasks_get_device();
    if (!dev) return;

    // -----------------------------------------------------------------------
    // Group AB: Channel fault status
    // Read per-channel CHANNEL_ALERT_STATUS[0..3] and global CH_ALERT bits.
    // FAULT if any channel reports any alert (OC/HI/LO/saturation/overrange/alert).
    // UNINIT if all channels are HIGH_IMP (ADC disabled).
    // -----------------------------------------------------------------------
    {
        uint16_t alertStatus = 0;
        dev->readAlertStatus(&alertStatus);

        // CH_ALERT_A..D are bits 8..11 of ALERT_STATUS
        bool any_ch_alert_in_global =
            (alertStatus & (ALERT_STATUS_CH_ALERT_A_MASK |
                            ALERT_STATUS_CH_ALERT_B_MASK |
                            ALERT_STATUS_CH_ALERT_C_MASK |
                            ALERT_STATUS_CH_ALERT_D_MASK)) != 0;

        // Also check per-channel registers for any non-zero fault bits
        bool any_channel_fault = any_ch_alert_in_global;
        if (!any_channel_fault) {
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                uint16_t chanAlert = 0;
                dev->readChannelAlertStatus(ch, &chanAlert);
                if (chanAlert != 0) {
                    any_channel_fault = true;
                    break;
                }
            }
        }

        // Determine if ADC is active: check g_deviceState for any non-HIGH_IMP channel
        bool adc_active = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                if (g_deviceState.channels[ch].function != CH_FUNC_HIGH_IMP) {
                    adc_active = true;
                    break;
                }
            }
            xSemaphoreGive(g_stateMutex);
        }

        if (!adc_active) {
            s_channel_state = LED_STATE_UNINIT;
        } else if (any_channel_fault) {
            s_channel_state = LED_STATE_FAULT;
        } else {
            s_channel_state = LED_STATE_OK;
        }
    }

    // -----------------------------------------------------------------------
    // Group CD: Self-test & calibration
    // OK: boot selftest ran and passed AND both IDAC rails have valid cal data.
    // FAULT: selftest failed, cal invalid, or diagnostic out of range.
    // UNINIT: selftest never run (pre-init).
    // -----------------------------------------------------------------------
    {
        const SelftestBootResult *boot = selftest_get_boot_result();
        const SelftestCalResult  *cal  = selftest_get_cal_result();

        if (!boot || !boot->ran) {
            s_selftest_state = LED_STATE_UNINIT;
        } else if (!boot->passed ||
                   (cal && cal->status == CAL_STATUS_FAILED)) {
            s_selftest_state = LED_STATE_FAULT;
        } else {
            // Passed boot test; cal either succeeded or hasn't been run yet
            // (not-run is not a failure, only explicit failure counts)
            s_selftest_state = LED_STATE_OK;
        }
    }

    // -----------------------------------------------------------------------
    // Group EF: Supply / reference / comm
    // OK: SUPPLY_ALERT_STATUS == 0 AND no thermal/SPI-CRC/unexpected-reset/
    //     watchdog bits in global ALERT_STATUS.
    // FAULT: any supply fault, thermal warning, SPI CRC error, unexpected reset.
    // UNINIT: device not initialized (s_initialized check at top covers this).
    // -----------------------------------------------------------------------
    {
        uint16_t alertStatus = 0;
        uint16_t supplyAlertStatus = 0;
        dev->readAlertStatus(&alertStatus);
        dev->readSupplyAlertStatus(&supplyAlertStatus);

        // Supply/comm fault bits in global ALERT_STATUS:
        //   bit0 RESET_OCCURRED — unexpected reset
        //   bit2 SUPPLY_ERR     — supply fault
        //   bit3 SPI_ERR        — SPI CRC error
        //   bit4 TEMP_ALERT     — thermal warning
        // ADC_ERR (bit5) is excluded here — it belongs to channel group.
        const uint16_t supply_comm_mask = ALERT_STATUS_RESET_OCCURRED_MASK |
                                          ALERT_STATUS_SUPPLY_ERR_MASK      |
                                          ALERT_STATUS_SPI_ERR_MASK         |
                                          ALERT_STATUS_TEMP_ALERT_MASK;

        bool supply_fault = (supplyAlertStatus != 0) ||
                            ((alertStatus & supply_comm_mask) != 0);

        s_supply_state = supply_fault ? LED_STATE_FAULT : LED_STATE_OK;
    }

    apply_all();
}

void adc_leds_set_manual(bool manual)
{
    s_manual_override = manual;
    ESP_LOGI(TAG, "LED manual override: %s", manual ? "ON" : "OFF");
}

bool adc_leds_manual_active(void)
{
    return s_manual_override;
}
