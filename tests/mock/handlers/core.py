"""
Core command handlers for SimulatedDevice.

Handles: PING, GET_STATUS, GET_DEVICE_INFO, GET_FAULTS, GET_DIAGNOSTICS,
         GET_ADMIN_TOKEN, DEVICE_RESET, DISCONNECT, selftest stubs,
         CLEAR_ALL_ALERTS, CLEAR_CHAN_ALERT, SET_ALERT_MASK, SET_CH_ALERT_MASK.
"""

import struct
from bugbuster.constants import CmdId


_ADMIN_TOKEN = b"SIMTOKEN"   # fixed 8-char ASCII token


def register(device) -> None:
    device.register_handler(CmdId.PING,             _ping(device))
    device.register_handler(CmdId.GET_STATUS,        _get_status(device))
    device.register_handler(CmdId.GET_DEVICE_INFO,   _get_device_info(device))
    device.register_handler(CmdId.GET_FAULTS,        _get_faults(device))
    device.register_handler(CmdId.GET_DIAGNOSTICS,   _get_diagnostics(device))
    device.register_handler(CmdId.GET_ADMIN_TOKEN,   _get_admin_token(device))
    device.register_handler(CmdId.DEVICE_RESET,      _device_reset(device))
    device.register_handler(CmdId.DISCONNECT,        _disconnect(device))

    # Selftest stubs
    device.register_handler(CmdId.SELFTEST_STATUS,          _selftest_status(device))
    device.register_handler(CmdId.SELFTEST_MEASURE_SUPPLY,  _selftest_measure_supply(device))
    device.register_handler(CmdId.SELFTEST_EFUSE_CURRENTS,  _selftest_efuse_currents(device))
    device.register_handler(CmdId.SELFTEST_AUTO_CAL,        _selftest_auto_cal(device))
    device.register_handler(CmdId.SELFTEST_INT_SUPPLIES,    _selftest_int_supplies(device))

    # Minimal channel stub so reset test can call set_channel_function
    # (full channel handlers live in channels.py)
    device.register_handler(CmdId.SET_CHANNEL_FUNC,  _set_channel_func(device))

    # Alert / fault management
    device.register_handler(CmdId.CLEAR_ALL_ALERTS,  _clear_all_alerts(device))
    device.register_handler(CmdId.CLEAR_CHAN_ALERT,  _clear_chan_alert(device))
    device.register_handler(CmdId.SET_ALERT_MASK,    _set_alert_mask(device))
    device.register_handler(CmdId.SET_CH_ALERT_MASK, _set_ch_alert_mask(device))

    # Missing dummy handlers
    device.register_handler(CmdId.SELFTEST_WORKER,   _selftest_worker(device))
    device.register_handler(CmdId.QS_LIST,           _qs_list(device))
    device.register_handler(CmdId.QS_GET,            _qs_get(device))
    device.register_handler(CmdId.QS_SAVE,           _qs_save(device))
    device.register_handler(CmdId.QS_APPLY,          _qs_apply(device))
    device.register_handler(CmdId.QS_DELETE,         _qs_delete(device))


# ---------------------------------------------------------------------------
# Dummy implementations for completeness test
# ---------------------------------------------------------------------------

def _selftest_worker(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler

def _qs_list(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler

def _qs_get(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler

def _qs_save(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler

def _qs_apply(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler

def _qs_delete(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler

# ---------------------------------------------------------------------------
# PING (0xFE)
# client.py: tok, uptime = struct.unpack_from('<II', resp)
# ---------------------------------------------------------------------------

def _ping(device):
    def handler(payload: bytes) -> bytes:
        token, = struct.unpack_from('<I', payload)
        return struct.pack('<II', token, device.uptime_ms)
    return handler


# ---------------------------------------------------------------------------
# GET_STATUS (0x01)
# client.py: _parse_status(resp)
#   resp[0]         spi_ok (bool byte)
#   resp[1:5]       die_temp_c (float LE)
#   resp[5:15]      alert_status, alert_mask, supply_alert_status,
#                   supply_alert_mask, live_status  (5× uint16 LE)
#   resp[15:]       4 channels × 30 bytes each
#     off+0:2       ch_id, func (BB)
#     off+2:5       adc_raw (3-byte LE int)
#     off+5:9       adc_val (float LE)
#     off+9:12      adc_rng, adc_rate, adc_mux (BBB)
#     off+12:14     dac_code (uint16 LE)
#     off+14:18     dac_val (float LE)
#     off+18        din_state (bool byte)
#     off+19:23     din_counter (uint32 LE)
#     off+23        do_state (bool byte)
#     off+24:26     ch_alert (uint16 LE)
#     off+26:28     ch_alert_mask (uint16 LE)
#     off+28:30     rtd_excitation_ua (uint16 LE)
#   resp[135:]      4 diagnostics × 7 bytes each
#     off+0         source (B)
#     off+1:3       raw_code (uint16 LE)
#     off+3:7       value (float LE)
# ---------------------------------------------------------------------------

def _get_status(device):
    def handler(payload: bytes) -> bytes:
        buf = bytearray()

        buf += struct.pack('<B', int(device.spi_ok))
        buf += struct.pack('<f', device.die_temp_c)
        buf += struct.pack('<HHHHH',
                           device.alert_status,
                           device.alert_mask,
                           device.supply_alert_status,
                           device.supply_alert_mask,
                           device.live_status)

        for ch in device.channels:
            adc_raw = ch["adc_raw"] & 0xFFFFFF
            adc_raw_bytes = adc_raw.to_bytes(3, 'little')
            buf += struct.pack('<BB', ch["id"], ch["function"])
            buf += adc_raw_bytes
            buf += struct.pack('<f', ch["adc_value"])
            buf += struct.pack('<BBB', ch["adc_range"], ch["adc_rate"], ch["adc_mux"])
            buf += struct.pack('<H', ch["dac_code"])
            buf += struct.pack('<f', ch["dac_value"])
            buf += struct.pack('<B', int(ch["din_state"]))
            buf += struct.pack('<I', ch["din_counter"])
            buf += struct.pack('<B', int(ch["do_state"]))
            buf += struct.pack('<H', ch["channel_alert"])
            buf += struct.pack('<H', ch["channel_alert_mask"])
            buf += struct.pack('<H', ch["rtd_excitation_ua"])

        # 4 diagnostic slots — all zeroed
        for _ in range(4):
            buf += struct.pack('<BHf', 0, 0, 0.0)

        # MUX state (4 bytes at offset 163) — added for BBP v4 GET_STATUS.
        # Source the 4-device state from the simulator if available.
        mux_states = getattr(device, "mux_states", [0, 0, 0, 0])
        if not isinstance(mux_states, (list, tuple)) or len(mux_states) != 4:
            mux_states = [0, 0, 0, 0]
        buf += struct.pack('<BBBB', *(b & 0xFF for b in mux_states))

        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# GET_DEVICE_INFO (0x02)
# client.py: spi_ok, rev = struct.unpack_from('<BB', resp)
#            id0, id1    = struct.unpack_from('<HH', resp, 2)
# ---------------------------------------------------------------------------

def _get_device_info(device):
    def handler(payload: bytes) -> bytes:
        return struct.pack('<BBHH', 1, 0x01, 0xABCD, 0x1234)
    return handler


# ---------------------------------------------------------------------------
# GET_FAULTS (0x03)
# client.py: _parse_faults(resp)
#   alert, alert_mask, supply, supply_mask = struct.unpack_from('<HHHH', resp)
#   then 4 × (ch_id B, ch_alert H, ch_mask H) at offsets 8, 13, 18, 23
# ---------------------------------------------------------------------------

def _get_faults(device):
    def handler(payload: bytes) -> bytes:
        buf = struct.pack('<HHHH',
                          device.alert_status,
                          device.alert_mask,
                          device.supply_alert_status,
                          device.supply_alert_mask)
        for ch in device.channels:
            buf += struct.pack('<BHH', ch["id"], ch["channel_alert"], ch["channel_alert_mask"])
        return buf
    return handler


# ---------------------------------------------------------------------------
# GET_DIAGNOSTICS (0x04) — stub, return empty
# ---------------------------------------------------------------------------

def _get_diagnostics(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# GET_ADMIN_TOKEN (0x74)
# client.py: length = resp[0]; token = resp[1:1+length].decode('ascii')
# ---------------------------------------------------------------------------

def _get_admin_token(device):
    def handler(payload: bytes) -> bytes:
        length = len(_ADMIN_TOKEN)
        return struct.pack('<B', length) + _ADMIN_TOKEN
    return handler


# ---------------------------------------------------------------------------
# DEVICE_RESET (0x70)
# ---------------------------------------------------------------------------

def _device_reset(device):
    def handler(payload: bytes) -> bytes:
        for ch in device.channels:
            ch["function"] = 0  # HIGH_IMP
            ch["dac_code"] = 0
            ch["dac_value"] = 0.0
            ch["channel_alert"] = 0
        device.alert_status = 0
        device.supply_alert_status = 0
        return b''
    return handler


# ---------------------------------------------------------------------------
# DISCONNECT (0xFF) — no-op
# ---------------------------------------------------------------------------

def _disconnect(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# SELFTEST_STATUS (0x05)
# client.py unpacks:
#   boot_ran (B), boot_passed (B),
#   vadj1 (f), vadj2 (f), vlogic (f),
#   cal_status (B), cal_ch (B), cal_pts (B), cal_err (f)
# ---------------------------------------------------------------------------

def _selftest_status(device):
    def handler(payload: bytes) -> bytes:
        return struct.pack('<BBfffBBBf',
                           1,     # boot_ran
                           1,     # boot_passed
                           12.0,  # vadj1_v
                           5.0,   # vadj2_v
                           3.3,   # vlogic_v
                           0,     # cal_status = idle
                           0,     # cal_ch
                           0,     # cal_pts
                           0.0)   # cal_err_mv
    return handler


# ---------------------------------------------------------------------------
# SELFTEST_MEASURE_SUPPLY (0x06)
# client.py: _, voltage = struct.unpack_from('<Bf', resp, 0)
# ---------------------------------------------------------------------------

def _selftest_measure_supply(device):
    _voltages = [12.0, 5.0, 3.3]
    def handler(payload: bytes) -> bytes:
        rail = payload[0] if payload else 0
        v = _voltages[rail] if rail < len(_voltages) else -1.0
        return struct.pack('<Bf', rail, v)
    return handler


# ---------------------------------------------------------------------------
# SELFTEST_EFUSE_CURRENTS (0x07)
# client.py: avail (B), ts (I), 4× current (f)
# ---------------------------------------------------------------------------

def _selftest_efuse_currents(device):
    def handler(payload: bytes) -> bytes:
        return struct.pack('<BIffff',
                           1,      # available
                           0,      # timestamp_ms
                           0.0, 0.0, 0.0, 0.0)
    return handler


# ---------------------------------------------------------------------------
# SELFTEST_AUTO_CAL (0x08)
# client.py: status (B), channel (B), points (B), error_mv (f at offset 3)
# ---------------------------------------------------------------------------

def _selftest_auto_cal(device):
    def handler(payload: bytes) -> bytes:
        ch = payload[0] if payload else 0
        return struct.pack('<BBBf', 2, ch, 10, 0.0)  # status=2=success
    return handler


# ---------------------------------------------------------------------------
# SELFTEST_INT_SUPPLIES (0x09)
# client.py: valid (B), ok (B), avdd (f), dvcc (f), avcc (f), avss (f), temp (f)
# ---------------------------------------------------------------------------

def _selftest_int_supplies(device):
    def handler(payload: bytes) -> bytes:
        return struct.pack('<BBfffff',
                           1,      # valid
                           1,      # supplies_ok
                           21.5,   # avdd_hi_v
                           5.0,    # dvcc_v
                           5.0,    # avcc_v
                           -16.0,  # avss_v
                           25.0)   # temp_c
    return handler


# ---------------------------------------------------------------------------
# SET_CHANNEL_FUNC (0x10) — stub; full impl in channels.py
# client.py sends struct.pack('<BB', channel, int(function))
# ---------------------------------------------------------------------------

def _set_channel_func(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, func = struct.unpack_from('<BB', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["function"] = func
        return b''
    return handler


# ---------------------------------------------------------------------------
# CLEAR_ALL_ALERTS (0x20)
# ---------------------------------------------------------------------------

def _clear_all_alerts(device):
    def handler(payload: bytes) -> bytes:
        device.alert_status = 0
        device.supply_alert_status = 0
        for ch in device.channels:
            ch["channel_alert"] = 0
        return b''
    return handler


# ---------------------------------------------------------------------------
# CLEAR_CHAN_ALERT (0x21)
# client.py sends struct.pack('<B', channel)
# ---------------------------------------------------------------------------

def _clear_chan_alert(device):
    def handler(payload: bytes) -> bytes:
        ch_idx = payload[0] if payload else 0
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["channel_alert"] = 0
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_ALERT_MASK (0x22)
# client.py sends struct.pack('<HH', alert_mask, supply_mask)
# ---------------------------------------------------------------------------

def _set_alert_mask(device):
    def handler(payload: bytes) -> bytes:
        alert_mask, supply_mask = struct.unpack_from('<HH', payload)
        device.alert_mask = alert_mask
        device.supply_alert_mask = supply_mask
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_CH_ALERT_MASK (0x23)
# client.py sends struct.pack('<BH', channel, mask)
# ---------------------------------------------------------------------------

def _set_ch_alert_mask(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, mask = struct.unpack_from('<BH', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["channel_alert_mask"] = mask
        return b''
    return handler
