"""
HAT expansion board handlers for SimulatedDevice.

Handles: HAT_DETECT, HAT_GET_STATUS, HAT_SET_PIN, HAT_SET_ALL_PINS,
         HAT_RESET, HAT_SET_POWER, HAT_GET_POWER, HAT_SET_IO_VOLT,
         HAT_SETUP_SWD, HAT_GET_HVPAK_INFO, HAT_LA_*, HAT_GET_HVPAK_*.
"""

import struct
from bugbuster.constants import CmdId, ErrorCode
from bugbuster.transport.usb import DeviceError


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
        device.hat_power = False
    if not hasattr(device, "hat_io_volt"):
        device.hat_io_volt = 3300
    if not hasattr(device, "hvpak"):
        device.hvpak = {
            "identity": 0x15,
            "ready": True,
            "caps": 0x0F,
            "lut": [{"kind": 0, "index": i, "width_bits": 2, "truth_table": 0} for i in range(16)],
            "bridge": {
                "output_mode": [0, 0],
                "ocp_retry": [0, 0],
                "predriver_enabled": False,
                "full_bridge_enabled": False,
                "control_selection_ph_en": False,
                "ocp_deglitch_enabled": False,
                "uvlo_enabled": False,
            },
            "analog": {
                "vref_mode": 0,
                "vref_powered": False,
                "vref_power_from_matrix": False,
                "vref_sink_12ua": False,
                "vref_input_selection": 0,
                "current_sense_vref": 0,
                "current_sense_dynamic_from_pwm": False,
                "current_sense_gain": 0,
                "current_sense_invert": False,
                "current_sense_enabled": False,
                "acmp0_gain": 0,
                "acmp0_vref": 0,
                "has_acmp1": False,
                "acmp1_gain": 0,
                "acmp1_vref": 0,
            },
            "pwm": [
                {
                    "index": i,
                    "initial_value": 0,
                    "current_value": 0,
                    "resolution_7bit": False,
                    "out_plus_inverted": False,
                    "out_minus_inverted": False,
                    "async_powerdown": False,
                    "autostop_mode": False,
                    "boundary_osc_disable": False,
                    "phase_correct": False,
                    "deadband": 0,
                    "stop_mode": False,
                    "i2c_trigger": False,
                    "duty_source": 0,
                    "period_clock_source": 0,
                    "duty_clock_source": 0,
                    "last_error": 0,
                }
                for i in range(4)
            ],
        }


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
    device.register_handler(CmdId.HAT_GET_HVPAK_INFO, _hat_get_hvpak_info(device))

    # LA handlers
    device.register_handler(CmdId.HAT_LA_CONFIG,  _hat_la_config(device))
    device.register_handler(CmdId.HAT_LA_ARM,     _hat_la_arm(device))
    device.register_handler(CmdId.HAT_LA_FORCE,   _hat_la_force(device))
    device.register_handler(CmdId.HAT_LA_STATUS,  _hat_la_status(device))
    device.register_handler(CmdId.HAT_LA_READ,    _hat_la_read(device))
    device.register_handler(CmdId.HAT_LA_STOP,    _hat_la_stop(device))
    device.register_handler(CmdId.HAT_LA_TRIGGER, _hat_la_trigger(device))

    # HVPAK advanced handlers
    device.register_handler(CmdId.HAT_GET_HVPAK_CAPS,        _hat_get_hvpak_caps(device))
    device.register_handler(CmdId.HAT_GET_HVPAK_LUT,         _hat_get_hvpak_lut(device))
    device.register_handler(CmdId.HAT_SET_HVPAK_LUT,         _hat_set_hvpak_lut(device))
    device.register_handler(CmdId.HAT_GET_HVPAK_BRIDGE,      _hat_get_hvpak_bridge(device))
    device.register_handler(CmdId.HAT_SET_HVPAK_BRIDGE,      _hat_set_hvpak_bridge(device))
    device.register_handler(CmdId.HAT_GET_HVPAK_ANALOG,      _hat_get_hvpak_analog(device))
    device.register_handler(CmdId.HAT_SET_HVPAK_ANALOG,      _hat_set_hvpak_analog(device))
    device.register_handler(CmdId.HAT_GET_HVPAK_PWM,         _hat_get_hvpak_pwm(device))
    device.register_handler(CmdId.HAT_SET_HVPAK_PWM,         _hat_set_hvpak_pwm(device))
    device.register_handler(CmdId.HAT_HVPAK_REG_READ,        _hat_hvpak_reg_read(device))
    device.register_handler(CmdId.HAT_HVPAK_REG_WRITE_MASKED, _hat_hvpak_reg_write_masked(device))


# ---------------------------------------------------------------------------
# HAT_DETECT (0xC9)
# client.py: detected(B), hat_type(B), detect_v(f), connected(B)
# ---------------------------------------------------------------------------

def _hat_detect(device):
    def handler(payload: bytes) -> bytes:
        detected = device.hat_present
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
#            [hvpak_part(B), hvpak_ready(B), hvpak_last_error(B)],
#            [dap_connected(B), target_detected(B), target_dpidr(I)]
# ---------------------------------------------------------------------------

def _hat_get_status(device):
    def handler(payload: bytes) -> bytes:
        detected = device.hat_present
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
        for _ in range(2):
            buf += struct.pack('<BfB', int(device.hat_power), 0.0, 0)
        # io_voltage_mv
        buf += struct.pack('<H', getattr(device, 'hat_io_volt', 3300))
        # hvpak fields
        hvpak = device.hvpak
        buf += struct.pack('<BBB',
                           hvpak["identity"],
                           int(hvpak["ready"] and detected),
                           0)  # last_error
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
        device.hat_power = bool(enable)
        return b''
    return handler


# ---------------------------------------------------------------------------
# HAT_GET_POWER (0xCB)
# client.py: 2x [enabled(B), current(f), fault(B)], io_mv(H),
#            [hvpak_part(B), hvpak_ready(B), hvpak_last_error(B)]
# ---------------------------------------------------------------------------

def _hat_get_power(device):
    def handler(payload: bytes) -> bytes:
        buf = bytearray()
        for _ in range(2):
            buf += struct.pack('<BfB', int(device.hat_power), 0.0, 0)
        buf += struct.pack('<H', getattr(device, 'hat_io_volt', 3300))
        hvpak = device.hvpak
        buf += struct.pack('<BBB',
                           hvpak["identity"],
                           int(hvpak["ready"] and device.hat_present),
                           0)
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
# HAT_GET_HVPAK_INFO (0xCE)
# client.py: part(B), ready(B), last_error(B), factory_virgin(B),
#            service_window_ok(B), requested_mv(H), applied_mv(H),
#            service_f5(B), service_fd(B), service_fe(B)
# ---------------------------------------------------------------------------

def _hat_get_hvpak_info(device):
    def handler(payload: bytes) -> bytes:
        hvpak = device.hvpak
        ready = hvpak["ready"] and device.hat_present
        mv = getattr(device, 'hat_io_volt', 3300)
        return struct.pack('<BBBBBHHBBB',
                           hvpak["identity"],
                           int(ready),
                           0,       # last_error
                           1,       # factory_virgin
                           1,       # service_window_ok
                           mv,      # requested_mv
                           mv,      # applied_mv
                           0,       # service_f5
                           0,       # service_fd
                           0)       # service_fe
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
        device.la_state = "DONE"
        return b''
    return handler


def _hat_la_force(device):
    def handler(payload: bytes) -> bytes:
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


# ---------------------------------------------------------------------------
# HVPAK advanced handlers
# ---------------------------------------------------------------------------

def _hat_get_hvpak_caps(device):
    def handler(payload: bytes) -> bytes:
        hvpak = device.hvpak
        # flags(I), lut2_count(B), lut3_count(B), lut4_count(B),
        # pwm_count(B), comparator_count(B), bridge_count(B)
        return struct.pack('<IBBBBBB',
                           hvpak["caps"],
                           4,   # lut2_count
                           4,   # lut3_count
                           4,   # lut4_count
                           4,   # pwm_count
                           2,   # comparator_count
                           1)   # bridge_count
    return handler


def _hat_get_hvpak_lut(device):
    def handler(payload: bytes) -> bytes:
        kind, index = struct.unpack_from('<BB', payload)
        lut = device.hvpak["lut"]
        entry = lut[index % len(lut)]
        # kind(B), index(B), width_bits(B), truth_table(H)
        return struct.pack('<BBBH', kind, index, entry.get("width_bits", 2), entry.get("truth_table", 0))
    return handler


def _hat_set_hvpak_lut(device):
    def handler(payload: bytes) -> bytes:
        kind, index, truth_table = struct.unpack_from('<BBH', payload)
        lut = device.hvpak["lut"]
        if index < len(lut):
            lut[index]["kind"] = kind
            lut[index]["truth_table"] = truth_table
        entry = lut[index % len(lut)]
        return struct.pack('<BBBH', kind, index, entry.get("width_bits", 2), entry.get("truth_table", 0))
    return handler


def _pack_hvpak_bridge(bridge: dict) -> bytes:
    om = bridge["output_mode"]
    ocpr = bridge["ocp_retry"]
    return struct.pack('<BBBBBBBBB',
                       om[0], ocpr[0],
                       om[1], ocpr[1],
                       int(bridge["predriver_enabled"]),
                       int(bridge["full_bridge_enabled"]),
                       int(bridge["control_selection_ph_en"]),
                       int(bridge["ocp_deglitch_enabled"]),
                       int(bridge["uvlo_enabled"]))


def _hat_get_hvpak_bridge(device):
    def handler(payload: bytes) -> bytes:
        return _pack_hvpak_bridge(device.hvpak["bridge"])
    return handler


def _hat_set_hvpak_bridge(device):
    def handler(payload: bytes) -> bytes:
        (om0, ocpr0, om1, ocpr1,
         pre, full, ctrl, ocp_deg, uvlo) = struct.unpack_from('<BBBBBBBBB', payload)
        bridge = device.hvpak["bridge"]
        bridge["output_mode"] = [om0, om1]
        bridge["ocp_retry"] = [ocpr0, ocpr1]
        bridge["predriver_enabled"] = bool(pre)
        bridge["full_bridge_enabled"] = bool(full)
        bridge["control_selection_ph_en"] = bool(ctrl)
        bridge["ocp_deglitch_enabled"] = bool(ocp_deg)
        bridge["uvlo_enabled"] = bool(uvlo)
        return _pack_hvpak_bridge(bridge)
    return handler


def _pack_hvpak_analog(analog: dict) -> bytes:
    return struct.pack('<BBBBBBBBBBBBBBB',
                       analog["vref_mode"],
                       int(analog["vref_powered"]),
                       int(analog["vref_power_from_matrix"]),
                       int(analog["vref_sink_12ua"]),
                       analog["vref_input_selection"],
                       analog["current_sense_vref"],
                       int(analog["current_sense_dynamic_from_pwm"]),
                       analog["current_sense_gain"],
                       int(analog["current_sense_invert"]),
                       int(analog["current_sense_enabled"]),
                       analog["acmp0_gain"],
                       analog["acmp0_vref"],
                       int(analog["has_acmp1"]),
                       analog["acmp1_gain"],
                       analog["acmp1_vref"])


def _hat_get_hvpak_analog(device):
    def handler(payload: bytes) -> bytes:
        return _pack_hvpak_analog(device.hvpak["analog"])
    return handler


def _hat_set_hvpak_analog(device):
    def handler(payload: bytes) -> bytes:
        fields = struct.unpack_from('<BBBBBBBBBBBBBBB', payload)
        analog = device.hvpak["analog"]
        keys = [
            "vref_mode", "vref_powered", "vref_power_from_matrix", "vref_sink_12ua",
            "vref_input_selection", "current_sense_vref", "current_sense_dynamic_from_pwm",
            "current_sense_gain", "current_sense_invert", "current_sense_enabled",
            "acmp0_gain", "acmp0_vref", "has_acmp1", "acmp1_gain", "acmp1_vref",
        ]
        bool_keys = {
            "vref_powered", "vref_power_from_matrix", "vref_sink_12ua",
            "current_sense_dynamic_from_pwm", "current_sense_invert",
            "current_sense_enabled", "has_acmp1",
        }
        for key, val in zip(keys, fields):
            analog[key] = bool(val) if key in bool_keys else val
        return _pack_hvpak_analog(analog)
    return handler


def _pack_hvpak_pwm(pwm: dict) -> bytes:
    return struct.pack('<BBBBBBBBBBBBBBBBB',
                       pwm["index"],
                       pwm["initial_value"],
                       pwm["current_value"],
                       int(pwm["resolution_7bit"]),
                       int(pwm["out_plus_inverted"]),
                       int(pwm["out_minus_inverted"]),
                       int(pwm["async_powerdown"]),
                       int(pwm["autostop_mode"]),
                       int(pwm["boundary_osc_disable"]),
                       int(pwm["phase_correct"]),
                       pwm["deadband"],
                       int(pwm["stop_mode"]),
                       int(pwm["i2c_trigger"]),
                       pwm["duty_source"],
                       pwm["period_clock_source"],
                       pwm["duty_clock_source"],
                       pwm["last_error"])


def _hat_get_hvpak_pwm(device):
    def handler(payload: bytes) -> bytes:
        index = payload[0] if payload else 0
        pwms = device.hvpak["pwm"]
        pwm = pwms[index % len(pwms)]
        return _pack_hvpak_pwm(pwm)
    return handler


def _hat_set_hvpak_pwm(device):
    def handler(payload: bytes) -> bytes:
        # payload: index(B), initial_value(B), _reserved(B), resolution_7bit(B),
        #          out_plus_inverted(B), out_minus_inverted(B), async_powerdown(B),
        #          autostop_mode(B), boundary_osc_disable(B), phase_correct(B),
        #          deadband(B), stop_mode(B), i2c_trigger(B), duty_source(B),
        #          period_clock_source(B), duty_clock_source(B)
        fields = struct.unpack_from('<BBBBBBBBBBBBBBBB', payload)
        index = fields[0]
        pwms = device.hvpak["pwm"]
        pwm = pwms[index % len(pwms)]
        pwm["index"] = fields[0]
        pwm["initial_value"] = fields[1]
        # fields[2] is reserved
        pwm["resolution_7bit"] = bool(fields[3])
        pwm["out_plus_inverted"] = bool(fields[4])
        pwm["out_minus_inverted"] = bool(fields[5])
        pwm["async_powerdown"] = bool(fields[6])
        pwm["autostop_mode"] = bool(fields[7])
        pwm["boundary_osc_disable"] = bool(fields[8])
        pwm["phase_correct"] = bool(fields[9])
        pwm["deadband"] = fields[10]
        pwm["stop_mode"] = bool(fields[11])
        pwm["i2c_trigger"] = bool(fields[12])
        pwm["duty_source"] = fields[13]
        pwm["period_clock_source"] = fields[14]
        pwm["duty_clock_source"] = fields[15]
        return _pack_hvpak_pwm(pwm)
    return handler


def _hat_hvpak_reg_read(device):
    def handler(payload: bytes) -> bytes:
        addr = payload[0] if payload else 0
        return struct.pack('<BB', addr, 0x00)
    return handler


_HVPAK_UNSAFE_REGISTERS = frozenset({
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
})


def _hat_hvpak_reg_write_masked(device):
    def handler(payload: bytes) -> bytes:
        addr, mask, value = struct.unpack_from('<BBB', payload)
        if addr in _HVPAK_UNSAFE_REGISTERS:
            raise DeviceError(ErrorCode.HVPAK_UNSAFE_REGISTER, 0)
        return struct.pack('<BBBB', addr, mask, value, value & mask)
    return handler
