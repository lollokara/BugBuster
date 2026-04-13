"""
UART bridge handlers for SimulatedDevice.

Handles: GET_UART_CONFIG, SET_UART_CONFIG, GET_UART_PINS.
"""

import struct
from bugbuster.constants import CmdId


def register(device) -> None:
    device.register_handler(CmdId.GET_UART_CONFIG, _get_uart_config(device))
    device.register_handler(CmdId.SET_UART_CONFIG, _set_uart_config(device))
    device.register_handler(CmdId.GET_UART_PINS,   _get_uart_pins(device))


# ---------------------------------------------------------------------------
# GET_UART_CONFIG (0x50)
# client response format:
#   resp[0]       count (B)
#   per bridge (12 bytes):
#     off+0       bridge_id (B)
#     off+1       uart_num  (B)
#     off+2       tx_pin    (B)
#     off+3       rx_pin    (B)
#     off+4:8     baudrate  (I LE)
#     off+8       data_bits (B)
#     off+9       parity    (B)
#     off+10      stop_bits (B)
#     off+11      enabled   (B)
# ---------------------------------------------------------------------------

def _get_uart_config(device):
    def handler(payload: bytes) -> bytes:
        cfgs = device.uart_config if isinstance(device.uart_config, list) else [device.uart_config]
        buf = bytearray()
        buf.append(len(cfgs))
        for i, cfg in enumerate(cfgs):
            buf += struct.pack(
                '<BBBBIBBBB',
                i,
                cfg.get('uart_num', 1),
                cfg.get('tx_pin', 17),
                cfg.get('rx_pin', 18),
                cfg.get('baud', cfg.get('baudrate', 115200)),
                cfg.get('data_bits', 8),
                cfg.get('parity', 0),
                cfg.get('stop_bits', 0),
                int(cfg.get('enabled', False)),
            )
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# SET_UART_CONFIG (0x51)
# client sends: struct.pack('<BBBBIBBBB', bridge_id, uart_num, tx_pin, rx_pin,
#                            baudrate, data_bits, parity, stop_bits, int(enabled))
# ---------------------------------------------------------------------------

def _set_uart_config(device):
    def handler(payload: bytes) -> bytes:
        bridge_id, uart_num, tx_pin, rx_pin = struct.unpack_from('<BBBB', payload)
        baudrate, = struct.unpack_from('<I', payload, 4)
        data_bits, parity, stop_bits, enabled = struct.unpack_from('<BBBB', payload, 8)
        cfg = {
            'uart_num': uart_num,
            'tx_pin': tx_pin,
            'rx_pin': rx_pin,
            'baudrate': baudrate,
            'data_bits': data_bits,
            'parity': parity,
            'stop_bits': stop_bits,
            'enabled': bool(enabled),
        }
        if isinstance(device.uart_config, list):
            while len(device.uart_config) <= bridge_id:
                device.uart_config.append({})
            device.uart_config[bridge_id] = cfg
        else:
            device.uart_config = cfg
        return b''
    return handler


# ---------------------------------------------------------------------------
# GET_UART_PINS (0x52)
# client: count (B), then count bytes of pin numbers
# Return a small set of available pins.
# ---------------------------------------------------------------------------

def _get_uart_pins(device):
    _AVAILABLE_PINS = [17, 18, 19, 20, 21, 22]

    def handler(payload: bytes) -> bytes:
        pins = _AVAILABLE_PINS
        return bytes([len(pins)] + pins)
    return handler
