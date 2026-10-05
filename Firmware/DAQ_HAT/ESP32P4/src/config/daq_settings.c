// =============================================================================
// daq_settings.c — ESP32-P4 authoritative settings store implementation.
// =============================================================================

#include "daq_settings.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "daqcfg";
#define NVS_NS "daqcfg"

// Per-setting live state, indexed in lockstep with daq_config_table().
typedef struct {
    int32_t ival;       // scalar value (also length-agnostic store)
    char   *sval;       // heap string for DAQ_T_STR keys, else NULL
} slot_t;

static const daq_setting_schema_t *s_schema;   // == daq_config_table()
static size_t                      s_count;
static slot_t                     *s_slots;
static SemaphoreHandle_t           s_lock;

static daq_settings_apply_cb_t   s_apply;
static daq_settings_notify_cb_t  s_notify;
static daq_settings_action_cb_t  s_action;
static daq_settings_guard_cb_t   s_guard;
static void                     *s_user;

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------
static int slot_index(uint16_t key)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_schema[i].key == key) return (int)i;
    }
    return -1;
}

static void nvs_key_for(uint16_t key, char out[8])
{
    // 4 hex digits -> "k1A2B"; well under the 15-char NVS key limit.
    snprintf(out, 8, "k%04X", key);
}

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---------------------------------------------------------------------------
// NVS persistence.
// ---------------------------------------------------------------------------
static void persist_scalar(uint16_t key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k[8]; nvs_key_for(key, k);
    nvs_set_i32(h, k, value);
    nvs_commit(h);
    nvs_close(h);
}

static void persist_str(uint16_t key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k[8]; nvs_key_for(key, k);
    nvs_set_str(h, k, value ? value : "");
    nvs_commit(h);
    nvs_close(h);
}

static void load_overrides(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings; using defaults");
        return;
    }
    for (size_t i = 0; i < s_count; i++) {
        const daq_setting_schema_t *sc = &s_schema[i];
        if (!(sc->flags & DAQ_F_PERSIST)) continue;
        char k[8]; nvs_key_for(sc->key, k);
        if (sc->type == DAQ_T_STR) {
            size_t len = 0;
            if (nvs_get_str(h, k, NULL, &len) == ESP_OK && len > 0) {
                char *buf = malloc(len);
                if (buf && nvs_get_str(h, k, buf, &len) == ESP_OK) {
                    free(s_slots[i].sval);
                    s_slots[i].sval = buf;
                } else {
                    free(buf);
                }
            }
        } else {
            int32_t v;
            if (nvs_get_i32(h, k, &v) == ESP_OK) {
                s_slots[i].ival = daq_config_clamp(sc->key, v);
            }
        }
    }
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Init.
// ---------------------------------------------------------------------------
void daq_settings_init(void)
{
    s_schema = daq_config_table(&s_count);
    s_slots  = calloc(s_count, sizeof(slot_t));
    s_lock   = xSemaphoreCreateMutex();

    // Seed defaults from the schema.
    for (size_t i = 0; i < s_count; i++) {
        s_slots[i].ival = s_schema[i].def;
        s_slots[i].sval = NULL;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        load_overrides();
    } else {
        ESP_LOGW(TAG, "NVS init failed (%s); defaults only", esp_err_to_name(err));
    }
}

void daq_settings_set_callbacks(daq_settings_apply_cb_t apply,
                                daq_settings_notify_cb_t notify,
                                daq_settings_action_cb_t action,
                                void *user)
{
    s_apply  = apply;
    s_notify = notify;
    s_action = action;
    s_user   = user;
}

void daq_settings_set_guard(daq_settings_guard_cb_t guard)
{
    s_guard = guard;
}

// ---------------------------------------------------------------------------
// C6-20: the C6 menu resends the whole settings set on every edit. Re-applying
// an UNCHANGED value from it restarted the acquisition (rate/range/filter) on a
// brightness change. Every other source keeps re-applying unchanged values:
// the CLI, S3 and local firmware rely on that re-assert (e.g. retrying
// `vdut on` after a USB-PD refusal left the slot at 1 with the supply off).
// ---------------------------------------------------------------------------
static bool should_apply(bool changed, daq_src_t src)
{
    return changed || src != DAQ_SRC_C6;
}

// ---------------------------------------------------------------------------
// Scalar accessors.
// ---------------------------------------------------------------------------
bool daq_settings_get_i32(uint16_t key, int32_t *out)
{
    int i = slot_index(key);
    if (i < 0 || s_schema[i].type == DAQ_T_STR) return false;
    lock();
    if (out) *out = s_slots[i].ival;
    unlock();
    return true;
}

bool daq_settings_set_i32(uint16_t key, int32_t value, daq_src_t src)
{
    int i = slot_index(key);
    if (i < 0) return false;
    const daq_setting_schema_t *sc = &s_schema[i];
    if (sc->type == DAQ_T_STR) return false;
    if (sc->flags & DAQ_F_READONLY) return false;

    value = daq_config_clamp(key, value);
    if (s_guard && !s_guard(key, 0, value, src)) return false;

    lock();
    bool changed = (s_slots[i].ival != value);
    s_slots[i].ival = value;
    unlock();

    if (changed && (sc->flags & DAQ_F_PERSIST)) persist_scalar(key, value);
    if (s_apply && should_apply(changed, src))  s_apply(key, value, NULL, s_user);
    if (changed && s_notify) s_notify(key, src, s_user);
    return true;
}

bool daq_settings_shadow_set_i32(uint16_t key, int32_t value)
{
    int i = slot_index(key);
    if (i < 0) return false;
    const daq_setting_schema_t *sc = &s_schema[i];
    if (sc->type == DAQ_T_STR || (sc->flags & DAQ_F_READONLY)) return false;
    value = daq_config_clamp(key, value);
    lock();
    s_slots[i].ival = value;
    unlock();
    return true;
}

// ---------------------------------------------------------------------------
// String accessors.
// ---------------------------------------------------------------------------
bool daq_settings_get_str(uint16_t key, char *buf, size_t cap)
{
    int i = slot_index(key);
    if (i < 0 || s_schema[i].type != DAQ_T_STR || !buf || cap == 0) return false;
    lock();
    const char *s = s_slots[i].sval ? s_slots[i].sval : "";
    strncpy(buf, s, cap - 1);
    buf[cap - 1] = '\0';
    unlock();
    return true;
}

bool daq_settings_set_str(uint16_t key, const char *val, daq_src_t src)
{
    int i = slot_index(key);
    if (i < 0) return false;
    const daq_setting_schema_t *sc = &s_schema[i];
    if (sc->type != DAQ_T_STR || (sc->flags & DAQ_F_READONLY)) return false;
    if (!val) val = "";
    if (s_guard && !s_guard(key, 0, 0, src)) return false;

    // Enforce the schema max length (max == byte cap for strings).
    size_t maxlen = (sc->max > 0) ? (size_t)sc->max : 0;
    size_t len = strlen(val);
    if (maxlen && len > maxlen) len = maxlen;

    char *copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, val, len);
    copy[len] = '\0';

    lock();
    bool changed = (s_slots[i].sval == NULL) || (strcmp(s_slots[i].sval, copy) != 0);
    free(s_slots[i].sval);
    s_slots[i].sval = copy;
    unlock();

    if (changed && (sc->flags & DAQ_F_PERSIST)) persist_str(key, copy);
    if (s_apply && should_apply(changed, src))  s_apply(key, 0, copy, s_user);
    if (changed && s_notify) s_notify(key, src, s_user);
    return true;
}

// ---------------------------------------------------------------------------
// TLV bridges.
// ---------------------------------------------------------------------------
bool daq_settings_apply_tlv(const uint8_t *tlv, size_t len, daq_src_t src)
{
    uint16_t key; uint8_t type, vlen; const uint8_t *val;
    if (daq_tlv_parse(tlv, len, &key, &type, &val, &vlen) < 0) return false;

    const daq_setting_schema_t *sc = daq_config_schema(key);
    if (!sc) return false;

    if (sc->type == DAQ_T_STR) {
        char tmp[DAQ_TLV_MAX_VAL + 1];
        if (vlen > DAQ_TLV_MAX_VAL) vlen = DAQ_TLV_MAX_VAL;
        if (vlen && val) memcpy(tmp, val, vlen);
        tmp[vlen] = '\0';
        return daq_settings_set_str(key, tmp, src);
    }

    int32_t iv;
    if (!daq_tlv_value_i32(type, val, vlen, &iv)) return false;
    return daq_settings_set_i32(key, iv, src);
}

int daq_settings_encode_one(uint16_t key, uint8_t *buf, size_t cap)
{
    int i = slot_index(key);
    if (i < 0) return -1;
    const daq_setting_schema_t *sc = &s_schema[i];

    if (sc->type == DAQ_T_STR) {
        char s[DAQ_TLV_MAX_VAL + 1];
        daq_settings_get_str(key, s, sizeof(s));
        uint8_t l = (uint8_t)strlen(s);
        if (l > DAQ_TLV_MAX_VAL) l = DAQ_TLV_MAX_VAL;
        return daq_tlv_encode(buf, cap, key, DAQ_T_STR, s, l);
    }
    int32_t v = 0;
    daq_settings_get_i32(key, &v);
    return daq_tlv_encode_i32(buf, cap, key, sc->type, v);
}

int daq_settings_encode_all(uint8_t *buf, size_t cap, bool include_secret)
{
    size_t off = 0;
    for (size_t i = 0; i < s_count; i++) {
        const daq_setting_schema_t *sc = &s_schema[i];
        int n;
        if ((sc->flags & DAQ_F_SECRET) && !include_secret) {
            // Emit the key with an empty value so UIs know it exists.
            n = daq_tlv_encode(buf + off, cap - off, sc->key, sc->type, NULL, 0);
        } else {
            n = daq_settings_encode_one(sc->key, buf + off, cap - off);
        }
        if (n < 0) return -1;
        off += (size_t)n;
    }
    return (int)off;
}

// ---------------------------------------------------------------------------
// Actions + bulk apply.
// ---------------------------------------------------------------------------
bool daq_settings_action(uint8_t action_id, daq_src_t src)
{
    if (s_guard && !s_guard(0, action_id, 0, src)) return false;
    if (action_id == DAQ_ACT_FACTORY_RESET) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        // Reset live values to defaults and re-apply.
        lock();
        for (size_t i = 0; i < s_count; i++) {
            s_slots[i].ival = s_schema[i].def;
            free(s_slots[i].sval);
            s_slots[i].sval = NULL;
        }
        unlock();
        daq_settings_apply_all();
        // C6-21: every value just changed, so tell the mirrors (the glue pushes
        // non-C6 notifications to the C6). Tagged LOCAL so a reset started from
        // the C6 menu is still pushed back to it. Secrets are not mirrored.
        if (s_notify) {
            for (size_t i = 0; i < s_count; i++) {
                if (s_schema[i].flags & DAQ_F_SECRET) continue;
                s_notify(s_schema[i].key, DAQ_SRC_LOCAL, s_user);
            }
        }
    }
    if (s_action) return s_action(action_id, s_user);
    return true;
}

size_t daq_settings_count_nonsecret(void)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (!(s_schema[i].flags & DAQ_F_SECRET)) n++;
    }
    return n;
}

int daq_settings_encode_chunk(size_t *idx, uint8_t *buf, size_t cap)
{
    size_t off = 0;
    while (*idx < s_count) {
        const daq_setting_schema_t *sc = &s_schema[*idx];
        if (sc->flags & DAQ_F_SECRET) { (*idx)++; continue; }
        int n = daq_settings_encode_one(sc->key, buf + off, cap - off);
        if (n < 0) {
            if (off == 0) { (*idx)++; continue; }   // cannot ever fit: skip it
            break;                                    // next chunk
        }
        off += (size_t)n;
        (*idx)++;
    }
    return (int)off;
}

void daq_settings_apply_all(void)
{
    if (!s_apply) return;
    for (size_t i = 0; i < s_count; i++) {
        const daq_setting_schema_t *sc = &s_schema[i];
        if (sc->type == DAQ_T_STR) {
            char s[DAQ_TLV_MAX_VAL + 1];
            daq_settings_get_str(sc->key, s, sizeof(s));
            s_apply(sc->key, 0, s, s_user);
        } else {
            int32_t v = 0;
            daq_settings_get_i32(sc->key, &v);
            s_apply(sc->key, v, NULL, s_user);
        }
    }
}
