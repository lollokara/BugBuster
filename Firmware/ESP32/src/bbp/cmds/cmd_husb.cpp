// =============================================================================
// cmd_husb.cpp — Registry handlers for HUSB238 USB-PD commands
//
//   BBP_CMD_USBPD_GET_STATUS (0xC0)
//   BBP_CMD_USBPD_SELECT_PDO (0xC1)
//   BBP_CMD_USBPD_GO         (0xC2)
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "husb238.h"

// ---------------------------------------------------------------------------
// USBPD_GET_STATUS  payload: (none)
// resp: bool present, bool attached, bool cc_direction, u8 pd_response,
//       u8 voltage, u8 current, f32 voltage_v, f32 current_a, f32 power_w,
//       then 6 PDOs: (bool detected, u8 max_current) for 5V/9V/12V/15V/18V/20V,
//       u8 selected_pdo
// Wire format matches legacy handleUsbpdGetStatus (bbp.cpp:1977-2005).
// Calls husb238_update() before reading, matching legacy.
// ---------------------------------------------------------------------------
static int handler_usbpd_get_status(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    husb238_update();  // Live I2C read (matching legacy bbp.cpp:1979)
    const Husb238State *st = husb238_get_state();

    size_t pos = 0;
    bbp_put_bool(resp, &pos, st->present);
    bbp_put_bool(resp, &pos, st->attached);
    bbp_put_bool(resp, &pos, st->cc_direction);
    bbp_put_u8(resp, &pos, st->pd_response);
    bbp_put_u8(resp, &pos, (uint8_t)st->voltage);
    bbp_put_u8(resp, &pos, (uint8_t)st->current);
    bbp_put_f32(resp, &pos, st->voltage_v);
    bbp_put_f32(resp, &pos, st->current_a);
    bbp_put_f32(resp, &pos, st->power_w);

    // PDO table (matching legacy bbp.cpp:1992-2004)
    struct { bool det; Husb238Current cur; } pdos[] = {
        {st->pdo_5v.detected,  st->pdo_5v.max_current},
        {st->pdo_9v.detected,  st->pdo_9v.max_current},
        {st->pdo_12v.detected, st->pdo_12v.max_current},
        {st->pdo_15v.detected, st->pdo_15v.max_current},
        {st->pdo_18v.detected, st->pdo_18v.max_current},
        {st->pdo_20v.detected, st->pdo_20v.max_current},
    };
    for (int i = 0; i < 6; i++) {
        bbp_put_bool(resp, &pos, pdos[i].det);
        bbp_put_u8(resp, &pos, (uint8_t)pdos[i].cur);
    }
    bbp_put_u8(resp, &pos, st->selected_pdo);

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// USBPD_SELECT_PDO  payload: u8 voltage  → resp: u8 voltage
// Wire format matches legacy handleUsbpdSelectPdo (bbp.cpp:2008-2022).
// ---------------------------------------------------------------------------
static int handler_usbpd_select_pdo(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t voltage = bbp_get_u8(payload, &rpos);

    if (!husb238_select_pdo((Husb238Voltage)voltage)) return -CMD_ERR_HARDWARE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, voltage);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// USBPD_GO  payload: u8 cmd  → resp: u8 cmd
// Wire format matches legacy handleUsbpdGo (bbp.cpp:2025-2035).
// ---------------------------------------------------------------------------
static int handler_usbpd_go(const uint8_t *payload, size_t len,
                             uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t cmd = payload[0];

    if (!husb238_go_command(cmd)) return -CMD_ERR_HARDWARE;

    resp[0] = cmd;
    *resp_len = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// usbpd_get_status: complex multi-field response — rsp=NULL.

static const ArgSpec s_usbpd_select_pdo_args[] = {
    { "voltage", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_usbpd_select_pdo_rsp[] = {
    { "voltage", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_usbpd_go_args[] = {
    { "cmd", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_usbpd_go_rsp[] = {
    { "cmd", ARG_U8, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_husb_cmds[] = {
    { BBP_CMD_USBPD_GET_STATUS, "usbpd_get_status",
      NULL,                    0, NULL,                    0, handler_usbpd_get_status, CMD_FLAG_READS_STATE },
    { BBP_CMD_USBPD_SELECT_PDO, "usbpd_select_pdo",
      s_usbpd_select_pdo_args, 1, s_usbpd_select_pdo_rsp, 1, handler_usbpd_select_pdo, 0                   },
    { BBP_CMD_USBPD_GO,         "usbpd_go",
      s_usbpd_go_args,         1, s_usbpd_go_rsp,         1, handler_usbpd_go,         0                   },
};

extern "C" void register_cmds_husb(void)
{
    cmd_registry_register_block(s_husb_cmds,
        sizeof(s_husb_cmds) / sizeof(s_husb_cmds[0]));
}
