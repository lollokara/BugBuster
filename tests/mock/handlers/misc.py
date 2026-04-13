"""
Miscellaneous handlers for SimulatedDevice.

Handles: REGISTER_READ, REGISTER_WRITE, SET_WATCHDOG, SET_LSHIFT_OE,
         SET_SPI_CLOCK, USBPD_GET_STATUS, USBPD_SELECT_PDO, USBPD_GO,
         WIFI_GET_STATUS, WIFI_CONNECT, WIFI_SCAN, START_WAVEGEN, STOP_WAVEGEN.
"""

import struct
from bugbuster.constants import CmdId


def register(device) -> None:
    device.register_handler(CmdId.REGISTER_READ,    _register_read(device))
    device.register_handler(CmdId.REGISTER_WRITE,   _register_write(device))
    device.register_handler(CmdId.SET_WATCHDOG,     _set_watchdog(device))
    device.register_handler(CmdId.SET_LSHIFT_OE,    _set_lshift_oe(device))
    device.register_handler(CmdId.SET_SPI_CLOCK,    _set_spi_clock(device))
    device.register_handler(CmdId.USBPD_GET_STATUS, _usbpd_get_status(device))
    device.register_handler(CmdId.USBPD_SELECT_PDO, _usbpd_select_pdo(device))
    device.register_handler(CmdId.USBPD_GO,         _usbpd_go(device))
    device.register_handler(CmdId.WIFI_GET_STATUS,  _wifi_get_status(device))
    device.register_handler(CmdId.WIFI_CONNECT,     _wifi_connect(device))
    device.register_handler(CmdId.WIFI_SCAN,        _wifi_scan(device))
    device.register_handler(CmdId.START_WAVEGEN,    _start_wavegen(device))
    device.register_handler(CmdId.STOP_WAVEGEN,     _stop_wavegen(device))


# ---------------------------------------------------------------------------
# REGISTER_READ (0x71)
# client sends: struct.pack('<B', address)
# client reads: struct.unpack_from('<H', resp, 1)[0]  — resp[0] is echo of address
# ---------------------------------------------------------------------------

def _register_read(device):
    def handler(payload: bytes) -> bytes:
        address = payload[0] if payload else 0
        return struct.pack('<BH', address, 0x0000)
    return handler


# ---------------------------------------------------------------------------
# REGISTER_WRITE (0x72) — no-op
# client sends: struct.pack('<BH', address, value)
# ---------------------------------------------------------------------------

def _register_write(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_WATCHDOG (0x73)
# client sends: struct.pack('<BB', int(enable), timeout_code)
# ---------------------------------------------------------------------------

def _set_watchdog(device):
    def handler(payload: bytes) -> bytes:
        enable, timeout_code = struct.unpack_from('<BB', payload)
        device.watchdog_enable = bool(enable)
        device.watchdog_timeout_code = timeout_code
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_LSHIFT_OE (0xE0) — no-op
# client sends: struct.pack('<B', int(on))
# ---------------------------------------------------------------------------

def _set_lshift_oe(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_SPI_CLOCK (0xE3)
# client sends: struct.pack('<I', clock_hz)
# ---------------------------------------------------------------------------

def _set_spi_clock(device):
    def handler(payload: bytes) -> bytes:
        clock_hz, = struct.unpack_from('<I', payload)
        device.spi_clock_hz = clock_hz
        return b''
    return handler


# ---------------------------------------------------------------------------
# USBPD_GET_STATUS (0xC0)
# client: _parse_usbpd_status(resp)
#   present, attached, cc_dir, pd_resp, v_code, i_code = struct.unpack_from('<BBBBBB', resp)
#   voltage_v, current_a, power_w = struct.unpack_from('<fff', resp, 6)
#   pdos: 6 × (detected B, max_i B) starting at offset 18
# Return canned "5V/3A" status.
# ---------------------------------------------------------------------------

def _usbpd_get_status(device):
    def handler(payload: bytes) -> bytes:
        pdo_code = getattr(device, 'usbpd_voltage', 1)  # 1 = 5V
        buf = bytearray()
        # present, attached, cc_dir, pd_resp, v_code, i_code
        buf += struct.pack('<BBBBBB', 1, 1, 0, 3, pdo_code, 3)
        # voltage_v=5.0, current_a=3.0, power_w=15.0
        buf += struct.pack('<fff', 5.0, 3.0, 15.0)
        # 6 PDOs: first one detected (5V/3A), rest not detected
        for i in range(6):
            detected = 1 if i == 0 else 0
            max_i = 3 if i == 0 else 0
            buf += struct.pack('<BB', detected, max_i)
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# USBPD_SELECT_PDO (0xC1)
# client sends: struct.pack('<B', code)
# ---------------------------------------------------------------------------

def _usbpd_select_pdo(device):
    def handler(payload: bytes) -> bytes:
        if payload:
            device.usbpd_voltage = payload[0]
        return b''
    return handler


# ---------------------------------------------------------------------------
# USBPD_GO (0xC2) — no-op
# ---------------------------------------------------------------------------

def _usbpd_go(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# WIFI_GET_STATUS (0xE1)
# client: _parse_wifi_status(resp)
#   resp[0]       connected (B)
#   then length-prefixed strings: ssid, ip, rssi(i32), ap_ssid, ap_ip, ap_mac
# Return disconnected status.
# ---------------------------------------------------------------------------

def _wifi_get_status(device):
    def handler(payload: bytes) -> bytes:
        connected = getattr(device, 'wifi_connected', False)
        buf = bytearray()
        buf.append(int(connected))

        def write_str(s: str) -> bytes:
            b = s.encode()
            return bytes([len(b)]) + b

        buf += write_str('')       # sta_ssid
        buf += write_str('')       # sta_ip
        buf += struct.pack('<i', 0)  # rssi
        buf += write_str('')       # ap_ssid
        buf += write_str('')       # ap_ip
        buf += write_str('')       # ap_mac
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# WIFI_CONNECT (0xE2) — no-op, return False
# client reads: bool(resp[0])
# ---------------------------------------------------------------------------

def _wifi_connect(device):
    def handler(payload: bytes) -> bytes:
        return struct.pack('<B', 0)   # connected = False
    return handler


# ---------------------------------------------------------------------------
# WIFI_SCAN (0xE4)
# client: _parse_wifi_scan(resp)
#   count (B), then count × (n B, ssid nB, rssi b, auth B)
# Return empty list.
# ---------------------------------------------------------------------------

def _wifi_scan(device):
    def handler(payload: bytes) -> bytes:
        return struct.pack('<B', 0)   # count = 0
    return handler


# ---------------------------------------------------------------------------
# START_WAVEGEN (0xD0)
# client sends: struct.pack('<BBfffB', channel, int(waveform),
#                            float(freq_hz), float(amplitude), float(offset), int(mode))
# ---------------------------------------------------------------------------

def _start_wavegen(device):
    def handler(payload: bytes) -> bytes:
        channel, waveform = struct.unpack_from('<BB', payload)
        freq_hz, amplitude, offset = struct.unpack_from('<fff', payload, 2)
        mode, = struct.unpack_from('<B', payload, 14)
        device.wavegen_running = True
        device.wavegen_config = {
            'channel': channel,
            'waveform': waveform,
            'freq_hz': freq_hz,
            'amplitude': amplitude,
            'offset': offset,
            'mode': mode,
        }
        return b''
    return handler


# ---------------------------------------------------------------------------
# STOP_WAVEGEN (0xD1)
# ---------------------------------------------------------------------------

def _stop_wavegen(device):
    def handler(payload: bytes) -> bytes:
        device.wavegen_running = False
        device.wavegen_config = None
        return b''
    return handler
