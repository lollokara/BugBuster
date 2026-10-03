#pragma once
// =============================================================================
// daq_bridge.h — C-linkage bridge from the MicroPython `daq` module (moddaq.c)
// to the C++ HAT driver (hat.cpp). Same pattern as modbugbuster_bridge.h.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool  present, enabled, fault;
    float set_v, ilimit_a;      // setpoints (V, A)
    float meas_v, meas_i;       // measured (V, A)
} daq_mp_vdut_t;

bool daq_mp_present(void);                                       // connected DAQ HAT
bool daq_mp_vdut_status(daq_mp_vdut_t *out);
bool daq_mp_vdut_enable(bool enable);
int  daq_mp_vdut_setpoint(float volts, float amps_limit);        // 0 ok, 1 out of range, -1 HAT error
int  daq_mp_bs(const uint8_t *req, uint8_t len, uint8_t *rsp, uint16_t cap); // -1 io, -2 rejected
bool daq_mp_cfg_set(const uint8_t *tlv, uint8_t len);             // HAT cmd 0x71
bool daq_mp_cfg_action(uint8_t action_id);                        // HAT cmd 0x74

#ifdef __cplusplus
}
#endif
