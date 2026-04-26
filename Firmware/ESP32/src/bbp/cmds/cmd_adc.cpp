// =============================================================================
// cmd_adc.cpp — Registry handlers for ADC commands
//   BBP_CMD_GET_ADC_VALUE      (0x1B)
//   BBP_CMD_SET_ADC_CONFIG     (0x14)
//   BBP_CMD_START_ADC_STREAM   (0x60)
//   BBP_CMD_STOP_ADC_STREAM    (0x61)
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "tasks.h"
#include "state_lock.h"

// ---------------------------------------------------------------------------
// GET_ADC_VALUE  payload: u8 ch
//   resp: u8 ch, u24 rawCode, f32 adcValue, u8 range, u8 rate, u8 mux
// Wire format matches legacy handleGetAdcValue (bbp.cpp:452-476).
// Reads from g_deviceState under ScopedStateLock (legacy holds g_stateMutex
// for the same 50 ms window).
// ---------------------------------------------------------------------------
static int handler_get_adc_value(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch = bbp_get_u8(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    ScopedStateLock lock;
    if (!lock.held()) return -CMD_ERR_BUSY;

    const ChannelState &cs = g_deviceState.channels[ch];
    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u24(resp, &pos, cs.adcRawCode);
    bbp_put_f32(resp, &pos, cs.adcValue);
    bbp_put_u8(resp, &pos, (uint8_t)cs.adcRange);
    bbp_put_u8(resp, &pos, (uint8_t)cs.adcRate);
    bbp_put_u8(resp, &pos, (uint8_t)cs.adcMux);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SET_ADC_CONFIG  payload: u8 ch, u8 mux, u8 range, u8 rate
//   resp: u8 ch, u8 mux, u8 range, u8 rate
// Wire format matches legacy handleSetAdcConfig (bbp.cpp:666-691).
// ---------------------------------------------------------------------------
static int handler_set_adc_config(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 4) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch    = bbp_get_u8(payload, &rpos);
    uint8_t mux   = bbp_get_u8(payload, &rpos);
    uint8_t range = bbp_get_u8(payload, &rpos);
    uint8_t rate  = bbp_get_u8(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type          = CMD_ADC_CONFIG;
    cmd.channel       = ch;
    cmd.adcCfg.mux    = (AdcConvMux)mux;
    cmd.adcCfg.range  = (AdcRange)range;
    cmd.adcCfg.rate   = (AdcRate)rate;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u8(resp, &pos, mux);
    bbp_put_u8(resp, &pos, range);
    bbp_put_u8(resp, &pos, rate);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// START_ADC_STREAM  payload: u8 mask, u8 div
//   resp: u8 mask, u8 div, u16 effectiveRate
// Wire format matches legacy handleStartAdcStream (bbp.cpp:2609-2664).
// Delegates to bbpStartAdcStream() which owns s_adcStreamMask/Div/etc.
// ---------------------------------------------------------------------------
static int handler_start_adc_stream(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    if (bbpAdcStreamMask() != 0) return -CMD_ERR_BUSY;  // BBP_ERR_STREAM_ACTIVE
    if (len < 2) return -CMD_ERR_BAD_ARG;

    size_t rpos = 0;
    uint8_t mask = bbp_get_u8(payload, &rpos);
    uint8_t div  = bbp_get_u8(payload, &rpos);
    if (div == 0) div = 1;  // clamp before echo, matching legacy bbp.cpp:2622
    if (mask == 0 || mask > 0x0F) return -CMD_ERR_OUT_OF_RANGE;

    uint16_t effectiveRate = 0;
    bbpStartAdcStream(mask, div, &effectiveRate);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, mask);
    bbp_put_u8(resp, &pos, div);
    bbp_put_u16(resp, &pos, effectiveRate);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// STOP_ADC_STREAM  payload: (none)  resp: (none, 0 bytes)
// Wire format matches legacy handleStopAdcStream (bbp.cpp:2667-2672).
// ---------------------------------------------------------------------------
static int handler_stop_adc_stream(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    bbpStopAdcStream();
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// Note: get_adc_value response contains u24 rawCode (no ARG_U24 in enum).
// rsp is left as NULL/0 — CLI adapter prints raw hex for non-specced responses.
static const ArgSpec s_get_adc_value_args[] = {
    { "channel", ARG_U8, true, 0, 3 },
};

static const ArgSpec s_set_adc_config_args[] = {
    { "channel", ARG_U8, true, 0, 3 },
    { "mux",     ARG_U8, true, 0, 255 },
    { "range",   ARG_U8, true, 0, 255 },
    { "rate",    ARG_U8, true, 0, 255 },
};
static const ArgSpec s_set_adc_config_rsp[] = {
    { "channel", ARG_U8, true, 0, 0 },
    { "mux",     ARG_U8, true, 0, 0 },
    { "range",   ARG_U8, true, 0, 0 },
    { "rate",    ARG_U8, true, 0, 0 },
};

static const ArgSpec s_start_adc_stream_args[] = {
    { "mask", ARG_U8, true, 0, 15 },
    { "div",  ARG_U8, true, 0, 255 },
};
static const ArgSpec s_start_adc_stream_rsp[] = {
    { "mask",          ARG_U8,  true, 0, 0 },
    { "div",           ARG_U8,  true, 0, 0 },
    { "effectiveRate", ARG_U16, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_adc_cmds[] = {
    { BBP_CMD_GET_ADC_VALUE,    "get_adc_value",
      s_get_adc_value_args,    1, NULL,                   0, handler_get_adc_value,    CMD_FLAG_READS_STATE },
    { BBP_CMD_SET_ADC_CONFIG,   "set_adc_config",
      s_set_adc_config_args,   4, s_set_adc_config_rsp,   4, handler_set_adc_config,   0                   },
    { BBP_CMD_START_ADC_STREAM, "start_adc_stream",
      s_start_adc_stream_args, 2, s_start_adc_stream_rsp, 3, handler_start_adc_stream, CMD_FLAG_STREAMING  },
    { BBP_CMD_STOP_ADC_STREAM,  "stop_adc_stream",
      NULL,                    0, NULL,                   0, handler_stop_adc_stream,  CMD_FLAG_STREAMING  },
};

extern "C" void register_cmds_adc(void)
{
    cmd_registry_register_block(s_adc_cmds,
        sizeof(s_adc_cmds) / sizeof(s_adc_cmds[0]));
}
