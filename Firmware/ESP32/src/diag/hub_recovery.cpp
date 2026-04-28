// =============================================================================
// hub_recovery.cpp - USB hub enumeration recovery watchdog
//
// Detects failed USB enumeration (PD cable plugged before data cable) and
// recovers by pulsing the USB2422 hub RESET_N line via PCA9535 EN_USB_HUB.
// RTC NOINIT storage (with magic word) tracks retry attempts across soft/
// brownout resets while clearing on true power-on reset.
// =============================================================================

#include "hub_recovery.h"
#include "hal/pca9535.h"
#include "tusb.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hub_recov";

#define HUBREC_MAGIC      0x48425245u   // 'HBRE'
#define HUBREC_MAX_TRIES  2u
#define GRACE_US          (10ULL * 1000 * 1000)
#define PULSE_LOW_MS      10
#define POST_PULSE_OBS_US (3ULL * 1000 * 1000)

// Survives soft/brownout reboots; cleared on power-on reset.
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_attempt_count;

typedef enum { ST_GRACE, ST_PULSING, ST_OBSERVING, ST_DONE } HubRecState;

static HubRecState s_state     = ST_GRACE;
static int64_t     s_start_us  = 0;
static int64_t     s_obs_start_us = 0;

void hub_recovery_init(void)
{
    if (s_magic != HUBREC_MAGIC) {
        // Cold boot (POR or magic corrupted). Reset the counter.
        s_magic = HUBREC_MAGIC;
        s_attempt_count = 0;
    }
    s_state    = ST_GRACE;
    s_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "armed (prior_attempts=%u, reset_reason=%d)",
             (unsigned)s_attempt_count, (int)esp_reset_reason());
}

void hub_recovery_tick(void)
{
    int64_t now = esp_timer_get_time();

    switch (s_state) {
    case ST_GRACE:
        if (tud_mounted()) {
            s_attempt_count = 0;   // success — clear so next cold boot starts fresh
            s_state = ST_DONE;
            ESP_LOGI(TAG, "USB enumerated within grace; nothing to do");
            return;
        }
        if (now - s_start_us < (int64_t)GRACE_US) return;
        if (s_attempt_count >= HUBREC_MAX_TRIES) {
            ESP_LOGW(TAG, "grace expired and retry budget exhausted (%u); giving up",
                     (unsigned)s_attempt_count);
            s_state = ST_DONE;
            return;
        }
        ESP_LOGW(TAG, "grace expired without enumeration; pulsing hub RESET (attempt %u/%u)",
                 (unsigned)(s_attempt_count + 1), (unsigned)HUBREC_MAX_TRIES);
        s_attempt_count++;   // increment before pulse so brownout-and-reboot still counts
        // Drive RESET_N low (asserts hub reset; may brownout this MCU briefly)
        pca9535_set_control(PCA_CTRL_USB_HUB_EN, false);
        vTaskDelay(pdMS_TO_TICKS(PULSE_LOW_MS));
        // Deassert reset
        pca9535_set_control(PCA_CTRL_USB_HUB_EN, true);
        s_obs_start_us = esp_timer_get_time();
        s_state = ST_OBSERVING;
        return;

    case ST_OBSERVING:
        if (tud_mounted()) {
            ESP_LOGI(TAG, "USB enumerated after hub pulse (attempt %u)",
                     (unsigned)s_attempt_count);
            s_attempt_count = 0;   // success; clear so next cold boot starts at zero
            s_state = ST_DONE;
            return;
        }
        if (now - s_obs_start_us > (int64_t)POST_PULSE_OBS_US) {
            ESP_LOGW(TAG, "no enumeration in observation window after attempt %u",
                     (unsigned)s_attempt_count);
            // Re-enter grace for the next attempt window
            s_state    = ST_GRACE;
            s_start_us = esp_timer_get_time();
        }
        return;

    case ST_DONE:
    case ST_PULSING:
    default:
        return;
    }
}
