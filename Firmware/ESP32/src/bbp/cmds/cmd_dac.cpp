// =============================================================================
// cmd_dac.cpp — Registry handlers for DAC commands
//   BBP_CMD_SET_DAC_CODE    (0x11)
//   BBP_CMD_SET_DAC_VOLTAGE (0x12)
//   BBP_CMD_SET_DAC_CURRENT (0x13)
//   BBP_CMD_GET_DAC_READBACK(0x1C)
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "tasks.h"

// ---------------------------------------------------------------------------
// SET_DAC_CODE  payload: u8 ch, u16 code  → resp: u8 ch, u16 code
// ---------------------------------------------------------------------------
static int handler_set_dac_code(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 3) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  ch   = bbp_get_u8(payload, &rpos);
    uint16_t code = bbp_get_u16(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type    = CMD_SET_DAC_CODE;
    cmd.channel = ch;
    cmd.dacCode = code;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u16(resp, &pos, code);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SET_DAC_VOLTAGE  payload: u8 ch, f32 voltage, bool bipolar → resp: same
// ---------------------------------------------------------------------------
static int handler_set_dac_voltage(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    if (len < 6) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch      = bbp_get_u8(payload, &rpos);
    float   voltage = bbp_get_f32(payload, &rpos);
    bool    bipolar = bbp_get_bool(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type              = CMD_SET_DAC_VOLTAGE;
    cmd.channel           = ch;
    cmd.dacVoltage.voltage = voltage;
    cmd.dacVoltage.bipolar = bipolar;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_f32(resp, &pos, voltage);
    bbp_put_bool(resp, &pos, bipolar);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SET_DAC_CURRENT  payload: u8 ch, f32 current_mA → resp: u8 ch, f32 mA
// ---------------------------------------------------------------------------
static int handler_set_dac_current(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    if (len < 5) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch         = bbp_get_u8(payload, &rpos);
    float   current_mA = bbp_get_f32(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    Command cmd = {};
    cmd.type     = CMD_SET_DAC_CURRENT;
    cmd.channel  = ch;
    cmd.floatVal = current_mA;
    if (!sendCommand(cmd)) return -CMD_ERR_BUSY;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_f32(resp, &pos, current_mA);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// GET_DAC_READBACK  payload: u8 ch → resp: u8 ch, u16 activeCode
// Wire format is byte-identical to the legacy BBP handler (bbp.cpp:477-490).
// Uses bbp_dac_read_active() for a live SPI read of the DAC_ACTIVE register,
// matching s_dev->getDacActive(ch) in the legacy path exactly.
// ---------------------------------------------------------------------------
static int handler_get_dac_readback(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ch = bbp_get_u8(payload, &rpos);
    if (ch >= 4) return -CMD_ERR_OUT_OF_RANGE;

    uint16_t activeCode = 0;
    if (!bbp_dac_read_active(ch, &activeCode)) return -CMD_ERR_HARDWARE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ch);
    bbp_put_u16(resp, &pos, activeCode);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
static const ArgSpec s_set_dac_code_args[] = {
    { "channel", ARG_U8,  true, 0, 3 },
    { "code",    ARG_U16, true, 0, 65535 },
};
static const ArgSpec s_set_dac_code_rsp[] = {
    { "channel", ARG_U8,  true, 0, 0 },
    { "code",    ARG_U16, true, 0, 0 },
};

static const ArgSpec s_set_dac_voltage_args[] = {
    { "channel", ARG_U8,   true, 0, 3 },
    { "voltage", ARG_F32,  true, -12.0f, 12.0f },
    { "bipolar", ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_set_dac_voltage_rsp[] = {
    { "channel", ARG_U8,   true, 0, 0 },
    { "voltage", ARG_F32,  true, 0, 0 },
    { "bipolar", ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_set_dac_current_args[] = {
    { "channel",    ARG_U8,  true, 0, 3 },
    { "current_mA", ARG_F32, true, -25.0f, 25.0f },
};
static const ArgSpec s_set_dac_current_rsp[] = {
    { "channel",    ARG_U8,  true, 0, 0 },
    { "current_mA", ARG_F32, true, 0, 0 },
};

static const ArgSpec s_get_dac_readback_args[] = {
    { "channel", ARG_U8, true, 0, 3 },
};
static const ArgSpec s_get_dac_readback_rsp[] = {
    { "channel",    ARG_U8,  true, 0, 0 },
    { "activeCode", ARG_U16, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_dac_cmds[] = {
    { BBP_CMD_SET_DAC_CODE,     "set_dac_code",
      s_set_dac_code_args,     2, s_set_dac_code_rsp,     2, handler_set_dac_code,     0 },
    { BBP_CMD_SET_DAC_VOLTAGE,  "set_dac_voltage",
      s_set_dac_voltage_args,  3, s_set_dac_voltage_rsp,  3, handler_set_dac_voltage,  0 },
    { BBP_CMD_SET_DAC_CURRENT,  "set_dac_current",
      s_set_dac_current_args,  2, s_set_dac_current_rsp,  2, handler_set_dac_current,  0 },
    { BBP_CMD_GET_DAC_READBACK, "get_dac_readback",
      s_get_dac_readback_args, 1, s_get_dac_readback_rsp, 2, handler_get_dac_readback, CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_dac(void)
{
    cmd_registry_register_block(s_dac_cmds,
        sizeof(s_dac_cmds) / sizeof(s_dac_cmds[0]));
}
