"""
IDAC (DS4424) handlers for SimulatedDevice.

Handles: IDAC_GET_STATUS, IDAC_SET_CODE, IDAC_SET_VOLTAGE, IDAC_CALIBRATE,
         IDAC_CAL_ADD_POINT, IDAC_CAL_CLEAR, IDAC_CAL_SAVE.

State: device.idac — list of 4 channel dicts (indices 0-3). The DS4424 has
four channels but only 0-2 are wired on this board, and the firmware reports
only those three — see _NUM_REPORTED_CHANNELS.
"""

import struct
from bugbuster.constants import CmdId, ErrorCode
from bugbuster.transport.usb import DeviceError

_NUM_CHANNELS = 4

# handler_idac_get_status() in Firmware/ESP32/src/bbp/cmds/cmd_idac.cpp loops
# `ch < 3`: channel 3 is not connected and emitting it was a real firmware bug
# once already. The simulator must report the same three, or a host parser that
# assumes four passes here and returns garbage on hardware.
_NUM_REPORTED_CHANNELS = 3


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
#
# Wire format is dictated by the FIRMWARE, not by whatever the client happens
# to parse. Per channel, 44 bytes, mirroring the bbp_put_* calls in
# cmd_idac.cpp:handler_idac_get_status():
#   u8 ch, i8 dac_code, f32 target_v, f32 actual_v, f32 midpoint_v,
#   f32 v_min, f32 v_max, f32 step_mv, u8 cal_valid, u8 have_poly, f32 poly[4]
# Preceded by a single u8 `present`.
#
# This previously emitted 26 bytes for 4 channels to match a client parser that
# was itself wrong, so the whole suite was green while idac_get_status()
# returned garbage against real hardware.
# ---------------------------------------------------------------------------

_CH_FMT = "<BbffffffBB4f"


def _idac_get_status(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        buf = bytearray()
        buf.append(1)   # present = True
        for idx in range(_NUM_REPORTED_CHANNELS):
            ch = device.idac[idx]
            cal = bool(ch.get('calibrated', False))
            buf += struct.pack(
                _CH_FMT,
                idx,
                ch.get('code', 0),
                ch.get('target_v', 5.0),
                ch.get('actual_v', 5.0),
                ch.get('midpoint_v', 5.0),
                ch.get('v_min', 3.0),
                ch.get('v_max', 15.0),
                ch.get('step_mv', 100.0),
                int(cal),
                int(cal),          # have_poly: only fitted once calibrated
                0.0, 0.0, 0.0, 0.0,
            )
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
# payload: u8 ch, u8 step, u16 settle_ms, u8 adc_ch  -> resp: u8 ch, u8 points
# ---------------------------------------------------------------------------

def _idac_calibrate(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        if len(payload) < 5:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        ch_idx, step, _settle, _adc = struct.unpack('<BBHB', payload[:5])
        if ch_idx >= 3:
            raise DeviceError(ErrorCode.INVALID_CHANNEL, 0)
        # The real sweep walks the code range in `step` increments.
        points = max(1, 255 // max(1, step))
        device.idac[ch_idx]['calibrated'] = True
        device.idac[ch_idx]['cal_points'] = points
        return struct.pack('<BB', ch_idx, points & 0xFF)
    return handler


# ---------------------------------------------------------------------------
# IDAC_CAL_ADD_POINT (0xA4)
# payload: u8 ch, i8 code, f32 measured_v -> resp: u8 ch, u8 count, bool valid
# ---------------------------------------------------------------------------

def _idac_cal_add_point(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        if len(payload) < 6:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        ch_idx, code, measured = struct.unpack('<Bbf', payload[:6])
        if ch_idx >= 3:
            raise DeviceError(ErrorCode.INVALID_CHANNEL, 0)
        pts = device.idac[ch_idx].setdefault('cal_curve', [])
        pts.append((code, measured))
        # ds4424 needs at least two points before the curve can be used.
        valid = len(pts) >= 2
        device.idac[ch_idx]['cal_points'] = len(pts)
        device.idac[ch_idx]['calibrated'] = valid
        return struct.pack('<BBB', ch_idx, len(pts) & 0xFF, 1 if valid else 0)
    return handler


# ---------------------------------------------------------------------------
# IDAC_CAL_CLEAR (0xA5)
# payload: u8 ch -> resp: u8 ch
# ---------------------------------------------------------------------------

def _idac_cal_clear(device):
    def handler(payload: bytes) -> bytes:
        _ensure_idac(device)
        if not payload:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        ch_idx = payload[0]
        if ch_idx >= 3:
            raise DeviceError(ErrorCode.INVALID_CHANNEL, 0)
        device.idac[ch_idx]['calibrated'] = False
        device.idac[ch_idx]['cal_curve'] = []
        device.idac[ch_idx]['cal_points'] = 0
        return struct.pack('<B', ch_idx)
    return handler


# ---------------------------------------------------------------------------
# IDAC_CAL_SAVE (0xA6) — no-op
# ---------------------------------------------------------------------------

def _idac_cal_save(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler
