"""
Power management handlers for SimulatedDevice.

Handles: PCA_GET_STATUS, PCA_SET_CONTROL, PCA_SET_PORT, PCA_SET_FAULT_CFG,
         PCA_GET_FAULT_LOG, EFUSE_IMON_SET, EFUSE_IMON_GET.
"""

import struct
from bugbuster.constants import CmdId, ErrorCode
from bugbuster.transport.usb import DeviceError


def register(device) -> None:
    device.register_handler(CmdId.PCA_GET_STATUS,    _pca_get_status(device))
    device.register_handler(CmdId.PCA_SET_CONTROL,   _pca_set_control(device))
    device.register_handler(CmdId.PCA_SET_PORT,      _pca_set_port(device))
    device.register_handler(CmdId.PCA_SET_FAULT_CFG, _pca_set_fault_cfg(device))
    device.register_handler(CmdId.PCA_GET_FAULT_LOG, _pca_get_fault_log(device))
    device.register_handler(CmdId.EFUSE_IMON_SET,    _efuse_imon_set(device))
    device.register_handler(CmdId.EFUSE_IMON_GET,    _efuse_imon_get(device))


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
        for f in getattr(device, "efuse_faults", [False] * 4):
            buf.append(int(bool(f)))   # efuse_fault[0..3]
        # Decoded enables (9 bytes added in fw 3.4.0).
        # Keys match PcaControl enum: 0=VADJ1_EN, 1=VADJ2_EN, 2=15V_EN,
        # 3=MUX_EN, 4=USB_HUB_EN, 5-8=EFUSE1-4_EN.
        buf.append(int(device.pca_control.get(0, False)))   # VADJ1_EN
        buf.append(int(device.pca_control.get(1, False)))   # VADJ2_EN
        buf.append(int(device.pca_control.get(2, False)))   # 15V_EN
        buf.append(int(device.pca_control.get(3, True)))    # MUX_EN
        buf.append(int(device.pca_control.get(4, True)))    # USB_HUB_EN
        buf.append(int(device.pca_control.get(5, False)))   # EFUSE1_EN
        buf.append(int(device.pca_control.get(6, False)))   # EFUSE2_EN
        buf.append(int(device.pca_control.get(7, False)))   # EFUSE3_EN
        buf.append(int(device.pca_control.get(8, False)))   # EFUSE4_EN
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
# payload: u8 port(0-1), u8 val -> resp: u8 port, u8 val
# ---------------------------------------------------------------------------

def _pca_set_port(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 2:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        port, val = payload[0], payload[1]
        if port > 1:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        device.pca_ports[port] = val
        return bytes([port, val])
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
# device.pca_fault_log: list of (ftype, channel 0-based, ts_ms); default empty.
# ---------------------------------------------------------------------------

def _pca_get_fault_log(device):
    def handler(payload: bytes) -> bytes:
        log = getattr(device, "pca_fault_log", [])
        return struct.pack('<B', len(log)) + b"".join(
            struct.pack('<BBI', t, ch, ts) for t, ch, ts in log)
    return handler


# ---------------------------------------------------------------------------
# EFUSE_IMON_SET (0x0D) / EFUSE_IMON_GET (0x0E)
# SET payload: u8 efuse (0=off, 1..4), u8 flags (bit0 confirm power-cycle)
# SET resp:    u8 result, status block.  GET resp: status block.
# status block: u8 efuse, u8 flags (b0 valid, b1 saturated, b2 efuse_on),
#               f32 imon_v, f32 current_ma
# E-fuse n is PCA control id 4+n (EFUSE1_EN = 5).
# ---------------------------------------------------------------------------

_IMON_OK, _IMON_NEEDS_CONFIRM, _IMON_INVALID = 0, 1, 3


def _efuse_is_on(device, efuse: int) -> bool:
    return efuse != 0 and bool(device.pca_control.get(4 + efuse, False))


def _efuse_imon_block(device) -> bytes:
    efuse = getattr(device, "efuse_imon", 0)
    on = _efuse_is_on(device, efuse)
    flags = (0x01 if efuse else 0) | (0x04 if on else 0)
    return struct.pack('<BBff', efuse, flags, 0.0, 0.0)


def _efuse_imon_set(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        efuse = payload[0]
        confirm = len(payload) >= 2 and bool(payload[1] & 0x01)
        if efuse > 4:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        old = getattr(device, "efuse_imon", 0)
        rc = _IMON_OK
        if efuse != old:
            if (_efuse_is_on(device, old) or _efuse_is_on(device, efuse)) and not confirm:
                rc = _IMON_NEEDS_CONFIRM
            else:
                device.efuse_imon = efuse
        return bytes([rc]) + _efuse_imon_block(device)
    return handler


def _efuse_imon_get(device):
    def handler(payload: bytes) -> bytes:
        return _efuse_imon_block(device)
    return handler
