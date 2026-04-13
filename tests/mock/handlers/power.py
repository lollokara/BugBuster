"""
Power management handlers for SimulatedDevice.

Handles: PCA_GET_STATUS, PCA_SET_CONTROL, PCA_SET_PORT, PCA_SET_FAULT_CFG,
         PCA_GET_FAULT_LOG.
"""

import struct
from bugbuster.constants import CmdId


def register(device) -> None:
    device.register_handler(CmdId.PCA_GET_STATUS,    _pca_get_status(device))
    device.register_handler(CmdId.PCA_SET_CONTROL,   _pca_set_control(device))
    device.register_handler(CmdId.PCA_SET_PORT,      _pca_set_port(device))
    device.register_handler(CmdId.PCA_SET_FAULT_CFG, _pca_set_fault_cfg(device))
    device.register_handler(CmdId.PCA_GET_FAULT_LOG, _pca_get_fault_log(device))


# ---------------------------------------------------------------------------
# PCA_GET_STATUS (0xB0)
# client: _parse_pca_status(resp)
#   resp[0]    present (B)
#   resp[1]    input0  (B)
#   resp[2]    input1  (B)
#   resp[3]    out0    (B)
#   resp[4]    out1    (B)
#   resp[5]    logic_pg (B)
#   resp[6]    vadj1_pg (B)
#   resp[7]    vadj2_pg (B)
#   resp[8:12] efuse_faults[0..3] (4×B)
# ---------------------------------------------------------------------------

def _pca_get_status(device):
    def handler(payload: bytes) -> bytes:
        buf = bytearray()
        buf.append(1)            # present = True
        buf.append(0)            # input0
        buf.append(0)            # input1
        buf.append(0)            # out0
        buf.append(0)            # out1
        buf.append(1)            # logic_pg = True
        buf.append(1)            # vadj1_pg = True
        buf.append(1)            # vadj2_pg = True
        buf.append(0)            # efuse_fault[0]
        buf.append(0)            # efuse_fault[1]
        buf.append(0)            # efuse_fault[2]
        buf.append(0)            # efuse_fault[3]
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# PCA_SET_CONTROL (0xB1)
# client: struct.pack('<BB', int(control), int(on))
# ---------------------------------------------------------------------------

def _pca_set_control(device):
    def handler(payload: bytes) -> bytes:
        control_id, state = struct.unpack_from('<BB', payload)
        device.pca_control[control_id] = bool(state)
        return b''
    return handler


# ---------------------------------------------------------------------------
# PCA_SET_PORT (0xB2)
# ---------------------------------------------------------------------------

def _pca_set_port(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# PCA_SET_FAULT_CFG (0xB3)
# client: struct.pack('<BB', int(auto_disable), int(log_events))
# ---------------------------------------------------------------------------

def _pca_set_fault_cfg(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# PCA_GET_FAULT_LOG (0xB4)
# client: count (B), then count × (ftype B, ch B, ts I)
# Return empty log (count=0).
# ---------------------------------------------------------------------------

def _pca_get_fault_log(device):
    def handler(payload: bytes) -> bytes:
        return struct.pack('<B', 0)  # count = 0
    return handler
