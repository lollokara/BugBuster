"""
HAT expansion board handlers for SimulatedDevice.

Handles: HAT_DETECT, HAT_GET_STATUS, HAT_SET_PIN, HAT_SET_ALL_PINS,
         HAT_RESET, HAT_SET_POWER, HAT_GET_POWER, HAT_SET_IO_VOLT,
         HAT_SETUP_SWD, HAT_LA_*.
"""

import struct
from bugbuster.constants import CmdId, ErrorCode
from bugbuster.transport.usb import DeviceError


def _noop_ok(payload: bytes) -> bytes:
    """Handler that acknowledges the command with an empty response."""
    return b""


def _ensure_hat_state(device) -> None:
    """Add HAT-related state attributes to device if not already present."""
    if not hasattr(device, "hat_pins"):
        device.hat_pins = [0] * 8
    if not hasattr(device, "la_state"):
        device.la_state = "IDLE"
    if not hasattr(device, "la_config"):
        device.la_config = {
            "n_channels": 4,
            "sample_rate": 1000,
            "trigger_type": 0,
            "trigger_channel": 0,
        }
    if not hasattr(device, "hat_power"):
        device.hat_power = [False, False]
    if not hasattr(device, "hat_io_volt"):
        device.hat_io_volt = 3300
    if not hasattr(device, "hat_la_route"):
        device.hat_la_route = 0
    if not hasattr(device, "hat_led_states"):
        device.hat_led_states = [[0, 0, 0, 0] for _ in range(8)]
    if not hasattr(device, "hat_rails"):
        device.hat_rails = [
            {"rail_id": 0, "enabled": False, "voltage_mv": 0, "current_ma": 0, "status": 0, "target_mv": 3300},
            {"rail_id": 1, "enabled": False, "voltage_mv": 0, "current_ma": 0, "status": 0, "target_mv": 12000},
            {"rail_id": 2, "enabled": False, "voltage_mv": 0, "current_ma": 0, "status": 0, "target_mv": 12000},
        ]
    if not hasattr(device, "hat_cal_state"):
        device.hat_cal_state = 0
    if not hasattr(device, "hat_cal_progress"):
        device.hat_cal_progress = 0
    if not hasattr(device, "hat_cal_rail_id"):
        device.hat_cal_rail_id = 0
    if not hasattr(device, "hat_cal_last_error"):
        device.hat_cal_last_error = 0
    if not hasattr(device, "hat_cal_persist_state"):
        device.hat_cal_persist_state = 0
    if not hasattr(device, "hat_cal_stage"):
        device.hat_cal_stage = 5
    if not hasattr(device, "hat_cal_point"):
        device.hat_cal_point = 128
    if not hasattr(device, "hat_cal_code"):
        device.hat_cal_code = 0
    if not hasattr(device, "hat_cal_measured_mv"):
        device.hat_cal_measured_mv = 3300
    if not hasattr(device, "hat_cal_min_mv"):
        device.hat_cal_min_mv = 0
    if not hasattr(device, "hat_cal_max_mv"):
        device.hat_cal_max_mv = 36000
    if not hasattr(device, "hat_cal_max_gap_mv"):
        device.hat_cal_max_gap_mv = 500
    if not hasattr(device, "hat_cal_max_error_mv"):
        device.hat_cal_max_error_mv = 0
    if not hasattr(device, "hat_cal_validation_flags"):
        device.hat_cal_validation_flags = 0
    if not hasattr(device, "hat_io_bank"):
        device.hat_io_bank = {"dirs": 0, "ups": 0, "dns": 0}
    if not hasattr(device, "hat_level_shift"):
        device.hat_level_shift = {"oe": False, "dir": False}


def register(device) -> None:
    _ensure_hat_state(device)

    # Core HAT handlers
    device.register_handler(CmdId.HAT_DETECT,       _hat_detect(device))
    device.register_handler(CmdId.HAT_GET_STATUS,   _hat_get_status(device))
    device.register_handler(CmdId.HAT_SET_PIN,      _hat_set_pin(device))
    device.register_handler(CmdId.HAT_SET_ALL_PINS, _hat_set_all_pins(device))
    device.register_handler(CmdId.HAT_RESET,        _hat_reset(device))
    device.register_handler(CmdId.HAT_SET_POWER,    _hat_set_power(device))
    device.register_handler(CmdId.HAT_GET_POWER,    _hat_get_power(device))
    device.register_handler(CmdId.HAT_SET_IO_VOLT,  _hat_set_io_volt(device))
    device.register_handler(CmdId.HAT_SETUP_SWD,    _hat_setup_swd(device))
    device.register_handler(CmdId.HAT_DETECT_TARGET, _hat_detect_target(device))

    # LA handlers
    device.register_handler(CmdId.HAT_LA_CONFIG,  _hat_la_config(device))
    device.register_handler(CmdId.HAT_LA_ARM,     _hat_la_arm(device))
    device.register_handler(CmdId.HAT_LA_FORCE,   _hat_la_force(device))
    device.register_handler(CmdId.HAT_LA_STATUS,  _hat_la_status(device))
    device.register_handler(CmdId.HAT_LA_READ,    _hat_la_read(device))
    device.register_handler(CmdId.HAT_LA_STOP,    _hat_la_stop(device))
    device.register_handler(CmdId.HAT_LA_TRIGGER, _hat_la_trigger(device))
    # LA streaming / log-relay handlers (BBP v4 additions).  The simulator
    # acknowledges these so the client surface is exercised end-to-end, but
    # does not model USB-bulk streaming or RP2040 log fan-out.
    device.register_handler(CmdId.HAT_LA_LOG_ENABLE,   _noop_ok)
    device.register_handler(CmdId.HAT_LA_USB_RESET,    _noop_ok)
    device.register_handler(CmdId.HAT_LA_STREAM_START, _noop_ok)

    # HAT v2 command handlers
    device.register_handler(CmdId.HAT_GET_CAPS,        _hat_get_caps(device))
    device.register_handler(CmdId.HAT_GET_RAIL_STATUS, _hat_get_rail_status(device))
    device.register_handler(CmdId.HAT_SET_RAIL_ENABLE, _hat_set_rail_enable(device))
    device.register_handler(CmdId.HAT_SET_RAIL_VOLTAGE, _hat_set_rail_voltage(device))
    device.register_handler(CmdId.HAT_SET_LED_STATE,   _hat_set_led_state(device))
    device.register_handler(CmdId.HAT_LA_SET_ROUTE,    _hat_la_set_route(device))
    device.register_handler(CmdId.HAT_CALIBRATE_START,  _hat_calibrate_start(device))
    device.register_handler(CmdId.HAT_CALIBRATE_STATUS, _hat_calibrate_status(device))
    device.register_handler(CmdId.HAT_CALIBRATE_IMPORT, _hat_calibrate_import(device))
    device.register_handler(CmdId.HAT_SET_IO_BANK,      _hat_set_io_bank(device))
    device.register_handler(CmdId.HAT_SET_LEVEL_SHIFT,  _hat_set_level_shift(device))


# ---------------------------------------------------------------------------
# HAT_DETECT (0xC9)
# client.py: detected(B), hat_type(B), detect_v(f), connected(B)
# ---------------------------------------------------------------------------

def _hat_detect(device):
    def handler(payload: bytes) -> bytes:
        detected = device.hat.present
        hat_type = 1 if detected else 0
        detect_v = 3.3 if detected else 0.0
        connected = detected
        return struct.pack('<BBfB', int(detected), hat_type, detect_v, int(connected))
    return handler


# ---------------------------------------------------------------------------
# HAT_GET_STATUS (0xC5)
# client.py: detected(B), connected(B), hat_type(B), detect_v(f),
#            fw_major(B), fw_minor(B), confirmed(B), 4x pins(B),
#            [2x connectors: enabled(B), current(f), fault(B)], io_mv(H),
#            [dap_connected(B), target_detected(B), target_dpidr(I)]
# ---------------------------------------------------------------------------

def _hat_get_status(device):
    def handler(payload: bytes) -> bytes:
        detected = device.hat.present
        connected = detected
        hat_type = 1 if detected else 0
        detect_v = 3.3 if detected else 0.0
        fw_major = 1
        fw_minor = 0
        confirmed = detected

        buf = bytearray()
        buf += struct.pack('<BBBfBBB',
                           int(detected), int(connected), hat_type, detect_v,
                           fw_major, fw_minor, int(confirmed))
        # 4 pins
        pins = device.hat_pins[:4] if hasattr(device, 'hat_pins') else [0, 0, 0, 0]
        buf += struct.pack('<4B', *pins)
        # 2 connectors
        for i in range(2):
            buf += struct.pack('<BfB', int(device.hat_power[i]), 0.0, 0)
        # io_voltage_mv
        buf += struct.pack('<H', getattr(device, 'hat_io_volt', 3300))
        # DAP/SWD fields
        buf += struct.pack('<BBI', 0, 0, 0)
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# HAT_SET_PIN (0xC6)
# payload: pin_id(B), function_code(B)
# Raises INVALID_PARAM if function_code in {1,2,3,4}
# ---------------------------------------------------------------------------

def _hat_set_pin(device):
    from bugbuster.constants import HAT_FUNC_RESERVED_CODES
    def handler(payload: bytes) -> bytes:
        pin_id, func_code = struct.unpack_from('<BB', payload)
        if func_code in HAT_FUNC_RESERVED_CODES:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        if 0 <= pin_id < len(device.hat_pins):
            device.hat_pins[pin_id] = func_code
        return b''
    return handler


# ---------------------------------------------------------------------------
# HAT_SET_ALL_PINS (0xC7)
# payload: 4B pin functions
# ---------------------------------------------------------------------------

def _hat_set_all_pins(device):
    def handler(payload: bytes) -> bytes:
        pins = struct.unpack_from('<4B', payload)
        for i, p in enumerate(pins):
            if i < len(device.hat_pins):
                device.hat_pins[i] = p
        return b''
    return handler


# ---------------------------------------------------------------------------
# HAT_RESET (0xC8)
# ---------------------------------------------------------------------------

def _hat_reset(device):
    def handler(payload: bytes) -> bytes:
        device.la_state = "IDLE"
        device.hat_pins = [0] * 8
        return b''
    return handler


# ---------------------------------------------------------------------------
# HAT_SET_POWER (0xCA)
# payload: connector(B), enable(B)
# ---------------------------------------------------------------------------

def _hat_set_power(device):
    def handler(payload: bytes) -> bytes:
        connector, enable = struct.unpack_from('<BB', payload)
        if 0 <= connector < 2:
            device.hat_power[connector] = bool(enable)
        return b''
    return handler


# ---------------------------------------------------------------------------
# HAT_GET_POWER (0xCB)
# client.py: 2x [enabled(B), current(f), fault(B)], io_mv(H)
# ---------------------------------------------------------------------------

def _hat_get_power(device):
    def handler(payload: bytes) -> bytes:
        buf = bytearray()
        for i in range(2):
            buf += struct.pack('<BfB', int(device.hat_power[i]), 0.0, 0)
        buf += struct.pack('<H', getattr(device, 'hat_io_volt', 3300))
        return bytes(buf)
    return handler


# ---------------------------------------------------------------------------
# HAT_SET_IO_VOLT (0xCC)
# payload: voltage_mv(H)
# ---------------------------------------------------------------------------

def _hat_set_io_volt(device):
    def handler(payload: bytes) -> bytes:
        voltage_mv, = struct.unpack_from('<H', payload)
        device.hat_io_volt = voltage_mv
        return b''
    return handler


# ---------------------------------------------------------------------------
# HAT_SETUP_SWD (0xCD)
# ---------------------------------------------------------------------------

def _hat_setup_swd(device):
    def handler(payload: bytes) -> bytes:
        return b''
    return handler


# ---------------------------------------------------------------------------
# HAT_DETECT_TARGET (0xDB)
# resp: detected(B), dpidr(I). Simulated: a target is "present" once VADJ4 and
# the level-shifter OE are enabled (mirrors a real SWD bring-up).
# ---------------------------------------------------------------------------

def _hat_detect_target(device):
    def handler(payload: bytes) -> bytes:
        oe = device.hat_level_shift.get('oe', False)
        vadj4_on = device.hat_rails[2]['enabled'] if len(device.hat_rails) > 2 else False
        detected = bool(oe and vadj4_on)
        dpidr = 0x2BA01477 if detected else 0
        return struct.pack('<BI', int(detected), dpidr)
    return handler


# ---------------------------------------------------------------------------
# HAT LA handlers
# ---------------------------------------------------------------------------

def _hat_la_config(device):
    def handler(payload: bytes) -> bytes:
        # payload: channels(B), rate_hz(I), depth(I)
        channels, rate_hz, depth = struct.unpack_from('<BII', payload)
        device.la_config["n_channels"] = channels
        device.la_config["sample_rate"] = rate_hz
        device.la_config["depth"] = depth
        return b''
    return handler


def _hat_la_arm(device):
    def handler(payload: bytes) -> bytes:
        # Correct transition: IDLE → ARMED (hardware waits for trigger)
        device.la_state = "ARMED"
        return b''
    return handler


def _hat_la_force(device):
    def handler(payload: bytes) -> bytes:
        # Force-trigger: immediately completes capture → DONE
        device.la_state = "DONE"
        return b''
    return handler


def _hat_la_stop(device):
    def handler(payload: bytes) -> bytes:
        device.la_state = "IDLE"
        return b''
    return handler


def _hat_la_trigger(device):
    def handler(payload: bytes) -> bytes:
        trigger_type, channel = struct.unpack_from('<BB', payload)
        device.la_config["trigger_type"] = trigger_type
        device.la_config["trigger_channel"] = channel
        return b''
    return handler


def _hat_la_status(device):
    # state encoding: 0=idle, 1=armed, 2=capturing, 3=done, 4=streaming, 5=error
    _STATE_MAP = {"IDLE": 0, "ARMED": 1, "CAPTURING": 2, "DONE": 3}

    def handler(payload: bytes) -> bytes:
        state_name = getattr(device, 'la_state', 'IDLE')
        state = _STATE_MAP.get(state_name, 0)
        channels = device.la_config.get("n_channels", 4)
        captured = device.la_config.get("depth", 100) if state == 3 else 0
        total = device.la_config.get("depth", 100)
        rate = device.la_config.get("sample_rate", 1000000)

        buf = bytearray()
        buf += struct.pack('<BBIII', state, channels, captured, total, rate)
        # usb_connected, usb_mounted
        buf += struct.pack('<BB', 1, 1)
        # stop_reason
        buf += struct.pack('<B', 0)
        # stream_overrun_count, stream_short_write_count
        buf += struct.pack('<II', 0, 0)
        # usb_rearm_pending, request_count, complete_count
        buf += struct.pack('<BBB', 0, 0, 0)
        return bytes(buf)
    return handler


def _hat_la_read(device):
    def handler(payload: bytes) -> bytes:
        if getattr(device, 'la_state', 'IDLE') != "DONE":
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)

        from tests.mock.la_integration import generate_la_oneshot
        n_channels = device.la_config.get("n_channels", 4)
        n_samples = device.la_config.get("depth", 100)
        oneshot = generate_la_oneshot(n_channels=n_channels, n_samples=n_samples)

        # oneshot = [4-byte LE raw_len][raw bytes]
        raw_len = struct.unpack_from('<I', oneshot)[0]
        raw = oneshot[4:]

        # Client sends struct.pack('<IH', offset, req_len)
        # Response: [offset:u32][actual_len:u8][data...]
        req_offset = 0
        req_len = raw_len
        if len(payload) >= 6:
            req_offset, req_len = struct.unpack_from('<IH', payload)

        chunk = raw[req_offset:req_offset + req_len]
        actual = len(chunk)
        return struct.pack('<IB', req_offset, actual) + chunk
    return handler


def _hat_get_caps(device):
    def handler(payload: bytes) -> bytes:
        flags = 0x17  # RAILS|LEDS|LA_LOW_SPEED|SHIFTED_IO
        return struct.pack('<BIBBBBBb',
            2,       # hw_revision
            flags,   # capability flags
            3,       # rail_count
            8,       # led_count
            8,       # shifted_io_count
            2,       # la_route_count
            2,       # fw_major
            1,       # fw_minor
        )
    return handler


def _hat_get_rail_status(device):
    def handler(payload: bytes) -> bytes:
        rails = device.hat_rails
        result = struct.pack('B', len(rails))
        for i, rail in enumerate(rails):
            result += struct.pack('<BBHHBH',
                i,
                1 if rail['enabled'] else 0,
                rail['voltage_mv'],
                rail['current_ma'],
                rail['status'],
                rail.get('target_mv', rail['voltage_mv']),
            )
        return result
    return handler


def _hat_set_rail_enable(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 2:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        rail_id = payload[0]
        enable = payload[1] != 0
        if rail_id >= len(device.hat_rails):
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        device.hat_rails[rail_id]['enabled'] = enable
        # Mock some dummy readbacks
        if enable:
            if rail_id == 0:  # 3V3_ADJ
                device.hat_rails[rail_id]['voltage_mv'] = 3300
                device.hat_rails[rail_id]['current_ma'] = 150
            elif rail_id == 1:  # VADJ3
                device.hat_rails[rail_id]['voltage_mv'] = 3300
                device.hat_rails[rail_id]['current_ma'] = 100
            elif rail_id == 2:  # VADJ4
                device.hat_rails[rail_id]['voltage_mv'] = 5000
                device.hat_rails[rail_id]['current_ma'] = 200
        else:
            device.hat_rails[rail_id]['voltage_mv'] = 0
            device.hat_rails[rail_id]['current_ma'] = 0
        return _hat_get_rail_status(device)(b"")
    return handler


def _hat_set_rail_voltage(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 3:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        rail_id, voltage_mv = struct.unpack_from('<BH', payload, 0)
        if rail_id >= len(device.hat_rails):
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        device.hat_rails[rail_id]['voltage_mv'] = voltage_mv
        device.hat_rails[rail_id]['target_mv'] = voltage_mv
        if device.hat_rails[rail_id]['enabled']:
            device.hat_rails[rail_id]['current_ma'] = 150 if rail_id == 0 else 100
        return _hat_get_rail_status(device)(b"")
    return handler


def _hat_set_led_state(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 2:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        led_id = payload[0]
        if led_id >= 8:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        if len(payload) >= 5:
            device.hat_led_states[led_id] = [payload[1], payload[2], payload[3], payload[4]]
        else:
            device.hat_led_states[led_id] = [0, 0, 0, payload[1]]
        return b''
    return handler


def _hat_la_set_route(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        route = payload[0]
        if route == 1:  # High-speed is unsupported on simulator/reverts with error
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        device.hat_la_route = route
        return struct.pack('<B', route)
    return handler


def _hat_calibrate_start(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        rail_id = payload[0]
        if rail_id >= 3:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        device.hat_cal_state = 2  # success/done
        device.hat_cal_progress = 100
        device.hat_cal_rail_id = rail_id
        device.hat_cal_last_error = 0
        return struct.pack('<B', device.hat_cal_state)
    return handler


def _hat_calibrate_status(device):
    def handler(payload: bytes) -> bytes:
        return struct.pack('<BBBBBBBbiiiiiH',
            device.hat_cal_state,
            device.hat_cal_progress,
            device.hat_cal_rail_id,
            device.hat_cal_last_error,
            device.hat_cal_persist_state,
            device.hat_cal_stage,
            device.hat_cal_point,
            device.hat_cal_code,
            device.hat_cal_measured_mv,
            device.hat_cal_min_mv,
            device.hat_cal_max_mv,
            device.hat_cal_max_gap_mv,
            device.hat_cal_max_error_mv,
            device.hat_cal_validation_flags
        )
    return handler


def _hat_calibrate_import(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 2:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        rail_id = payload[0]
        count = payload[1]
        if len(payload) != 2 + count * 5:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        return b''
    return handler


def _hat_set_io_bank(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 3:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        device.hat_io_bank["dirs"] = payload[0]
        device.hat_io_bank["ups"] = payload[1]
        device.hat_io_bank["dns"] = payload[2]
        return b''
    return handler


def _hat_set_level_shift(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 2:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        oe = payload[0] != 0
        dir = payload[1] != 0
        device.hat_level_shift["oe"] = oe
        device.hat_level_shift["dir"] = dir
        return struct.pack('<BB', 1 if oe else 0, 1 if dir else 0)
    return handler
