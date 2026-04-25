// =============================================================================
// cmd_pca.cpp — Registry handlers for PCA9535 GPIO expander commands
//
//   BBP_CMD_PCA_GET_STATUS    (0xB0)
//   BBP_CMD_PCA_SET_CONTROL   (0xB1)
//   BBP_CMD_PCA_SET_PORT      (0xB2)
//   BBP_CMD_PCA_SET_FAULT_CFG (0xB3)
//   BBP_CMD_PCA_GET_FAULT_LOG (0xB4)
// =============================================================================
#include "../cmd_registry.h"
#include "../cmd_errors.h"
#include "../bbp_codec.h"
#include "../bbp.h"
#include "../tasks.h"
#include "../state_lock.h"
#include "../pca9535.h"

// ---------------------------------------------------------------------------
// PCA_GET_STATUS  payload: (none)
// resp: bool present, u8 input0, u8 input1, u8 output0, u8 output1,
//       bool logic_pg, bool vadj1_pg, bool vadj2_pg, bool efuse_flt[4],
//       bool vadj1_en, bool vadj2_en, bool en_15v, bool en_mux,
//       bool en_usb_hub, bool efuse_en[4]
// Wire format matches legacy handlePcaGetStatus (bbp.cpp:1472-1494).
// Calls pca9535_update() to refresh inputs before reading, matching legacy.
// ---------------------------------------------------------------------------
static int handler_pca_get_status(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    pca9535_update();  // Read latest inputs (matching legacy bbp.cpp:1474)
    const PCA9535State *st = pca9535_get_state();

    size_t pos = 0;
    bbp_put_bool(resp, &pos, st->present);
    bbp_put_u8(resp, &pos, st->input0);
    bbp_put_u8(resp, &pos, st->input1);
    bbp_put_u8(resp, &pos, st->output0);
    bbp_put_u8(resp, &pos, st->output1);
    // Decoded status
    bbp_put_bool(resp, &pos, st->logic_pg);
    bbp_put_bool(resp, &pos, st->vadj1_pg);
    bbp_put_bool(resp, &pos, st->vadj2_pg);
    for (int i = 0; i < 4; i++) bbp_put_bool(resp, &pos, st->efuse_flt[i]);
    // Decoded enables
    bbp_put_bool(resp, &pos, st->vadj1_en);
    bbp_put_bool(resp, &pos, st->vadj2_en);
    bbp_put_bool(resp, &pos, st->en_15v);
    bbp_put_bool(resp, &pos, st->en_mux);
    bbp_put_bool(resp, &pos, st->en_usb_hub);
    for (int i = 0; i < 4; i++) bbp_put_bool(resp, &pos, st->efuse_en[i]);

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// PCA_SET_CONTROL  payload: u8 ctrl, bool on  → resp: u8 ctrl, bool on
// Wire format matches legacy handlePcaSetControl (bbp.cpp:1497-1514).
// ---------------------------------------------------------------------------
static int handler_pca_set_control(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t ctrl = bbp_get_u8(payload, &rpos);
    bool on      = bbp_get_bool(payload, &rpos);
    if (ctrl >= PCA_CTRL_COUNT) return -CMD_ERR_OUT_OF_RANGE;

    if (!pca9535_set_control((PcaControl)ctrl, on)) return -CMD_ERR_HARDWARE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, ctrl);
    bbp_put_bool(resp, &pos, on);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// PCA_SET_PORT  payload: u8 port(0-1), u8 val  → resp: u8 port, u8 val
// Wire format matches legacy handlePcaSetPort (bbp.cpp:1517-1534).
// ---------------------------------------------------------------------------
static int handler_pca_set_port(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t port = bbp_get_u8(payload, &rpos);
    uint8_t val  = bbp_get_u8(payload, &rpos);
    if (port > 1) return -CMD_ERR_OUT_OF_RANGE;

    if (!pca9535_set_port(port, val)) return -CMD_ERR_HARDWARE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, port);
    bbp_put_u8(resp, &pos, val);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// PCA_SET_FAULT_CFG  payload: u8 auto_disable_efuse, u8 log_events
// resp: u8 auto_disable_efuse, u8 log_events
// Wire format matches legacy handlePcaSetFaultConfig (bbp.cpp:1539-1552).
// ---------------------------------------------------------------------------
static int handler_pca_set_fault_cfg(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    PcaFaultConfig cfg;
    cfg.auto_disable_efuse = bbp_get_u8(payload, &rpos) != 0;
    cfg.log_events         = bbp_get_u8(payload, &rpos) != 0;
    pca9535_set_fault_config(&cfg);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, cfg.auto_disable_efuse ? 1 : 0);
    bbp_put_u8(resp, &pos, cfg.log_events ? 1 : 0);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// PCA_GET_FAULT_LOG  payload: (none)
// resp: u8 count, then count * (u8 type, u8 channel, u32 timestamp_ms)
// Wire format matches legacy handlePcaGetFaultLog (bbp.cpp:1555-1573).
// Reads g_deviceState under ScopedStateLock matching legacy 50ms timeout.
// On lock failure: returns count=0 (matching legacy bbp.cpp:1571).
// ---------------------------------------------------------------------------
static int handler_pca_get_fault_log(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    size_t pos = 0;
    ScopedStateLock lock;
    if (lock.held()) {
        uint8_t count = g_deviceState.pcaFaultLogCount;
        bbp_put_u8(resp, &pos, count);
        for (uint8_t i = 0; i < count && i < DeviceState::PCA_FAULT_LOG_SIZE; i++) {
            uint8_t idx = (g_deviceState.pcaFaultLogHead - count + i + DeviceState::PCA_FAULT_LOG_SIZE)
                          % DeviceState::PCA_FAULT_LOG_SIZE;
            const auto &entry = g_deviceState.pcaFaultLog[idx];
            bbp_put_u8(resp, &pos, entry.type);
            bbp_put_u8(resp, &pos, entry.channel);
            bbp_put_u32(resp, &pos, entry.timestamp_ms);
        }
    } else {
        // Lock timeout — return empty log (matching legacy bbp.cpp:1571)
        bbp_put_u8(resp, &pos, 0);
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// pca_get_status: complex multi-bit response — rsp=NULL.
// pca_get_fault_log: variable-length — rsp=NULL.

static const ArgSpec s_pca_set_control_args[] = {
    { "ctrl", ARG_U8,   true, 0, 255 },
    { "on",   ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_pca_set_control_rsp[] = {
    { "ctrl", ARG_U8,   true, 0, 0 },
    { "on",   ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_pca_set_port_args[] = {
    { "port", ARG_U8, true, 0, 1 },
    { "val",  ARG_U8, true, 0, 255 },
};
static const ArgSpec s_pca_set_port_rsp[] = {
    { "port", ARG_U8, true, 0, 0 },
    { "val",  ARG_U8, true, 0, 0 },
};

static const ArgSpec s_pca_set_fault_cfg_args[] = {
    { "auto_disable_efuse", ARG_U8, true, 0, 1 },
    { "log_events",         ARG_U8, true, 0, 1 },
};
static const ArgSpec s_pca_set_fault_cfg_rsp[] = {
    { "auto_disable_efuse", ARG_U8, true, 0, 0 },
    { "log_events",         ARG_U8, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_pca_cmds[] = {
    { BBP_CMD_PCA_GET_STATUS,    "pca_get_status",
      NULL,                      0, NULL,                      0, handler_pca_get_status,    CMD_FLAG_READS_STATE },
    { BBP_CMD_PCA_SET_CONTROL,   "pca_set_control",
      s_pca_set_control_args,    2, s_pca_set_control_rsp,    2, handler_pca_set_control,   0                   },
    { BBP_CMD_PCA_SET_PORT,      "pca_set_port",
      s_pca_set_port_args,       2, s_pca_set_port_rsp,       2, handler_pca_set_port,      0                   },
    { BBP_CMD_PCA_SET_FAULT_CFG, "pca_set_fault_cfg",
      s_pca_set_fault_cfg_args,  2, s_pca_set_fault_cfg_rsp,  2, handler_pca_set_fault_cfg, 0                   },
    { BBP_CMD_PCA_GET_FAULT_LOG, "pca_get_fault_log",
      NULL,                      0, NULL,                      0, handler_pca_get_fault_log, CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_pca(void)
{
    cmd_registry_register_block(s_pca_cmds,
        sizeof(s_pca_cmds) / sizeof(s_pca_cmds[0]));
}
