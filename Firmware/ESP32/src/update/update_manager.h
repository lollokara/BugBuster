#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPDATE_STATE_IDLE = 0,
    UPDATE_STATE_CHECKING,
    UPDATE_STATE_DOWNLOADING_RP2040,
    UPDATE_STATE_FLASHING_RP2040,
    UPDATE_STATE_DOWNLOADING_ESP32,
    UPDATE_STATE_REBOOTING,
    UPDATE_STATE_FAILED,
} update_state_t;

void update_manager_init(void);
esp_err_t update_manager_check(cJSON **out);
esp_err_t update_manager_apply(bool update_rp2040, bool update_esp32, cJSON **out);
esp_err_t update_manager_release_options(uint8_t max_options, cJSON **out);
esp_err_t update_manager_apply_release_index(uint8_t index, bool update_rp2040, bool update_esp32, cJSON **out);
cJSON *update_manager_status_json(void);
bool update_manager_reboot_pending(void);

#ifdef __cplusplus
}
#endif
