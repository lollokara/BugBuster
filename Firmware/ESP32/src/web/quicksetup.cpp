// =============================================================================
// quicksetup.cpp - NVS-backed quick setup slots
// =============================================================================

#include "quicksetup.h"

#include "ad74416h_regs.h"
#include "adgs2414d.h"
#include "config.h"
#include "dio.h"
#include "bus_planner.h"
#include "ds4424.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pca9535.h"
#include "tasks.h"

#include "cJSON.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "quicksetup";
static const char *QS_NAMESPACE = "quicksetup";

static bool is_valid_slot(uint8_t slot)
{
    return slot < QUICKSETUP_SLOT_COUNT;
}

static void slot_key(uint8_t slot, char key[16])
{
    snprintf(key, 16, "qs_slot_%u", (unsigned)slot);
}

static uint8_t summary_hash(const char *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 16777619u;
    }
    return (uint8_t)((h >> 24) ^ (h >> 16) ^ (h >> 8) ^ h);
}

static int json_int(cJSON *obj, const char *key, int def)
{
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsNumber(item)) ? item->valueint : def;
}

static bool json_bool(cJSON *obj, const char *key, bool def)
{
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return item ? cJSON_IsTrue(item) : def;
}

static cJSON *json_obj(cJSON *obj, const char *key)
{
    if (!obj) return NULL;
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsObject(item)) ? item : NULL;
}

static cJSON *json_arr(cJSON *obj, const char *key)
{
    if (!obj) return NULL;
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsArray(item)) ? item : NULL;
}

static bool is_valid_channel_function(uint8_t func)
{
    switch (func) {
        case CH_FUNC_HIGH_IMP:
        case CH_FUNC_VOUT:
        case CH_FUNC_IOUT:
        case CH_FUNC_VIN:
        case CH_FUNC_IIN_EXT_PWR:
        case CH_FUNC_IIN_LOOP_PWR:
        case CH_FUNC_RES_MEAS:
        case CH_FUNC_DIN_LOGIC:
        case CH_FUNC_DIN_LOOP:
        case CH_FUNC_IOUT_HART:
        case CH_FUNC_IIN_EXT_PWR_HART:
        case CH_FUNC_IIN_LOOP_PWR_HART:
            return true;
        default:
            return false;
    }
}

static bool is_valid_adc_rate(uint8_t rate)
{
    switch (rate) {
        case ADC_RATE_10SPS_H:
        case ADC_RATE_20SPS:
        case ADC_RATE_20SPS_H:
        case ADC_RATE_200SPS_H1:
        case ADC_RATE_200SPS_H:
        case ADC_RATE_1_2KSPS:
        case ADC_RATE_1_2KSPS_H:
        case ADC_RATE_4_8KSPS:
        case ADC_RATE_9_6KSPS:
            return true;
        default:
            return false;
    }
}

static void add_failed(QuickSetupApplyReport *report, const char *name)
{
    if (!report || report->failed_count >= QUICKSETUP_FAILED_MAX) return;
    snprintf(report->failed[report->failed_count],
             QUICKSETUP_FAILED_NAME_MAX, "%s", name);
    report->failed_count++;
    report->ok = false;
    report->status = QUICKSETUP_APPLY_ERROR;
}

static void add_bool_array_item(cJSON *arr, bool value)
{
    cJSON_AddItemToArray(arr, cJSON_CreateBool(value));
}

static void add_number_array_item(cJSON *arr, int value)
{
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(value));
}

static cJSON *create_snapshot(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "ts", millis_now() / 1000U);
    cJSON_AddStringToObject(root, "name", "Current Setup");

    cJSON *analog = cJSON_AddObjectToObject(root, "analog");
    cJSON *channels = cJSON_AddArrayToObject(analog, "channels");
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            const ChannelState &cs = g_deviceState.channels[ch];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "fn", (int)cs.function);
            cJSON_AddNumberToObject(obj, "adcMux", (int)cs.adcMux);
            cJSON_AddNumberToObject(obj, "adcRange", (int)cs.adcRange);
            cJSON_AddNumberToObject(obj, "adcRate", (int)cs.adcRate);
            cJSON_AddNumberToObject(obj, "dacCode", (int)cs.dacCode);
            cJSON_AddBoolToObject(obj, "bipolar", false);
            cJSON_AddItemToArray(channels, obj);
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "fn", CH_FUNC_HIGH_IMP);
            cJSON_AddNumberToObject(obj, "adcMux", 0);
            cJSON_AddNumberToObject(obj, "adcRange", 0);
            cJSON_AddNumberToObject(obj, "adcRate", ADC_RATE_200SPS_H1);
            cJSON_AddNumberToObject(obj, "dacCode", 0);
            cJSON_AddBoolToObject(obj, "bipolar", false);
            cJSON_AddItemToArray(channels, obj);
        }
    }

    cJSON *idac = cJSON_AddObjectToObject(root, "idac");
    cJSON *codes = cJSON_AddArrayToObject(idac, "codes");
    const DS4424State *idac_state = ds4424_get_state();
    for (uint8_t ch = 0; ch < 3; ch++) {
        add_number_array_item(codes, idac_state ? idac_state->state[ch].dac_code : 0);
    }

    const PCA9535State *pca_state = pca9535_get_state();
    cJSON *pca = cJSON_AddObjectToObject(root, "pca");
    cJSON_AddBoolToObject(pca, "vadj1", pca_state ? pca_state->vadj1_en : false);
    cJSON_AddBoolToObject(pca, "vadj2", pca_state ? pca_state->vadj2_en : false);
    cJSON_AddBoolToObject(pca, "logic", pca_state ? ((pca_state->output0 & PCA9535_LOGIC_EN) != 0) : false);
    cJSON *efuse = cJSON_AddArrayToObject(pca, "efuse");
    for (uint8_t i = 0; i < 4; i++) {
        add_bool_array_item(efuse, pca_state ? pca_state->efuse_en[i] : false);
    }
    cJSON_AddBoolToObject(pca, "v15", pca_state ? pca_state->en_15v : false);
    cJSON_AddBoolToObject(pca, "usbHub", pca_state ? pca_state->en_usb_hub : false);

    dio_poll_inputs();
    const DioState *dio = dio_get_all();
    cJSON *gpio = cJSON_AddArrayToObject(root, "gpio");
    for (uint8_t i = 0; i < DIO_NUM_IOS; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "mode", dio ? dio[i].mode : DIO_MODE_DISABLED);
        cJSON_AddBoolToObject(obj, "value", dio ? dio[i].output_level : false);
        cJSON_AddItemToArray(gpio, obj);
    }

    cJSON *mux = cJSON_AddObjectToObject(root, "mux");
    cJSON *devices = cJSON_AddArrayToObject(mux, "devices");
    uint8_t mux_states[ADGS_API_MAIN_DEVICES] = {};
    adgs_get_api_states(mux_states);
    for (uint8_t i = 0; i < ADGS_API_MAIN_DEVICES; i++) {
        add_number_array_item(devices, mux_states[i]);
    }

    return root;
}

static QuickSetupStatus read_slot(uint8_t slot, char *out, size_t out_size, size_t *out_len)
{
    if (!is_valid_slot(slot)) return QUICKSETUP_INVALID_SLOT;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(QS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return QUICKSETUP_NOT_FOUND;
    if (err != ESP_OK) return QUICKSETUP_STORAGE_ERROR;

    char key[16];
    slot_key(slot, key);
    size_t len = 0;
    err = nvs_get_blob(nvs, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return QUICKSETUP_NOT_FOUND;
    }
    if (err != ESP_OK || len > QUICKSETUP_MAX_JSON_BYTES) {
        nvs_close(nvs);
        return QUICKSETUP_STORAGE_ERROR;
    }
    if (out_size < len + 1) {
        nvs_close(nvs);
        return QUICKSETUP_BUFFER_TOO_SMALL;
    }

    err = nvs_get_blob(nvs, key, out, &len);
    nvs_close(nvs);
    if (err != ESP_OK) return QUICKSETUP_STORAGE_ERROR;

    out[len] = '\0';
    if (out_len) *out_len = len;
    return QUICKSETUP_OK;
}

bool quicksetup_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(QS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s",
                 QS_NAMESPACE, esp_err_to_name(err));
        return false;
    }
    nvs_close(nvs);
    return true;
}

QuickSetupStatus quicksetup_list(QuickSetupSlotInfo slots[QUICKSETUP_SLOT_COUNT])
{
    if (!slots) return QUICKSETUP_STORAGE_ERROR;
    memset(slots, 0, sizeof(QuickSetupSlotInfo) * QUICKSETUP_SLOT_COUNT);

    for (uint8_t slot = 0; slot < QUICKSETUP_SLOT_COUNT; slot++) {
        slots[slot].index = slot;
        snprintf(slots[slot].name, sizeof(slots[slot].name), "Slot %u", (unsigned)slot);

        char json[QUICKSETUP_MAX_JSON_BYTES + 1];
        size_t len = 0;
        QuickSetupStatus st = read_slot(slot, json, sizeof(json), &len);
        if (st == QUICKSETUP_NOT_FOUND) {
            continue;
        }
        if (st != QUICKSETUP_OK) {
            return st;
        }

        slots[slot].occupied = true;
        slots[slot].size = (uint16_t)len;
        slots[slot].summary_hash = summary_hash(json, len);

        cJSON *doc = cJSON_Parse(json);
        if (doc) {
            cJSON *ts = cJSON_GetObjectItem(doc, "ts");
            if (ts && cJSON_IsNumber(ts)) {
                slots[slot].ts = (uint32_t)ts->valuedouble;
            }
            cJSON *name = cJSON_GetObjectItem(doc, "name");
            if (name && cJSON_IsString(name) && name->valuestring) {
                snprintf(slots[slot].name, sizeof(slots[slot].name), "%s", name->valuestring);
            }
            cJSON_Delete(doc);
        }
    }
    return QUICKSETUP_OK;
}

QuickSetupStatus quicksetup_get(uint8_t slot, char *out, size_t out_size, size_t *out_len)
{
    return read_slot(slot, out, out_size, out_len);
}

QuickSetupStatus quicksetup_save(uint8_t slot, char *out, size_t out_size, size_t *out_len)
{
    if (!is_valid_slot(slot)) return QUICKSETUP_INVALID_SLOT;

    cJSON *snapshot = create_snapshot();
    if (!snapshot) return QUICKSETUP_STORAGE_ERROR;

    char *json = cJSON_PrintUnformatted(snapshot);
    cJSON_Delete(snapshot);
    if (!json) return QUICKSETUP_STORAGE_ERROR;

    size_t len = strlen(json);
    if (len > QUICKSETUP_MAX_JSON_BYTES) {
        free(json);
        return QUICKSETUP_TOO_LARGE;
    }
    if (out && out_size < len + 1) {
        free(json);
        return QUICKSETUP_BUFFER_TOO_SMALL;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(QS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        char key[16];
        slot_key(slot, key);
        err = nvs_set_blob(nvs, key, json, len);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed for slot %u: %s",
                 (unsigned)slot, esp_err_to_name(err));
        free(json);
        return QUICKSETUP_STORAGE_ERROR;
    }

    if (out) {
        memcpy(out, json, len);
        out[len] = '\0';
    }
    if (out_len) *out_len = len;
    free(json);
    return QUICKSETUP_OK;
}

QuickSetupStatus quicksetup_apply(uint8_t slot, QuickSetupApplyReport *report)
{
    if (report) {
        memset(report, 0, sizeof(*report));
        report->ok = true;
        report->status = QUICKSETUP_OK;
    }
    if (!is_valid_slot(slot)) {
        if (report) { report->ok = false; report->status = QUICKSETUP_INVALID_SLOT; }
        return QUICKSETUP_INVALID_SLOT;
    }

    char json[QUICKSETUP_MAX_JSON_BYTES + 1];
    size_t len = 0;
    QuickSetupStatus st = read_slot(slot, json, sizeof(json), &len);
    if (st != QUICKSETUP_OK) {
        if (report) { report->ok = false; report->status = st; }
        return st;
    }

    cJSON *doc = cJSON_Parse(json);

    cJSON *analog = doc ? json_obj(doc, "analog") : NULL;
    cJSON *channels = analog ? json_arr(analog, "channels") : NULL;
    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
        cJSON *ch_obj = channels ? cJSON_GetArrayItem(channels, ch) : NULL;
        if (ch_obj && !cJSON_IsObject(ch_obj)) ch_obj = NULL;

        uint8_t fn = (uint8_t)json_int(ch_obj, "fn", CH_FUNC_HIGH_IMP);
        if (!is_valid_channel_function(fn)) fn = CH_FUNC_HIGH_IMP;
        tasks_apply_channel_function(ch, (ChannelFunction)fn);

        uint8_t mux = (uint8_t)json_int(ch_obj, "adcMux", 0);
        uint8_t range = (uint8_t)json_int(ch_obj, "adcRange", 0);
        uint8_t rate = (uint8_t)json_int(ch_obj, "adcRate", ADC_RATE_200SPS_H1);
        if (!is_valid_adc_rate(rate)) rate = ADC_RATE_200SPS_H1;

        Command adc_cmd = {};
        adc_cmd.type = CMD_ADC_CONFIG;
        adc_cmd.channel = ch;
        adc_cmd.adcCfg.mux = (AdcConvMux)mux;
        adc_cmd.adcCfg.range = (AdcRange)range;
        adc_cmd.adcCfg.rate = (AdcRate)rate;
        if (!sendCommand(adc_cmd)) {
            char name[QUICKSETUP_FAILED_NAME_MAX];
            snprintf(name, sizeof(name), "adc_ch%u", (unsigned)ch);
            add_failed(report, name);
        }

        bool bipolar = json_bool(ch_obj, "bipolar", false);
        if (!tasks_apply_vout_range(ch, bipolar)) {
            char name[QUICKSETUP_FAILED_NAME_MAX];
            snprintf(name, sizeof(name), "vout_ch%u", (unsigned)ch);
            add_failed(report, name);
        }

        int dac = json_int(ch_obj, "dacCode", 0);
        if (dac < 0) dac = 0;
        if (dac > 65535) dac = 65535;
        if (!tasks_apply_dac_code(ch, (uint16_t)dac)) {
            char name[QUICKSETUP_FAILED_NAME_MAX];
            snprintf(name, sizeof(name), "dac_ch%u", (unsigned)ch);
            add_failed(report, name);
        }
    }

    cJSON *idac = doc ? json_obj(doc, "idac") : NULL;
    cJSON *codes = idac ? json_arr(idac, "codes") : NULL;
    for (uint8_t ch = 0; ch < 3; ch++) {
        cJSON *item = codes ? cJSON_GetArrayItem(codes, ch) : NULL;
        int code = (item && cJSON_IsNumber(item)) ? item->valueint : 0;
        if (code < -127) code = -127;
        if (code > 127) code = 127;
        if (!ds4424_set_code(ch, (int8_t)code)) {
            char name[QUICKSETUP_FAILED_NAME_MAX];
            snprintf(name, sizeof(name), "idac_ch%u", (unsigned)ch);
            add_failed(report, name);
        }
    }

    cJSON *pca = doc ? json_obj(doc, "pca") : NULL;
    if (!pca9535_set_control(PCA_CTRL_VADJ1_EN, json_bool(pca, "vadj1", false))) add_failed(report, "pca_vadj1");
    if (!pca9535_set_control(PCA_CTRL_VADJ2_EN, json_bool(pca, "vadj2", false))) add_failed(report, "pca_vadj2");
    if (!pca9535_set_bit(0, 0, json_bool(pca, "logic", false))) add_failed(report, "pca_logic");
    if (!pca9535_set_control(PCA_CTRL_15V_EN, json_bool(pca, "v15", false))) add_failed(report, "pca_15v");
    if (!pca9535_set_control(PCA_CTRL_USB_HUB_EN, json_bool(pca, "usbHub", false))) add_failed(report, "pca_usb");

    cJSON *efuse = pca ? json_arr(pca, "efuse") : NULL;
    const PcaControl efuse_ctrls[4] = {
        PCA_CTRL_EFUSE1_EN,
        PCA_CTRL_EFUSE2_EN,
        PCA_CTRL_EFUSE3_EN,
        PCA_CTRL_EFUSE4_EN,
    };
    for (uint8_t i = 0; i < 4; i++) {
        cJSON *item = efuse ? cJSON_GetArrayItem(efuse, i) : NULL;
        bool on = item ? cJSON_IsTrue(item) : false;
        if (!pca9535_set_control(efuse_ctrls[i], on)) {
            char name[QUICKSETUP_FAILED_NAME_MAX];
            snprintf(name, sizeof(name), "efuse%u", (unsigned)(i + 1));
            add_failed(report, name);
        }
    }

    cJSON *gpio = doc ? json_arr(doc, "gpio") : NULL;
    for (uint8_t i = 0; i < DIO_NUM_IOS; i++) {
        cJSON *io = gpio ? cJSON_GetArrayItem(gpio, i) : NULL;
        if (io && !cJSON_IsObject(io)) io = NULL;
        int mode = json_int(io, "mode", DIO_MODE_DISABLED);
        if (mode < DIO_MODE_DISABLED || mode > DIO_MODE_OUTPUT) {
            mode = DIO_MODE_DISABLED;
        }
        bool value = json_bool(io, "value", false);
        // Route IO terminal through MUX before configuring GPIO (non-fatal, log warning).
        {
            char bp_err[64];
            if (!bus_planner_route_digital_input(i + 1, bp_err, sizeof(bp_err))) {
                ESP_LOGW("quicksetup", "bus_planner io=%u: %s", (unsigned)(i + 1), bp_err);
            }
        }
        if (!dio_configure_ext(i + 1, (uint8_t)mode, false)) {
            char name[QUICKSETUP_FAILED_NAME_MAX];
            snprintf(name, sizeof(name), "gpio%u", (unsigned)(i + 1));
            add_failed(report, name);
            continue;
        }
        if (mode == DIO_MODE_OUTPUT && !dio_write(i + 1, value)) {
            char name[QUICKSETUP_FAILED_NAME_MAX];
            snprintf(name, sizeof(name), "gpio%u_val", (unsigned)(i + 1));
            add_failed(report, name);
        }
    }

    cJSON *mux = doc ? json_obj(doc, "mux") : NULL;
    cJSON *devices = mux ? json_arr(mux, "devices") : NULL;
    uint8_t states[ADGS_API_MAIN_DEVICES] = {};
    for (uint8_t i = 0; i < ADGS_API_MAIN_DEVICES; i++) {
        cJSON *item = devices ? cJSON_GetArrayItem(devices, i) : NULL;
        states[i] = (item && cJSON_IsNumber(item)) ? (uint8_t)item->valueint : 0;
    }
    adgs_set_api_all_safe(states);

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        adgs_get_all_states(g_deviceState.muxState);
        for (uint8_t i = 0; i < DIO_NUM_IOS; i++) {
            DioState st_io;
            if (dio_get_state(i + 1, &st_io)) {
                g_deviceState.dio[i].mode = st_io.mode;
                g_deviceState.dio[i].outputVal = st_io.output_level;
                g_deviceState.dio[i].inputVal = st_io.input_level;
                g_deviceState.dio[i].pulldown = st_io.pulldown;
            }
        }
        xSemaphoreGive(g_stateMutex);
    }

    if (doc) cJSON_Delete(doc);
    if (report && !report->ok) return QUICKSETUP_APPLY_ERROR;
    return QUICKSETUP_OK;
}

QuickSetupStatus quicksetup_delete(uint8_t slot, bool *existed)
{
    if (existed) *existed = false;
    if (!is_valid_slot(slot)) return QUICKSETUP_INVALID_SLOT;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(QS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return QUICKSETUP_STORAGE_ERROR;

    char key[16];
    slot_key(slot, key);
    err = nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return QUICKSETUP_NOT_FOUND;
    }
    if (err == ESP_OK) {
        if (existed) *existed = true;
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK ? QUICKSETUP_OK : QUICKSETUP_STORAGE_ERROR;
}
