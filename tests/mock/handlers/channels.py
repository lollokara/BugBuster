"""
Channel command handlers for SimulatedDevice.

Handles: SET_CHANNEL_FUNC, SET_DAC_CODE, SET_DAC_VOLTAGE, SET_DAC_CURRENT,
         SET_ADC_CONFIG, SET_DIN_CONFIG, SET_DO_CONFIG, SET_DO_STATE,
         SET_VOUT_RANGE, SET_CURRENT_LIMIT, SET_AVDD_SELECT,
         GET_ADC_VALUE, GET_DAC_READBACK, SET_RTD_CONFIG.
"""

import struct
from bugbuster.constants import CmdId


def register(device) -> None:
    # Override the stub registered in core.py
    device.register_handler(CmdId.SET_CHANNEL_FUNC,  _set_channel_func(device))
    device.register_handler(CmdId.SET_DAC_CODE,      _set_dac_code(device))
    device.register_handler(CmdId.SET_DAC_VOLTAGE,   _set_dac_voltage(device))
    device.register_handler(CmdId.SET_DAC_CURRENT,   _set_dac_current(device))
    device.register_handler(CmdId.SET_ADC_CONFIG,    _set_adc_config(device))
    device.register_handler(CmdId.SET_DIN_CONFIG,    _set_din_config(device))
    device.register_handler(CmdId.SET_DO_CONFIG,     _set_do_config(device))
    device.register_handler(CmdId.SET_DO_STATE,      _set_do_state(device))
    device.register_handler(CmdId.SET_VOUT_RANGE,    _set_vout_range(device))
    device.register_handler(CmdId.SET_CURRENT_LIMIT, _set_current_limit(device))
    device.register_handler(CmdId.SET_AVDD_SELECT,   _set_avdd_select(device))
    device.register_handler(CmdId.GET_ADC_VALUE,     _get_adc_value(device))
    device.register_handler(CmdId.GET_DAC_READBACK,  _get_dac_readback(device))
    device.register_handler(CmdId.SET_RTD_CONFIG,    _set_rtd_config(device))


# ---------------------------------------------------------------------------
# SET_CHANNEL_FUNC (0x10)
# client sends: struct.pack('<BB', channel, int(function))
# ---------------------------------------------------------------------------

def _set_channel_func(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, func = struct.unpack_from('<BB', payload)
        if 0 <= ch_idx < len(device.channels):
            ch = device.channels[ch_idx]
            ch["function"] = func
            # Resetting to HIGH_IMP (0) also resets DAC/ADC state
            if func == 0:
                ch["dac_code"] = 0
                ch["dac_value"] = 0.0
                ch["adc_raw"] = 0
                ch["adc_value"] = 0.0
                ch["adc_range"] = 0
                ch["adc_rate"] = 0
                ch["adc_mux"] = 0
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_DAC_CODE (0x11)
# client sends: struct.pack('<BH', channel, code & 0xFFFF)
# ---------------------------------------------------------------------------

def _set_dac_code(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, code = struct.unpack_from('<BH', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["dac_code"] = code
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_DAC_VOLTAGE (0x12)
# client sends: struct.pack('<BfB', channel, float(voltage), int(bipolar))
# ---------------------------------------------------------------------------

def _set_dac_voltage(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, voltage, bipolar = struct.unpack_from('<BfB', payload)
        if 0 <= ch_idx < len(device.channels):
            ch = device.channels[ch_idx]
            ch["dac_value"] = voltage
            # Simulate a non-zero DAC code for non-zero voltage
            if bipolar:
                # ±12 V range: mid-scale = 0x8000
                code = int((voltage / 24.0 + 0.5) * 65535) & 0xFFFF
            else:
                # 0–12 V range
                code = int((voltage / 12.0) * 65535) & 0xFFFF
            ch["dac_code"] = code
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_DAC_CURRENT (0x13)
# client sends: struct.pack('<Bf', channel, float(current_ma))
# ---------------------------------------------------------------------------

def _set_dac_current(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, current_ma = struct.unpack_from('<Bf', payload)
        if 0 <= ch_idx < len(device.channels):
            ch = device.channels[ch_idx]
            ch["dac_value"] = current_ma
            # Simulate DAC code: 0–25 mA maps to 0–0xFFFF
            code = int((current_ma / 25.0) * 65535) & 0xFFFF
            ch["dac_code"] = code
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_ADC_CONFIG (0x14)
# client sends: struct.pack('<BBBB', channel, int(mux), int(range_), int(rate))
# ---------------------------------------------------------------------------

def _set_adc_config(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, mux, rng, rate = struct.unpack_from('<BBBB', payload)
        if 0 <= ch_idx < len(device.channels):
            ch = device.channels[ch_idx]
            ch["adc_mux"] = mux
            ch["adc_range"] = rng
            ch["adc_rate"] = rate
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_DIN_CONFIG (0x15)
# client sends: struct.pack('<BBBBBBBB', channel, threshold, thresh_mode,
#                           debounce, sink, sink_range, oc_detect, sc_detect)
# ---------------------------------------------------------------------------

def _set_din_config(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, threshold, thresh_mode, debounce, sink, sink_range, oc_det, sc_det = \
            struct.unpack_from('<BBBBBBBB', payload)
        if 0 <= ch_idx < len(device.channels):
            ch = device.channels[ch_idx]
            ch["din_threshold"] = threshold
            ch["din_thresh_mode"] = thresh_mode
            ch["din_debounce"] = debounce
            ch["din_sink"] = sink
            ch["din_sink_range"] = sink_range
            ch["din_oc_detect"] = oc_det
            ch["din_sc_detect"] = sc_det
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_DO_CONFIG (0x16)
# payload: byte 0=ch, byte 1=do_mode
# ---------------------------------------------------------------------------

def _set_do_config(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, do_mode = struct.unpack_from('<BB', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["do_mode"] = do_mode
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_DO_STATE (0x17)
# client sends: struct.pack('<BB', channel, int(on))
# ---------------------------------------------------------------------------

def _set_do_state(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, state = struct.unpack_from('<BB', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["do_state"] = bool(state)
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_VOUT_RANGE (0x18)
# client sends: struct.pack('<BB', channel, int(bipolar))
# ---------------------------------------------------------------------------

def _set_vout_range(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, bipolar = struct.unpack_from('<BB', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["vout_bipolar"] = bool(bipolar)
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_CURRENT_LIMIT (0x19)
# client sends: struct.pack('<BB', channel, int(limit))
# ---------------------------------------------------------------------------

def _set_current_limit(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, limit = struct.unpack_from('<BB', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["current_limit"] = limit
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_AVDD_SELECT (0x1A)
# payload: byte 0=ch, byte 1=avdd_select
# ---------------------------------------------------------------------------

def _set_avdd_select(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, avdd_sel = struct.unpack_from('<BB', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["avdd_select"] = avdd_sel
        return b''
    return handler


# ---------------------------------------------------------------------------
# GET_ADC_VALUE (0x1B)
# client sends: struct.pack('<B', channel)
# client unpacks:
#   resp[0]   = ch (B)
#   resp[1:4] = adc_raw (3-byte LE int)
#   resp[4:8] = adc_value (float LE)
#   resp[8]   = range (B)
#   resp[9]   = rate (B)
#   resp[10]  = mux (B)
# ---------------------------------------------------------------------------

def _get_adc_value(device):
    def handler(payload: bytes) -> bytes:
        ch_idx = payload[0] if payload else 0
        if 0 <= ch_idx < len(device.channels):
            ch = device.channels[ch_idx]
            adc_raw = ch["adc_raw"] & 0xFFFFFF
            adc_value = ch["adc_value"]
            adc_range = ch["adc_range"]
            adc_rate = ch["adc_rate"]
            adc_mux = ch["adc_mux"]
        else:
            adc_raw = 0
            adc_value = 0.0
            adc_range = 0
            adc_rate = 0
            adc_mux = 0

        buf = bytearray()
        buf += struct.pack('<B', ch_idx)
        buf += adc_raw.to_bytes(3, 'little')
        buf += struct.pack('<f', adc_value)
        buf += struct.pack('<BBB', adc_range, adc_rate, adc_mux)
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# GET_DAC_READBACK (0x1C)
# client sends: struct.pack('<B', channel)
# client unpacks: struct.unpack_from('<H', resp, 1)[0]
#   resp[0]   = ch (B)
#   resp[1:3] = dac_code (uint16 LE)
# ---------------------------------------------------------------------------

def _get_dac_readback(device):
    def handler(payload: bytes) -> bytes:
        ch_idx = payload[0] if payload else 0
        if 0 <= ch_idx < len(device.channels):
            code = device.channels[ch_idx]["dac_code"]
        else:
            code = 0
        return struct.pack('<BH', ch_idx, code & 0xFFFF)
    return handler


# ---------------------------------------------------------------------------
# SET_RTD_CONFIG (0x1D)
# client sends: struct.pack('<BB', channel, int(current))
# ---------------------------------------------------------------------------

def _set_rtd_config(device):
    def handler(payload: bytes) -> bytes:
        ch_idx, current = struct.unpack_from('<BB', payload)
        if 0 <= ch_idx < len(device.channels):
            device.channels[ch_idx]["rtd_excitation_ua"] = current
        return b''
    return handler
