// =============================================================================
// bbp_adapter.cpp — BBP transport adapter for the command registry
// =============================================================================
#include "bbp_adapter.h"
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp.h"
#include "esp_log.h"

static const char *TAG = "bbp_adapter";

int bbp_adapter_dispatch(uint8_t cmd_id,
                         const uint8_t *payload, size_t payload_len,
                         uint8_t *rsp_buf, size_t *rsp_len)
{
    const CmdDescriptor *desc = cmd_registry_lookup_opcode((uint16_t)cmd_id);
    if (!desc) {
        return -1;  // Not in registry — fall through to legacy switch
    }

    size_t out_len = 0;
    int rc = desc->handler(payload, payload_len, rsp_buf, &out_len);
    if (rc < 0) {
        // rc is negative CmdError; map to BBP error code
        CmdError ce = (CmdError)(-rc);
        int bbp_err = cmd_error_to_bbp(ce);
        ESP_LOGD(TAG, "cmd 0x%02X -> CmdError %d -> BBP err 0x%02X", cmd_id, (int)ce, bbp_err);
        return bbp_err;  // >0: caller sends BBP_MSG_ERR with this code
    }

    *rsp_len = out_len;
    return 0;
}
