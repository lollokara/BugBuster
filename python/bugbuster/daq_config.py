"""
daq_config.py — host-side access to the DAQ HAT (ESP32-P4) settings registry.

Mirrors Firmware/DAQ_HAT/common/daq_config_registry.{h,c}. Every setting is
addressed by a stable 16-bit key and carried as TLV ([key u16 LE][type u8]
[len u8][value]). The desktop / Python / web front-ends reach the P4 through the
ESP32-S3, which forwards a single multiplexed BBP opcode (CmdId.DAQ_CONFIG) to
the P4's CONFIG commands.

Usage::

    bb = BugBuster(...)              # USB transport
    bb.daq.set(DaqKey.DUT_VOLTAGE_MV, 3300)
    print(bb.daq.get(DaqKey.DUT_VOLTAGE_MV))
    print(bb.daq.get_all())         # {DaqKey: value, ...}
    bb.daq.action(DaqAction.ENERGY_RESET)
"""
from __future__ import annotations

import struct
from enum import IntEnum
from typing import Any, Dict, Optional, Tuple

from .constants import CmdId


# --- Sub-ops (payload[0] of CmdId.DAQ_CONFIG), map to P4 HAT cmd 0x70+op ------
class DaqCfgOp(IntEnum):
    GET     = 0x00
    SET     = 0x01
    GET_ALL = 0x02
    SCHEMA  = 0x03
    ACTION  = 0x04


# --- TLV value type tags (mirror daq_type_t) ----------------------------------
class DaqType(IntEnum):
    NONE = 0
    BOOL = 1
    U8   = 2
    I8   = 3
    U16  = 4
    I16  = 5
    U32  = 6
    I32  = 7
    F32  = 8
    ENUM = 9
    STR  = 10


def _key(group: int, idx: int) -> int:
    return ((group & 0xFF) << 8) | (idx & 0xFF)


# --- Setting keys (mirror daq_key_t) ------------------------------------------
class DaqKey(IntEnum):
    # Acquisition
    AUTORANGING     = _key(0x01, 0x01)
    RANGE_IDX       = _key(0x01, 0x02)
    SAMPLE_RATE_IDX = _key(0x01, 0x03)
    STREAMING       = _key(0x01, 0x04)
    USB_DECIMATION  = _key(0x01, 0x05)
    FILTER          = _key(0x01, 0x06)   # wideband / sinc5 / sinc3 (FINE+COARSE)
    DECIMATION      = _key(0x01, 0x07)   # x32 .. x1024
    REJECT_5060     = _key(0x01, 0x08)   # Sinc3 50/60 Hz reject
    SR_MODE         = _key(0x01, 0x09)   # super-resolution
    RANGE_DWELL_US  = _key(0x01, 0x0A)   # autorange down-range dwell (us)
    RANGE_LOCK_US   = _key(0x01, 0x0B)   # post-switch settle lock (us)
    RANGE_FLAP      = _key(0x01, 0x0C)   # adaptive anti-flap backoff
    # Source / SMU
    SOURCE_ENABLE   = _key(0x02, 0x01)
    DUT_VOLTAGE_MV  = _key(0x02, 0x02)
    DUT_ILIMIT_MA   = _key(0x02, 0x03)
    # DSP
    FFT_ENABLE      = _key(0x03, 0x01)
    FFT_LENGTH      = _key(0x03, 0x02)
    FFT_WINDOW      = _key(0x03, 0x03)
    FFT_SOURCE      = _key(0x03, 0x04)
    MULTIRES_TIERS  = _key(0x03, 0x05)
    STATS_WINDOW_MS = _key(0x03, 0x06)
    # Display (C6)
    BRIGHTNESS_PCT  = _key(0x04, 0x01)
    DARK_MODE       = _key(0x04, 0x02)
    # Neopixel (C6)
    NPX_MODE        = _key(0x05, 0x01)
    NPX_COLOR       = _key(0x05, 0x02)
    NPX_BRIGHTNESS  = _key(0x05, 0x03)
    # WiFi (S3 mainboard, relayed)
    WIFI_ENABLE     = _key(0x06, 0x01)
    WIFI_MODE       = _key(0x06, 0x02)
    WIFI_SSID       = _key(0x06, 0x03)
    WIFI_PASSWORD   = _key(0x06, 0x04)
    # System
    DEVICE_LABEL    = _key(0x07, 0x01)


# --- Actions (CmdId.DAQ_CONFIG / DaqCfgOp.ACTION) -----------------------------
class DaqAction(IntEnum):
    ENERGY_RESET  = 1
    CHARGE_RESET  = 2
    FACTORY_RESET = 3


# --- SMU calibration (CmdId.DAQ_CAL sub-ops, map to P4 HAT cmd 0x56+op) --------
class DaqCalOp(IntEnum):
    START  = 0x00  # payload: [op][mode u8]
    ACK    = 0x01
    STATUS = 0x02
    ABORT  = 0x03


class DaqCalMode(IntEnum):
    VOLTAGE = 0  # DS4424 ch1 -> V_DUT
    CURRENT = 1  # DS4424 ch0 -> current limit


class DaqCalPhase(IntEnum):
    IDLE    = 0
    PROMPT  = 1  # blocked on operator action (see prompt); call cal_ack()
    RUNNING = 2
    SUCCESS = 3
    FAILED  = 4


class DaqCalPrompt(IntEnum):
    NONE            = 0
    DISCONNECT_LOAD = 1  # voltage cal: remove the DUT load, then ack
    SHORT_OUTPUT    = 2  # current cal: short the output, then ack


class DaqCalPersist(IntEnum):
    RAM    = 0
    SAVING = 1
    SAVED  = 2
    FAILED = 3


# --- Trigger / flag config (CmdId.DAQ_TRIG sub-ops, handled on the S3) ---------
class DaqTrigOp(IntEnum):
    SET_IO    = 0x00  # payload: [op][io u8][role][edge][src][_pad][thr f32]
    GET_IO    = 0x01  # payload: [op][io u8] -> [role][edge][src][_pad][thr f32]
    SET_LOGIC = 0x02  # payload: [op][logic u8]
    ARM       = 0x03  # payload: [op][armed u8][_pad u8][pre_samples u32]
    STATUS    = 0x04  # payload: [op] -> [logic][armed][fired][_pad]
    GET_ALL   = 0x05  # payload: [op] -> [logic][armed][fired][_pad] + 12*io-cfg


class DaqTrigRole(IntEnum):
    OFF     = 0
    FLAG    = 1  # every matching edge emits a USB MARKER (vertical line)
    TRIGGER = 2  # matching edge(s) start the acquisition window (t=0)


class DaqTrigEdge(IntEnum):
    RISING  = 0
    FALLING = 1
    ANY     = 2


class DaqTrigSource(IntEnum):
    DIGITAL = 0  # ESP32 GPIO level (LV) or AD74416H DIN comparator (HV)
    ANALOG  = 1  # AD74416H ADC voltage vs threshold (HV IOs 3/6/9/12 only)


class DaqTrigLogic(IntEnum):
    NONE = 0
    OR   = 1  # first matching trigger IO fires the run
    AND  = 2  # all trigger IOs must match before firing


# Validation flag bits (mirror smu_cal.h SMU_CAL_FLAG_*).
class DaqCalFlag(IntEnum):
    TOO_FEW_POINTS   = 0x0001
    LOW_COVERAGE     = 0x0002
    HIGH_COVERAGE    = 0x0004
    NON_MONOTONIC    = 0x0008
    GAP_TOO_LARGE    = 0x0010
    NO_SETTLE        = 0x0020
    TARGET_UNREACHED = 0x0040
    HARDWARE         = 0x0080


# Wire layout of smu_cal_status_t (packed, little-endian).
#   phase u8, prompt u8, mode u8, progress u8, point u8, code i8,
#   persist u8, _pad u8, measured f32, min_v f32, max_v f32,
#   flags u16, vcount u8, icount u8
_CAL_STATUS_FMT = "<BBBBBbBB fff HBB"


def parse_cal_status(raw: bytes) -> Dict[str, Any]:
    """Parse a smu_cal_status_t response into a dict."""
    size = struct.calcsize(_CAL_STATUS_FMT)
    if len(raw) < size:
        raise ValueError(f"short cal status: {len(raw)} < {size} bytes")
    (phase, prompt, mode, progress, point, code, persist, _pad,
     measured, min_v, max_v, flags, vcount, icount) = struct.unpack_from(
        _CAL_STATUS_FMT, raw, 0)
    return {
        "phase": DaqCalPhase(phase) if phase in DaqCalPhase._value2member_map_ else phase,
        "prompt": DaqCalPrompt(prompt) if prompt in DaqCalPrompt._value2member_map_ else prompt,
        "mode": DaqCalMode(mode) if mode in DaqCalMode._value2member_map_ else mode,
        "progress": progress,
        "point": point,
        "code": code,
        "persist": DaqCalPersist(persist) if persist in DaqCalPersist._value2member_map_ else persist,
        "measured": measured,
        "min": min_v,
        "max": max_v,
        "flags": flags,
        "vcount": vcount,
        "icount": icount,
    }


# --- Live measurement readback (CmdId.DAQ_MEASURE -> P4 HAT GET_STATUS) --------
class DaqRange(IntEnum):
    HI      = 0    # 51 ohm   (FINE)
    MID     = 1    # 2 ohm    (FINE)
    LO      = 2    # 50 mohm  (COARSE)
    UNKNOWN = 0xFF


# Wire layout of s3link_daq_status_t (packed, little-endian).
#   range u8, streaming u8, source_enabled u8, _pad u8,
#   last_i f32, last_v f32, last_p f32, energy_mwh f32
_DAQ_STATUS_FMT = "<BBBB ffff"


def parse_measure(raw: bytes) -> Dict[str, Any]:
    """Parse an s3link_daq_status_t response into a dict."""
    size = struct.calcsize(_DAQ_STATUS_FMT)
    if len(raw) < size:
        raise ValueError(f"short DAQ status: {len(raw)} < {size} bytes")
    (rng, streaming, source_en, _pad,
     last_i, last_v, last_p, energy_mwh) = struct.unpack_from(
        _DAQ_STATUS_FMT, raw, 0)
    return {
        "range": DaqRange(rng) if rng in DaqRange._value2member_map_ else rng,
        "streaming": bool(streaming),
        "source_enabled": bool(source_en),
        "current_a": last_i,
        "voltage_v": last_v,
        "power_w": last_p,
        "energy_mwh": energy_mwh,
    }


# Key -> wire type (mirror the schema table's type column).
KEY_TYPE: Dict[int, DaqType] = {
    DaqKey.AUTORANGING:     DaqType.BOOL,
    DaqKey.RANGE_IDX:       DaqType.ENUM,
    DaqKey.SAMPLE_RATE_IDX: DaqType.ENUM,
    DaqKey.STREAMING:       DaqType.BOOL,
    DaqKey.USB_DECIMATION:  DaqType.U16,
    DaqKey.FILTER:          DaqType.ENUM,
    DaqKey.DECIMATION:      DaqType.ENUM,
    DaqKey.REJECT_5060:     DaqType.BOOL,
    DaqKey.SR_MODE:         DaqType.BOOL,
    DaqKey.RANGE_DWELL_US:  DaqType.U32,
    DaqKey.RANGE_LOCK_US:   DaqType.U16,
    DaqKey.RANGE_FLAP:      DaqType.BOOL,
    DaqKey.SOURCE_ENABLE:   DaqType.BOOL,
    DaqKey.DUT_VOLTAGE_MV:  DaqType.U16,
    DaqKey.DUT_ILIMIT_MA:   DaqType.U16,
    DaqKey.FFT_ENABLE:      DaqType.BOOL,
    DaqKey.FFT_LENGTH:      DaqType.ENUM,
    DaqKey.FFT_WINDOW:      DaqType.ENUM,
    DaqKey.FFT_SOURCE:      DaqType.ENUM,
    DaqKey.MULTIRES_TIERS:  DaqType.U8,
    DaqKey.STATS_WINDOW_MS: DaqType.U16,
    DaqKey.BRIGHTNESS_PCT:  DaqType.U8,
    DaqKey.DARK_MODE:       DaqType.BOOL,
    DaqKey.NPX_MODE:        DaqType.ENUM,
    DaqKey.NPX_COLOR:       DaqType.U32,
    DaqKey.NPX_BRIGHTNESS:  DaqType.U8,
    DaqKey.WIFI_ENABLE:     DaqType.BOOL,
    DaqKey.WIFI_MODE:       DaqType.ENUM,
    DaqKey.WIFI_SSID:       DaqType.STR,
    DaqKey.WIFI_PASSWORD:   DaqType.STR,
    DaqKey.DEVICE_LABEL:    DaqType.STR,
}

_SCALAR_FMT = {
    DaqType.BOOL: "<B", DaqType.U8: "<B", DaqType.I8: "<b",
    DaqType.U16: "<H", DaqType.I16: "<h",
    DaqType.U32: "<I", DaqType.I32: "<i", DaqType.F32: "<f",
    DaqType.ENUM: "<B",
}


def tlv_encode(key: int, type_: DaqType, value: Any) -> bytes:
    """Encode one TLV: [key u16 LE][type u8][len u8][value]."""
    if type_ == DaqType.STR:
        raw = value.encode("utf-8") if isinstance(value, str) else bytes(value)
    elif type_ == DaqType.NONE:
        raw = b""
    elif type_ == DaqType.F32:
        raw = struct.pack("<f", float(value))
    else:
        fmt = _SCALAR_FMT[type_]
        iv = int(value)
        if type_ in (DaqType.BOOL, DaqType.U8, DaqType.U16, DaqType.U32, DaqType.ENUM):
            iv &= (1 << (8 * struct.calcsize(fmt))) - 1   # wrap unsigned
        raw = struct.pack(fmt, iv)
    if len(raw) > 0xFF:
        raise ValueError("TLV value too long")
    return struct.pack("<HBB", key, int(type_), len(raw)) + raw


def tlv_parse(buf: bytes, off: int = 0) -> Tuple[int, DaqType, Any, int]:
    """Parse one TLV at @off. Returns (key, type, value, next_off)."""
    if len(buf) - off < 4:
        raise ValueError("short TLV")
    key, type_b, vlen = struct.unpack_from("<HBB", buf, off)
    off += 4
    if len(buf) - off < vlen:
        raise ValueError("truncated TLV value")
    raw = buf[off:off + vlen]
    off += vlen
    type_ = DaqType(type_b)
    value = _decode_value(type_, raw)
    return key, type_, value, off


def _decode_value(type_: DaqType, raw: bytes) -> Any:
    if type_ == DaqType.STR:
        return raw.decode("utf-8", errors="replace")
    if type_ == DaqType.NONE or len(raw) == 0:
        return None
    if type_ == DaqType.BOOL:
        return bool(raw[0])
    if type_ == DaqType.F32:
        return struct.unpack("<f", raw)[0]
    fmt = _SCALAR_FMT.get(type_)
    if fmt is None:
        return raw
    return struct.unpack(fmt, raw)[0]


class DaqConfig:
    """Bound accessor for the DAQ HAT settings registry (USB transport only)."""

    def __init__(self, client) -> None:
        self._c = client

    # -- single value ------------------------------------------------------
    def get(self, key: DaqKey) -> Any:
        payload = bytes([DaqCfgOp.GET]) + struct.pack("<H", int(key))
        resp = self._c._usb_cmd(CmdId.DAQ_CONFIG, payload)
        if not resp:
            raise KeyError(f"unknown DAQ key 0x{int(key):04X}")
        _, _, value, _ = tlv_parse(resp, 0)
        return value

    def set(self, key: DaqKey, value: Any, type_: Optional[DaqType] = None) -> None:
        if type_ is None:
            type_ = KEY_TYPE.get(int(key))
            if type_ is None:
                raise ValueError(f"unknown type for key 0x{int(key):04X}; pass type_")
        tlv = tlv_encode(int(key), type_, value)
        self._c._usb_cmd(CmdId.DAQ_CONFIG, bytes([DaqCfgOp.SET]) + tlv)

    # -- bulk --------------------------------------------------------------
    def get_all(self, include_secret: bool = False) -> Dict[int, Any]:
        out: Dict[int, Any] = {}
        start = 0
        flags = 0x01 if include_secret else 0x00
        for _ in range(64):  # safety bound; registry is far smaller
            payload = bytes([DaqCfgOp.GET_ALL, start & 0xFF, flags])
            resp = self._c._usb_cmd(CmdId.DAQ_CONFIG, payload)
            if not resp:
                break
            next_idx = resp[0]
            off = 1
            while off < len(resp):
                key, _, value, off = tlv_parse(resp, off)
                out[key] = value
            if next_idx == 0xFF:
                break
            start = next_idx
        return out

    def schema(self, key: DaqKey) -> Dict[str, Any]:
        payload = bytes([DaqCfgOp.SCHEMA]) + struct.pack("<H", int(key))
        resp = self._c._usb_cmd(CmdId.DAQ_CONFIG, payload)
        if len(resp) < 21:
            raise KeyError(f"unknown DAQ key 0x{int(key):04X}")
        k, type_b, flags, vmin, vmax, vstep, vdef, label_len = struct.unpack_from(
            "<HBBiiiiB", resp, 0)
        label = resp[21:21 + label_len].decode("utf-8", errors="replace")
        return {
            "key": k, "type": DaqType(type_b), "flags": flags,
            "min": vmin, "max": vmax, "step": vstep, "default": vdef,
            "label": label,
        }

    def action(self, action_id: DaqAction) -> None:
        self._c._usb_cmd(CmdId.DAQ_CONFIG, bytes([DaqCfgOp.ACTION, int(action_id)]))

    # -- SMU factory calibration ------------------------------------------
    def cal_start(self, mode: DaqCalMode) -> None:
        """Begin an SMU calibration run.

        ``mode`` is :class:`DaqCalMode` (VOLTAGE or CURRENT). The run blocks on
        an operator prompt (see :meth:`cal_status` -> ``prompt``); call
        :meth:`cal_ack` once the requested action (disconnect load / short
        output) is done. Poll :meth:`cal_status` for progress and the result.
        """
        self._c._usb_cmd(CmdId.DAQ_CAL, bytes([DaqCalOp.START, int(mode)]))

    def cal_ack(self) -> None:
        """Acknowledge the current operator prompt and let the run proceed."""
        self._c._usb_cmd(CmdId.DAQ_CAL, bytes([DaqCalOp.ACK]))

    def cal_abort(self) -> None:
        """Abort the calibration run and restore a safe SMU state."""
        self._c._usb_cmd(CmdId.DAQ_CAL, bytes([DaqCalOp.ABORT]))

    def cal_status(self) -> Dict[str, Any]:
        """Return the live calibration status (see :func:`parse_cal_status`)."""
        resp = self._c._usb_cmd(CmdId.DAQ_CAL, bytes([DaqCalOp.STATUS]))
        return parse_cal_status(resp)

    # -- live measurement readback ----------------------------------------
    def measure(self) -> Dict[str, Any]:
        """Read the latest fused measurement: current/voltage/power, energy,
        active range, and the streaming/source state (see
        :func:`parse_measure`)."""
        resp = self._c._usb_cmd(CmdId.DAQ_MEASURE)
        return parse_measure(resp)


# IOs that are analog-capable (position 3 of each connector block -> AD74416H).
DAQ_HV_IOS = (3, 6, 9, 12)


def daq_io_is_analog_capable(io: int) -> bool:
    """True if ``io`` (1..12) is an HV / analog-capable IO (AD74416H channel)."""
    return io in DAQ_HV_IOS


class DaqTrigger:
    """Bound accessor for the DAQ trigger / flag engine (S3 mainboard side).

    The 12 mainboard IOs can each be tagged OFF / FLAG / TRIGGER. Flag edges
    emit USB MARKER records (rendered as vertical lines on the host); trigger
    edges start the acquisition window. HV IOs (3/6/9/12) may use an analog
    voltage threshold instead of a digital level. Triggers combine with OR / AND.
    """

    def __init__(self, client) -> None:
        self._c = client

    def set_io(
        self,
        io: int,
        role: DaqTrigRole,
        edge: DaqTrigEdge = DaqTrigEdge.RISING,
        source: DaqTrigSource = DaqTrigSource.DIGITAL,
        threshold_v: float = 1.5,
    ) -> None:
        """Configure one IO (1..12). ``source``/``threshold_v`` only apply to
        the analog-capable HV IOs (3/6/9/12)."""
        if not (1 <= io <= 12):
            raise ValueError(f"io must be 1..12, got {io}")
        if source == DaqTrigSource.ANALOG and not daq_io_is_analog_capable(io):
            raise ValueError(f"IO{io} is digital-only; analog source needs IO 3/6/9/12")
        payload = bytes([DaqTrigOp.SET_IO, io, int(role), int(edge), int(source), 0])
        payload += struct.pack("<f", float(threshold_v))
        self._c._usb_cmd(CmdId.DAQ_TRIG, payload)

    def get_io(self, io: int) -> Dict[str, Any]:
        """Read one IO's trigger/flag configuration."""
        if not (1 <= io <= 12):
            raise ValueError(f"io must be 1..12, got {io}")
        resp = self._c._usb_cmd(CmdId.DAQ_TRIG, bytes([DaqTrigOp.GET_IO, io]))
        role, edge, source, _pad, thr = struct.unpack_from("<BBBBf", resp, 0)
        return {
            "io": io,
            "role": DaqTrigRole(role),
            "edge": DaqTrigEdge(edge),
            "source": DaqTrigSource(source),
            "threshold_v": thr,
        }

    def set_logic(self, logic: DaqTrigLogic) -> None:
        """Set the trigger-group combination logic (OR / AND)."""
        self._c._usb_cmd(CmdId.DAQ_TRIG, bytes([DaqTrigOp.SET_LOGIC, int(logic)]))

    def arm(self, armed: bool = True, pre_samples: int = 0) -> None:
        """Arm or disarm the trigger latch with a pre-trigger depth in samples."""
        payload = bytes([DaqTrigOp.ARM, 1 if armed else 0, 0])
        payload += struct.pack("<I", int(pre_samples) & 0xFFFFFFFF)
        self._c._usb_cmd(CmdId.DAQ_TRIG, payload)

    def status(self) -> Dict[str, Any]:
        """Return {logic, armed, fired}."""
        resp = self._c._usb_cmd(CmdId.DAQ_TRIG, bytes([DaqTrigOp.STATUS]))
        return {
            "logic": DaqTrigLogic(resp[0]),
            "armed": bool(resp[1]),
            "fired": bool(resp[2]),
        }

    def get_all(self) -> Dict[str, Any]:
        """Return {logic, armed, fired, ios:[12 configs]}."""
        resp = self._c._usb_cmd(CmdId.DAQ_TRIG, bytes([DaqTrigOp.GET_ALL]))
        out: Dict[str, Any] = {
            "logic": DaqTrigLogic(resp[0]),
            "armed": bool(resp[1]),
            "fired": bool(resp[2]),
            "ios": [],
        }
        off = 4
        for io in range(1, 13):
            role, edge, source, _pad, thr = struct.unpack_from("<BBBBf", resp, off)
            out["ios"].append({
                "io": io,
                "role": DaqTrigRole(role),
                "edge": DaqTrigEdge(edge),
                "source": DaqTrigSource(source),
                "threshold_v": thr,
            })
            off += 8
        return out

