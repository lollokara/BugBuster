// =============================================================================
// cmd_ota.cpp — Registry handlers for USB OTA control
//
//   BBP_CMD_OTA (0x77)
//     op 0x01 — OTA info snapshot
//     op 0x02 — OTA rollback
//     op 0x10 — OTA begin
//     op 0x11 — OTA chunk / finalise
//     op 0x12 — OTA abort
// =============================================================================

#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"

#include "update/update_manager.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

static const char *TAG = "cmd_ota";

enum OtaOp : uint8_t {
    OTA_OP_INFO = 0x01,
    OTA_OP_ROLLBACK = 0x02,
    OTA_OP_BEGIN = 0x10,
    OTA_OP_CHUNK = 0x11,
    OTA_OP_ABORT = 0x12,
};

enum OtaTarget : uint8_t {
    OTA_TARGET_ESP32 = 0x00,
    OTA_TARGET_RP2040 = 0x01,
    OTA_TARGET_SPIFFS = 0x02,
};

static const char *const RP2040_STAGE_PATH = "/scripts/update-rp2040.bin";

typedef struct {
    bool active;
    uint8_t target;
    uint32_t total_size;
    uint32_t written;
    bool has_sha;
    bool sha_ready;
    uint8_t expected_sha[32];
    mbedtls_sha256_context sha;
    bool esp_ota_started;
    esp_ota_handle_t ota_handle;
    const esp_partition_t *ota_partition;
    FILE *stage_file;
    bool spiffs_unmounted;
    const esp_partition_t *spiffs_partition;
} OtaSession;

static OtaSession s_ota = {};

static void ota_reset_session(bool keep_partition_cleanup)
{
    if (s_ota.stage_file) {
        fclose(s_ota.stage_file);
        s_ota.stage_file = NULL;
    }
    if (s_ota.target == OTA_TARGET_RP2040) {
        remove(RP2040_STAGE_PATH);
    }
    if (s_ota.esp_ota_started) {
        esp_ota_abort(s_ota.ota_handle);
        s_ota.esp_ota_started = false;
    }
    if (s_ota.spiffs_unmounted) {
        esp_vfs_spiffs_conf_t conf = {
            .base_path = "/spiffs",
            .partition_label = "spiffs",
            .max_files = 5,
            .format_if_mount_failed = false,
        };
        esp_err_t err = esp_vfs_spiffs_register(&conf);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SPIFFS remount after OTA cleanup failed: %s", esp_err_to_name(err));
        }
        s_ota.spiffs_unmounted = false;
    }
    if (s_ota.sha_ready) {
        mbedtls_sha256_free(&s_ota.sha);
        s_ota.sha_ready = false;
    }
    if (!keep_partition_cleanup) {
        s_ota.ota_partition = NULL;
        s_ota.spiffs_partition = NULL;
    }
    memset(&s_ota, 0, sizeof(s_ota));
}

typedef struct {
    uint32_t delay_ms;
} OtaRebootArgs;

static void ota_reboot_task(void *arg)
{
    OtaRebootArgs *a = (OtaRebootArgs *)arg;
    vTaskDelay(pdMS_TO_TICKS(a->delay_ms));
    delete a;
    esp_restart();
}

static void ota_schedule_reboot(uint32_t delay_ms)
{
    auto *args = new OtaRebootArgs{delay_ms};
    BaseType_t ok = xTaskCreate(ota_reboot_task, "ota_reboot", 3072, args, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        delete args;
        ESP_LOGE(TAG, "Failed to schedule reboot");
    }
}

static bool ota_append_sha(const uint8_t *data, size_t len)
{
    if (!s_ota.has_sha) return true;
    mbedtls_sha256_update(&s_ota.sha, data, len);
    return true;
}

static bool ota_verify_sha(void)
{
    if (!s_ota.has_sha) return true;
    uint8_t actual[32] = {};
    mbedtls_sha256_finish(&s_ota.sha, actual);
    if (memcmp(actual, s_ota.expected_sha, sizeof(actual)) != 0) {
        ESP_LOGW(TAG, "USB OTA SHA-256 mismatch");
        return false;
    }
    return true;
}

static int ota_json_response(cJSON *root, uint8_t *resp, size_t *resp_len)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return -CMD_ERR_INTERNAL;
    }

    size_t len = strlen(json);
    if (len > BBP_MAX_PAYLOAD) {
        free(json);
        return -CMD_ERR_FRAME_TOO_LARGE;
    }

    memcpy(resp, json, len);
    *resp_len = len;
    free(json);
    return (int)len;
}

static int ota_send_info(uint8_t *resp, size_t *resp_len)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "date", app->date);
    cJSON_AddStringToObject(root, "time", app->time);
    cJSON_AddStringToObject(root, "idfVersion", app->idf_ver);
    cJSON_AddNumberToObject(root, "fwMajor", BBP_FW_VERSION_MAJOR);
    cJSON_AddNumberToObject(root, "fwMinor", BBP_FW_VERSION_MINOR);
    cJSON_AddNumberToObject(root, "fwPatch", BBP_FW_VERSION_PATCH);
    cJSON_AddNumberToObject(root, "protoVersion", BBP_PROTO_VERSION);

    if (running) {
        cJSON_AddStringToObject(root, "partition", running->label);
        cJSON_AddNumberToObject(root, "partitionSize", running->size);
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
            const char *state = "UNDEFINED";
            switch (st) {
                case ESP_OTA_IMG_NEW:            state = "NEW"; break;
                case ESP_OTA_IMG_PENDING_VERIFY:  state = "PENDING_VERIFY"; break;
                case ESP_OTA_IMG_VALID:           state = "VALID"; break;
                case ESP_OTA_IMG_INVALID:         state = "INVALID"; break;
                case ESP_OTA_IMG_ABORTED:         state = "ABORTED"; break;
                case ESP_OTA_IMG_UNDEFINED:       state = "UNDEFINED"; break;
                default:                          state = "UNKNOWN"; break;
            }
            cJSON_AddStringToObject(root, "runningState", state);
        }
    }
    if (next) {
        cJSON_AddStringToObject(root, "nextPartition", next->label);
        cJSON_AddNumberToObject(root, "nextPartitionSize", next->size);
    }
    if (invalid) {
        cJSON_AddStringToObject(root, "lastInvalidPartition", invalid->label);
    }
    cJSON_AddBoolToObject(root, "canRollback", esp_ota_check_rollback_is_possible());
    return ota_json_response(root, resp, resp_len);
}

static bool ota_start_esp32(uint32_t total_size)
{
    s_ota.ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_ota.ota_partition) {
        ESP_LOGE(TAG, "No ESP32 OTA partition available");
        return false;
    }
    esp_err_t err = esp_ota_begin(s_ota.ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_ota.ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return false;
    }
    s_ota.esp_ota_started = true;
    (void)total_size;
    return true;
}

static bool ota_start_rp2040(void)
{
    remove(RP2040_STAGE_PATH);
    s_ota.stage_file = fopen(RP2040_STAGE_PATH, "wb");
    if (!s_ota.stage_file) {
        ESP_LOGE(TAG, "Failed to open RP2040 stage file");
        return false;
    }
    return true;
}

static bool ota_start_spiffs(uint32_t total_size)
{
    s_ota.spiffs_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!s_ota.spiffs_partition) {
        ESP_LOGE(TAG, "SPIFFS partition not found");
        return false;
    }
    if (total_size != s_ota.spiffs_partition->size) {
        ESP_LOGW(TAG, "SPIFFS size mismatch: got %lu expected %lu",
                 (unsigned long)total_size, (unsigned long)s_ota.spiffs_partition->size);
        return false;
    }

    esp_vfs_spiffs_unregister("spiffs");
    s_ota.spiffs_unmounted = true;

    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < s_ota.spiffs_partition->size; off += 4096u) {
        err = esp_partition_erase_range(s_ota.spiffs_partition, off, 4096u);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS erase failed at 0x%zx: %s", off, esp_err_to_name(err));
            return false;
        }
        esp_task_wdt_reset();
    }

    return true;
}

static bool ota_begin_target(uint8_t target, uint32_t total_size, const uint8_t *sha, bool has_sha)
{
    if (s_ota.active) {
        ESP_LOGW(TAG, "OTA begin rejected: session already active");
        return false;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.active = true;
    s_ota.target = target;
    s_ota.total_size = total_size;
    s_ota.has_sha = has_sha;
    if (has_sha) {
        memcpy(s_ota.expected_sha, sha, sizeof(s_ota.expected_sha));
        mbedtls_sha256_init(&s_ota.sha);
        mbedtls_sha256_starts(&s_ota.sha, 0);
        s_ota.sha_ready = true;
    }

    switch (target) {
        case OTA_TARGET_ESP32:
            return ota_start_esp32(total_size);
        case OTA_TARGET_RP2040:
            return ota_start_rp2040();
        case OTA_TARGET_SPIFFS:
            return ota_start_spiffs(total_size);
        default:
            ESP_LOGW(TAG, "Unknown OTA target %u", target);
            return false;
    }
}

static bool ota_finalize_target(void)
{
    if (!ota_verify_sha()) {
        return false;
    }

    switch (s_ota.target) {
        case OTA_TARGET_ESP32: {
            if (!s_ota.ota_partition) {
                return false;
            }
            if (esp_ota_end(s_ota.ota_handle) != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end failed");
                return false;
            }
            if (esp_ota_set_boot_partition(s_ota.ota_partition) != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
                return false;
            }
            ota_schedule_reboot(1000);
            return true;
        }
        case OTA_TARGET_RP2040: {
            if (!s_ota.stage_file) {
                return false;
            }
            fclose(s_ota.stage_file);
            s_ota.stage_file = NULL;
            if (update_manager_flash_rp2040_stage(s_ota.total_size) != ESP_OK) {
                return false;
            }
            return true;
        }
        case OTA_TARGET_SPIFFS: {
            if (s_ota.spiffs_unmounted) {
                esp_vfs_spiffs_conf_t conf = {
                    .base_path = "/spiffs",
                    .partition_label = "spiffs",
                    .max_files = 5,
                    .format_if_mount_failed = false,
                };
                esp_err_t err = esp_vfs_spiffs_register(&conf);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "SPIFFS remount failed: %s", esp_err_to_name(err));
                    return false;
                }
                s_ota.spiffs_unmounted = false;
            }
            return true;
        }
        default:
            return false;
    }
}

static int handler_ota(const uint8_t *payload, size_t len, uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;

    size_t rpos = 0;
    uint8_t op = bbp_get_u8(payload, &rpos);

    if (op == OTA_OP_INFO) {
        return ota_send_info(resp, resp_len);
    }

    if (op == OTA_OP_ROLLBACK) {
        if (!esp_ota_check_rollback_is_possible()) {
            return -CMD_ERR_INVALID_STATE;
        }

        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", true);
        cJSON_AddStringToObject(root, "message", "Rolling back; device rebooting");
        int rc = ota_json_response(root, resp, resp_len);
        ota_schedule_reboot(500);
        return rc;
    }

    if (op == OTA_OP_ABORT) {
        ota_reset_session(false);
        resp[0] = 1;
        *resp_len = 1;
        return 1;
    }

    if (op == OTA_OP_BEGIN) {
        if (len < 1 + 1 + 4 + 1) return -CMD_ERR_BAD_ARG;
        uint8_t target = bbp_get_u8(payload, &rpos);
        uint32_t total_size = bbp_get_u32(payload, &rpos);
        bool has_sha = bbp_get_bool(payload, &rpos);
        uint8_t sha[32] = {};
        if (has_sha) {
            if (len < rpos + sizeof(sha)) return -CMD_ERR_BAD_ARG;
            memcpy(sha, payload + rpos, sizeof(sha));
            rpos += sizeof(sha);
        }
        if (total_size == 0) return -CMD_ERR_BAD_ARG;
        if (!ota_begin_target(target, total_size, sha, has_sha)) {
            ota_reset_session(false);
            return -CMD_ERR_INVALID_STATE;
        }
        resp[0] = 1;
        *resp_len = 1;
        return 1;
    }

    if (op == OTA_OP_CHUNK) {
        if (!s_ota.active) return -CMD_ERR_INVALID_STATE;
        if (len < 1 + 1 + 4 + 2 + 1) return -CMD_ERR_BAD_ARG;

        uint8_t target = bbp_get_u8(payload, &rpos);
        uint32_t offset = bbp_get_u32(payload, &rpos);
        uint16_t chunk_len = bbp_get_u16(payload, &rpos);
        bool final_chunk = bbp_get_bool(payload, &rpos);
        if (target != s_ota.target || offset != s_ota.written) return -CMD_ERR_INVALID_STATE;
        if (len < rpos + chunk_len) return -CMD_ERR_BAD_ARG;

        const uint8_t *chunk = payload + rpos;
        bool ok = true;

        switch (s_ota.target) {
            case OTA_TARGET_ESP32: {
                esp_err_t err = esp_ota_write(s_ota.ota_handle, chunk, chunk_len);
                ok = (err == ESP_OK);
                break;
            }
            case OTA_TARGET_RP2040: {
                ok = s_ota.stage_file && fwrite(chunk, 1, chunk_len, s_ota.stage_file) == chunk_len;
                break;
            }
            case OTA_TARGET_SPIFFS: {
                if (!s_ota.spiffs_partition) {
                    ok = false;
                    break;
                }
                size_t write_len = chunk_len;
                uint8_t padded[1024];
                const uint8_t *write_ptr = chunk;
                if (final_chunk && (write_len % 4u) != 0u) {
                    size_t padded_len = (write_len + 3u) & ~3u;
                    if (padded_len > sizeof(padded)) {
                        ok = false;
                        break;
                    }
                    memcpy(padded, chunk, write_len);
                    memset(padded + write_len, 0xFF, padded_len - write_len);
                    write_ptr = padded;
                    write_len = padded_len;
                }
                if (write_len % 4u != 0u) {
                    ok = false;
                    break;
                }
                esp_err_t err = esp_partition_write(s_ota.spiffs_partition, s_ota.written, write_ptr, write_len);
                ok = (err == ESP_OK);
                break;
            }
            default:
                ok = false;
                break;
        }

        if (!ok) {
            ota_reset_session(false);
            return -CMD_ERR_HARDWARE;
        }

        ota_append_sha(chunk, chunk_len);
        s_ota.written += chunk_len;

        uint32_t written = s_ota.written;
        if (final_chunk) {
            if (s_ota.written != s_ota.total_size) {
                ota_reset_session(false);
                return -CMD_ERR_INVALID_STATE;
            }
            if (!ota_finalize_target()) {
                ota_reset_session(false);
                return -CMD_ERR_INTERNAL;
            }
            ota_reset_session(true);
        }

        bbp_put_u32(resp, &rpos, written);
        *resp_len = rpos;
        return (int)rpos;
    }

    return -CMD_ERR_BAD_ARG;
}

static const CmdDescriptor s_ota_cmds[] = {
    {
        BBP_CMD_OTA,
        "ota",
        nullptr, 0,
        nullptr, 0,
        handler_ota,
        CMD_FLAG_ADMIN_REQUIRED,
    },
};

extern "C" void register_cmds_ota(void)
{
    cmd_registry_register_block(s_ota_cmds, sizeof(s_ota_cmds) / sizeof(s_ota_cmds[0]));
}
