"""
IDAC (DS4424) handlers for SimulatedDevice.

Handles: IDAC_GET_STATUS, IDAC_SET_CODE, IDAC_SET_VOLTAGE, IDAC_CALIBRATE,
         IDAC_CAL_ADD_POINT, IDAC_CAL_CLEAR, IDAC_CAL_SAVE.

State: device.idac — list of 4 channel dicts (indices 0-3).
"""

import struct
from bugbuster.constants import CmdId

_NUM_CHANNELS = 4


def _ensure_idac(device):
    if not hasattr(device, 'idac') or device.idac is None:
        device.idac = [
            {
                "code": 0,
                "target_v": 5.0,
                "actual_v": 5.0,
                "v_min": 3.0,
                "v_max": 15.0,
                "step_mv": 100.0,
                "calibrated": False,
            }
            for _ in range(_NUM_CHANNELS)
        ]


def register(device) -> None:
    _ensure_idac(device)
    device.register_handler(CmdId.IDAC_GET_STATUS,    _idac_get_status(device))
    device.register_handler(CmdId.IDAC_SET_CODE,      _idac_set_code(device))
    device.register_handler(CmdId.IDAC_SET_VOLTAGE,   _idac_set_voltage(device))
    device.register_handler(CmdId.IDAC_CALIBRATE,     _idac_calibrate(device))
    device.register_handler(CmdId.IDAC_CAL_ADD_POINT, _idac_cal_add_point(device))
    device.register_handler(CmdId.IDAC_CAL_CLEAR,     _idac_cal_clear(device))
    device.register_handler(CmdId.IDAC_CAL_SAVE,      _idac_cal_save(device))


# ---------------------------------------------------------------------------
# IDAC_GET_STATUS (0xA0)
# client unpacks per channel (26 bytes each):
#   code    (<b)   1 byte  signed
#   tgt     (<f)   4 bytes
#   act     (<f)   4 bytes
#   mid     (<f)   4 bytes  (unused mid placeholder)
#   vmin    (<f)   4 bytes
#   vmax    (<f)   4 bytes
#   step_mv (<f)   4 bytes
#   cal     (B)    1 byte
# Preceded by present (B).
# ---------------------------------------------------------------------------

def _idac_get_status(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        buf = bytearray()
        buf.append(1)   # present = True
        for ch in device.idac:
            buf += struct.pack('<b', ch.get('code', 0))
            buf += struct.pack('<f', ch.get('target_v', 5.0))
            buf += struct.pack('<f', ch.get('actual_v', 5.0))
            buf += struct.pack('<f', 0.0)   # mid placeholder
            buf += struct.pack('<f', ch.get('v_min', 3.0))
            buf += struct.pack('<f', ch.get('v_max', 15.0))
            buf += struct.pack('<f', ch.get('step_mv', 100.0))
            buf.append(int(ch.get('calibrated', False)))
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# IDAC_SET_CODE (0xA1)
# client: struct.pack('<Bb', channel, code)
# ---------------------------------------------------------------------------

def _idac_set_code(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        ch_idx, code = struct.unpack_from('<Bb', payload)
        if 0 <= ch_idx < len(device.idac):
            device.idac[ch_idx]['code'] = code
        return b''
    return handler


# ---------------------------------------------------------------------------
# IDAC_SET_VOLTAGE (0xA2)
# client: struct.pack('<Bf', channel, float(voltage))
# ---------------------------------------------------------------------------

def _idac_set_voltage(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        ch_idx, = struct.unpack_from('<B', payload)
        voltage, = struct.unpack_from('<f', payload, 1)
        if 0 <= ch_idx < len(device.idac):
            device.idac[ch_idx]['target_v'] = voltage
            device.idac[ch_idx]['actual_v'] = voltage
        return b''
    return handler


# ---------------------------------------------------------------------------
# IDAC_CALIBRATE (0xA3)
# ---------------------------------------------------------------------------

def _idac_calibrate(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        ch_idx = payload[0] if payload else 0
        if 0 <= ch_idx < len(device.idac):
            device.idac[ch_idx]['calibrated'] = True
        return b''
    return handler


# ---------------------------------------------------------------------------
# IDAC_CAL_ADD_POINT (0xA4) — no-op
# ---------------------------------------------------------------------------

def _idac_cal_add_point(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# IDAC_CAL_CLEAR (0xA5)
# ---------------------------------------------------------------------------

def _idac_cal_clear(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        ch_idx = payload[0] if payload else 0
        if 0 <= ch_idx < len(device.idac):
            device.idac[ch_idx]['calibrated'] = False
        return b''
    return handler


# ---------------------------------------------------------------------------
# IDAC_CAL_SAVE (0xA6) — no-op
# ---------------------------------------------------------------------------

def _idac_cal_save(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler
