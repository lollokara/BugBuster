// =============================================================================
// cmd_daq.cpp — BBP commands for the DAQ HAT (ESP32-P4), forwarded over the HAT
// UART bridge.
//
// The P4 implements key-addressed TLV config commands (CONFIG_GET/SET/GET_ALL/
// SCHEMA/ACTION = 0x70..0x74 on the HAT wire). The opcode space on the S3 is
// full, so the host reaches them through a single multiplexed BBP opcode
// (BBP_CMD_DAQ_CONFIG); payload[0] is the sub-op, which maps to HAT cmd
// 0x70 + sub-op. The rest of the payload and the response are passed through
// verbatim — the S3 does not interpret the TLV/schema bytes, keeping the
// registry a single source of truth on the P4 and in the host library.
// =============================================================================

#include <string.h>
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp.h"
#include "hat.h"

// The DAQ HAT's CONFIG responses (mirror Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h).
#define DAQ_HAT_RSP_CONFIG_VALUE   0x93
#define DAQ_HAT_RSP_CONFIG_SCHEMA  0x94
#define DAQ_HAT_RSP_CAL_STATUS     0x95
#define DAQ_HAT_RSP_DAQ_STATUS     0x90
#define DAQ_HAT_CMD_GET_STATUS     0x53

// Map the HAT bridge's last error to a BBP CmdError.
static int daq_hat_err(void)
{
    switch (hat_get_last_error()) {
        case HAT_ERR_BUSY:        return -CMD_ERR_BUSY;
        case HAT_ERR_UNSUPPORTED: return -CMD_ERR_INVALID_STATE;
        default:                  return -CMD_ERR_HARDWARE;
    }
}

// ---------------------------------------------------------------------------
// DAQ_CONFIG (sub-op multiplexed) -> P4 HAT CONFIG command 0x70 + sub-op.
//   payload[0]   = sub-op (BBP_DAQ_CFG_*)
//   payload[1..] = forwarded to the HAT as-is
//   response     = the HAT's response payload, verbatim
// ---------------------------------------------------------------------------
static int handler_daq_config(const uint8_t *payload, size_t len,
                              uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t subop = payload[0];
    if (subop > BBP_DAQ_CFG_ACTION) return -CMD_ERR_BAD_ARG;

    if (!hat_get_state()->connected) return -CMD_ERR_INVALID_STATE;

    const uint8_t hat_cmd = (uint8_t)(0x70 + subop);
    uint8_t fwd_len = (uint8_t)(len - 1);

    uint8_t rsp[240];
    uint8_t rsp_len = 0;
    uint8_t rsp_code = hat_request(hat_cmd, payload + 1, fwd_len,
                                   rsp, &rsp_len, /*timeout_ms=*/300, sizeof(rsp));

    if (rsp_code == 0) return -CMD_ERR_TIMEOUT;
    // OK (0x80, e.g. SET/ACTION) and the data responses are all success; any
    // other code (incl. HAT error) maps to a CmdError.
    if (rsp_code != HAT_RSP_OK &&
        rsp_code != DAQ_HAT_RSP_CONFIG_VALUE &&
        rsp_code != DAQ_HAT_RSP_CONFIG_SCHEMA) {
        return daq_hat_err();
    }

    if (rsp_len) memcpy(resp, rsp, rsp_len);
    *resp_len = rsp_len;
    return (int)rsp_len;
}

// ---------------------------------------------------------------------------
// DAQ_CAL (sub-op multiplexed) -> P4 HAT calibration command 0x56 + sub-op.
//   payload[0]   = sub-op (BBP_DAQ_CAL_*)
//   payload[1..] = forwarded to the HAT as-is (START carries the mode byte)
//   response     = the HAT's response payload, verbatim (STATUS -> 0x95 blob)
// ---------------------------------------------------------------------------
static int handler_daq_cal(const uint8_t *payload, size_t len,
                           uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t subop = payload[0];
    if (subop > BBP_DAQ_CAL_ABORT) return -CMD_ERR_BAD_ARG;

    if (!hat_get_state()->connected) return -CMD_ERR_INVALID_STATE;

    const uint8_t hat_cmd = (uint8_t)(0x56 + subop);
    uint8_t fwd_len = (uint8_t)(len - 1);

    uint8_t rsp[240];
    uint8_t rsp_len = 0;
    uint8_t rsp_code = hat_request(hat_cmd, payload + 1, fwd_len,
                                   rsp, &rsp_len, /*timeout_ms=*/300, sizeof(rsp));

    if (rsp_code == 0) return -CMD_ERR_TIMEOUT;
    if (rsp_code != HAT_RSP_OK && rsp_code != DAQ_HAT_RSP_CAL_STATUS) {
        return daq_hat_err();
    }

    if (rsp_len) memcpy(resp, rsp, rsp_len);
    *resp_len = rsp_len;
    return (int)rsp_len;
}

// ---------------------------------------------------------------------------
// DAQ_MEASURE -> P4 HAT GET_STATUS (0x53). Returns s3link_daq_status_t verbatim
// (range, streaming, source_enabled, _pad, last_i, last_v, last_p, energy_mwh).
// ---------------------------------------------------------------------------
static int handler_daq_measure(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;
    if (!hat_get_state()->connected) return -CMD_ERR_INVALID_STATE;

    uint8_t rsp[240];
    uint8_t rsp_len = 0;
    uint8_t rsp_code = hat_request(DAQ_HAT_CMD_GET_STATUS, NULL, 0,
                                   rsp, &rsp_len, /*timeout_ms=*/300, sizeof(rsp));

    if (rsp_code == 0) return -CMD_ERR_TIMEOUT;
    if (rsp_code != HAT_RSP_OK && rsp_code != DAQ_HAT_RSP_DAQ_STATUS) {
        return daq_hat_err();
    }

    if (rsp_len) memcpy(resp, rsp, rsp_len);
    *resp_len = rsp_len;
    return (int)rsp_len;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
static const CmdDescriptor s_daq_cmds[] = {
    { BBP_CMD_DAQ_CONFIG, "daq_config",
      NULL, 0, NULL, 0, handler_daq_config, CMD_FLAG_READS_STATE },
    { BBP_CMD_DAQ_CAL, "daq_cal",
      NULL, 0, NULL, 0, handler_daq_cal, CMD_FLAG_READS_STATE },
    { BBP_CMD_DAQ_MEASURE, "daq_measure",
      NULL, 0, NULL, 0, handler_daq_measure, CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_daq(void)
{
    cmd_registry_register_block(s_daq_cmds,
        sizeof(s_daq_cmds) / sizeof(s_daq_cmds[0]));
}
