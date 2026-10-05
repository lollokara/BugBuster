// =============================================================================
// daq_bridge.cpp — see daq_bridge.h.
// =============================================================================

#include "daq_bridge.h"
#include "daq_codec.h"
#include "hat.h"

bool daq_mp_present(void)
{
    const HatState *hs = hat_get_state();
    return hs && hs->connected && hs->type == HAT_TYPE_DAQ_POWER;
}

bool daq_mp_vdut_status(daq_mp_vdut_t *out)
{
    hat_vdut_status_t st;
    if (!out || !hat_daq_vdut_status(&st)) return false;
    out->present  = st.present != 0;
    out->enabled  = st.enabled != 0;
    out->fault    = st.fault != 0;
    out->set_v    = st.vdut_set_v;
    out->ilimit_a = st.ilimit_set_a;
    out->meas_v   = st.meas_v;
    out->meas_i   = st.meas_i;
    return true;
}

bool daq_mp_vdut_enable(bool enable)
{
    return hat_daq_vdut_enable(enable);
}

int daq_mp_vdut_owner_run(void)
{
    return hat_daq_vdut_owner_run();
}

int daq_mp_vdut_setpoint(float volts, float amps_limit)
{
    // Same bounds as api_daq_vdut_setpoint(): reject, never clamp.
    if (volts < HAT_DAQ_VDUT_MIN_V || volts > HAT_DAQ_VDUT_MAX_V) return 1;
    if (amps_limit < HAT_DAQ_VDUT_ILIMIT_MIN_A || amps_limit > HAT_DAQ_VDUT_ILIMIT_MAX_A) return 1;
    return hat_daq_vdut_setpoint(volts, amps_limit) ? 0 : -1;
}

int daq_mp_bs(const uint8_t *req, uint8_t len, uint8_t *rsp, uint16_t cap)
{
    return hat_bs_request(req, len, rsp, cap, 600);
}

bool daq_mp_cfg_set(const uint8_t *tlv, uint8_t len)
{
    uint8_t rsp[16];
    uint8_t rsp_len = 0;
    return hat_request(DAQC_CFG_CMD_SET, tlv, len, rsp, &rsp_len, 300, sizeof(rsp)) == HAT_RSP_OK;
}

bool daq_mp_cfg_action(uint8_t action_id)
{
    uint8_t rsp[16];
    uint8_t rsp_len = 0;
    return hat_request(DAQC_CFG_CMD_ACTION, &action_id, 1, rsp, &rsp_len, 1000, sizeof(rsp)) ==
           HAT_RSP_OK;
}
