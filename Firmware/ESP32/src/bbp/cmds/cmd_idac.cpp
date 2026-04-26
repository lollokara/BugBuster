// =============================================================================
// cmd_idac.cpp — Registry handlers for DS4424 IDAC commands
//
//   BBP_CMD_IDAC_GET_STATUS     (0xA0)
//   BBP_CMD_IDAC_SET_CODE       (0xA1)
//   BBP_CMD_IDAC_SET_VOLTAGE    (0xA2)
//   BBP_CMD_IDAC_CALIBRATE      (0xA3)
//   BBP_CMD_IDAC_CAL_ADD_POINT  (0xA4)
//   BBP_CMD_IDAC_CAL_CLEAR      (0xA5)
//   BBP_CMD_IDAC_CAL_SAVE       (0xA6)
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "tasks.h"
#include "state_lock.h"
#include "ds4424.h"
#include "esp_log.h"

static const char *TAG = "cmd_idac";

// ---------------------------------------------------------------------------
// IDAC_GET_STATUS  payload: (none)
// resp: bool present, then for each channel (DS4424_NUM_CHANNELS):
//   u8 ch, u8 code, f32 target_v, f32 actual_v, f32 midpoint_v, f32 v_min,
//   f32 v_max, f32 step_mv, bool calibrated, bool polyValid,
//   f32 poly[4]  (a0..a3)
// Wire format matches legacy handleIdacGetStatus (bbp.cpp:1292-1322).
// ---------------------------------------------------------------------------
static int handler_idac_get_status(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    const DS4424State *st = ds4424_get_state();
    size_t pos = 0;
    bbp_put_bool(resp, &pos, st->present);

    for (uint8_t ch = 0; ch < DS4424_NUM_CHANNELS; ch++) {
        // Per-channel footprint same as legacy: 44 bytes max
        if (pos + 44 > BBP_MAX_PAYLOAD) break;
        bbp_put_u8(resp, &pos, ch);
        bbp_put_u8(resp, &pos, (uint8_t)(st->state[ch].dac_code & 0xFF));
        bbp_put_f32(resp, &pos, st->state[ch].target_v);
        bbp_put_f32(resp, &pos, st->state[ch].actual_v);
        bbp_put_f32(resp, &pos, st->config[ch].midpoint_v);
        bbp_put_f32(resp, &pos, st->config[ch].v_min);
        bbp_put_f32(resp, &pos, st->config[ch].v_max);
        bbp_put_f32(resp, &pos, ds4424_step_mv(ch));
        bbp_put_bool(resp, &pos, st->cal[ch].valid);
        float poly[4] = {0};
        bool have_poly = ds4424_cal_fit_cubic(ch, poly);
        bbp_put_bool(resp, &pos, have_poly);
        bbp_put_f32(resp, &pos, poly[0]);
        bbp_put_f32(resp, &pos, poly[1]);
        bbp_put_f32(resp, &pos, poly[2]);
        bbp_put_f32(resp, &pos, poly[3]);
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IDAC_SET_CODE  payload: u8 ch, u8 code(signed)
// resp: u8 ch, u8 code, f32 voltage
// Wire format matches legacy handleIdacSetCode (bbp.cpp:1325-1343).
// ---------------------------------------------------------------------------
static int handler_idac_set_code(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch    = bbp_get_u8(payload, &rpos);
    int8_t  code  = (int8_t)bbp_get_u8(payload, &rpos);
    if (ch >= DS4424_NUM_CHANNELS) return -CMD_ERR_OUT_OF_RANGE;

    if (!ds4424_set_code(ch, code)) return -CMD_ERR_HARDWARE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u8(resp, &pos, (uint8_t)(code & 0xFF));
    bbp_put_f32(resp, &pos, ds4424_code_to_voltage(ch, code));
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IDAC_SET_VOLTAGE  payload: u8 ch, f32 voltage
// resp: u8 ch, u8 code, f32 target_v
// Wire format matches legacy handleIdacSetVoltage (bbp.cpp:1346-1365).
// Note: ch >= 3 check is from legacy (not DS4424_NUM_CHANNELS).
// ---------------------------------------------------------------------------
static int handler_idac_set_voltage(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    if (len < 5) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch      = bbp_get_u8(payload, &rpos);
    float   voltage = bbp_get_f32(payload, &rpos);
    if (ch >= 3) return -CMD_ERR_OUT_OF_RANGE;  // legacy uses ch >= 3

    if (!ds4424_set_voltage(ch, voltage)) return -CMD_ERR_HARDWARE;

    const DS4424State *st = ds4424_get_state();
    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u8(resp, &pos, (uint8_t)(st->state[ch].dac_code & 0xFF));
    bbp_put_f32(resp, &pos, st->state[ch].target_v);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IDAC_CALIBRATE  payload: u8 idac_ch, u8 step, u16 settle_ms, u8 adc_ch
// resp: u8 ch, u8 points
// Wire format matches legacy handleIdacCalibrate (bbp.cpp:1385-1425).
// Reads ADC from g_deviceState.channels[adc_ch].adcValue under stateMutex.
// Checks VIN mode and runs ds4424_cal_auto + ds4424_cal_save.
// ---------------------------------------------------------------------------

// Module-level storage for cal ADC channel (matches legacy s_cal_adc_channel)
static uint8_t s_cal_adc_channel = 0;

static float idac_cal_read_adc(uint8_t idac_ch)
{
    (void)idac_ch;
    float val = 0.0f;
    ScopedStateLock lock;
    if (lock.held()) {
        val = g_deviceState.channels[s_cal_adc_channel].adcValue;
    }
    return val;
}

static int handler_idac_calibrate(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 5) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  ch      = bbp_get_u8(payload, &rpos);
    uint8_t  step    = bbp_get_u8(payload, &rpos);
    uint16_t settle  = bbp_get_u16(payload, &rpos);
    uint8_t  adc_ch  = bbp_get_u8(payload, &rpos);
    if (ch >= 3)     return -CMD_ERR_OUT_OF_RANGE;
    if (adc_ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    // Verify ADC channel is in VIN mode (matching legacy bbp.cpp:1398-1408)
    ChannelFunction func = CH_FUNC_HIGH_IMP;
    {
        ScopedStateLock lock;
        if (lock.held()) {
            func = g_deviceState.channels[adc_ch].function;
        }
    }
    if (func != CH_FUNC_VIN) return -CMD_ERR_INVALID_STATE;

    s_cal_adc_channel = adc_ch;

    ESP_LOGI(TAG, "Starting IDAC%d auto-calibration (step=%d, settle=%dms, ADC=ch%d)",
             ch, step, settle, adc_ch);

    int points = ds4424_cal_auto(ch, idac_cal_read_adc, step, settle);
    ds4424_cal_save();

    ESP_LOGI(TAG, "IDAC%d calibration complete: %d points, saved to NVS", ch, points);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u8(resp, &pos, (uint8_t)points);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IDAC_CAL_ADD_POINT  payload: u8 ch, u8 code(signed), f32 measured_v
// resp: u8 ch, u8 count, bool valid
// Wire format matches legacy handleIdacCalAddPoint (bbp.cpp:1428-1445).
// ---------------------------------------------------------------------------
static int handler_idac_cal_add_point(const uint8_t *payload, size_t len,
                                      uint8_t *resp, size_t *resp_len)
{
    if (len < 6) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch         = bbp_get_u8(payload, &rpos);
    int8_t  code       = (int8_t)bbp_get_u8(payload, &rpos);
    float   measured_v = bbp_get_f32(payload, &rpos);
    if (ch >= 3) return -CMD_ERR_OUT_OF_RANGE;

    ds4424_cal_add_point(ch, code, measured_v);

    const DS4424State *st = ds4424_get_state();
    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u8(resp, &pos, st->cal[ch].count);
    bbp_put_bool(resp, &pos, st->cal[ch].valid);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IDAC_CAL_CLEAR  payload: u8 ch  → resp: u8 ch
// Wire format matches legacy handleIdacCalClear (bbp.cpp:1448-1459).
// ---------------------------------------------------------------------------
static int handler_idac_cal_clear(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t ch = payload[0];
    if (ch >= 3) return -CMD_ERR_OUT_OF_RANGE;

    ds4424_cal_clear(ch);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IDAC_CAL_SAVE  payload: (none)  → resp: bool ok
// Wire format matches legacy handleIdacCalSave (bbp.cpp:1462-1467).
// ---------------------------------------------------------------------------
static int handler_idac_cal_save(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;
    bool ok = ds4424_cal_save();
    size_t pos = 0;
    bbp_put_bool(resp, &pos, ok);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// idac_get_status: complex per-channel response — rsp=NULL.

static const ArgSpec s_idac_set_code_args[] = {
    { "channel", ARG_U8, true, 0, 255 },
    { "code",    ARG_U8, true, 0, 255 },  // treated as signed int8 by handler
};
static const ArgSpec s_idac_set_code_rsp[] = {
    { "channel", ARG_U8,  true, 0, 0 },
    { "code",    ARG_U8,  true, 0, 0 },
    { "voltage", ARG_F32, true, 0, 0 },
};

static const ArgSpec s_idac_set_voltage_args[] = {
    { "channel", ARG_U8,  true, 0, 255 },
    { "voltage", ARG_F32, true, -5.0f, 5.0f },
};
static const ArgSpec s_idac_set_voltage_rsp[] = {
    { "channel",  ARG_U8,  true, 0, 0 },
    { "code",     ARG_U8,  true, 0, 0 },
    { "target_v", ARG_F32, true, 0, 0 },
};

static const ArgSpec s_idac_calibrate_args[] = {
    { "idac_ch",  ARG_U8,  true, 0, 255 },
    { "step",     ARG_U8,  true, 0, 255 },
    { "settle_ms", ARG_U16, true, 0, 60000 },
    { "adc_ch",   ARG_U8,  true, 0, 3 },
};
static const ArgSpec s_idac_calibrate_rsp[] = {
    { "channel", ARG_U8, true, 0, 0 },
    { "points",  ARG_U8, true, 0, 0 },
};

static const ArgSpec s_idac_cal_add_point_args[] = {
    { "channel",    ARG_U8,  true, 0, 255 },
    { "code",       ARG_U8,  true, 0, 255 },
    { "measured_v", ARG_F32, true, -5.0f, 5.0f },
};
static const ArgSpec s_idac_cal_add_point_rsp[] = {
    { "channel", ARG_U8,   true, 0, 0 },
    { "count",   ARG_U8,   true, 0, 0 },
    { "valid",   ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_idac_cal_clear_args[] = {
    { "channel", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_idac_cal_clear_rsp[] = {
    { "channel", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_idac_cal_save_rsp[] = {
    { "ok", ARG_BOOL, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_idac_cmds[] = {
    { BBP_CMD_IDAC_GET_STATUS,    "idac_get_status",
      NULL,                       0, NULL,                       0, handler_idac_get_status,    CMD_FLAG_READS_STATE },
    { BBP_CMD_IDAC_SET_CODE,      "idac_set_code",
      s_idac_set_code_args,       2, s_idac_set_code_rsp,       3, handler_idac_set_code,      0                   },
    { BBP_CMD_IDAC_SET_VOLTAGE,   "idac_set_voltage",
      s_idac_set_voltage_args,    2, s_idac_set_voltage_rsp,    3, handler_idac_set_voltage,   0                   },
    { BBP_CMD_IDAC_CALIBRATE,     "idac_calibrate",
      s_idac_calibrate_args,      4, s_idac_calibrate_rsp,      2, handler_idac_calibrate,     0                   },
    { BBP_CMD_IDAC_CAL_ADD_POINT, "idac_cal_add_point",
      s_idac_cal_add_point_args,  3, s_idac_cal_add_point_rsp,  3, handler_idac_cal_add_point, 0                   },
    { BBP_CMD_IDAC_CAL_CLEAR,     "idac_cal_clear",
      s_idac_cal_clear_args,      1, s_idac_cal_clear_rsp,      1, handler_idac_cal_clear,     0                   },
    { BBP_CMD_IDAC_CAL_SAVE,      "idac_cal_save",
      NULL,                       0, s_idac_cal_save_rsp,       1, handler_idac_cal_save,      0                   },
};

extern "C" void register_cmds_idac(void)
{
    cmd_registry_register_block(s_idac_cmds,
        sizeof(s_idac_cmds) / sizeof(s_idac_cmds[0]));
}
