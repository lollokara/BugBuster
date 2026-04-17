// =============================================================================
// board_profile.cpp — DUT board profile registry (firmware-side)
// =============================================================================

#include "board_profile.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "board_profile";

// -----------------------------------------------------------------------------
// Built-in profile table
//
// Schema mirrors python/bugbuster_mcp/board_profiles/*.json so the host and
// firmware agree on field semantics. New profiles can be added here or loaded
// from host tooling via a future /api/board/upload endpoint.
// -----------------------------------------------------------------------------

static const BoardProfile kProfiles[] = {
    {
        .id            = "new-board",
        .name          = "New Board",
        .description   = "Custom DUT profile (defaults)",
        .vlogic        = 3.3f, .vlogicLocked = true,
        .vadj1         = 3.3f, .vadj1Locked  = false,
        .vadj2         = 5.0f, .vadj2Locked  = true,
        .pinCount      = 12,
    },
    {
        .id            = "stm32f4-discovery",
        .name          = "STM32F4 Discovery",
        .description   = "STM32F407 Discovery board (SWD + USART2)",
        .vlogic        = 3.3f, .vlogicLocked = true,
        .vadj1         = 3.3f, .vadj1Locked  = false,
        .vadj2         = 5.0f, .vadj2Locked  = true,
        .pinCount      = 12,
    },
};

static constexpr uint8_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

// -----------------------------------------------------------------------------
// Active selection (NVS-backed, cached in RAM)
// -----------------------------------------------------------------------------

static const BoardProfile *s_active = nullptr;

static const BoardProfile *find_by_id(const char *id)
{
    if (!id) return nullptr;
    for (uint8_t i = 0; i < kProfileCount; i++) {
        if (strcmp(kProfiles[i].id, id) == 0) {
            return &kProfiles[i];
        }
    }
    return nullptr;
}

void board_profile_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("board", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace 'board': %s", esp_err_to_name(err));
        return;
    }

    char id[32] = {0};
    size_t len = sizeof(id);
    err = nvs_get_str(nvs, "active_id", id, &len);
    nvs_close(nvs);

    if (err == ESP_OK) {
        s_active = find_by_id(id);
        if (s_active) {
            ESP_LOGI(TAG, "Active board profile restored: %s (%s)", s_active->name, s_active->id);
        } else {
            ESP_LOGW(TAG, "Stored board id '%s' not in registry; no active profile", id);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No board profile selected yet");
    } else {
        ESP_LOGW(TAG, "NVS read failed for board.active_id: %s", esp_err_to_name(err));
    }
}

const BoardProfile *board_profile_get_active(void)
{
    return s_active;
}

bool board_profile_select(const char *id)
{
    const BoardProfile *p = find_by_id(id);
    if (!p) {
        ESP_LOGW(TAG, "Unknown board id: %s", id ? id : "(null)");
        return false;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("board", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs, "active_id", p->id);
    if (err == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
        return false;
    }

    s_active = p;
    ESP_LOGI(TAG, "Board profile selected: %s (%s)", p->name, p->id);
    return true;
}

uint8_t board_profile_count(void)
{
    return kProfileCount;
}

const BoardProfile *board_profile_at(uint8_t index)
{
    if (index >= kProfileCount) return nullptr;
    return &kProfiles[index];
}
