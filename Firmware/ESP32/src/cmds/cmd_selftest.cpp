// =============================================================================
// cmd_selftest.cpp — Registry handlers for self-test/calibration commands
//
//   BBP_CMD_SELFTEST_STATUS                 (0x05)
//   BBP_CMD_SELFTEST_MEASURE_SUPPLY         (0x06)
//   BBP_CMD_SELFTEST_SUPPLY_VOLTAGES_CACHED (0x07)
//   BBP_CMD_SELFTEST_AUTO_CAL               (0x08)
//   BBP_CMD_SELFTEST_INT_SUPPLIES           (0x09)
//   BBP_CMD_SELFTEST_WORKER                 (0x0B)
// =============================================================================
#include "../cmd_registry.h"
#include "../cmd_errors.h"
#include "../bbp_codec.h"
#include "../bbp.h"
#include "../tasks.h"
#include "../selftest.h"

// ---------------------------------------------------------------------------
// SELFTEST_STATUS  payload: (none)
// resp: bool ran, bool passed, f32 vadj1_v, f32 vadj2_v, f32 vlogic_v,
//       u8 cal_status, u8 cal_channel, u8 points_collected,
//       f32 last_measured_v, f32 error_mv
// Wire format matches legacy handleSelftestStatus (bbp.cpp:1056-1074).
// ---------------------------------------------------------------------------
static int handler_selftest_status(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    const SelftestBootResult *boot = selftest_get_boot_result();
    const SelftestCalResult  *cal  = selftest_get_cal_result();

    size_t pos = 0;
    bbp_put_bool(resp, &pos, boot->ran);
    bbp_put_bool(resp, &pos, boot->passed);
    bbp_put_f32(resp, &pos, boot->vadj1_v);
    bbp_put_f32(resp, &pos, boot->vadj2_v);
    bbp_put_f32(resp, &pos, boot->vlogic_v);
    bbp_put_u8(resp, &pos, cal->status);
    bbp_put_u8(resp, &pos, cal->channel);
    bbp_put_u8(resp, &pos, cal->points_collected);
    bbp_put_f32(resp, &pos, cal->last_measured_v);
    bbp_put_f32(resp, &pos, cal->error_mv);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SELFTEST_MEASURE_SUPPLY  payload: u8 rail  → resp: u8 rail, f32 voltage
// Wire format matches legacy handleSelftestMeasureSupply (bbp.cpp:1077-1089).
// ---------------------------------------------------------------------------
static int handler_selftest_measure_supply(const uint8_t *payload, size_t len,
                                           uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t rail = payload[0];

    float voltage = selftest_measure_supply(rail);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, rail);
    bbp_put_f32(resp, &pos, voltage);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SELFTEST_SUPPLY_VOLTAGES_CACHED  payload: (none)
// resp: bool available, u32 timestamp_ms, then SELFTEST_RAIL_COUNT f32 voltages
// Wire format matches legacy handleSelftestSupplyVoltagesCached (bbp.cpp:1092-1107).
// Calls selftest_monitor_step() if worker enabled, matching legacy behavior.
// ---------------------------------------------------------------------------
static int handler_selftest_supply_voltages_cached(const uint8_t *payload, size_t len,
                                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    if (selftest_worker_enabled()) {
        selftest_monitor_step();
    }
    const SelftestSupplyVoltages *sv = selftest_get_supply_voltages();

    size_t pos = 0;
    bbp_put_bool(resp, &pos, sv->available);
    bbp_put_u32(resp, &pos, sv->timestamp_ms);
    for (int i = 0; i < SELFTEST_RAIL_COUNT; i++) {
        bbp_put_f32(resp, &pos, sv->voltage[i]);
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SELFTEST_AUTO_CAL  payload: u8 idac_ch
// resp: u8 status, u8 channel, u8 points_collected, f32 last_measured_v, f32 error_mv
// Wire format matches legacy handleSelftestAutoCal (bbp.cpp:1131-1151).
// ---------------------------------------------------------------------------
static int handler_selftest_auto_cal(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t idac_ch = payload[0];

    bool ok = selftest_start_auto_calibrate(idac_ch);
    if (!ok) return -CMD_ERR_BUSY;

    const SelftestCalResult *cal = selftest_get_cal_result();
    size_t pos = 0;
    bbp_put_u8(resp, &pos, cal->status);
    bbp_put_u8(resp, &pos, cal->channel);
    bbp_put_u8(resp, &pos, cal->points_collected);
    bbp_put_f32(resp, &pos, cal->last_measured_v);
    bbp_put_f32(resp, &pos, cal->error_mv);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SELFTEST_INT_SUPPLIES  payload: (none)
// resp: bool valid, bool supplies_ok, f32 avdd_hi_v, f32 dvcc_v, f32 avcc_v,
//       f32 avss_v, f32 temp_c
// Wire format matches legacy handleSelftestIntSupplies (bbp.cpp:1154-1166).
// ---------------------------------------------------------------------------
static int handler_selftest_int_supplies(const uint8_t *payload, size_t len,
                                         uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    const SelftestInternalSupplies *s = selftest_measure_internal_supplies();
    size_t pos = 0;
    bbp_put_bool(resp, &pos, s->valid);
    bbp_put_bool(resp, &pos, s->supplies_ok);
    bbp_put_f32(resp, &pos, s->avdd_hi_v);
    bbp_put_f32(resp, &pos, s->dvcc_v);
    bbp_put_f32(resp, &pos, s->avcc_v);
    bbp_put_f32(resp, &pos, s->avss_v);
    bbp_put_f32(resp, &pos, s->temp_c);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SELFTEST_WORKER  payload: u8 op (0=disable, 1=enable, 0xFF=query)
// resp: u8 enabled
// Wire format matches legacy handleSelftestWorker (bbp.cpp:1109-1128).
// ---------------------------------------------------------------------------
static int handler_selftest_worker(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t op = payload[0];

    if (op == 0x00 || op == 0x01) {
        if (!selftest_set_worker_enabled(op != 0)) return -CMD_ERR_INVALID_STATE;
    } else if (op != 0xFF) {
        return -CMD_ERR_BAD_ARG;
    }

    size_t pos = 0;
    bbp_put_u8(resp, &pos, selftest_worker_enabled() ? 1 : 0);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// selftest_status, selftest_supply_voltages_cached, selftest_int_supplies:
// complex multi-field responses — rsp=NULL (CLI shows raw response byte count).

static const ArgSpec s_selftest_measure_supply_args[] = {
    { "rail", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_selftest_measure_supply_rsp[] = {
    { "rail",    ARG_U8,  true, 0, 0 },
    { "voltage", ARG_F32, true, 0, 0 },
};

static const ArgSpec s_selftest_auto_cal_args[] = {
    { "idac_ch", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_selftest_auto_cal_rsp[] = {
    { "status",           ARG_U8,  true, 0, 0 },
    { "channel",          ARG_U8,  true, 0, 0 },
    { "points_collected", ARG_U8,  true, 0, 0 },
    { "last_measured_v",  ARG_F32, true, 0, 0 },
    { "error_mv",         ARG_F32, true, 0, 0 },
};

static const ArgSpec s_selftest_worker_args[] = {
    { "op", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_selftest_worker_rsp[] = {
    { "enabled", ARG_U8, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_selftest_cmds[] = {
    { BBP_CMD_SELFTEST_STATUS,                 "selftest_status",
      NULL,                           0, NULL,                          0, handler_selftest_status,                 CMD_FLAG_READS_STATE },
    { BBP_CMD_SELFTEST_MEASURE_SUPPLY,         "selftest_measure_supply",
      s_selftest_measure_supply_args, 1, s_selftest_measure_supply_rsp, 2, handler_selftest_measure_supply,         0                   },
    { BBP_CMD_SELFTEST_SUPPLY_VOLTAGES_CACHED, "selftest_supply_voltages_cached",
      NULL,                           0, NULL,                          0, handler_selftest_supply_voltages_cached, CMD_FLAG_READS_STATE },
    { BBP_CMD_SELFTEST_AUTO_CAL,               "selftest_auto_cal",
      s_selftest_auto_cal_args,       1, s_selftest_auto_cal_rsp,       5, handler_selftest_auto_cal,               0                   },
    { BBP_CMD_SELFTEST_INT_SUPPLIES,           "selftest_int_supplies",
      NULL,                           0, NULL,                          0, handler_selftest_int_supplies,           0                   },
    { BBP_CMD_SELFTEST_WORKER,                 "selftest_worker",
      s_selftest_worker_args,         1, s_selftest_worker_rsp,         1, handler_selftest_worker,                 0                   },
};

extern "C" void register_cmds_selftest(void)
{
    cmd_registry_register_block(s_selftest_cmds,
        sizeof(s_selftest_cmds) / sizeof(s_selftest_cmds[0]));
}
