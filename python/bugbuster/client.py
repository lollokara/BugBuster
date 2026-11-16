"""
BugBuster — high-level Python client.

Provides a single ``BugBuster`` class that works identically over both
transport backends (USB binary and HTTP).  Instantiate via the factory
functions at the bottom of this file::

    from bugbuster import connect_usb, connect_http

    # USB (binary protocol, low latency, supports ADC streaming)
    bb = connect_usb("/dev/ttyACM0")

    # HTTP (WiFi REST API, no streaming, good for remote or ad-hoc use)
    bb = connect_http("192.168.4.1")

    with bb:
        bb.set_dac_voltage(0, 5.0)
        print(bb.get_adc_value(0))
"""

import hashlib
import struct
import logging
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Optional

import serial  # pyserial - needed for SerialException in drain-loop guard

from .transport.usb  import USBTransport
from .transport.http import HTTPTransport
from .transport.protocol import Transport

if TYPE_CHECKING:
    # Imported lazily: bugbuster.script imports this module, so a runtime
    # import here would be circular.
    from .memory import MemoryStatus
    from .hal import BugBusterHAL
    from .bus import BugBusterBusManager


class BugBusterWarning(UserWarning):
    """General-purpose warning for the BugBuster host-side library."""


PD_LIMITED_VADJ_MAX_V = 5.0
PD_LIMITED_VADJ_TOLERANCE_V = 0.05


def _pd_limited_vadj_warning(status: dict, rail: int, voltage: float) -> Optional[str]:
    """Return a user-facing warning when VADJ1/2 is requested above the
    voltage supported by the currently negotiated USB-C PD/input supply.

    VADJ1 and VADJ2 are generated from the main USB-C input by buck supplies.
    If the board is powered only from the data USB port, or the PD controller
    has no/only a 5 V contract, those rails cannot rise meaningfully above
    about 5 V. With a higher PD contract, VADJ1/VADJ2 still cannot be set
    above the negotiated input voltage because they are buck rails. HAT
    VADJ3/VADJ4 are buck-boost rails and are intentionally not covered by this
    guard.
    """
    if rail not in (1, 2) or voltage <= PD_LIMITED_VADJ_MAX_V + PD_LIMITED_VADJ_TOLERANCE_V:
        return None

    pd_present = bool(status.get("present"))
    attached = bool(status.get("attached"))
    negotiated_v = float(status.get("voltage_v") or 0.0)
    detected_pdos = [pdo for pdo in status.get("pdos", []) if isinstance(pdo, dict) and pdo.get("detected")]
    higher_pdo_detected = len(detected_pdos) > 1

    negotiated_ceiling = negotiated_v if negotiated_v > 0 else PD_LIMITED_VADJ_MAX_V
    if (
        (not pd_present)
        or (not attached)
        or voltage > negotiated_ceiling + PD_LIMITED_VADJ_TOLERANCE_V
    ):
        if not pd_present:
            reason = "USB-C PD controller is not detected"
        elif not attached:
            reason = "USB-C PD power port is not attached"
        elif negotiated_v > 0:
            reason = f"negotiated USB-C input is only {negotiated_v:.1f} V"
        elif not higher_pdo_detected:
            reason = "no higher-voltage USB-C PD profile is available or negotiated"
        else:
            reason = "USB-C PD voltage is unknown"
        return (
            f"Requested VADJ{rail}={voltage:.2f} V, but {reason}. "
            "VADJ1/VADJ2 are buck rails and cannot regulate above the active USB-C input voltage "
            "(about 5 V when powered from the data USB port or a 5 V/no-PD source). "
            "Connect/select a high-enough PD profile before requesting higher VADJ1/VADJ2 voltages; "
            "HAT VADJ3/VADJ4 are buck-boost rails and are unaffected."
        )
    return None
from .constants import (
    CmdId, ChannelFunction, PowerControl,
)
from .protocol import ProtocolError
from .standby import (
    PresenceSession, StandbyRefusedError, StandbyStatus, StandbyUnsupportedError,
    encode_policy, encode_presence, encode_sleep, encode_status_request, encode_wake,
    is_refusal_error, is_unsupported_error, parse_standby_status, parse_standby_status_json,
    validate_timeout,
)

log = logging.getLogger(__name__)

# TR-7: commands that are safe to resend after a timeout - pure reads with no
# device-side effect. Derived from names so a new GET_/_STATUS command is
# covered; EXT_* bus transactions are excluded (a bus read is still a bus
# transaction), as are draining reads such as SCRIPT_LOGS.
_RETRY_SAFE_CMDS = frozenset(
    int(c) for n, c in CmdId.__members__.items()
    if not n.startswith("EXT_") and (
        n == "PING" or n.startswith("GET_")
        or n.endswith(("_STATUS", "_LIST", "_INFO", "_GET", "_GET_ALL"))
        or "_GET_" in n)
)


def _require_resp_len(resp: bytes, min_len: int, cmd_name: str) -> None:
    """Raise ProtocolError if *resp* is shorter than *min_len* bytes."""
    if len(resp) < min_len:
        raise ProtocolError(
            f"{cmd_name}: response too short - got {len(resp)} bytes, expected >= {min_len}"
        )


def _i2c_addr7(address: int) -> int:
    """Validate a 7-bit I2C address. Masking an 8-bit one hits another device."""
    if not 0 <= address <= 0x7F:
        hint = f" - looks like an 8-bit address, use 0x{address >> 1:02X}" if address <= 0xFF else ""
        raise ValueError(f"I2C address 0x{address:X} is not 7-bit{hint}")
    return address


def ble_passkey(admin_token: str) -> str:
    """Six-digit BLE pairing passkey for a device with this admin token.

    Mirrors ``ble_pairing_passkey()`` in ``net/ble_service.cpp``.
    """
    digest = hashlib.sha256(admin_token.encode() + b"bb-ble-passkey").digest()
    return f"{int.from_bytes(digest[:4], 'big') % 1_000_000:06d}"


# ---------------------------------------------------------------------------
# Small dataclass-like result types  (plain named tuples are fine here)
# ---------------------------------------------------------------------------

from collections import namedtuple

AdcResult   = namedtuple("AdcResult",   ["raw", "value", "range", "rate", "mux"])
AdcDspWindow = namedtuple("AdcDspWindow", [
    "channel",        # int: logical channel 0–3
    "timestamp_us",   # int: window start (µs, device clock)
    "n_samples",      # int: samples in window
    "min_v",          # float: minimum voltage in window
    "max_v",          # float: maximum voltage
    "mean_v",         # float: mean voltage
    "rms_v",          # float: RMS voltage
    "fft_peaks",      # list of (bin_index, magnitude) tuples, sorted by magnitude desc
    "spikes",         # list of (offset_us, value) tuples for samples above threshold
])
DeviceInfo  = namedtuple("DeviceInfo",  ["spi_ok", "silicon_rev", "silicon_id0", "silicon_id1"])
PingResult  = namedtuple("PingResult",  ["token", "uptime_ms"])
GpioStatus  = namedtuple("GpioStatus",  ["id", "mode", "output", "input", "pulldown"])
IdacChannel        = namedtuple("IdacChannel",        ["code", "target_v", "actual_v", "v_min", "v_max", "calibrated"])

# One IDAC_GET_STATUS channel record, matching the writes in
# Firmware/ESP32/src/bbp/cmds/cmd_idac.cpp:handler_idac_get_status():
#   u8 ch, i8 dac_code, f32 target_v, f32 actual_v, f32 midpoint_v,
#   f32 v_min, f32 v_max, f32 step_mv, u8 cal_valid, u8 have_poly, f32 poly[4]
_IDAC_CH_FMT = "<BbffffffBB4f"
_IDAC_CH_LEN = struct.calcsize(_IDAC_CH_FMT)
ScriptStatusResult = namedtuple("ScriptStatusResult",
    ["is_running", "script_id", "total_runs", "total_errors", "last_error",
     "mode", "globals_bytes_est", "globals_count", "auto_reset_count",
     "last_eval_at_ms", "idle_for_ms", "watermark_soft_hit",
     # runtime v2 (HTTP only; empty over USB)
     "name", "source", "state", "last_exit", "started_at", "file_slot_id"],
    defaults=(0, 0, 0, 0, 0, 0, False, "", "", "", "", 0, 0))


class ScriptBusyError(RuntimeError):
    """The device refused: a script holds the single run slot (HTTP 409)."""

    def __init__(self, running: str, script_id: int):
        super().__init__(f"script {running!r} (id {script_id}) is running; "
                         "pass replace=True to stop it")
        self.running = running
        self.script_id = script_id


def _script_busy_error(exc: Exception) -> Optional[ScriptBusyError]:
    resp = getattr(exc, "response", None)
    if resp is None or getattr(resp, "status_code", None) != 409:
        return None
    try:
        data = resp.json()
    except ValueError:
        data = {}
    return ScriptBusyError(str(data.get("running", "")), int(data.get("id", 0)))
AutorunStatus      = namedtuple("AutorunStatus",      ["enabled", "has_script", "io12_high", "last_run_ok", "last_run_id"])


@dataclass(frozen=True)
class ApPasswordResult:
    """Result of :meth:`BugBuster.wifi_set_ap_password`. Truthy when applied."""
    applied: bool
    persisted: bool   # False: live now, reverts to the old password on reboot

    def __bool__(self) -> bool:
        return self.applied


@dataclass(frozen=True)
class EfuseImonStatus:
    """E-fuse IMON monitor state. ``efuse`` 0 = monitor off, 1..4 = logical EFUSE1..4."""
    result: str          # "ok" or the firmware result name
    efuse: int
    valid: bool          # False while settling or after an AD74416H reset
    saturated: bool      # IMON >= 2.45 V (about 4.5 A)
    efuse_on: bool
    imon_v: float
    current_ma: float


_EFUSE_IMON_RESULTS = ("ok", "needs_confirm", "busy", "invalid",
                       "slot_held", "hw_fail", "unsupported")
_EFUSE_IMON_BLOCK_FMT = "<BBff"
_EFUSE_IMON_BLOCK_LEN = struct.calcsize(_EFUSE_IMON_BLOCK_FMT)


# ---------------------------------------------------------------------------
# HTTP compatibility helpers
# ---------------------------------------------------------------------------

def _first_present(mapping: dict, *keys, default=None):
    for key in keys:
        if key in mapping and mapping[key] is not None:
            return mapping[key]
    return default


def _parse_int_maybe_hex(value, default=0) -> int:
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            return default
    return default


def _channel_function_from_http(entry: dict) -> int:
    value = _first_present(entry, "function", "function_code", "functionCode")
    if isinstance(value, str):
        try:
            return int(ChannelFunction[value])
        except KeyError:
            return 0
    return _parse_int_maybe_hex(value, 0)


def _normalize_http_status(raw: dict) -> dict:
    channels = []
    for ch in raw.get("channels", []):
        channels.append({
            "id": _parse_int_maybe_hex(_first_present(ch, "id"), 0),
            "function": _channel_function_from_http(ch),
            "function_name": _first_present(ch, "function_name", "functionName")
                             or (_first_present(ch, "function") if isinstance(_first_present(ch, "function"), str) else None),
            "adc_raw": _parse_int_maybe_hex(_first_present(ch, "adc_raw", "raw_code", "adcRaw"), 0),
            "adc_value": float(_first_present(ch, "adc_value", "value", "adcValue", default=0.0) or 0.0),
            "adc_range": _parse_int_maybe_hex(_first_present(ch, "adc_range", "range", "adcRange"), 0),
            "adc_rate": _parse_int_maybe_hex(_first_present(ch, "adc_rate", "rate", "adcRate"), 0),
            "adc_mux": _parse_int_maybe_hex(_first_present(ch, "adc_mux", "mux", "adcMux"), 0),
            "dac_code": _parse_int_maybe_hex(_first_present(ch, "dac_code", "dacCode"), 0),
            "dac_value": float(_first_present(ch, "dac_value", "dacValue", default=0.0) or 0.0),
            "din_state": bool(_first_present(ch, "din_state", "dinState", default=False)),
            "din_counter": _parse_int_maybe_hex(_first_present(ch, "din_counter", "dinCounter"), 0),
            "do_state": bool(_first_present(ch, "do_state", "doState", default=False)),
            "channel_alert": _parse_int_maybe_hex(_first_present(ch, "channel_alert", "channelAlert"), 0),
            "channel_alert_mask": _parse_int_maybe_hex(_first_present(ch, "channel_alert_mask", "channelAlertMask"), 0),
            "rtd_excitation_ua": _parse_int_maybe_hex(_first_present(ch, "rtd_excitation_ua", "rtdExcitationUa"), 0),
        })

    diagnostics = []
    for diag in raw.get("diagnostics", []):
        diagnostics.append({
            "source": _parse_int_maybe_hex(_first_present(diag, "source"), 0),
            "raw_code": _parse_int_maybe_hex(_first_present(diag, "raw_code", "rawCode", "raw"), 0),
            "value": float(_first_present(diag, "value", default=0.0) or 0.0),
        })

    return {
        "spi_ok": bool(_first_present(raw, "spi_ok", "spiOk", default=False)),
        "die_temp_c": float(_first_present(raw, "die_temp_c", "dieTemp", default=0.0) or 0.0),
        "alert_status": _parse_int_maybe_hex(_first_present(raw, "alert_status", "alertStatus"), 0),
        "alert_mask": _parse_int_maybe_hex(_first_present(raw, "alert_mask", "alertMask"), 0),
        "supply_alert_status": _parse_int_maybe_hex(_first_present(raw, "supply_alert_status", "supplyAlertStatus"), 0),
        "supply_alert_mask": _parse_int_maybe_hex(_first_present(raw, "supply_alert_mask", "supplyAlertMask"), 0),
        "live_status": _parse_int_maybe_hex(_first_present(raw, "live_status", "liveStatus"), 0),
        "channels": channels,
        "diagnostics": diagnostics,
        "mux_states": list(_first_present(raw, "mux_states", "muxStates", default=[])),
    }


def _normalize_http_faults(raw: dict) -> dict:
    channels = []
    for ch in raw.get("channels", []):
        channels.append({
            "id": _parse_int_maybe_hex(_first_present(ch, "id"), 0),
            "alert": _parse_int_maybe_hex(_first_present(ch, "alert", "channel_alert", "channelAlert"), 0),
            "mask": _parse_int_maybe_hex(_first_present(ch, "mask", "channel_alert_mask", "channelAlertMask"), 0),
        })
    return {
        "alert_status": _parse_int_maybe_hex(_first_present(raw, "alert_status", "alertStatus"), 0),
        "alert_mask": _parse_int_maybe_hex(_first_present(raw, "alert_mask", "alertMask"), 0),
        "supply_alert_status": _parse_int_maybe_hex(_first_present(raw, "supply_alert_status", "supplyAlertStatus"), 0),
        "supply_alert_mask": _parse_int_maybe_hex(_first_present(raw, "supply_alert_mask", "supplyAlertMask"), 0),
        "channels": channels,
    }


def _normalize_http_wifi_status(raw: dict) -> dict:
    return {
        "connected": bool(_first_present(raw, "connected", "sta_connected", default=False)),
        "sta_ssid": str(_first_present(raw, "sta_ssid", "staSSID", default="") or ""),
        "sta_ip": str(_first_present(raw, "sta_ip", "staIP", "ip", default="") or ""),
        "rssi": _first_present(raw, "rssi"),
        "ap_ssid": str(_first_present(raw, "ap_ssid", "apSSID", default="") or ""),
        "ap_ip": str(_first_present(raw, "ap_ip", "apIP", default="") or ""),
        "ap_mac": str(_first_present(raw, "ap_mac", "apMAC", default="") or ""),
    }


def _normalize_http_usbpd(raw: dict) -> dict:
    """Map the firmware /api/usbpd camelCase payload onto the snake_case
    shape the USB binary parser produces (``voltage_v``/``current_a``/
    ``power_w``/``pdos``), so callers can ignore the transport."""
    raw_pdos = _first_present(raw, "pdos", "sourcePdos", "source_pdos", default=[]) or []
    pdos = []
    for entry in raw_pdos:
        if not isinstance(entry, dict):
            continue
        mc = _first_present(entry, "max_current", "maxCurrentA", "maxCurrent",
                            "max_current_a", default=0) or 0
        pdos.append({
            "detected": bool(_first_present(entry, "detected", default=False)),
            "max_current": mc,
            "voltage": _first_present(entry, "voltage", default=None),
        })
    return {
        "present": bool(_first_present(raw, "present", default=False)),
        "attached": bool(_first_present(raw, "attached", default=False)),
        "cc_direction": _first_present(raw, "cc_direction", "cc", "ccDirection", default=0),
        "pd_response": _parse_int_maybe_hex(
            _first_present(raw, "pd_response", "pdResponse", default=0), 0
        ),
        "voltage_v": float(_first_present(raw, "voltage_v", "voltageV", default=0.0) or 0.0),
        "current_a": float(_first_present(raw, "current_a", "currentA", default=0.0) or 0.0),
        "power_w":   float(_first_present(raw, "power_w", "powerW", default=0.0) or 0.0),
        "pdos": pdos,
    }


def _normalize_http_hat_status(raw: dict) -> dict:
    pin_cfg = _first_present(raw, "pin_config", "pinConfig", default=[])
    if pin_cfg and isinstance(pin_cfg[0], dict):
        pin_cfg = [_parse_int_maybe_hex(_first_present(p, "function"), 0) for p in pin_cfg]
    return {
        "detected": bool(_first_present(raw, "detected", default=False)),
        "connected": bool(_first_present(raw, "connected", default=False)),
        "type": _parse_int_maybe_hex(_first_present(raw, "type"), 0),
        "detect_voltage": float(_first_present(raw, "detect_voltage", "detectVoltage", default=0.0) or 0.0),
        "fw_version": _first_present(raw, "fw_version")
                      or f"{_parse_int_maybe_hex(_first_present(raw, 'fw_major', 'fwMajor'), 0)}.{_parse_int_maybe_hex(_first_present(raw, 'fw_minor', 'fwMinor'), 0)}",
        "config_confirmed": bool(_first_present(raw, "config_confirmed", "configConfirmed", default=False)),
        "pin_config": list(pin_cfg or []),
    }


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class HatNotPresentError(RuntimeError):
    """
    Raised when an operation requires the BugBuster HAT expansion board
    but no HAT has been detected on the device.

    The Logic Analyzer and SWD debug functions live on the RP2040 on the
    HAT, so they cannot work on a bare BugBuster board.  Connect the HAT
    and call :meth:`BugBuster.hat_detect` to refresh the cached presence
    state, then retry.
    """


class EfuseImonConfirmRequired(RuntimeError):
    """
    Raised by :meth:`BugBuster.efuse_imon_set` when attaching or detaching the
    monitor would interrupt an e-fuse that is currently ON.  The e-fuse is
    power-cycled (off, 200 ms, on); retry with ``confirm_power_cycle=True``.
    """


class HatPinFunctionError(ValueError):
    """
    Raised when :meth:`BugBuster.hat_set_pin` is called with a function
    code that is no longer assignable to an EXP_EXT pin.

    Numeric slots ``1``, ``2``, ``3``, ``4`` are reserved for wire-
    protocol compatibility — they used to be ``SWDIO``, ``SWCLK``,
    ``TRACE1``, ``TRACE2``.  SWD now lives on the dedicated 3-pin HAT
    connector (``SWDIO``/``SWCLK``/``TRACE``) driven directly by the
    RP2040 debugprobe pins.  To enable SWD, call
    :meth:`BugBuster.hat_setup_swd` instead — it configures target
    voltage and power without touching any EXP_EXT pin.
    """


# ---------------------------------------------------------------------------
# Main client
# ---------------------------------------------------------------------------

class _ClientCore:
    """Connection lifecycle and transport helpers shared by the BugBuster mixins."""

    def __init__(self, transport: Transport, *, presence: bool = True):
        self._t: Any                   = transport
        self._usb                      = isinstance(transport, USBTransport)
        self._connected                = False
        self._presence_enabled         = presence
        self._presence: Optional[PresenceSession] = None
        self._presence_gen: Any        = None
        self._hal: Optional['BugBusterHAL'] = None
        self._bus: Optional['BugBusterBusManager'] = None
        # Cached HAT presence: None = unknown (probe on demand), bool = known
        self._hat_present_cache: Optional[bool] = None
        self._admin_token: Optional[str]        = None
        # Unit-testable pre-send hook: set to a callable to inject failures.
        # Cleared automatically after each fire (one-shot).
        self._usb_pre_send_hook = None
        # IO ownership tracking: set of slot indices currently claimed via
        # io_claim() context manager (None = no active context).
        self._io_claimed_slots: Optional[set] = None

    def get_admin_token(self) -> str:
        """
        Retrieve the hardware-derived admin token.  USB only.
        Used to authorize destructive operations over HTTP.
        """
        if self._admin_token:
            return self._admin_token
        
        self._require_usb("get_admin_token")
        resp = self._usb_cmd(CmdId.GET_ADMIN_TOKEN)
        _require_resp_len(resp, 2, "GET_ADMIN_TOKEN")
        length = resp[0]
        token = resp[1:1+length].decode('ascii')
        self._admin_token = token
        return token

    @property
    def hal(self):
        """
        Lazy-initialized Hardware Abstraction Layer.

        Returns a :class:`~bugbuster.hal.BugBusterHAL` instance bound to this
        client.  The HAL is created on first access — call ``bb.hal.begin()``
        before using it.
        """
        if self._hal is None:
            from .hal import BugBusterHAL
            self._hal = BugBusterHAL(self)
        return self._hal

    @property
    def bus(self):
        """
        Lazy-initialized external I2C/SPI bus manager.

        The current implementation exposes side-effect-free route planning so
        callers can preview the power, MUX, and ESP32 GPIO mapping before the
        firmware-owned timing engine is enabled.
        """
        if self._bus is None:
            from .bus import BugBusterBusManager
            self._bus = BugBusterBusManager(self)
        return self._bus

    def bus_plan(self, kind: str, **kwargs) -> dict:
        """Return a serializable I2C/SPI route plan without touching hardware."""
        kind = kind.lower()
        if kind == "i2c":
            return self.bus.plan_i2c(**kwargs).as_dict()
        if kind == "spi":
            return self.bus.plan_spi(**kwargs).as_dict()
        raise ValueError("kind must be 'i2c' or 'spi'")

    # ------------------------------------------------------------------
    # Connection lifecycle
    # ------------------------------------------------------------------

    def connect(self):
        """Open the connection and perform the protocol handshake."""
        if not self._connected:
            self._t.connect()
            self._connected = True
            if self._usb:
                try:
                    self.get_admin_token()
                except Exception:
                    log.warning("Connected via USB but failed to retrieve admin token")
            self._presence_start()
        return self

    def disconnect(self):
        """Close the connection cleanly."""
        if self._connected:
            self._presence_stop()
            self._t.disconnect()
            self._connected = False

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.disconnect()

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    @property
    def daq(self) -> "DaqConfig":  # type: ignore[name-defined]  # noqa: F821
        """DAQ HAT (ESP32-P4) settings registry accessor (USB transport).

        Example::

            bb.daq.set(DaqKey.DUT_VOLTAGE_MV, 3300)
            bb.daq.get_all()
        """
        from .daq_config import DaqConfig
        return DaqConfig(self)

    @property
    def daq_trigger(self) -> "DaqTrigger":  # type: ignore[name-defined]  # noqa: F821
        """DAQ trigger / flag engine accessor (S3 mainboard, USB transport).

        Example::

            from bugbuster.daq_config import DaqTrigRole, DaqTrigEdge
            bb.daq_trigger.set_io(3, DaqTrigRole.FLAG, DaqTrigEdge.ANY)
            bb.daq_trigger.arm(True, pre_samples=12500)
        """
        from .daq_config import DaqTrigger
        return DaqTrigger(self)

    @property
    def battsim(self) -> "BattSim":  # type: ignore[name-defined]  # noqa: F821
        """DAQ HAT battery simulator: status, runs, history download (USB or HTTP)."""
        from .battsim import BattSim
        return BattSim(self)

    def _usb_cmd(self, cmd_id: int, payload: bytes = b'') -> bytes:
        """
        Send a binary command and return the raw response payload.

        On ``TimeoutError`` an idempotent read (``_RETRY_SAFE_CMDS``) is
        retried once after a 50 ms drain; anything else re-raises at once.
        If the retry also times out the original exception is re-raised.

        A unit-testable pre-send hook is available: set
        ``client._usb_pre_send_hook`` to a callable that is invoked (with no
        arguments) before each ``send_command`` call.  The hook may raise to
        simulate transport failures.  After firing, the hook is cleared to
        None so it triggers only once per installation.
        """
        def _attempt():
            hook = self._usb_pre_send_hook
            if hook is not None:
                self._usb_pre_send_hook = None
                hook()
            return self._t.send_command(cmd_id, payload)

        try:
            return _attempt()
        except TimeoutError as exc:
            # Drain any stale bytes from the serial input buffer (best-effort).
            # NOTE: self._t._port is the device path string; self._t._serial
            # is the pyserial Serial instance. We accept either via duck-typing
            # because reading from the live Serial would race the RX worker
            # thread. If the attribute does not expose .read(), drain is a
            # safe no-op.
            _port = getattr(self._t, '_port', None)
            if _port is not None and hasattr(_port, 'read'):
                import time as _time
                deadline = _time.monotonic() + 0.05
                while _time.monotonic() < deadline:
                    try:
                        if not _port.read(256):
                            break
                    except (OSError, serial.SerialException) as _drain_exc:
                        log.debug("Drain read error (ignored): %s", _drain_exc)
                        break
            # Single retry - only for idempotent reads (TR-7). A timed-out
            # write usually DID run on the device; resending it would run it
            # twice (I2C/SPI writes, SCRIPT_EVAL, power commands).
            if cmd_id not in _RETRY_SAFE_CMDS:
                raise
            try:
                return _attempt()
            except TimeoutError:
                # Surface the original timeout; the retry's is redundant noise.
                raise exc from None

    def _http_get(self, path: str, **params) -> dict:
        return self._t.get(path, params=params or None)

    def _http_post(self, path: str, body: Optional[dict] = None) -> dict:
        headers = {}
        if self._admin_token:
            headers["X-BugBuster-Admin-Token"] = self._admin_token
        return self._t.post(path, body, headers=headers)

    def _require_usb(self, method: str):
        if not self._usb:
            raise NotImplementedError(f"{method} is only available over USB")

    def adc_leds_set_mode(self, manual: bool) -> bool:
        """
        Choose who drives the ADC status LEDs. **USB only.**

        ``manual=False`` hands them back to the firmware's automatic display.
        Any GPIO write (``SET_GPIO_VALUE``) switches them to manual, and this
        is the only command that switches them back.

        :return: the mode the device applied (True = manual).
        """
        self._require_usb("adc_leds_set_mode")
        resp = self._usb_cmd(CmdId.ADC_LEDS_SET_MODE, bytes([1 if manual else 0]))
        _require_resp_len(resp, 1, "ADC_LEDS_SET_MODE")
        return bool(resp[0])

    # ------------------------------------------------------------------
    # ── Device ──────────────────────────────────────────────────────────
    # ------------------------------------------------------------------

    def ping(self, token: int = 0xDEADBEEF) -> PingResult:
        """
        Ping the device.  USB only.
        Returns the echoed token and device uptime in milliseconds.
        """
        self._require_usb("ping")
        payload = struct.pack('<I', token & 0xFFFFFFFF)
        resp    = self._usb_cmd(CmdId.PING, payload)
        _require_resp_len(resp, 8, "PING")
        tok, uptime = struct.unpack_from('<II', resp)
        return PingResult(token=tok, uptime_ms=uptime)


    # ------------------------------------------------------------------
    # ── System standby ──────────────────────────────────────────────────
    # ------------------------------------------------------------------

    def _standby_call(self, payload: bytes, http_path: str,
                      http_body: Optional[dict] = None, *, get: bool = False) -> StandbyStatus:
        """One standby request over the active transport -> status record."""
        try:
            if self._usb:
                return parse_standby_status(self._usb_cmd(CmdId.STANDBY, payload))
            if get:
                return parse_standby_status_json(self._http_get(http_path))
            return parse_standby_status_json(self._http_post(http_path, http_body or {}))
        except Exception as exc:
            if is_unsupported_error(exc):
                raise StandbyUnsupportedError(
                    "this firmware has no system standby (BBP 0x78 / /api/standby)") from exc
            raise

    def standby_status(self) -> StandbyStatus:
        """Standby state, readiness, policy timeout, client/inhibitor counts.

        Raises :class:`StandbyUnsupportedError` on firmware without standby.
        """
        return self._standby_call(encode_status_request(), "/standby/status", get=True)

    def standby_set_timeout(self, seconds: int) -> StandbyStatus:
        """Persist the automatic standby timeout on the device: 0 (off), 60, 300 or 900 s.

        The setting lives on the mainboard, not on this host.
        """
        seconds = validate_timeout(seconds)
        return self._standby_call(encode_policy(seconds), "/standby/policy",
                                  {"timeoutSeconds": seconds})

    def standby_wake(self) -> StandbyStatus:
        """Wake the device and re-attach this client's presence if it was detached.

        Hardware commands answer BUSY until ``standby_status().ready``; nothing is
        replayed automatically, and outputs stay off until you enable them.
        """
        presence = self._presence
        if presence is not None and presence.detached:
            presence.attach()
        return self._standby_call(encode_wake(), "/standby/wake")

    def standby_sleep(self, *, release_client: bool = True) -> StandbyStatus:
        """Ask the device to enter standby now (DUT-facing rails are switched off).

        A connected client inhibits standby, so by default this client's presence
        is released first and stays released (no heartbeat) until
        :meth:`standby_wake` / :meth:`standby_attach`. Real owners (scripts,
        capture, other clients) still refuse with :class:`StandbyRefusedError`
        (``.status`` has the current state); the release is not undone.
        """
        if release_client:
            self.standby_detach()
        try:
            return self._standby_call(encode_sleep(), "/standby/sleep")
        except StandbyUnsupportedError:
            raise
        except Exception as exc:
            if not is_refusal_error(exc):
                raise
            raise StandbyRefusedError(
                "device refused standby: a client or running work still owns it",
                self.standby_status()) from exc

    def standby_wait_ready(self, timeout_s: float = 10.0, poll_s: float = 0.25) -> StandbyStatus:
        """Poll until the device is ready for hardware commands; ``TimeoutError`` otherwise."""
        import time as _time
        deadline = _time.monotonic() + timeout_s
        while True:
            status = self.standby_status()
            if status.ready:
                return status
            if _time.monotonic() >= deadline:
                raise TimeoutError(
                    f"device not ready after {timeout_s:.1f}s (state={status.state_name}, stage={status.stage})")
            _time.sleep(poll_s)

    def standby_detach(self) -> None:
        """Release this client's presence and stop its heartbeat (no reopen until attach/wake)."""
        if self._presence is not None:
            self._presence.detach()

    def standby_attach(self) -> bool:
        """Re-register this client's presence. A new presence wakes a sleeping device."""
        return self._presence is not None and self._presence.attach()

    @property
    def standby_client_id(self) -> int:
        """This connection epoch's presence id (0 when closed, detached or unsupported)."""
        return self._presence.client_id if self._presence is not None else 0

    @property
    def standby_capability(self) -> str:
        """'supported', 'unsupported', 'unknown', or 'disabled' (presence=False)."""
        return self._presence.capability if self._presence is not None else "disabled"

    def _standby_presence_send(self, client_id: int, present: bool) -> Optional[StandbyStatus]:
        if self._usb:
            is_healthy = getattr(self._t, "is_healthy", None)
            if callable(is_healthy) and not is_healthy():
                raise ConnectionError("USB link is down")
            return parse_standby_status(
                self._usb_cmd(CmdId.STANDBY, encode_presence(client_id, present)))
        reply = self._http_post("/standby/presence", {"clientId": client_id, "present": present})
        try:
            return parse_standby_status_json(reply)
        except (ValueError, TypeError):
            return None

    def _presence_start(self) -> None:
        if not self._presence_enabled:
            return
        session = PresenceSession(self._standby_presence_send)
        self._presence = session
        self._presence_gen = getattr(self._t, "connect_gen", None)
        session.open()
        hook = getattr(self._t, "set_presence_hook", None)
        if callable(hook) and session.capability != "unsupported":
            hook(self._presence_tick)

    def _presence_tick(self) -> None:
        session = self._presence
        if session is None:
            return
        gen = getattr(self._t, "connect_gen", None)
        if gen != self._presence_gen:
            self._presence_gen = gen
            if session.active:
                session.new_epoch()
            return
        session.tick()

    def _presence_stop(self) -> None:
        hook = getattr(self._t, "set_presence_hook", None)
        if callable(hook):
            hook(None)
        session, self._presence = getattr(self, "_presence", None), None
        if session is not None:
            session.close()

    def get_firmware_version(self) -> tuple:
        """
        Return ``(major, minor, patch)`` from the active connection.
        USB reads from the handshake; HTTP fetches ``/api/device/version``.
        """
        if self._usb:
            return self._t.fw_version
        info = self._http_get("/device/version")
        return info["fwMajor"], info["fwMinor"], info["fwPatch"]

    def get_mac_address(self) -> Optional[str]:
        """
        Return the device MAC address as ``"aa:bb:cc:dd:ee:ff"``, or ``None``
        if unavailable.

        USB: taken from the 14-byte BBP v4 handshake response. Legacy
        firmware that returns only 8 bytes yields ``None``.

        HTTP: read from ``/api/device/info`` (``macAddress`` / ``mac_address``
        field). Legacy firmware that omits the field yields ``None``.
        """
        if self._usb:
            mac = getattr(self._t, "mac", None)
            if not mac:
                return None
            return ":".join(f"{b:02x}" for b in mac)
        info = self._http_get("/device/info")
        mac = _first_present(info, "macAddress", "mac_address", default=None)
        if not mac or mac == "00:00:00:00:00:00":
            return None
        return str(mac)

    def get_device_info(self) -> DeviceInfo:
        """Read silicon identification (revision + ID words)."""
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_DEVICE_INFO)
            _require_resp_len(resp, 6, "GET_DEVICE_INFO")
            spi_ok, rev = struct.unpack_from('<BB', resp)
            id0, id1    = struct.unpack_from('<HH', resp, 2)
            return DeviceInfo(bool(spi_ok), rev, id0, id1)
        else:
            d = self._http_get("/device/info")
            return DeviceInfo(
                bool(_first_present(d, "spi_ok", "spiOk", default=True)),
                _parse_int_maybe_hex(_first_present(d, "silicon_rev", "siliconRev"), 0),
                _parse_int_maybe_hex(_first_present(d, "silicon_id0", "siliconId0"), 0),
                _parse_int_maybe_hex(_first_present(d, "silicon_id1", "siliconId1"), 0),
            )

    def get_status(self) -> dict:
        """
        Return a full device status snapshot.

        Keys: ``spi_ok``, ``die_temp_c``, ``alert_status``, ``supply_alert_status``,
        ``channels`` (list of 4 dicts), ``diagnostics`` (list of 4 dicts).
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_STATUS)
            return _parse_status(resp)
        else:
            return _normalize_http_status(self._http_get("/status"))

    def get_faults(self) -> dict:
        """Return global and per-channel fault/alert registers."""
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_FAULTS)
            return _parse_faults(resp)
        else:
            return _normalize_http_faults(self._http_get("/faults"))

    def get_diagnostics(self) -> list[dict]:
        """
        Read the four AD74416H diagnostic multiplexer slots.

        Each slot is routed to a source by :meth:`set_diag_config`; this
        returns what those slots currently measure.

        :return: four dicts with keys ``slot``, ``source``, ``raw``, ``value``.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_DIAGNOSTICS)
            _require_resp_len(resp, 32, "GET_DIAGNOSTICS")
            out = []
            for i in range(4):
                slot, source, raw, value = struct.unpack_from('<BBHf', resp, i * 8)
                out.append({"slot": slot, "source": source,
                            "raw": raw, "value": value})
            return out
        raw = self._http_get("/diagnostics")
        if isinstance(raw, dict):
            items = _first_present(raw, "slots", "diagnostics", default=[])
        else:
            items = raw
        out = []
        for i, d in enumerate(items or []):
            out.append({
                "slot":   int(_first_present(d, "slot", default=i) or i),
                "source": int(_first_present(d, "source", default=0) or 0),
                "raw":    int(_first_present(d, "raw", "rawCode", "raw_code",
                                             default=0) or 0),
                "value":  float(_first_present(d, "value", default=0.0) or 0.0),
            })
        return out

    def get_memory_status(self) -> "MemoryStatus":
        """
        Return live memory pressure for the ESP32-S3 mainboard.

        Splits internal SRAM from PSRAM, reports the largest contiguous block
        in each, and gives per-task stack headroom. Prefer this over
        :meth:`get_status`'s ``free_heap``: that figure sums both pools, so on
        a PSRAM board it stays healthy-looking while internal RAM — the pool
        that actually runs out — is nearly exhausted.

        Works over USB (BBP) and HTTP.
        """
        from .memory import parse_mem_status, parse_mem_status_json
        if self._usb:
            return parse_mem_status(self._usb_cmd(CmdId.MEM_STATUS))
        return parse_mem_status_json(self._http_get("/system/memory"))

# ---------------------------------------------------------------------------
# Response parsers for binary protocol
# ---------------------------------------------------------------------------

def _parse_status(resp: bytes) -> dict:
    spi_ok = bool(resp[0])
    die_temp, = struct.unpack_from('<f', resp, 1)
    alert_status, alert_mask, supply_alert_status, supply_alert_mask, live_status = \
        struct.unpack_from('<HHHHH', resp, 5)

    channels = []
    for i in range(4):
        off = 15 + i * 30  # 30 bytes per channel (added channelAlertMask)
        ch_id, func   = struct.unpack_from('<BB', resp, off)
        raw            = int.from_bytes(resp[off+2:off+5], 'little')
        adc_val,       = struct.unpack_from('<f', resp, off + 5)
        adc_rng, adc_rate, adc_mux = struct.unpack_from('<BBB', resp, off + 9)
        dac_code,      = struct.unpack_from('<H', resp, off + 12)
        dac_val,       = struct.unpack_from('<f', resp, off + 14)
        din_state      = bool(resp[off + 18])
        din_counter,   = struct.unpack_from('<I', resp, off + 19)
        do_state       = bool(resp[off + 23])
        ch_alert,      = struct.unpack_from('<H', resp, off + 24)
        ch_alert_mask, = struct.unpack_from('<H', resp, off + 26)
        rtd_ua,        = struct.unpack_from('<H', resp, off + 28)
        channels.append({
            "id": ch_id, "function": func,
            "adc_raw": raw, "adc_value": adc_val,
            "adc_range": adc_rng, "adc_rate": adc_rate, "adc_mux": adc_mux,
            "dac_code": dac_code, "dac_value": dac_val,
            "din_state": din_state, "din_counter": din_counter,
            "do_state": do_state, "channel_alert": ch_alert,
            "channel_alert_mask": ch_alert_mask,
            "rtd_excitation_ua": rtd_ua,
        })

    diagnostics = []
    for i in range(4):
        off     = 135 + i * 7  # 15 + 4*30 = 135
        src, rc = struct.unpack_from('<BH', resp, off)
        val,    = struct.unpack_from('<f', resp, off + 3)
        diagnostics.append({"source": src, "raw_code": rc, "value": val})

    # MUX state: 4 bytes at offset 163 (bbp-protocol.md §GET_STATUS).
    # Older firmware returns a 163-byte response without this block — degrade
    # gracefully to an empty list so clients don't crash mid-upgrade.
    mux_states: list[int] = []
    if len(resp) >= 167:
        mux_states = list(struct.unpack_from('<BBBB', resp, 163))

    return {
        "spi_ok": spi_ok, "die_temp_c": die_temp,
        "alert_status": alert_status, "alert_mask": alert_mask,
        "supply_alert_status": supply_alert_status,
        "supply_alert_mask": supply_alert_mask,
        "live_status": live_status,
        "channels": channels, "diagnostics": diagnostics,
        "mux_states": mux_states,
    }


def _parse_faults(resp: bytes) -> dict:
    alert, alert_mask, supply, supply_mask = struct.unpack_from('<HHHH', resp)
    channels = []
    for i in range(4):
        off = 8 + i * 5
        ch_id,    = struct.unpack_from('<B', resp, off)
        ch_alert, = struct.unpack_from('<H', resp, off + 1)
        ch_mask,  = struct.unpack_from('<H', resp, off + 3)
        channels.append({"id": ch_id, "alert": ch_alert, "mask": ch_mask})
    return {
        "alert_status": alert, "alert_mask": alert_mask,
        "supply_alert_status": supply, "supply_alert_mask": supply_mask,
        "channels": channels,
    }


def _normalize_http_pca_status(raw: dict) -> dict:
    """Add the documented keys to the firmware's /api/ioexp JSON (raw keys kept)."""
    out = dict(raw)
    pg = raw.get("powerGood") or {}
    efuses = sorted(raw.get("efuses") or [], key=lambda e: e.get("id", 0))
    out.setdefault("logic_pg", bool(pg.get("logic", False)))
    out.setdefault("vadj1_pg", bool(pg.get("vadj1", False)))
    out.setdefault("vadj2_pg", bool(pg.get("vadj2", False)))
    out.setdefault("efuse_faults", [bool(e.get("fault", False)) for e in efuses] or [False] * 4)
    out.setdefault("efuse_enables", [bool(e.get("enabled", False)) for e in efuses] or [False] * 4)
    en = raw.get("enables") or {}
    for key, src in (("vadj1_en", "vadj1"), ("vadj2_en", "vadj2"), ("en_15v", "analog15v"),
                     ("en_mux", "mux"), ("en_usb_hub", "usbHub")):
        out.setdefault(key, bool(en.get(src, False)))
    return out


def _parse_pca_status(resp: bytes) -> dict:
    present        = bool(resp[0])
    input0, input1 = resp[1], resp[2]
    out0, out1     = resp[3], resp[4]
    logic_pg       = bool(resp[5])
    vadj1_pg       = bool(resp[6])
    vadj2_pg       = bool(resp[7])
    efuse_faults   = [bool(resp[8 + i]) for i in range(4)]
    # Decoded enable flags — firmware appends these 9 bytes after efuse_faults.
    # Older firmware (< 3.4.0) returned only 12 bytes; degrade gracefully.
    vadj1_en = vadj2_en = en_15v = en_mux = en_usb_hub = False
    efuse_enables: list[bool] = [False] * 4
    if len(resp) >= 21:
        vadj1_en   = bool(resp[12])
        vadj2_en   = bool(resp[13])
        en_15v     = bool(resp[14])
        en_mux     = bool(resp[15])
        en_usb_hub = bool(resp[16])
        efuse_enables = [bool(resp[17 + i]) for i in range(4)]
    return {
        "present": present, "logic_pg": logic_pg,
        "vadj1_pg": vadj1_pg, "vadj2_pg": vadj2_pg,
        "efuse_faults": efuse_faults,
        "input0": input0, "input1": input1,
        "output0": out0,  "output1": out1,
        "vadj1_en": vadj1_en, "vadj2_en": vadj2_en,
        "en_15v": en_15v, "en_mux": en_mux, "en_usb_hub": en_usb_hub,
        "efuse_enables": efuse_enables,
    }


def _parse_usbpd_status(resp: bytes) -> dict:
    present, attached, cc_dir, pd_resp, v_code, i_code = struct.unpack_from('<BBBBBB', resp)
    voltage_v, current_a, power_w = struct.unpack_from('<fff', resp, 6)
    pdos = []
    for i in range(6):
        off     = 18 + i * 2
        detected, max_i = struct.unpack_from('<BB', resp, off)
        pdos.append({"detected": bool(detected), "max_current": max_i})
    return {
        "present": bool(present), "attached": bool(attached),
        "cc_direction": cc_dir, "pd_response": pd_resp,
        "voltage_v": voltage_v, "current_a": current_a, "power_w": power_w,
        "pdos": pdos,
    }


def _parse_wifi_status(resp: bytes) -> dict:
    connected = bool(resp[0])
    off       = 1

    def read_str(b, o):
        n = b[o]; o += 1
        return b[o:o+n].decode(errors='replace'), o + n

    ssid,    off = read_str(resp, off)
    ip,      off = read_str(resp, off)
    rssi     = struct.unpack_from('<i', resp, off)[0]; off += 4   # firmware sends i32
    ap_ssid, off = read_str(resp, off)
    ap_ip,   off = read_str(resp, off)
    ap_mac,  off = read_str(resp, off)
    return {
        "connected": connected,
        "sta_ssid": ssid, "sta_ip": ip, "rssi": rssi,
        "ap_ssid": ap_ssid, "ap_ip": ap_ip, "ap_mac": ap_mac,
    }


def _parse_wifi_scan(resp: bytes) -> list[dict]:
    count = resp[0]
    off   = 1
    nets  = []
    for _ in range(count):
        n       = resp[off]; off += 1
        ssid    = resp[off:off+n].decode(errors='replace'); off += n
        rssi    = struct.unpack_from('<b', resp, off)[0]; off += 1
        auth    = resp[off]; off += 1
        nets.append({"ssid": ssid, "rssi": rssi, "auth": auth})
    return nets


# The method groups live in client_*.py as a chain of classes ending in
# _ClientHat. They import names from this module, so the chain is imported
# here, after everything above is defined.
from .client_hat import _ClientHat  # noqa: E402


class BugBuster(_ClientHat):
    """
    High-level interface to a BugBuster device.

    All public methods work with both the USB binary transport and the
    HTTP REST transport.  Methods that are USB-only (ADC streaming, raw
    register access) raise :class:`NotImplementedError` when called with
    an HTTP transport.

    Parameters
    ----------
    transport:
        An already-constructed (but not necessarily connected) transport
        object.  Use :func:`connect_usb` or :func:`connect_http` instead
        of calling this directly.
    presence:
        Register this connection with the device as a logical control client
        (blocks automatic standby while connected). Disable only for tools
        that must not keep the instrument awake.
    """

    def reset_to_defaults(self, *, admin_token: Optional[str] = None) -> list[str]:
        """
        Restore the device to its post-boot default state.

        Used by the device-test autouse teardown so every test leaves the
        unit exactly as it was at boot:
          - All 4 AD74416H channels HIGH_IMP, alert masks 0, alerts cleared.
          - PCA9535: LOGIC_EN/EN_15V_A/EN_USB_HUB/EN_MUX ON, VADJ1/VADJ2 OFF,
            e-fuses 1..4 OFF.
          - Level-shifter OE OFF.
          - All MUX switches open.
          - All 16 IO-owner slots released.

        Fail-soft: each step is independent; failures are collected and
        returned as a list of step names that errored. An empty list means
        full success.
        """
        errors: list[str] = []
        token = admin_token or self._admin_token
        if self._usb and not token:
            try:
                token = self.get_admin_token()
            except Exception as exc:  # noqa: BLE001 — reset stays fail-soft
                log.debug("reset_to_defaults admin token lookup failed: %s", exc)

        def _try(name: str, fn):
            try:
                fn()
            except Exception as exc:  # noqa: BLE001 — fail-soft by design
                log.debug("reset_to_defaults step %s failed: %s", name, exc)
                errors.append(name)

        # 0. Disable the selftest supply-monitor worker FIRST. If it stays
        # enabled, it periodically claims slot 14 (SELFTEST_CH_SLOT) as
        # IO_OWNER_INTERNAL between user commands, causing chronic 409 /
        # IO_OWNERSHIP_REQUIRED on the third channel.
        if self._usb:
            _try("selftest_worker_off",
                 lambda: self._usb_cmd(CmdId.SELFTEST_WORKER, b"\x00"))
        else:
            _try("selftest_worker_off",
                 lambda: self._http_post("/selftest/worker", {"enabled": False}))

        # 1. Release any held IO ownership so subsequent writes can proceed.
        # The firmware IO_FORCE_RELEASE handler only accepts slot indices
        # 0..15 (CMD_ERR_OUT_OF_RANGE on 0xFF), so we iterate per-slot on
        # both transports. The earlier "slot=255 = all" semantic was a
        # client-side fiction not honoured by firmware.
        if self._usb:
            if token:
                for slot in range(16):
                    _try(f"io_force_release[{slot}]",
                         lambda s=slot: self.io_force_release(s, token))
        else:
            for slot in range(16):
                _try(f"io_force_release[{slot}]",
                     lambda s=slot: self._http_post("/io/owner/force",
                                                   {"slot": s}))

        # 2. AD74416H channels → HIGH_IMP, alert masks → 0, clear alerts.
        for ch in range(4):
            _try(f"ch{ch}_high_imp",
                 lambda c=ch: self.set_channel_function(c, ChannelFunction.HIGH_IMP))
            _try(f"ch{ch}_alert_mask",
                 lambda c=ch: self.set_channel_alert_mask(c, 0))
        _try("alert_mask_global", lambda: self.set_alert_mask(0, 0))
        _try("clear_alerts", lambda: self.clear_alerts())

        # 3. Disable level-shifter OE before changing matrix/power state.
        _try("lshift_oe_off", lambda: self.set_level_shifter_oe(False))

        # 4. PCA9535: e-fuses off, VADJ rails off, fixed rails on.
        for ef in (PowerControl.EFUSE1, PowerControl.EFUSE2,
                   PowerControl.EFUSE3, PowerControl.EFUSE4):
            _try(f"{ef.name}_off", lambda c=ef: self.power_set(c, False))
        for rail_off in (PowerControl.VADJ1, PowerControl.VADJ2):
            _try(f"{rail_off.name}_off",
                 lambda c=rail_off: self.power_set(c, False))
        for rail_on in (PowerControl.V15A, PowerControl.MUX, PowerControl.USB_HUB):
            _try(f"{rail_on.name}_on",
                 lambda c=rail_on: self.power_set(c, True))

        # 5. Open all MUX switches after EN_MUX is known-on.
        _try("mux_all_open", lambda: self.mux_set_all([0, 0, 0, 0]))

        # 6. Mutating cleanup commands above may auto-claim IO slots. Force
        # release again at the end so reset never leaves fresh cleanup claims.
        if self._usb:
            if token:
                for slot in range(16):
                    _try(f"io_force_release_final[{slot}]",
                         lambda s=slot: self.io_force_release(s, token))
        else:
            for slot in range(16):
                _try(f"io_force_release_final[{slot}]",
                     lambda s=slot: self._http_post("/io/owner/force",
                                                   {"slot": s}))

        return errors



# ---------------------------------------------------------------------------
# Factory functions — preferred entry points
# ---------------------------------------------------------------------------

def connect_usb(
    port:     str,
    baudrate: int   = USBTransport.DEFAULT_BAUDRATE,
    timeout:  float = USBTransport.DEFAULT_TIMEOUT,
) -> BugBuster:
    """
    Create and connect a :class:`BugBuster` over USB binary protocol.

    *port* — serial port path, e.g. ``"/dev/ttyACM0"`` (Linux/Mac)
             or ``"COM3"`` (Windows).

    The returned object is already connected; use as a context manager to
    ensure clean disconnection::

        with connect_usb("/dev/ttyACM0") as bb:
            bb.reset()
            print(bb.get_firmware_version())
    """
    t = USBTransport(port, baudrate, timeout)
    bb = BugBuster(t)
    bb.connect()
    return bb


def connect_http(
    host:        str,
    port:        int   = 80,
    timeout:     float = 5.0,
    admin_token: Optional[str] = None,
) -> BugBuster:
    """
    Create and connect a :class:`BugBuster` over the HTTP REST API.

    *host* — IP address or hostname of the device, e.g. ``"192.168.4.1"``
             (default AP address) or a DHCP address when connected to STA.

    The returned object is already connected::

        with connect_http("192.168.4.1") as bb:
            status = bb.get_status()
            print(status["die_temp_c"])
    """
    t = HTTPTransport(host, port, timeout, admin_token=admin_token)
    bb = BugBuster(t)
    bb.connect()
    return bb
