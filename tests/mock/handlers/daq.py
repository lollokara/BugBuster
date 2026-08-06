"""
DAQ HAT (ESP32-P4) handlers for SimulatedDevice.

Covers the four multiplexed BBP opcodes the S3 forwards to the P4:
  DAQ_CONFIG  (0xB6) — settings registry: GET / SET / GET_ALL / SCHEMA / ACTION
  DAQ_CAL     (0xB7) — SMU calibration state machine: START / ACK / STATUS / ABORT
  DAQ_MEASURE (0xBF) — fused current/voltage/power readback
  DAQ_TRIG    (0xCE) — trigger & flag engine (this one lives on the S3)

Wire layouts mirror python/bugbuster/daq_config.py, which is the host-side
source of truth. The registry here is a real dict, so a SET is observable by a
later GET / GET_ALL instead of being swallowed.
"""

import struct

from bugbuster.constants import CmdId
from bugbuster.daq_config import (
    DaqCalMode,
    DaqCalOp,
    DaqCalPersist,
    DaqCalPhase,
    DaqCalPrompt,
    DaqCfgOp,
    DaqKey,
    DaqTrigEdge,
    DaqTrigLogic,
    DaqTrigOp,
    DaqTrigRole,
    DaqTrigSource,
    DaqType,
    KEY_TYPE,
    tlv_encode,
    tlv_parse,
)
from bugbuster.transport.usb import DeviceError, ErrorCode


# Registry defaults. Values are plausible post-boot settings, not zeros, so a
# test that forgets to SET still reads something meaningful.
_DEFAULTS = {
    int(DaqKey.AUTORANGING):     True,
    int(DaqKey.RANGE_IDX):       0,
    int(DaqKey.SAMPLE_RATE_IDX): 3,
    int(DaqKey.STREAMING):       False,
    int(DaqKey.USB_DECIMATION):  1,
    int(DaqKey.SOURCE_ENABLE):   False,
    int(DaqKey.DUT_VOLTAGE_MV):  3300,
    int(DaqKey.DUT_ILIMIT_MA):   100,
    int(DaqKey.FFT_ENABLE):      False,
    int(DaqKey.FFT_LENGTH):      1024,
    int(DaqKey.FFT_WINDOW):      0,
    int(DaqKey.FFT_SOURCE):      0,
    int(DaqKey.MULTIRES_TIERS):  3,
    int(DaqKey.STATS_WINDOW_MS): 100,
    int(DaqKey.BRIGHTNESS_PCT):  80,
    int(DaqKey.DARK_MODE):       True,
    int(DaqKey.NPX_MODE):        1,
    int(DaqKey.NPX_COLOR):       0x00FF00,
    int(DaqKey.NPX_BRIGHTNESS):  50,
    int(DaqKey.WIFI_ENABLE):     False,
    int(DaqKey.WIFI_MODE):       0,
    int(DaqKey.WIFI_SSID):       "",
    int(DaqKey.WIFI_PASSWORD):   "",
    int(DaqKey.DEVICE_LABEL):    "BugBuster DAQ",
}

# Keys withheld from GET_ALL unless the caller sets the include-secret flag.
_SECRET_KEYS = {int(DaqKey.WIFI_PASSWORD)}

# The real P4 pages GET_ALL replies to fit the 32-byte S3 HAT wire frame (see
# the handle_config_get_all() fix in ROUTER.md). Page here too, or the host's
# paging loop is never exercised.
_GET_ALL_PAGE_BYTES = 24

_SCHEMA_RANGES = {
    int(DaqKey.BRIGHTNESS_PCT):  (0, 100, 1, 80, "Display brightness"),
    int(DaqKey.DUT_VOLTAGE_MV):  (0, 15000, 10, 3300, "DUT voltage"),
    int(DaqKey.DUT_ILIMIT_MA):   (0, 1000, 1, 100, "DUT current limit"),
    int(DaqKey.SAMPLE_RATE_IDX): (0, 7, 1, 3, "Sample rate"),
    int(DaqKey.FFT_LENGTH):      (256, 4096, 256, 1024, "FFT length"),
}


def register(device) -> None:
    device.daq_registry = dict(_DEFAULTS)
    device.daq_cal = {
        "phase": int(DaqCalPhase.IDLE),
        "prompt": int(DaqCalPrompt.NONE),
        "mode": int(DaqCalMode.VOLTAGE),
        "progress": 0,
        "point": 0,
        "code": 0,
        "persist": int(DaqCalPersist.RAM),
        "measured": 0.0,
        "min": 0.0,
        "max": 0.0,
        "flags": 0,
        "vcount": 0,
        "icount": 0,
    }
    device.daq_trigger = {
        "logic": int(DaqTrigLogic.NONE),
        "armed": False,
        "fired": False,
        "pre_samples": 0,
        # 12 IOs, 1-based externally
        "ios": [
            {
                "role": int(DaqTrigRole.OFF),
                "edge": int(DaqTrigEdge.RISING),
                "source": int(DaqTrigSource.DIGITAL),
                "threshold_v": 1.5,
            }
            for _ in range(12)
        ],
    }

    # ------------------------------------------------------------------
    # DAQ_CONFIG
    # ------------------------------------------------------------------

    def handle_config(payload: bytes) -> bytes:
        if not payload:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        op = payload[0]

        if op == DaqCfgOp.GET:
            if len(payload) < 3:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            key, = struct.unpack_from("<H", payload, 1)
            if key not in device.daq_registry:
                return b''          # host turns an empty reply into KeyError
            type_ = KEY_TYPE.get(key, DaqType.U32)
            return tlv_encode(key, type_, device.daq_registry[key])

        if op == DaqCfgOp.SET:
            key, _type, value, _off = tlv_parse(payload, 1)
            if key not in device.daq_registry:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            device.daq_registry[key] = value
            return b''

        if op == DaqCfgOp.GET_ALL:
            start = payload[1] if len(payload) >= 2 else 0
            flags = payload[2] if len(payload) >= 3 else 0
            keys = sorted(device.daq_registry)
            if not (flags & 0x01):
                keys = [k for k in keys if k not in _SECRET_KEYS]

            out = bytearray()
            idx = start
            while idx < len(keys):
                key = keys[idx]
                tlv = tlv_encode(key, KEY_TYPE.get(key, DaqType.U32),
                                 device.daq_registry[key])
                if out and len(out) + len(tlv) > _GET_ALL_PAGE_BYTES:
                    break
                out += tlv
                idx += 1
            next_idx = 0xFF if idx >= len(keys) else idx
            return bytes([next_idx]) + bytes(out)

        if op == DaqCfgOp.SCHEMA:
            if len(payload) < 3:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            key, = struct.unpack_from("<H", payload, 1)
            if key not in device.daq_registry:
                return b''
            vmin, vmax, vstep, vdef, label = _SCHEMA_RANGES.get(
                key, (0, 0, 0, 0, "setting"))
            label_b = label.encode("utf-8")
            head = struct.pack("<HBBiiiiB", key,
                               int(KEY_TYPE.get(key, DaqType.U32)), 0,
                               vmin, vmax, vstep, vdef, len(label_b))
            return head + label_b

        if op == DaqCfgOp.ACTION:
            if len(payload) < 2:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            action = payload[1]
            if action == 3:  # FACTORY_RESET
                device.daq_registry.update(_DEFAULTS)
            return b''

        raise DeviceError(ErrorCode.INVALID_PARAM, 0)

    # ------------------------------------------------------------------
    # DAQ_CAL — operator-prompted state machine
    # ------------------------------------------------------------------

    def _pack_cal_status() -> bytes:
        c = device.daq_cal
        return struct.pack(
            "<BBBBBbBB fff HBB",
            c["phase"], c["prompt"], c["mode"], c["progress"], c["point"],
            c["code"], c["persist"], 0,
            c["measured"], c["min"], c["max"],
            c["flags"], c["vcount"], c["icount"],
        )

    def handle_cal(payload: bytes) -> bytes:
        if not payload:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        op = payload[0]
        c = device.daq_cal

        if op == DaqCalOp.START:
            if len(payload) < 2:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            mode = payload[1]
            c.update(
                mode=mode,
                phase=int(DaqCalPhase.PROMPT),
                # Each mode blocks on a different physical action.
                prompt=int(DaqCalPrompt.DISCONNECT_LOAD if mode == DaqCalMode.VOLTAGE
                           else DaqCalPrompt.SHORT_OUTPUT),
                progress=0, point=0, code=0, flags=0,
                persist=int(DaqCalPersist.RAM),
            )
            return b''

        if op == DaqCalOp.ACK:
            if c["phase"] != int(DaqCalPhase.PROMPT):
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            c.update(phase=int(DaqCalPhase.RUNNING),
                     prompt=int(DaqCalPrompt.NONE))
            return b''

        if op == DaqCalOp.STATUS:
            # Advance one step per poll so a host poll loop terminates.
            if c["phase"] == int(DaqCalPhase.RUNNING):
                c["progress"] = min(100, c["progress"] + 25)
                c["point"] += 1
                c["measured"] = 3.3 * c["progress"] / 100.0
                c["max"] = max(c["max"], c["measured"])
                if c["progress"] >= 100:
                    c.update(
                        phase=int(DaqCalPhase.SUCCESS),
                        persist=int(DaqCalPersist.SAVED),
                        min=0.0, max=3.3,
                    )
                    if c["mode"] == int(DaqCalMode.VOLTAGE):
                        c["vcount"] = c["point"]
                    else:
                        c["icount"] = c["point"]
            return _pack_cal_status()

        if op == DaqCalOp.ABORT:
            c.update(phase=int(DaqCalPhase.IDLE), prompt=int(DaqCalPrompt.NONE),
                     progress=0, point=0)
            return b''

        raise DeviceError(ErrorCode.INVALID_PARAM, 0)

    # ------------------------------------------------------------------
    # DAQ_MEASURE
    # ------------------------------------------------------------------

    def handle_measure(payload: bytes) -> bytes:
        reg = device.daq_registry
        source_on = bool(reg[int(DaqKey.SOURCE_ENABLE)])
        # Ohm's law through a nominal 100 ohm DUT, so voltage/current/power stay
        # mutually consistent and follow the configured setpoint.
        voltage = (reg[int(DaqKey.DUT_VOLTAGE_MV)] / 1000.0) if source_on else 0.0
        current = voltage / 100.0
        ilimit = reg[int(DaqKey.DUT_ILIMIT_MA)] / 1000.0
        current = min(current, ilimit)
        power = voltage * current
        return struct.pack(
            "<BBBB ffff",
            reg[int(DaqKey.RANGE_IDX)] & 0xFF,
            1 if reg[int(DaqKey.STREAMING)] else 0,
            1 if source_on else 0,
            0,
            current, voltage, power, power * 0.001,
        )

    # ------------------------------------------------------------------
    # DAQ_TRIG
    # ------------------------------------------------------------------

    def handle_trig(payload: bytes) -> bytes:
        if not payload:
            raise DeviceError(ErrorCode.INVALID_PARAM, 0)
        op = payload[0]
        t = device.daq_trigger

        if op == DaqTrigOp.SET_IO:
            if len(payload) < 10:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            io, role, edge, source, _pad = struct.unpack_from("<BBBBB", payload, 1)
            thr, = struct.unpack_from("<f", payload, 6)
            if not (1 <= io <= 12):
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            t["ios"][io - 1] = {
                "role": role, "edge": edge,
                "source": source, "threshold_v": thr,
            }
            return b''

        if op == DaqTrigOp.GET_IO:
            if len(payload) < 2:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            io = payload[1]
            if not (1 <= io <= 12):
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            cfg = t["ios"][io - 1]
            return struct.pack("<BBBBf", cfg["role"], cfg["edge"],
                               cfg["source"], 0, cfg["threshold_v"])

        if op == DaqTrigOp.SET_LOGIC:
            if len(payload) < 2:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            t["logic"] = payload[1]
            return b''

        if op == DaqTrigOp.ARM:
            if len(payload) < 7:
                raise DeviceError(ErrorCode.INVALID_PARAM, 0)
            t["armed"] = bool(payload[1])
            t["pre_samples"], = struct.unpack_from("<I", payload, 3)
            if not t["armed"]:
                t["fired"] = False
            return b''

        if op == DaqTrigOp.STATUS:
            return struct.pack("<BBBB", t["logic"],
                               1 if t["armed"] else 0,
                               1 if t["fired"] else 0, 0)

        if op == DaqTrigOp.GET_ALL:
            out = struct.pack("<BBBB", t["logic"],
                              1 if t["armed"] else 0,
                              1 if t["fired"] else 0, 0)
            for cfg in t["ios"]:
                out += struct.pack("<BBBBf", cfg["role"], cfg["edge"],
                                   cfg["source"], 0, cfg["threshold_v"])
            return out

        raise DeviceError(ErrorCode.INVALID_PARAM, 0)

    device.register_handler(CmdId.DAQ_CONFIG,  handle_config)
    device.register_handler(CmdId.DAQ_CAL,     handle_cal)
    device.register_handler(CmdId.DAQ_MEASURE, handle_measure)
    device.register_handler(CmdId.DAQ_TRIG,    handle_trig)
