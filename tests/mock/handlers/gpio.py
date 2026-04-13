"""
GPIO + DIO handlers for SimulatedDevice.

Commands handled:
  GET_GPIO_STATUS (0x40) — read all 6 GPIO pins
  SET_GPIO_CONFIG (0x41) — configure pin mode / pulldown
  SET_GPIO_VALUE  (0x42) — set output value
  DIO_GET_ALL     (0x43) — read all 12 digital IOs
  DIO_CONFIG      (0x44) — configure a DIO
  DIO_WRITE       (0x45) — write a DIO output
  DIO_READ        (0x46) — read a single DIO
"""

import struct
from bugbuster.constants import CmdId


def register(device) -> None:
    # Ensure gpio entries have all required fields (backwards compat with old "value" key)
    for i, g in enumerate(device.gpio):
        g.setdefault("id", i)
        g.setdefault("input", False)
        g.setdefault("pulldown", False)
        if "output" not in g and "value" in g:
            g["output"] = g.pop("value")
        else:
            g.setdefault("output", False)

    # Ensure dio state exists
    if not hasattr(device, "dio"):
        device.dio = [{"mode": 0, "output": False, "input": False} for _ in range(12)]

    device.register_handler(CmdId.GET_GPIO_STATUS, _get_gpio_status(device))
    device.register_handler(CmdId.SET_GPIO_CONFIG,  _set_gpio_config(device))
    device.register_handler(CmdId.SET_GPIO_VALUE,   _set_gpio_value(device))
    device.register_handler(CmdId.DIO_GET_ALL,       _dio_get_all(device))
    device.register_handler(CmdId.DIO_CONFIG,        _dio_config(device))
    device.register_handler(CmdId.DIO_WRITE,         _dio_write(device))
    device.register_handler(CmdId.DIO_READ,          _dio_read(device))


# ---------------------------------------------------------------------------
# GET_GPIO_STATUS (0x40)
# client.py: 6 pins × 5 bytes each: struct.unpack_from('<BBBBB', resp, off)
#   → (gid, mode, out_, in_, pd)
# ---------------------------------------------------------------------------

def _get_gpio_status(device):
    def handler(payload: bytes) -> bytes:
        buf = bytearray()
        for i, g in enumerate(device.gpio):
            buf += struct.pack('<BBBBB',
                               g.get("id", i),
                               g.get("mode", 0),
                               int(g.get("output", False)),
                               int(g.get("input", False)),
                               int(g.get("pulldown", False)))
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# SET_GPIO_CONFIG (0x41)
# client.py sends: struct.pack('<BBB', gpio, int(mode), int(pulldown))
# ---------------------------------------------------------------------------

def _set_gpio_config(device):
    def handler(payload: bytes) -> bytes:
        pin_id, mode, pulldown = struct.unpack_from('<BBB', payload)
        if 0 <= pin_id < len(device.gpio):
            device.gpio[pin_id]["mode"] = mode
            device.gpio[pin_id]["pulldown"] = bool(pulldown)
        return b''
    return handler


# ---------------------------------------------------------------------------
# SET_GPIO_VALUE (0x42)
# client.py sends: struct.pack('<BB', gpio, int(value))
# ---------------------------------------------------------------------------

def _set_gpio_value(device):
    def handler(payload: bytes) -> bytes:
        pin_id, value = struct.unpack_from('<BB', payload)
        if 0 <= pin_id < len(device.gpio):
            device.gpio[pin_id]["output"] = bool(value)
        return b''
    return handler


# ---------------------------------------------------------------------------
# DIO_GET_ALL (0x43)
# client.py: resp[0] = count; then for each: (io_num B, gpio b, mode B, output B, input B)
#   gpio is signed byte ('b') — use 0 as placeholder
# ---------------------------------------------------------------------------

def _dio_get_all(device):
    def handler(payload: bytes) -> bytes:
        buf = bytearray()
        count = len(device.dio)
        buf += struct.pack('<B', count)
        for i, d in enumerate(device.dio):
            io_num = i + 1  # logical IOs are 1-indexed
            gpio_pin = 0    # ESP32 GPIO number placeholder
            mode   = d.get("mode", 0)
            output = int(d.get("output", False))
            inp    = int(d.get("input", False))
            buf += struct.pack('<BbBBB', io_num, gpio_pin, mode, output, inp)
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# DIO_CONFIG (0x44)
# client.py sends: struct.pack('<BB', io, mode)
# ---------------------------------------------------------------------------

def _dio_config(device):
    def handler(payload: bytes) -> bytes:
        io_num, mode = struct.unpack_from('<BB', payload)
        idx = io_num - 1  # convert 1-indexed to 0-indexed
        if 0 <= idx < len(device.dio):
            device.dio[idx]["mode"] = mode
        return b''
    return handler


# ---------------------------------------------------------------------------
# DIO_WRITE (0x45)
# client.py sends: struct.pack('<BB', io, int(value))
# ---------------------------------------------------------------------------

def _dio_write(device):
    def handler(payload: bytes) -> bytes:
        io_num, value = struct.unpack_from('<BB', payload)
        idx = io_num - 1
        if 0 <= idx < len(device.dio):
            device.dio[idx]["output"] = bool(value)
        return b''
    return handler


# ---------------------------------------------------------------------------
# DIO_READ (0x46)
# client.py sends: struct.pack('<B', io)
# client.py parses: resp[0]=io, resp[1]=mode, resp[2]=value (bool)
# ---------------------------------------------------------------------------

def _dio_read(device):
    def handler(payload: bytes) -> bytes:
        io_num = payload[0]
        idx = io_num - 1
        if 0 <= idx < len(device.dio):
            d = device.dio[idx]
            mode  = d.get("mode", 0)
            value = int(d.get("output", False))
        else:
            mode  = 0
            value = 0
        return struct.pack('<BBB', io_num, mode, value)
    return handler
