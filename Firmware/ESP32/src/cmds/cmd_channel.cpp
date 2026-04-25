// =============================================================================
// cmd_channel.cpp — Registry handlers for channel-configuration commands
//   BBP_CMD_SET_CH_FUNC    (0x10)
//   BBP_CMD_SET_VOUT_RANGE (0x18)
//   BBP_CMD_SET_DIN_CONFIG (0x15)
//   BBP_CMD_SET_RTD_CONFIG (0x1D)
// =============================================================================
#include "../cmd_registry.h"
#include "../cmd_errors.h"
#include "../bbp_codec.h"
#include "../bbp.h"
#include "../tasks.h"

// ---------------------------------------------------------------------------
// Shared channel-function validator (one source of truth).
// Legacy copies: bbp.cpp:36, webserver.cpp:66, inline in CLI.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// SET_CH_FUNC  payload: u8 ch, u8 func  →  resp: u8 ch, u8 func
// Wire format matches legacy handleSetChFunc (bbp.cpp:579-598).
// Calls tasks_apply_channel_function() synchronously, same as legacy.
// ---------------------------------------------------------------------------
static int handler_set_ch_func(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch   = bbp_get_u8(payload, &rpos);
    uint8_t func = bbp_get_u8(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;
    if (!is_valid_channel_function(func)) return -CMD_ERR_OUT_OF_RANGE;

    tasks_apply_channel_function(ch, (ChannelFunction)func);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u8(resp, &pos, func);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SET_VOUT_RANGE  payload: u8 ch, bool bipolar  →  resp: u8 ch, bool bipolar
// Wire format matches legacy handleSetVoutRange (bbp.cpp:769-786).
// Response echoes payload (memcpy(out, payload, 2) in legacy).
// ---------------------------------------------------------------------------
static int handler_set_vout_range(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch      = bbp_get_u8(payload, &rpos);
    bool    bipolar = bbp_get_bool(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type    = CMD_SET_VOUT_RANGE;
    cmd.channel = ch;
    cmd.boolVal = bipolar;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    // Raw passthrough matching legacy memcpy(out, payload, 2) (bbp.cpp:784)
    memcpy(resp, payload, 2);
    *resp_len = 2;
    return 2;
}

// ---------------------------------------------------------------------------
// SET_DIN_CONFIG  payload: u8 ch, u8 thresh, bool threshMode, u8 debounce,
//                          u8 sink, bool sinkRange, bool ocDet, bool scDet
//   resp: same 8 bytes (payload echo)
// Wire format matches legacy handleSetDinConfig (bbp.cpp:693-723).
// Legacy does memcpy(out, payload, 8) — we rebuild field-by-field for clarity
// but the result is byte-identical.
// ---------------------------------------------------------------------------
static int handler_set_din_config(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 8) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch         = bbp_get_u8(payload, &rpos);
    uint8_t thresh     = bbp_get_u8(payload, &rpos);
    bool    threshMode = bbp_get_bool(payload, &rpos);
    uint8_t debounce   = bbp_get_u8(payload, &rpos);
    uint8_t sink       = bbp_get_u8(payload, &rpos);
    bool    sinkRange  = bbp_get_bool(payload, &rpos);
    bool    ocDet      = bbp_get_bool(payload, &rpos);
    bool    scDet      = bbp_get_bool(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type              = CMD_DIN_CONFIG;
    cmd.channel           = ch;
    cmd.dinCfg.thresh     = thresh;
    cmd.dinCfg.threshMode = threshMode;
    cmd.dinCfg.debounce   = debounce;
    cmd.dinCfg.sink       = sink;
    cmd.dinCfg.sinkRange  = sinkRange;
    cmd.dinCfg.ocDet      = ocDet;
    cmd.dinCfg.scDet      = scDet;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    // Raw passthrough matching legacy memcpy(out, payload, 8) (bbp.cpp:721)
    memcpy(resp, payload, 8);
    *resp_len = 8;
    return 8;
}

// ---------------------------------------------------------------------------
// SET_RTD_CONFIG  payload: u8 ch, u8 current(0=500µA, 1=1mA)
//   resp: u8 ch, u8 current
// Wire format matches legacy handleSetRtdConfig (bbp.cpp:553-575).
// ---------------------------------------------------------------------------
static int handler_set_rtd_config(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch      = bbp_get_u8(payload, &rpos);
    uint8_t current = bbp_get_u8(payload, &rpos);
    if (ch >= 4)     return -CMD_ERR_OUT_OF_RANGE;
    if (current > 1) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type           = CMD_SET_RTD_CONFIG;
    cmd.channel        = ch;
    cmd.rtdCfg.current = current;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u8(resp, &pos, current);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
static const ArgSpec s_set_ch_func_args[] = {
    { "channel",  ARG_U8, true, 0, 3 },
    { "function", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_set_ch_func_rsp[] = {
    { "channel",  ARG_U8, true, 0, 0 },
    { "function", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_set_vout_range_args[] = {
    { "channel", ARG_U8,   true, 0, 3 },
    { "bipolar", ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_set_vout_range_rsp[] = {
    { "channel", ARG_U8,   true, 0, 0 },
    { "bipolar", ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_set_din_config_args[] = {
    { "channel",    ARG_U8,   true, 0, 3 },
    { "thresh",     ARG_U8,   true, 0, 255 },
    { "threshMode", ARG_BOOL, true, 0, 0 },
    { "debounce",   ARG_U8,   true, 0, 255 },
    { "sink",       ARG_U8,   true, 0, 255 },
    { "sinkRange",  ARG_BOOL, true, 0, 0 },
    { "ocDet",      ARG_BOOL, true, 0, 0 },
    { "scDet",      ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_set_din_config_rsp[] = {
    { "channel",    ARG_U8,   true, 0, 0 },
    { "thresh",     ARG_U8,   true, 0, 0 },
    { "threshMode", ARG_BOOL, true, 0, 0 },
    { "debounce",   ARG_U8,   true, 0, 0 },
    { "sink",       ARG_U8,   true, 0, 0 },
    { "sinkRange",  ARG_BOOL, true, 0, 0 },
    { "ocDet",      ARG_BOOL, true, 0, 0 },
    { "scDet",      ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_set_rtd_config_args[] = {
    { "channel", ARG_U8, true, 0, 3 },
    { "current", ARG_U8, true, 0, 1 },
};
static const ArgSpec s_set_rtd_config_rsp[] = {
    { "channel", ARG_U8, true, 0, 0 },
    { "current", ARG_U8, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_channel_cmds[] = {
    { BBP_CMD_SET_CH_FUNC,    "set_ch_func",
      s_set_ch_func_args,    2, s_set_ch_func_rsp,    2, handler_set_ch_func,    0 },
    { BBP_CMD_SET_VOUT_RANGE, "set_vout_range",
      s_set_vout_range_args, 2, s_set_vout_range_rsp, 2, handler_set_vout_range, 0 },
    { BBP_CMD_SET_DIN_CONFIG, "set_din_config",
      s_set_din_config_args, 8, s_set_din_config_rsp, 8, handler_set_din_config, 0 },
    { BBP_CMD_SET_RTD_CONFIG, "set_rtd_config",
      s_set_rtd_config_args, 2, s_set_rtd_config_rsp, 2, handler_set_rtd_config, 0 },
};

extern "C" void register_cmds_channel(void)
{
    cmd_registry_register_block(s_channel_cmds,
        sizeof(s_channel_cmds) / sizeof(s_channel_cmds[0]));
}
