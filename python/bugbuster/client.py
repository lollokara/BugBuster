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

import struct
import logging
from typing import Callable, Optional, Union

from .transport.usb  import USBTransport
from .transport.http import HTTPTransport
from .constants import (
    CmdId, ChannelFunction, AdcRange, AdcRate, AdcMux,
    GpioMode, WaveformType, OutputMode, RtdCurrent,
    VoutRange, CurrentLimit, PowerControl,
)

log = logging.getLogger(__name__)

# Type alias for either transport
_Transport = Union[USBTransport, HTTPTransport]


# ---------------------------------------------------------------------------
# Small dataclass-like result types  (plain named tuples are fine here)
# ---------------------------------------------------------------------------

from collections import namedtuple

AdcResult   = namedtuple("AdcResult",   ["raw", "value", "range", "rate", "mux"])
DeviceInfo  = namedtuple("DeviceInfo",  ["spi_ok", "silicon_rev", "silicon_id0", "silicon_id1"])
PingResult  = namedtuple("PingResult",  ["token", "uptime_ms"])
GpioStatus  = namedtuple("GpioStatus",  ["id", "mode", "output", "input", "pulldown"])
IdacChannel = namedtuple("IdacChannel", ["code", "target_v", "actual_v", "v_min", "v_max", "calibrated"])


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

class BugBuster:
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
    """

    def __init__(self, transport: _Transport):
        self._t         = transport
        self._usb       = isinstance(transport, USBTransport)
        self._connected = False
        self._hal       = None
        self._bus       = None
        # Cached HAT presence: None = unknown (probe on demand), bool = known
        self._hat_present_cache = None
        self._admin_token = None

    def get_admin_token(self) -> str:
        """
        Retrieve the hardware-derived admin token.  USB only.
        Used to authorize destructive operations over HTTP.
        """
        if self._admin_token:
            return self._admin_token
        
        self._require_usb("get_admin_token")
        resp = self._usb_cmd(CmdId.GET_ADMIN_TOKEN)
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
        return self

    def disconnect(self):
        """Close the connection cleanly."""
        if self._connected:
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

    def _usb_cmd(self, cmd_id: int, payload: bytes = b'') -> bytes:
        """Send a binary command and return the raw response payload."""
        return self._t.send_command(cmd_id, payload)

    def _http_get(self, path: str, **params) -> dict:
        return self._t.get(path, params=params or None)

    def _http_post(self, path: str, body: dict = None) -> dict:
        headers = {}
        if self._admin_token:
            headers["X-BugBuster-Admin-Token"] = self._admin_token
        return self._t.post(path, body, headers=headers)

    def _require_usb(self, method: str):
        if not self._usb:
            raise NotImplementedError(f"{method} is only available over USB")

    @staticmethod
    def _parse_hvpak_bridge(resp: bytes) -> dict:
        return {
            "output_mode": [resp[0], resp[2]],
            "ocp_retry": [resp[1], resp[3]],
            "predriver_enabled": bool(resp[4]),
            "full_bridge_enabled": bool(resp[5]),
            "control_selection_ph_en": bool(resp[6]),
            "ocp_deglitch_enabled": bool(resp[7]),
            "uvlo_enabled": bool(resp[8]),
        }

    @staticmethod
    def _parse_hvpak_analog(resp: bytes) -> dict:
        return {
            "vref_mode": resp[0],
            "vref_powered": bool(resp[1]),
            "vref_power_from_matrix": bool(resp[2]),
            "vref_sink_12ua": bool(resp[3]),
            "vref_input_selection": resp[4],
            "current_sense_vref": resp[5],
            "current_sense_dynamic_from_pwm": bool(resp[6]),
            "current_sense_gain": resp[7],
            "current_sense_invert": bool(resp[8]),
            "current_sense_enabled": bool(resp[9]),
            "acmp0_gain": resp[10],
            "acmp0_vref": resp[11],
            "has_acmp1": bool(resp[12]),
            "acmp1_gain": resp[13],
            "acmp1_vref": resp[14],
        }

    @staticmethod
    def _parse_hvpak_pwm(resp: bytes) -> dict:
        return {
            "index": resp[0],
            "initial_value": resp[1],
            "current_value": resp[2],
            "resolution_7bit": bool(resp[3]),
            "out_plus_inverted": bool(resp[4]),
            "out_minus_inverted": bool(resp[5]),
            "async_powerdown": bool(resp[6]),
            "autostop_mode": bool(resp[7]),
            "boundary_osc_disable": bool(resp[8]),
            "phase_correct": bool(resp[9]),
            "deadband": resp[10],
            "stop_mode": bool(resp[11]),
            "i2c_trigger": bool(resp[12]),
            "duty_source": resp[13],
            "period_clock_source": resp[14],
            "duty_clock_source": resp[15],
            "last_error": resp[16],
        }

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
        tok, uptime = struct.unpack_from('<II', resp)
        return PingResult(token=tok, uptime_ms=uptime)

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

    def reset(self) -> None:
        """
        Reset all channels to HIGH_IMP and clear all alerts.
        Safe to call before reconfiguring the device.
        """
        if self._usb:
            self._usb_cmd(CmdId.DEVICE_RESET)
        else:
            self._http_post("/device/reset")

    # ------------------------------------------------------------------
    # ── Channel — function & DAC ─────────────────────────────────────
    # ------------------------------------------------------------------

    def set_channel_function(self, channel: int, function: ChannelFunction) -> None:
        """
        Set the operating mode of a channel (0–3).

        Always pass through HIGH_IMP first when switching between
        incompatible functions (e.g. VOUT → IOUT) to avoid glitches.

        Example::

            bb.set_channel_function(0, ChannelFunction.VOUT)
        """
        if self._usb:
            payload = struct.pack('<BB', channel, int(function))
            self._usb_cmd(CmdId.SET_CHANNEL_FUNC, payload)
        else:
            self._http_post(f"/channel/{channel}/function", {"function": int(function)})

    def set_dac_voltage(
        self,
        channel: int,
        voltage: float,
        bipolar: bool = False,
    ) -> None:
        """
        Set the DAC output voltage for a channel in VOUT mode.

        *voltage* — target voltage in volts.
        *bipolar* — ``True`` for ±12 V range, ``False`` for 0–12 V (default).

        The channel must already be in :attr:`ChannelFunction.VOUT` mode.
        """
        if self._usb:
            payload = struct.pack('<BfB', channel, float(voltage), int(bipolar))
            self._usb_cmd(CmdId.SET_DAC_VOLTAGE, payload)
        else:
            self._http_post(
                f"/channel/{channel}/dac",
                {"voltage": float(voltage), "bipolar": bipolar},
            )

    def set_dac_current(self, channel: int, current_ma: float) -> None:
        """
        Set the DAC output current for a channel in IOUT mode.

        *current_ma* — target current in milliamps (0–25 mA).

        The channel must already be in :attr:`ChannelFunction.IOUT` mode.
        """
        if self._usb:
            payload = struct.pack('<Bf', channel, float(current_ma))
            self._usb_cmd(CmdId.SET_DAC_CURRENT, payload)
        else:
            self._http_post(f"/channel/{channel}/dac", {"current_mA": float(current_ma)})

    def set_dac_code(self, channel: int, code: int) -> None:
        """
        Write a raw 16-bit DAC code directly (0–65535).
        Useful for precise calibration — normally prefer :meth:`set_dac_voltage`.
        """
        if self._usb:
            payload = struct.pack('<BH', channel, code & 0xFFFF)
            self._usb_cmd(CmdId.SET_DAC_CODE, payload)
        else:
            self._http_post(f"/channel/{channel}/dac", {"code": code & 0xFFFF})

    def get_dac_readback(self, channel: int) -> int:
        """Return the currently active 16-bit DAC code from hardware readback."""
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_DAC_READBACK, struct.pack('<B', channel))
            return struct.unpack_from('<H', resp, 1)[0]
        else:
            d = self._http_get(f"/channel/{channel}/dac/readback")
            return _parse_int_maybe_hex(_first_present(d, "code", "activeCode"), 0)

    def set_vout_range(self, channel: int, range_: VoutRange) -> None:
        """
        Set the voltage output range for a channel in VOUT mode.

        *range_* — :class:`VoutRange`:

        ┌────────────────┬───────────────┬─────────────────────────────────┐
        │ Value          │ Range         │ Use case                        │
        ├────────────────┼───────────────┼─────────────────────────────────┤
        │ VoutRange.UNIPOLAR │ 0 – +12 V │ Positive-only outputs (default) │
        │ VoutRange.BIPOLAR  │ -12 – +12 V│ Signed / AC control signals    │
        └────────────────┴───────────────┴─────────────────────────────────┘

        Example::

            bb.set_vout_range(0, VoutRange.BIPOLAR)   # enable ±12 V output
            bb.set_dac_voltage(0, -5.0, bipolar=True)
        """
        bipolar = (range_ == VoutRange.BIPOLAR)
        if self._usb:
            payload = struct.pack('<BB', channel, int(bipolar))
            self._usb_cmd(CmdId.SET_VOUT_RANGE, payload)
        else:
            self._http_post(f"/channel/{channel}/vout/range", {"bipolar": bipolar})

    def set_current_limit(self, channel: int, limit: CurrentLimit) -> None:
        """
        Set the maximum output current for a channel in IOUT mode.

        *limit* — :class:`CurrentLimit`:

        ┌──────────────────┬──────────┬──────────────────────────────────────┐
        │ Value            │ Max      │ Use case                             │
        ├──────────────────┼──────────┼──────────────────────────────────────┤
        │ CurrentLimit.MA_25 │ 25 mA  │ Full 4–20 mA loops (default)         │
        │ CurrentLimit.MA_8  │  8 mA  │ Low-power / protected outputs        │
        └──────────────────┴──────────┴──────────────────────────────────────┘

        Example::

            bb.set_current_limit(1, CurrentLimit.MA_8)   # protect a sensitive load
        """
        if self._usb:
            payload = struct.pack('<BB', channel, int(limit))
            self._usb_cmd(CmdId.SET_CURRENT_LIMIT, payload)
        else:
            self._http_post(f"/channel/{channel}/ilimit", {"limit_8mA": bool(limit)})

    # ------------------------------------------------------------------
    # ── Channel — ADC ───────────────────────────────────────────────────
    # ------------------------------------------------------------------

    def get_adc_value(self, channel: int) -> AdcResult:
        """
        Read the latest ADC conversion result for a channel.

        Returns :class:`AdcResult` with fields:
          - ``raw``   — 24-bit raw ADC code
          - ``value`` — converted value in engineering units (V, mA, or Ω)
          - ``range`` — :class:`AdcRange` code in use
          - ``rate``  — :class:`AdcRate` code in use
          - ``mux``   — :class:`AdcMux` code in use
        """
        if self._usb:
            resp   = self._usb_cmd(CmdId.GET_ADC_VALUE, struct.pack('<B', channel))
            raw    = int.from_bytes(resp[1:4], 'little')
            value, = struct.unpack_from('<f', resp, 4)
            rng, rate, mux = struct.unpack_from('<BBB', resp, 8)
            return AdcResult(raw=raw, value=value, range=rng, rate=rate, mux=mux)
        else:
            d = self._http_get(f"/channel/{channel}/adc")
            return AdcResult(
                raw=_parse_int_maybe_hex(_first_present(d, "raw_code", "rawCode", "adcRaw"), 0),
                value=float(_first_present(d, "value", "adcValue", default=0.0) or 0.0),
                range=_parse_int_maybe_hex(_first_present(d, "range", "adcRange"), 0),
                rate=_parse_int_maybe_hex(_first_present(d, "rate", "adcRate"), 0),
                mux=_parse_int_maybe_hex(_first_present(d, "mux", "adcMux"), 0),
            )

    def set_adc_config(
        self,
        channel: int,
        mux:     AdcMux   = AdcMux.LF_TO_AGND,
        range_:  AdcRange = AdcRange.V_0_12,
        rate:    AdcRate  = AdcRate.SPS_20,
    ) -> None:
        """
        Configure the ADC for a channel.

        Most use-cases don't need this — the firmware sets sensible defaults
        when :meth:`set_channel_function` is called.  Override when you need
        a specific range or sample rate.

        Example — configure channel 1 for high-speed streaming at 9.6 kSPS::

            bb.set_adc_config(1, rate=AdcRate.SPS_9600)
        """
        if self._usb:
            payload = struct.pack('<BBBB', channel, int(mux), int(range_), int(rate))
            self._usb_cmd(CmdId.SET_ADC_CONFIG, payload)
        else:
            self._http_post(
                f"/channel/{channel}/adc/config",
                {"mux": int(mux), "range": int(range_), "rate": int(rate)},
            )

    # ------------------------------------------------------------------
    # ── Channel — RTD ───────────────────────────────────────────────────
    # ------------------------------------------------------------------

    def set_rtd_config(
        self,
        channel: int,
        current: RtdCurrent = RtdCurrent.MA_1,
    ) -> None:
        """
        Configure RTD excitation current for a channel in RES_MEAS mode.

        Use :attr:`RtdCurrent.MA_1` (default) for most RTDs.
        Use :attr:`RtdCurrent.UA_500` for high-resistance sensors.
        """
        if self._usb:
            payload = struct.pack('<BB', channel, int(current))
            self._usb_cmd(CmdId.SET_RTD_CONFIG, payload)
        else:
            self._http_post(f"/channel/{channel}/rtd/config", {"current": int(current)})

    # ------------------------------------------------------------------
    # ── Diagnostics ─────────────────────────────────────────────────
    # ------------------------------------------------------------------

    def set_diag_config(self, slot: int, source: int) -> None:
        """
        Configure a diagnostic slot's measurement source.

        The AD74416H has 4 diagnostic slots (0–3) that can each be assigned
        a measurement source for internal diagnostics (supply voltages,
        temperature, etc.).

        Parameters
        ----------
        slot : int
            Diagnostic slot index (0–3).
        source : int
            Diagnostic source code (0–13).  Refer to the AD74416H datasheet
            DIAG_ASSIGN register for the full list of source codes.
        """
        if self._usb:
            payload = struct.pack('<BB', slot, source)
            self._usb_cmd(CmdId.SET_DIAG_CONFIG, payload)
        else:
            self._http_post("/diagnostics/config", {"slot": slot, "source": source})

    # ------------------------------------------------------------------
    # ── Channel — Digital I/O ────────────────────────────────────────
    # ------------------------------------------------------------------

    def set_digital_output(self, channel: int, on: bool) -> None:
        """
        Drive a channel's digital output high (``True``) or low (``False``).
        Channel must be in :attr:`ChannelFunction.DIN_LOGIC` or a DO mode.
        """
        if self._usb:
            payload = struct.pack('<BB', channel, int(on))
            self._usb_cmd(CmdId.SET_DO_STATE, payload)
        else:
            self._http_post(f"/channel/{channel}/do/set", {"on": on})

    def set_din_config(
        self,
        channel:     int,
        threshold:   int  = 64,
        thresh_mode: bool = True,
        debounce:    int  = 5,
        sink:        int  = 10,
        sink_range:  bool = False,
        oc_detect:   bool = False,
        sc_detect:   bool = False,
    ) -> None:
        """
        Configure the digital input comparator for a channel.

        *threshold*   — comparator threshold code (0–255, where 128 ≈ mid-supply).
        *thresh_mode* — ``True`` = threshold above which input is logic-1.
        *debounce*    — debounce filter code (0 = off, higher = longer).
        *sink*        — current sink code for loop-powered inputs.
        *oc_detect*   — enable open-circuit fault detection.
        *sc_detect*   — enable short-circuit fault detection.

        *sink_range*  — selects sink current range for loop-powered inputs (``False`` = low-range,
                        ``True`` = high-range). Refer to AD74416H datasheet DINX_SINK_SEL bit.
        """
        if not (0 <= threshold <= 255):
            raise ValueError(f"threshold must be 0–255, got {threshold}")
        if not (0 <= debounce <= 31):
            raise ValueError(f"debounce must be 0–31, got {debounce}")
        if not (0 <= sink <= 31):
            raise ValueError(f"sink must be 0–31, got {sink}")
        if self._usb:
            payload = struct.pack(
                '<BBBBBBBB',
                channel, threshold, int(thresh_mode), debounce,
                sink, int(sink_range), int(oc_detect), int(sc_detect),
            )
            self._usb_cmd(CmdId.SET_DIN_CONFIG, payload)
        else:
            self._http_post(f"/channel/{channel}/din/config", {
                "thresh":      threshold,
                "thresh_mode": thresh_mode,
                "debounce":    debounce,
                "sink":        sink,
                "sink_range":  sink_range,
                "oc_det":      oc_detect,
                "sc_det":      sc_detect,
            })

    # ------------------------------------------------------------------
    # ── GPIO (AD74416H GPIO pins A–F, indices 0–5) ───────────────────
    # ------------------------------------------------------------------

    def get_gpio(self) -> list[GpioStatus]:
        """
        Read the state of all 6 GPIO pins.
        Returns a list of :class:`GpioStatus` namedtuples.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_GPIO_STATUS)
            pins = []
            for i in range(6):
                off = i * 5
                gid, mode, out_, in_, pd = struct.unpack_from('<BBBBB', resp, off)
                pins.append(GpioStatus(gid, GpioMode(mode), bool(out_), bool(in_), bool(pd)))
            return pins
        else:
            raw = self._http_get("/gpio")
            if isinstance(raw, dict):
                raw = raw.get("gpios", raw.get("pins", []))
            return [
                GpioStatus(
                    id=_parse_int_maybe_hex(_first_present(g, "id", "pin"), 0),
                    mode=GpioMode(_parse_int_maybe_hex(_first_present(g, "mode"), 0)),
                    output=bool(_first_present(g, "output", default=False)),
                    input=bool(_first_present(g, "input", default=False)),
                    pulldown=bool(_first_present(g, "pulldown", default=False)),
                )
                for g in raw
            ]

    def set_gpio_config(
        self,
        gpio:     int,
        mode:     GpioMode,
        pulldown: bool = False,
    ) -> None:
        """
        Configure a GPIO pin mode.

        *gpio* — pin index 0–5 (A=0, B=1, … F=5).
        *mode* — see :class:`GpioMode`.
        """
        if self._usb:
            payload = struct.pack('<BBB', gpio, int(mode), int(pulldown))
            self._usb_cmd(CmdId.SET_GPIO_CONFIG, payload)
        else:
            self._http_post(f"/gpio/{gpio}/config", {"mode": int(mode), "pulldown": pulldown})

    def set_gpio_value(self, gpio: int, value: bool) -> None:
        """Drive GPIO *gpio* high (``True``) or low (``False``).  Pin must be OUTPUT mode."""
        if self._usb:
            payload = struct.pack('<BB', gpio, int(value))
            self._usb_cmd(CmdId.SET_GPIO_VALUE, payload)
        else:
            self._http_post(f"/gpio/{gpio}/set", {"value": value})

    # ------------------------------------------------------------------
    # ── Digital IO (ESP32 GPIO, 12 logical IOs) ────────────────────────
    # ------------------------------------------------------------------

    def dio_get_all(self) -> list[dict]:
        """
        Read the state of all 12 digital IOs.

        Returns a list of dicts::

            [
                {"io": 1, "gpio": 2, "mode": 0, "output": False, "input": False},
                …
            ]

        Mode values: 0=disabled, 1=input, 2=output.
        """
        if self._usb:
            resp  = self._usb_cmd(CmdId.DIO_GET_ALL)
            count = resp[0]
            result = []
            off = 1
            for _ in range(count):
                io_num = resp[off]
                gpio   = struct.unpack_from('b', resp, off + 1)[0]
                mode   = resp[off + 2]
                output = bool(resp[off + 3])
                inp    = bool(resp[off + 4])
                off   += 5
                result.append({
                    "io": io_num, "gpio": gpio, "mode": mode,
                    "output": output, "input": inp,
                })
            return result
        else:
            raw = self._http_get("/dio")
            return raw.get("ios", [])

    def dio_configure(self, io: int, mode: int) -> None:
        """
        Configure a digital IO's direction.

        Parameters
        ----------
        io : int
            IO number (1–12).
        mode : int
            0 = disabled (high-impedance), 1 = input, 2 = output.

        Example::

            bb.dio_configure(2, 2)   # IO 2 → output
            bb.dio_configure(5, 1)   # IO 5 → input
        """
        if self._usb:
            self._usb_cmd(CmdId.DIO_CONFIG, struct.pack('<BB', io, mode))
        else:
            self._http_post(f"/dio/{io}/config", {"mode": mode})

    def dio_write(self, io: int, value: bool) -> None:
        """
        Set a digital IO output level.

        *io* must be configured as output (mode=2) first.
        *value* — ``True`` for HIGH, ``False`` for LOW.
        """
        if self._usb:
            self._usb_cmd(CmdId.DIO_WRITE, struct.pack('<BB', io, int(value)))
        else:
            self._http_post(f"/dio/{io}/set", {"value": value})

    def dio_read(self, io: int) -> dict:
        """
        Read a single digital IO.

        Returns ``{"io": N, "mode": M, "value": True/False}``.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.DIO_READ, struct.pack('<B', io))
            return {
                "io":    resp[0],
                "mode":  resp[1],
                "value": bool(resp[2]),
            }
        else:
            return self._http_get(f"/dio/{io}")

    # ------------------------------------------------------------------
    # ── External target I2C bus (routed IO pins) ───────────────────────
    # ------------------------------------------------------------------

    def ext_i2c_setup(
        self,
        *,
        sda_gpio: int,
        scl_gpio: int,
        frequency_hz: int = 400_000,
        pullups: str = "external",
    ) -> dict:
        internal_pullups = pullups.lower() == "internal"
        if not self._usb:
            return self._http_post("/bus/i2c/setup", {
                "sdaGpio": sda_gpio,
                "sclGpio": scl_gpio,
                "frequencyHz": int(frequency_hz),
                "internalPullups": internal_pullups,
            })
        payload = struct.pack("<BBIB", sda_gpio, scl_gpio, int(frequency_hz), int(internal_pullups))
        resp = self._usb_cmd(CmdId.EXT_I2C_SETUP, payload)
        return {
            "sda_gpio": resp[0],
            "scl_gpio": resp[1],
            "frequency_hz": struct.unpack_from("<I", resp, 2)[0],
            "internal_pullups": bool(resp[6]),
        }

    def ext_i2c_scan(
        self,
        *,
        start_addr: int = 0x08,
        stop_addr: int = 0x77,
        skip_reserved: bool = True,
        timeout_ms: int = 50,
    ) -> list[int]:
        """Scan the configured external I2C bus."""
        if not self._usb:
            raw = self._http_post("/bus/i2c/scan", {
                "startAddr": start_addr,
                "stopAddr": stop_addr,
                "skipReserved": skip_reserved,
                "timeoutMs": timeout_ms,
            })
            return [int(addr) for addr in raw.get("addresses", [])]
        payload = struct.pack("<BBBH", start_addr & 0x7F, stop_addr & 0x7F, int(skip_reserved), timeout_ms & 0xFFFF)
        resp = self._usb_cmd(CmdId.EXT_I2C_SCAN, payload)
        count = resp[0]
        return list(resp[1:1 + count])

    def ext_i2c_write(self, address: int, data: bytes | bytearray | list[int], *, timeout_ms: int = 100) -> int:
        """Write bytes to the configured external I2C bus."""
        raw = bytes(data)
        if len(raw) > 255:
            raise ValueError("I2C write payload must be <=255 bytes")
        if not self._usb:
            resp = self._http_post("/bus/i2c/write", {
                "address": address & 0x7F,
                "timeoutMs": timeout_ms,
                "data": list(raw),
            })
            return int(resp.get("written", 0))
        payload = struct.pack("<BHB", address & 0x7F, timeout_ms & 0xFFFF, len(raw)) + raw
        resp = self._usb_cmd(CmdId.EXT_I2C_WRITE, payload)
        return resp[0]

    def ext_i2c_read(self, address: int, length: int, *, timeout_ms: int = 100) -> bytes:
        """Read bytes from the configured external I2C bus."""
        if not (1 <= length <= 255):
            raise ValueError("I2C read length must be 1-255 bytes")
        if not self._usb:
            resp = self._http_post("/bus/i2c/read", {
                "address": address & 0x7F,
                "length": length,
                "timeoutMs": timeout_ms,
            })
            return bytes(resp.get("data", []))
        payload = struct.pack("<BHB", address & 0x7F, timeout_ms & 0xFFFF, length)
        resp = self._usb_cmd(CmdId.EXT_I2C_READ, payload)
        count = resp[0]
        return bytes(resp[1:1 + count])

    def ext_i2c_write_read(
        self,
        address: int,
        write_data: bytes | bytearray | list[int],
        read_length: int,
        *,
        timeout_ms: int = 100,
    ) -> bytes:
        """Write bytes then repeated-start read from the configured external I2C bus."""
        raw = bytes(write_data)
        if not (1 <= len(raw) <= 255):
            raise ValueError("I2C write-read write payload must be 1-255 bytes")
        if not (1 <= read_length <= 255):
            raise ValueError("I2C write-read length must be 1-255 bytes")
        if not self._usb:
            resp = self._http_post("/bus/i2c/write_read", {
                "address": address & 0x7F,
                "writeData": list(raw),
                "readLength": read_length,
                "timeoutMs": timeout_ms,
            })
            return bytes(resp.get("data", []))
        payload = struct.pack("<BHBB", address & 0x7F, timeout_ms & 0xFFFF, len(raw), read_length) + raw
        resp = self._usb_cmd(CmdId.EXT_I2C_WRITE_READ, payload)
        count = resp[0]
        return bytes(resp[1:1 + count])

    # ------------------------------------------------------------------
    # ── External target SPI bus (routed IO pins) ───────────────────────
    # ------------------------------------------------------------------

    def ext_spi_setup(
        self,
        *,
        sck_gpio: int,
        mosi_gpio: int | None = None,
        miso_gpio: int | None = None,
        cs_gpio: int | None = None,
        frequency_hz: int = 1_000_000,
        mode: int = 0,
    ) -> dict:
        """Configure the ESP32 external SPI peripheral on already-routed GPIOs."""
        if not self._usb:
            return self._http_post("/bus/spi/setup", {
                "sckGpio": sck_gpio,
                "mosiGpio": mosi_gpio,
                "misoGpio": miso_gpio,
                "csGpio": cs_gpio,
                "frequencyHz": int(frequency_hz),
                "mode": mode,
            })
        nc = 0xFF
        payload = struct.pack(
            "<BBBBIB",
            sck_gpio,
            nc if mosi_gpio is None else mosi_gpio,
            nc if miso_gpio is None else miso_gpio,
            nc if cs_gpio is None else cs_gpio,
            int(frequency_hz),
            mode & 0x03,
        )
        resp = self._usb_cmd(CmdId.EXT_SPI_SETUP, payload)
        return {
            "sck_gpio": resp[0],
            "mosi_gpio": None if resp[1] == nc else resp[1],
            "miso_gpio": None if resp[2] == nc else resp[2],
            "cs_gpio": None if resp[3] == nc else resp[3],
            "frequency_hz": struct.unpack_from("<I", resp, 4)[0],
            "mode": resp[8],
        }

    def ext_spi_transfer(self, data: bytes | bytearray | list[int], *, timeout_ms: int = 100) -> bytes:
        """Run a bounded full-duplex transfer on the configured external SPI bus."""
        raw = bytes(data)
        if not (1 <= len(raw) <= 512):
            raise ValueError("SPI transfer length must be 1-512 bytes")
        if not self._usb:
            resp = self._http_post("/bus/spi/transfer", {
                "data": list(raw),
                "timeoutMs": timeout_ms,
            })
            return bytes(resp.get("data", []))
        payload = struct.pack("<HH", timeout_ms & 0xFFFF, len(raw)) + raw
        resp = self._usb_cmd(CmdId.EXT_SPI_TRANSFER, payload)
        count = struct.unpack_from("<H", resp, 0)[0]
        return bytes(resp[2:2 + count])

    def ext_job_submit_i2c_read(self, address: int, length: int, *, timeout_ms: int = 100) -> int:
        """Queue a deferred I2C read in ESP32 RAM/PSRAM and return its job id."""
        if not self._usb:
            raise NotImplementedError("deferred bus jobs are currently available over USB BBP only")
        if not (1 <= length <= 255):
            raise ValueError("I2C deferred read length must be 1-255 bytes")
        payload = struct.pack("<BHBB", 1, timeout_ms & 0xFFFF, address & 0x7F, length)
        resp = self._usb_cmd(CmdId.EXT_JOB_SUBMIT, payload)
        return struct.unpack_from("<I", resp, 0)[0]

    def ext_job_submit_i2c_write_read(
        self,
        address: int,
        write_data: bytes | bytearray | list[int],
        read_length: int,
        *,
        timeout_ms: int = 100,
    ) -> int:
        """Queue a deferred I2C write/read transaction and return its job id."""
        if not self._usb:
            raise NotImplementedError("deferred bus jobs are currently available over USB BBP only")
        raw = bytes(write_data)
        if not (1 <= len(raw) <= 255):
            raise ValueError("I2C deferred write payload must be 1-255 bytes")
        if not (1 <= read_length <= 255):
            raise ValueError("I2C deferred read length must be 1-255 bytes")
        payload = struct.pack(
            "<BHBBB",
            2,
            timeout_ms & 0xFFFF,
            address & 0x7F,
            len(raw),
            read_length,
        ) + raw
        resp = self._usb_cmd(CmdId.EXT_JOB_SUBMIT, payload)
        return struct.unpack_from("<I", resp, 0)[0]

    def ext_job_submit_spi_transfer(self, data: bytes | bytearray | list[int], *, timeout_ms: int = 100) -> int:
        """Queue a deferred SPI transfer and return its job id."""
        if not self._usb:
            raise NotImplementedError("deferred bus jobs are currently available over USB BBP only")
        raw = bytes(data)
        if not (1 <= len(raw) <= 512):
            raise ValueError("SPI deferred transfer length must be 1-512 bytes")
        payload = struct.pack("<BHH", 3, timeout_ms & 0xFFFF, len(raw)) + raw
        resp = self._usb_cmd(CmdId.EXT_JOB_SUBMIT, payload)
        return struct.unpack_from("<I", resp, 0)[0]

    def ext_job_get(self, job_id: int) -> dict:
        """Poll a deferred external bus job."""
        if not self._usb:
            raise NotImplementedError("deferred bus jobs are currently available over USB BBP only")
        resp = self._usb_cmd(CmdId.EXT_JOB_GET, struct.pack("<I", job_id & 0xFFFFFFFF))
        result_len = struct.unpack_from("<H", resp, 6)[0]
        return {
            "job_id": struct.unpack_from("<I", resp, 0)[0],
            "status": resp[4],
            "status_name": {
                0: "empty",
                1: "queued",
                2: "running",
                3: "done",
                4: "error",
            }.get(resp[4], "unknown"),
            "kind": resp[5],
            "kind_name": {
                1: "i2c_read",
                2: "i2c_write_read",
                3: "spi_transfer",
            }.get(resp[5], "unknown"),
            "data": bytes(resp[8:8 + result_len]),
        }

    # ------------------------------------------------------------------
    # ── Self-Test / Calibration / E-fuse Monitoring ────────────────────
    # ------------------------------------------------------------------

    def selftest_status(self) -> dict:
        """
        Get the boot self-test result and calibration status.

        Returns::

            {
                "boot": {"ran": True, "passed": True,
                         "vadj1_v": 12.0, "vadj2_v": 5.0, "vlogic_v": 3.3},
                "cal":  {"status": 0, "channel": 0, "points": 0, "error_mv": 0.0}
            }

        Cal status: 0=idle, 1=running, 2=success, 3=failed.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.SELFTEST_STATUS)
            off = 0
            boot_ran    = bool(resp[off]); off += 1
            boot_passed = bool(resp[off]); off += 1
            vadj1,  = struct.unpack_from('<f', resp, off); off += 4
            vadj2,  = struct.unpack_from('<f', resp, off); off += 4
            vlogic, = struct.unpack_from('<f', resp, off); off += 4
            cal_status = resp[off]; off += 1
            cal_ch     = resp[off]; off += 1
            cal_pts    = resp[off]; off += 1
            cal_err,   = struct.unpack_from('<f', resp, off); off += 4
            return {
                "boot": {"ran": boot_ran, "passed": boot_passed,
                         "vadj1_v": vadj1, "vadj2_v": vadj2, "vlogic_v": vlogic},
                "cal":  {"status": cal_status, "channel": cal_ch,
                         "points": cal_pts, "error_mv": cal_err},
            }
        else:
            return self._http_get("/selftest")

    def selftest_measure_supply(self, rail: int) -> float:
        """
        Measure a supply rail via U23 self-test MUX.

        Parameters
        ----------
        rail : int
            0 = VADJ1, 1 = VADJ2, 2 = 3V3_ADJ (VLOGIC).

        Returns voltage in volts (corrected for divider), or -1 if unavailable.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.SELFTEST_MEASURE_SUPPLY,
                                 struct.pack('<B', rail))
            _, voltage = struct.unpack_from('<Bf', resp, 0)
            return voltage
        else:
            r = self._http_get(f"/selftest/supply/{rail}")
            return r.get("voltage", -1.0)

    def selftest_efuse_currents(self) -> dict:
        """
        Get all 4 e-fuse output currents measured via IMON.

        Returns::

            {
                "available": True,
                "timestamp_ms": 12345,
                "currents": [0.5, 0.3, -1.0, 0.1]   # amps, -1 = unavailable
            }

        ``available`` is False when U17 S2 is closed (IO 10 in analog mode).
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.SELFTEST_EFUSE_CURRENTS)
            off = 0
            avail = bool(resp[off]); off += 1
            ts,   = struct.unpack_from('<I', resp, off); off += 4
            currents = []
            for _ in range(4):
                c, = struct.unpack_from('<f', resp, off); off += 4
                currents.append(c)
            return {"available": avail, "timestamp_ms": ts, "currents": currents}
        else:
            return self._http_get("/selftest/efuse")

    def selftest_auto_calibrate(self, idac_channel: int) -> dict:
        """
        Start automatic IDAC calibration for a supply channel.

        Sweeps IDAC codes, measures actual output voltage via U23,
        builds calibration curve, and saves to NVS.  This takes several
        seconds — the device is unresponsive to other commands during cal.

        Parameters
        ----------
        idac_channel : int
            1 = VADJ1, 2 = VADJ2.

        Returns cal result dict: ``{"status", "channel", "points", "error_mv"}``.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.SELFTEST_AUTO_CAL,
                                 struct.pack('<B', idac_channel))
            return {
                "status":   resp[0],
                "channel":  resp[1],
                "points":   resp[2],
                "error_mv": struct.unpack_from('<f', resp, 3)[0],
            }
        else:
            r = self._http_post("/selftest/calibrate", {"channel": idac_channel})
            return r

    def selftest_internal_supplies(self) -> dict:
        """
        Measure internal AD74416H supply rails using diagnostic slots.

        Works in both breadboard and PCB mode (no U23 required).
        Takes ~8 seconds (changes diagnostic sources, waits for fresh ADC data).

        Returns::

            {
                "valid": True,
                "supplies_ok": True,
                "avdd_hi_v": 21.5,    # positive analog supply
                "dvcc_v": 5.0,        # digital supply
                "avcc_v": 5.0,        # analog supply
                "avss_v": -16.0,      # negative analog supply
                "temp_c": 27.5        # die temperature
            }
        """
        if self._usb:
            # This command takes ~8s — temporarily increase timeout
            old_timeout = self._t._timeout
            self._t._timeout = 15.0
            try:
                resp = self._usb_cmd(CmdId.SELFTEST_INT_SUPPLIES)
            finally:
                self._t._timeout = old_timeout
            off = 0
            valid  = bool(resp[off]); off += 1
            ok     = bool(resp[off]); off += 1
            avdd,  = struct.unpack_from('<f', resp, off); off += 4
            dvcc,  = struct.unpack_from('<f', resp, off); off += 4
            avcc,  = struct.unpack_from('<f', resp, off); off += 4
            avss,  = struct.unpack_from('<f', resp, off); off += 4
            temp,  = struct.unpack_from('<f', resp, off); off += 4
            return {
                "valid": valid, "supplies_ok": ok,
                "avdd_hi_v": avdd, "dvcc_v": dvcc, "avcc_v": avcc,
                "avss_v": avss, "temp_c": temp,
            }
        else:
            return self._http_get("/selftest/supplies")

    # ------------------------------------------------------------------
    # ── UART bridge ────────────────────────────────────────────────────
    # ------------------------------------------------------------------

    def get_uart_config(self) -> list[dict]:
        """
        Read the configuration of all UART bridges.

        Returns a list of dicts, one per bridge::

            [
                {
                    "bridge_id": 0,
                    "uart_num":  1,        # ESP32 UART peripheral (0–2)
                    "tx_pin":    17,       # ESP32 GPIO for TX
                    "rx_pin":    18,       # ESP32 GPIO for RX
                    "baudrate":  115200,
                    "data_bits": 8,
                    "parity":    0,        # 0=none, 1=odd, 2=even
                    "stop_bits": 0,        # 0=1, 1=1.5, 2=2
                    "enabled":   False,
                },
                …
            ]
        """
        self._require_usb("get_uart_config")
        resp   = self._usb_cmd(CmdId.GET_UART_CONFIG)
        count  = resp[0]
        result = []
        off    = 1
        for _ in range(count):
            bridge_id = resp[off]
            uart_num  = resp[off + 1]
            tx_pin    = resp[off + 2]
            rx_pin    = resp[off + 3]
            baudrate, = struct.unpack_from('<I', resp, off + 4)
            data_bits = resp[off + 8]
            parity    = resp[off + 9]
            stop_bits = resp[off + 10]
            enabled   = bool(resp[off + 11])
            off      += 12
            result.append({
                "bridge_id": bridge_id,
                "uart_num":  uart_num,
                "tx_pin":    tx_pin,
                "rx_pin":    rx_pin,
                "baudrate":  baudrate,
                "data_bits": data_bits,
                "parity":    parity,
                "stop_bits": stop_bits,
                "enabled":   enabled,
            })
        return result

    def set_uart_config(
        self,
        bridge_id: int   = 0,
        uart_num:  int   = 1,
        tx_pin:    int   = 1,
        rx_pin:    int   = 2,
        baudrate:  int   = 115200,
        data_bits: int   = 8,
        parity:    int   = 0,
        stop_bits: int   = 0,
        enabled:   bool  = False,
    ) -> None:
        """
        Configure a UART bridge.

        Routes a secondary serial port to the specified ESP32 GPIO pins.
        The MUX must be configured to connect these ESP32 GPIOs to the
        desired physical IO terminals.

        Parameters
        ----------
        bridge_id : int
            Bridge index (0 or 1).
        uart_num : int
            ESP32 UART peripheral (0–2; default 1).
        tx_pin : int
            ESP32 GPIO pin number for TX output.
        rx_pin : int
            ESP32 GPIO pin number for RX input.
        baudrate : int
            Baud rate in bps (300–3,000,000; default 115200).
        data_bits : int
            Data bits per frame (5, 6, 7, or 8; default 8).
        parity : int
            Parity mode — 0=none, 1=odd, 2=even (default 0).
        stop_bits : int
            Stop bits — 0=1 stop bit, 1=1.5, 2=2 (default 0).
        enabled : bool
            Activate the bridge immediately (default False).
        """
        self._require_usb("set_uart_config")
        payload = struct.pack(
            '<BBBBIBBBB',
            bridge_id, uart_num, tx_pin, rx_pin,
            baudrate, data_bits, parity, stop_bits, int(enabled),
        )
        self._usb_cmd(CmdId.SET_UART_CONFIG, payload)

    def get_uart_pins(self) -> list[int]:
        """
        Return a list of ESP32 GPIO pin numbers available for UART routing.

        Pins currently in use by other functions (SPI, I2C, USB, etc.) are
        excluded.
        """
        self._require_usb("get_uart_pins")
        resp  = self._usb_cmd(CmdId.GET_UART_PINS)
        count = resp[0]
        return list(resp[1:1 + count])

    # ------------------------------------------------------------------
    # ── MUX switch matrix (4 × ADGS2414D, 32 SPST switches) ─────────
    # ------------------------------------------------------------------

    def mux_get(self) -> list[int]:
        """
        Return the current state of all 32 switches as a list of 4 bytes
        (one per ADGS2414D device, bit n = switch n state).
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.MUX_GET_ALL)
            return list(resp[:4])
        else:
            return list(self._http_get("/mux")["states"])

    def mux_set_all(self, states: list[int]) -> None:
        """
        Set all 32 switches at once.

        *states* — list of 4 bytes (device 0–3).  Bit n of each byte
        controls switch n of that device (1 = closed, 0 = open).

        The firmware enforces a 100 ms dead time between switch-group
        transitions to protect the level shifters.
        """
        if len(states) != 4:
            raise ValueError("states must be a list of 4 bytes")
        if self._usb:
            payload = bytes(states)
            self._usb_cmd(CmdId.MUX_SET_ALL, payload)
        else:
            self._http_post("/mux/all", {"states": list(states)})

    def mux_set_switch(self, device: int, switch: int, closed: bool) -> None:
        """
        Open or close a single switch.

        *device* — ADGS2414D index (0–3).
        *switch* — switch index within the device (0–7).
        *closed* — ``True`` to close, ``False`` to open.
        """
        if self._usb:
            payload = struct.pack('<BBB', device, switch, int(closed))
            self._usb_cmd(CmdId.MUX_SET_SWITCH, payload)
        else:
            self._http_post("/mux/switch", {"device": device, "switch": switch, "closed": closed})

    def get_debug_info(self) -> dict:
        """
        Combined I2C-bus diagnostics (DS4424 / HUSB238 / PCA9535).

        **HTTP only** — mirrors ``GET /api/debug``.  There is no equivalent
        single BBP opcode; on USB use :meth:`idac_get_status`,
        :meth:`usbpd_get_status` and :meth:`power_get_status` individually.
        """
        if self._usb:
            raise NotImplementedError(
                "get_debug_info() is HTTP-only. Use idac_get_status(), "
                "usbpd_get_status() and power_get_status() over USB."
            )
        return self._http_get("/debug")

    # ------------------------------------------------------------------
    # ── IDAC — adjustable power supply voltages ──────────────────────
    # ------------------------------------------------------------------

    def idac_get_status(self) -> dict:
        """
        Return the DS4424 IDAC status dict with keys:
        ``present``, ``channels`` (list of 4 :class:`IdacChannel`).

        IDAC channel mapping:
          - 0 → VLOGIC (TPS74601, 1.8–5 V logic level for digital IOs)
          - 1 → VADJ1 (LTM8063 #1, feeds IO_Block 1+2, IO 1–6)
          - 2 → VADJ2 (LTM8063 #2, feeds IO_Block 3+4, IO 7–12)
          - 3 → not connected
        """
        if self._usb:
            resp    = self._usb_cmd(CmdId.IDAC_GET_STATUS)
            present = bool(resp[0])
            chans   = []
            off     = 1
            for _ in range(4):
                code,         = struct.unpack_from('<b',  resp, off);      off += 1
                tgt, act, mid = struct.unpack_from('<fff',resp, off);      off += 12
                vmin, vmax,   = struct.unpack_from('<ff', resp, off);      off += 8
                step_mv,      = struct.unpack_from('<f',  resp, off);      off += 4
                cal           = bool(resp[off]);                           off += 1
                chans.append(IdacChannel(code, tgt, act, vmin, vmax, cal))
            return {"present": present, "channels": chans}
        else:
            return self._http_get("/idac")

    def idac_set_voltage(self, channel: int, voltage: float) -> None:
        """
        Set the target output voltage of an IDAC-controlled supply.

        *channel* — 0=level-shifter, 1=V_ADJ1, 2=V_ADJ2.
        *voltage* — target voltage in volts.

        The power supply must be enabled first via :meth:`power_set`.
        """
        if self._usb:
            payload = struct.pack('<Bf', channel, float(voltage))
            self._usb_cmd(CmdId.IDAC_SET_VOLTAGE, payload)
        else:
            self._http_post("/idac/voltage", {"ch": channel, "voltage": float(voltage)})

    def idac_set_code(self, channel: int, code: int) -> None:
        """
        Write a raw signed 8-bit DAC code (-127…+127) to an IDAC channel.
        Useful during manual calibration.
        """
        if self._usb:
            payload = struct.pack('<Bb', channel, code)
            self._usb_cmd(CmdId.IDAC_SET_CODE, payload)
        else:
            self._http_post("/idac/code", {"ch": channel, "code": code})

    def idac_cal_save(self) -> None:
        """Persist current IDAC calibration curves to non-volatile storage."""
        if self._usb:
            self._usb_cmd(CmdId.IDAC_CAL_SAVE)
        else:
            self._http_post("/idac/cal/save")

    # ------------------------------------------------------------------
    # ── Power management (PCA9535 I/O expander) ──────────────────────
    # ------------------------------------------------------------------

    def power_get_status(self) -> dict:
        """
        Return the PCA9535 power-management status dict with keys:
        ``present``, ``logic_pg``, ``vadj1_pg``, ``vadj2_pg``,
        ``efuse_faults`` (list of 4 bool), ``enables`` (dict).
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.PCA_GET_STATUS)
            return _parse_pca_status(resp)
        else:
            return self._http_get("/ioexp")

    def power_set(self, control: PowerControl, on: bool) -> None:
        """
        Enable or disable a power rail or protection device.

        Example — enable the V_ADJ1 regulator before using IDAC channel 1::

            bb.power_set(PowerControl.VADJ1, True)

        Example — enable e-fuse 2 to protect port P2::

            bb.power_set(PowerControl.EFUSE2, True)
        """
        _CONTROL_HTTP_NAMES = {
            PowerControl.VADJ1:   "vadj1",
            PowerControl.VADJ2:   "vadj2",
            PowerControl.V15A:    "15v",
            PowerControl.MUX:     "mux",
            PowerControl.USB_HUB: "usb_hub",
            PowerControl.EFUSE1:  "efuse1",
            PowerControl.EFUSE2:  "efuse2",
            PowerControl.EFUSE3:  "efuse3",
            PowerControl.EFUSE4:  "efuse4",
        }
        if self._usb:
            payload = struct.pack('<BB', int(control), int(on))
            self._usb_cmd(CmdId.PCA_SET_CONTROL, payload)
        else:
            self._http_post("/ioexp/control", {
                "control": _CONTROL_HTTP_NAMES[PowerControl(control)],
                "on": on,
            })

    def power_get_fault_log(self) -> list:
        """
        Return a list of recent PCA9535 fault events (e-fuse trips, PG changes).
        Each entry is a dict with ``type``, ``type_name``, ``channel``, ``timestamp_ms``.
        """
        _FAULT_NAMES = {0: "efuse_trip", 1: "efuse_clear", 2: "pg_lost", 3: "pg_restored"}
        if self._usb:
            resp = self._usb_cmd(CmdId.PCA_GET_FAULT_LOG)
            off = 0
            count = resp[off]; off += 1
            events = []
            for _ in range(count):
                ftype = resp[off]; off += 1
                ch = resp[off]; off += 1
                ts = struct.unpack_from('<I', resp, off)[0]; off += 4
                events.append({
                    "type": ftype,
                    "type_name": _FAULT_NAMES.get(ftype, "unknown"),
                    "channel": ch,
                    "timestamp_ms": ts,
                })
            return events
        else:
            data = self._http_get("/ioexp/faults")
            return data.get("faults", [])

    def power_set_fault_config(self, auto_disable: bool = True, log_events: bool = True) -> None:
        """
        Configure PCA9535 fault behavior.

        :param auto_disable: If True, automatically disable faulted e-fuse on trip.
        :param log_events:   If True, log fault events to console.
        """
        if self._usb:
            payload = struct.pack('<BB', int(auto_disable), int(log_events))
            self._usb_cmd(CmdId.PCA_SET_FAULT_CFG, payload)
        else:
            self._http_post("/ioexp/fault_config", {
                "auto_disable": auto_disable,
                "log_events": log_events,
            })

    # ------------------------------------------------------------------
    # ── HAT Expansion Board ──────────────────────────────────────────
    # ------------------------------------------------------------------

    def _require_hat_present(self) -> None:
        """
        Guard for HAT-only operations (Logic Analyzer, SWD debug).

        Lazily probes the HAT once and caches the result.  Raises
        :class:`HatNotPresentError` with an actionable message if no HAT
        has been detected on this device.  Call :meth:`hat_detect` to
        invalidate the cache after physically attaching a HAT.
        """
        if self._hat_present_cache is None:
            try:
                status = self.hat_get_status()
            except Exception as exc:  # noqa: BLE001
                raise HatNotPresentError(
                    "Could not query HAT status to verify HAT presence: "
                    f"{exc!s}. The Logic Analyzer and SWD functions are "
                    "HAT-only and require a working HAT expansion board."
                ) from exc
            self._hat_present_cache = bool(status.get("detected", False))

        if not self._hat_present_cache:
            raise HatNotPresentError(
                "This operation requires the BugBuster HAT expansion board, "
                "but no HAT was detected on the device. The Logic Analyzer "
                "and SWD debug functions live on the RP2040 on the HAT and "
                "cannot run on a bare BugBuster board. Connect the HAT and "
                "call hat_detect() to refresh, then retry."
            )

    def hat_get_status(self) -> dict:
        """
        Get HAT expansion board status: detection, connection, pin config.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_GET_STATUS)
            off = 0
            
            # Minimum expected payload is 14 bytes for v1 firmware
            if len(resp) >= 14:
                detected = bool(resp[off]); off += 1
                connected = bool(resp[off]); off += 1
                hat_type = resp[off]; off += 1
                detect_v = struct.unpack_from('<f', resp, off)[0]; off += 4
                fw_major = resp[off]; off += 1
                fw_minor = resp[off]; off += 1
                confirmed = bool(resp[off]); off += 1
                pins = []
                for i in range(4):
                    pins.append(resp[off]); off += 1
            else:
                # Fallback for truncated/mock responses
                detected  = bool(resp[0]) if len(resp) > 0 else False
                connected = bool(resp[1]) if len(resp) > 1 else False
                hat_type  = resp[2] if len(resp) > 2 else 0
                detect_v  = 0.0
                fw_major  = 0
                fw_minor  = 0
                confirmed = False
                pins      = [0, 0, 0, 0]
                off       = len(resp)

            result = {
                "detected": detected, "connected": connected,
                "type": hat_type, "detect_voltage": detect_v,
                "fw_version": f"{fw_major}.{fw_minor}",
                "config_confirmed": confirmed,
                "pin_config": pins,
            }
            if len(resp) >= off + 12:
                connectors = []
                for _ in range(2):
                    enabled = bool(resp[off]); off += 1
                    current = struct.unpack_from('<f', resp, off)[0]; off += 4
                    fault = bool(resp[off]); off += 1
                    connectors.append({
                        "enabled": enabled,
                        "current_ma": current,
                        "fault": fault,
                    })
                result["connectors"] = connectors
                result["io_voltage_mv"] = struct.unpack_from('<H', resp, off)[0]; off += 2
            if len(resp) >= off + 3:
                result["hvpak_part"] = resp[off]; off += 1
                result["hvpak_ready"] = bool(resp[off]); off += 1
                result["hvpak_last_error"] = resp[off]; off += 1
            if len(resp) >= off + 6:
                result["dap_connected"] = bool(resp[off]); off += 1
                result["target_detected"] = bool(resp[off]); off += 1
                result["target_dpidr"] = struct.unpack_from('<I', resp, off)[0]; off += 4
            return result
        else:
            return _normalize_http_hat_status(self._http_get("/hat"))

    def hat_set_pin(self, ext_pin: int, function) -> bool:
        """
        Set a single EXP_EXT pin function on the HAT.

        :param ext_pin:  Pin index 0-3 (EXP_EXT_1 to EXP_EXT_4)
        :param function: HatPinFunction value
        :return: True if HAT acknowledged
        :raises HatPinFunctionError: If ``function`` is one of the reserved
            numeric slots 1..4 (formerly SWDIO/SWCLK/TRACE1/TRACE2). Use
            :meth:`hat_setup_swd` instead — SWD now lives on a dedicated
            3-pin connector and is no longer assigned per-pin.
        """
        self._require_hat_present()
        func_val = int(function)
        from .constants import HAT_FUNC_RESERVED_CODES
        if func_val in HAT_FUNC_RESERVED_CODES:
            raise HatPinFunctionError(
                f"HatPinFunction code {func_val} is reserved (formerly "
                "SWDIO/SWCLK/TRACE1/TRACE2). SWD now uses the dedicated "
                "3-pin HAT connector — call hat_setup_swd(target_voltage_mv, "
                "connector) to enable SWD instead of assigning a function "
                "to an EXP_EXT pin."
            )
        if self._usb:
            payload = struct.pack('<BB', ext_pin, func_val)
            self._usb_cmd(CmdId.HAT_SET_PIN, payload)
            return True
        else:
            resp = self._http_post("/hat/config", {"pin": ext_pin, "function": func_val})
            return resp.get("ok", False)

    def hat_set_all_pins(self, functions: list) -> bool:
        """
        Set all 4 EXP_EXT pin functions at once.

        :param functions: List of 4 HatPinFunction values
        :return: True if HAT acknowledged
        """
        self._require_hat_present()
        assert len(functions) == 4
        if self._usb:
            payload = struct.pack('<4B', *[int(f) for f in functions])
            self._usb_cmd(CmdId.HAT_SET_ALL_PINS, payload)
            return True
        else:
            resp = self._http_post("/hat/config", {"pins": [int(f) for f in functions]})
            return resp.get("ok", False)

    def hat_reset(self) -> bool:
        """Reset HAT to default state (all pins disconnected)."""
        self._require_hat_present()
        if self._usb:
            self._usb_cmd(CmdId.HAT_RESET)
            return True
        else:
            resp = self._http_post("/hat/reset", {})
            return resp.get("ok", False)

    def hat_detect(self) -> dict:
        """Re-run HAT detection and return result.

        Also invalidates the cached HAT-presence flag used by the
        Logic Analyzer / SWD guard, so that newly attached HATs are
        picked up on the next guarded call.
        """
        # Invalidate the cached presence — the very point of calling
        # hat_detect() is usually to re-probe after wiring changes.
        self._hat_present_cache = None
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_DETECT)
            off = 0
            detected = bool(resp[off]); off += 1
            hat_type = resp[off]; off += 1
            detect_v = struct.unpack_from('<f', resp, off)[0]; off += 4
            connected = bool(resp[off]); off += 1
            result = {"detected": detected, "type": hat_type,
                      "detect_voltage": detect_v, "connected": connected}
        else:
            raw = self._http_post("/hat/detect", {})
            result = {
                "detected": bool(_first_present(raw, "detected", default=False)),
                "type": _parse_int_maybe_hex(_first_present(raw, "type"), 0),
                "detect_voltage": float(_first_present(raw, "detect_voltage", "detectVoltage", default=0.0) or 0.0),
                "connected": bool(_first_present(raw, "connected", default=False)),
            }
        # Refresh cache from this fresh probe so subsequent guarded calls
        # don't issue another status query.
        self._hat_present_cache = bool(result["detected"])
        return result

    def hat_set_power(self, connector: int, enable: bool) -> bool:
        """
        Enable or disable target power on a HAT connector.

        :param connector: 0 = Connector A (VADJ1), 1 = Connector B (VADJ2)
        :param enable: True to enable power
        """
        self._require_hat_present()
        if self._usb:
            payload = struct.pack('<BB', connector, int(enable))
            self._usb_cmd(CmdId.HAT_SET_POWER, payload)
            return True
        else:
            raise NotImplementedError(
                "hat_set_power() is not available over HTTP — firmware does not expose /api/hat/power. "
                "Use a USB connection instead."
            )

    def hat_get_power(self) -> dict:
        """Get power status for both HAT connectors (enabled, current, fault)."""
        self._require_hat_present()
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_GET_POWER)
            off = 0
            connectors = []
            for _ in range(2):
                enabled = bool(resp[off]); off += 1
                current = struct.unpack_from('<f', resp, off)[0]; off += 4
                fault = bool(resp[off]); off += 1
                connectors.append({"enabled": enabled, "current_ma": current, "fault": fault})
            io_mv = struct.unpack_from('<H', resp, off)[0]; off += 2
            result = {"connectors": connectors, "io_voltage_mv": io_mv}
            if len(resp) >= off + 3:
                result["hvpak_part"] = resp[off]; off += 1
                result["hvpak_ready"] = bool(resp[off]); off += 1
                result["hvpak_last_error"] = resp[off]; off += 1
            return result
        else:
            raise NotImplementedError(
                "hat_get_power() is not available over HTTP — firmware does not expose /api/hat/power. "
                "Use a USB connection instead."
            )

    def hat_set_io_voltage(self, voltage_mv: int) -> bool:
        """
        Set the HVPAK I/O level translation voltage.

        :param voltage_mv: Target I/O voltage in millivolts (1200-5500)
        """
        self._require_hat_present()
        if self._usb:
            payload = struct.pack('<H', voltage_mv)
            self._usb_cmd(CmdId.HAT_SET_IO_VOLT, payload)
            return True
        else:
            raise NotImplementedError(
                "hat_set_io_voltage() is not available over HTTP — firmware does not expose /api/hat/io_voltage. "
                "Use a USB connection instead."
            )

    def hat_setup_swd(self, target_voltage_mv: int = 3300, connector: int = 0) -> bool:
        """
        One-call SWD debug setup: sets I/O voltage, enables power, routes SWD pins.

        :param target_voltage_mv: Target voltage in mV (e.g. 3300 for 3.3V)
        :param connector: 0 = Connector A, 1 = Connector B
        :return: True if all steps succeeded
        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        if self._usb:
            payload = struct.pack('<HB', target_voltage_mv, connector)
            self._usb_cmd(CmdId.HAT_SETUP_SWD, payload)
            return True
        else:
            return self._http_post("/hat/setup_swd", {
                "target_voltage_mv": target_voltage_mv,
                "connector": connector,
            }).get("ok", False)

    def hat_get_hvpak_info(self) -> dict:
        self._require_usb("hat_get_hvpak_info")
        self._require_hat_present()
        resp = self._usb_cmd(CmdId.HAT_GET_HVPAK_INFO)
        return {
            "part": resp[0],
            "ready": bool(resp[1]),
            "last_error": resp[2],
            "factory_virgin": bool(resp[3]),
            "service_window_ok": bool(resp[4]),
            "requested_mv": struct.unpack_from('<H', resp, 5)[0],
            "applied_mv": struct.unpack_from('<H', resp, 7)[0],
            "service_f5": resp[9],
            "service_fd": resp[10],
            "service_fe": resp[11],
        }

    def hat_get_hvpak_caps(self) -> dict:
        self._require_usb("hat_get_hvpak_caps")
        self._require_hat_present()
        resp = self._usb_cmd(CmdId.HAT_GET_HVPAK_CAPS)
        return {
            "flags": struct.unpack_from('<I', resp, 0)[0],
            "lut2_count": resp[4],
            "lut3_count": resp[5],
            "lut4_count": resp[6],
            "pwm_count": resp[7],
            "comparator_count": resp[8],
            "bridge_count": resp[9],
        }

    def hat_get_hvpak_lut(self, kind: int, index: int) -> dict:
        self._require_usb("hat_get_hvpak_lut")
        self._require_hat_present()
        resp = self._usb_cmd(CmdId.HAT_GET_HVPAK_LUT, struct.pack('<BB', kind, index))
        return {
            "kind": resp[0],
            "index": resp[1],
            "width_bits": resp[2],
            "truth_table": struct.unpack_from('<H', resp, 3)[0],
        }

    def hat_set_hvpak_lut(self, kind: int, index: int, truth_table: int) -> dict:
        self._require_usb("hat_set_hvpak_lut")
        self._require_hat_present()
        payload = struct.pack('<BBH', kind, index, truth_table)
        resp = self._usb_cmd(CmdId.HAT_SET_HVPAK_LUT, payload)
        return {
            "kind": resp[0],
            "index": resp[1],
            "width_bits": resp[2],
            "truth_table": struct.unpack_from('<H', resp, 3)[0],
        }

    def hat_get_hvpak_bridge(self) -> dict:
        self._require_usb("hat_get_hvpak_bridge")
        self._require_hat_present()
        resp = self._usb_cmd(CmdId.HAT_GET_HVPAK_BRIDGE)
        return self._parse_hvpak_bridge(resp)

    def hat_set_hvpak_bridge(
        self,
        *,
        output_mode_0: int,
        ocp_retry_0: int,
        output_mode_1: int,
        ocp_retry_1: int,
        predriver_enabled: bool,
        full_bridge_enabled: bool,
        control_selection_ph_en: bool,
        ocp_deglitch_enabled: bool,
        uvlo_enabled: bool,
    ) -> dict:
        self._require_usb("hat_set_hvpak_bridge")
        self._require_hat_present()
        payload = struct.pack(
            '<BBBBBBBBB',
            output_mode_0, ocp_retry_0,
            output_mode_1, ocp_retry_1,
            int(predriver_enabled), int(full_bridge_enabled),
            int(control_selection_ph_en), int(ocp_deglitch_enabled),
            int(uvlo_enabled),
        )
        resp = self._usb_cmd(CmdId.HAT_SET_HVPAK_BRIDGE, payload)
        return self._parse_hvpak_bridge(resp)

    def hat_get_hvpak_analog(self) -> dict:
        self._require_usb("hat_get_hvpak_analog")
        self._require_hat_present()
        resp = self._usb_cmd(CmdId.HAT_GET_HVPAK_ANALOG)
        return self._parse_hvpak_analog(resp)

    def hat_set_hvpak_analog(self, **kwargs) -> dict:
        self._require_usb("hat_set_hvpak_analog")
        self._require_hat_present()
        payload = struct.pack(
            '<BBBBBBBBBBBBBBB',
            kwargs.get("vref_mode", 0),
            int(kwargs.get("vref_powered", False)),
            int(kwargs.get("vref_power_from_matrix", False)),
            int(kwargs.get("vref_sink_12ua", False)),
            kwargs.get("vref_input_selection", 0),
            kwargs.get("current_sense_vref", 0),
            int(kwargs.get("current_sense_dynamic_from_pwm", False)),
            kwargs.get("current_sense_gain", 0),
            int(kwargs.get("current_sense_invert", False)),
            int(kwargs.get("current_sense_enabled", False)),
            kwargs.get("acmp0_gain", 0),
            kwargs.get("acmp0_vref", 0),
            int(kwargs.get("has_acmp1", False)),
            kwargs.get("acmp1_gain", 0),
            kwargs.get("acmp1_vref", 0),
        )
        resp = self._usb_cmd(CmdId.HAT_SET_HVPAK_ANALOG, payload)
        return self._parse_hvpak_analog(resp)

    def hat_get_hvpak_pwm(self, index: int = 0) -> dict:
        self._require_usb("hat_get_hvpak_pwm")
        self._require_hat_present()
        resp = self._usb_cmd(CmdId.HAT_GET_HVPAK_PWM, struct.pack('<B', index))
        return self._parse_hvpak_pwm(resp)

    def hat_set_hvpak_pwm(self, index: int = 0, **kwargs) -> dict:
        self._require_usb("hat_set_hvpak_pwm")
        self._require_hat_present()
        payload = struct.pack(
            '<BBBBBBBBBBBBBBBB',
            index,
            kwargs.get("initial_value", 0),
            0,
            int(kwargs.get("resolution_7bit", False)),
            int(kwargs.get("out_plus_inverted", False)),
            int(kwargs.get("out_minus_inverted", False)),
            int(kwargs.get("async_powerdown", False)),
            int(kwargs.get("autostop_mode", False)),
            int(kwargs.get("boundary_osc_disable", False)),
            int(kwargs.get("phase_correct", False)),
            kwargs.get("deadband", 0),
            int(kwargs.get("stop_mode", False)),
            int(kwargs.get("i2c_trigger", False)),
            kwargs.get("duty_source", 0),
            kwargs.get("period_clock_source", 0),
            kwargs.get("duty_clock_source", 0),
        )
        resp = self._usb_cmd(CmdId.HAT_SET_HVPAK_PWM, payload)
        return self._parse_hvpak_pwm(resp)

    def hat_hvpak_reg_read(self, addr: int) -> dict:
        self._require_usb("hat_hvpak_reg_read")
        self._require_hat_present()
        resp = self._usb_cmd(CmdId.HAT_HVPAK_REG_READ, struct.pack('<B', addr))
        return {"addr": resp[0], "value": resp[1]}

    def hat_hvpak_reg_write_masked(self, addr: int, mask: int, value: int) -> dict:
        self._require_usb("hat_hvpak_reg_write_masked")
        self._require_hat_present()
        resp = self._usb_cmd(CmdId.HAT_HVPAK_REG_WRITE_MASKED, struct.pack('<BBB', addr, mask, value))
        return {"addr": resp[0], "mask": resp[1], "value": resp[2], "actual": resp[3]}

    # ------------------------------------------------------------------
    # ── HAT Logic Analyzer ───────────────────────────────────────────
    # ------------------------------------------------------------------

    def hat_la_configure(self, channels: int = 4, rate_hz: int = 1000000, depth: int = 100000, rle_enabled: bool = False) -> bool:
        """
        Configure the HAT logic analyzer.

        :param channels: Number of channels (1, 2, or 4)
        :param rate_hz:  Sample rate in Hz (max ~100MHz for 1ch, ~25MHz for 4ch)
        :param depth:    Total samples to capture
        :param rle_enabled: Enable run-length encoding for memory/stream modes
        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        payload = struct.pack('<BIIB', channels, rate_hz, depth, 1 if rle_enabled else 0)
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_CONFIG, payload)
            return True
        else:
            raise NotImplementedError("LA control is USB-only")

    def hat_la_set_trigger(self, trigger_type=0, channel: int = 0) -> bool:
        """
        Set the trigger condition.

        :param trigger_type: LaTriggerType (0=none, 1=rising, 2=falling, 3=both, 4=high, 5=low)
        :param channel: Which channel to trigger on (0-3)
        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        payload = struct.pack('<BB', int(trigger_type), channel)
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_TRIGGER, payload)
            return True
        else:
            raise NotImplementedError("LA control is USB-only")

    def hat_la_arm(self) -> bool:
        """Arm the logic analyzer trigger and start waiting for capture.

        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_ARM)
            return True
        raise NotImplementedError("LA control is USB-only")

    def hat_la_force(self) -> bool:
        """Force trigger immediately (bypass trigger condition).

        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_FORCE)
            return True
        raise NotImplementedError("LA control is USB-only")

    def hat_la_stop(self) -> bool:
        """Stop capture and return to idle.

        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_STOP)
            return True
        raise NotImplementedError("LA control is USB-only")

    def hat_la_stream_start(self) -> bool:
        """Start LA streaming over vendor bulk endpoint.

        Triggers HAT_CMD_LA_STREAM_START on the RP2040, which calls
        bb_la_start_stream() and queues PKT_START on EP_IN.

        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_STREAM_START)
            return True
        raise NotImplementedError("LA control is USB-only")

    def hat_la_stream_usb_cycle(self, duration_s: float = 1.0) -> list:
        """
        Execute a gapless capture reading from the dedicated Logic Analyzer USB Vendor Bulk Interface.
        Requires pyusb (`pip install pyusb`) as an optional dependency.

        This commands EP_OUT to start the stream, continuously polls EP_IN to aggregate data, and
        gracefully halts it. It handles decompression of RLE payloads inherently.

        :param duration_s: Native streaming duration boundary before graceful termination is invoked.
        :returns: List of decoded channel arrays extracted from the payloads.
        :raises ImportError: If PyUSB is omitted from the underlying python suite.
        """
        self._require_hat_present()
        import time
        try:
            import usb.core
            import usb.util
        except ImportError:
            raise ImportError(
                "pyusb is required for high-speed streaming capability: pip install pyusb"
            )

        dev = usb.core.find(idVendor=0x2E8A, idProduct=0x000C)
        if dev is None:
            raise RuntimeError("BugBuster device (VID 0x2E8A, PID 0x000C) not found.")
            
        LA_INTERFACE = 3
        EP_IN = 0x87
        EP_OUT = 0x06
        
        try:
            import platform
            if platform.system() != "Darwin":
                try: pass # dev.set_configuration() # Optional based on env
                except: pass
            
            # Flush existing claims
            try: usb.util.claim_interface(dev, LA_INTERFACE)
            except Exception:
                usb.util.release_interface(dev, LA_INTERFACE)
                usb.util.claim_interface(dev, LA_INTERFACE)

            # Pre-flight teardown to assert clean state
            try: dev.write(EP_OUT, bytes([0x00]), timeout=2000) # STREAM_CMD_STOP
            except usb.core.USBError: pass
            time.sleep(0.05)
            
            # Start hardware pump
            dev.write(EP_OUT, bytes([0x01]), timeout=2000) # STREAM_CMD_START
            
            t_start = time.monotonic()
            stream_buf = bytearray()
            aggregated_payload = bytearray()
            
            PKT_START = 0x01
            PKT_DATA = 0x02
            PKT_STOP = 0x03
            PKT_ERROR = 0x04

            active = False
            while (time.monotonic() - t_start) < duration_s:
                try:
                    chunk = dev.read(EP_IN, 16384, timeout=500)
                    if chunk:
                        stream_buf.extend(chunk)
                except usb.core.USBError as e:
                    if e.errno in (110, 60, 10060) or "timeout" in str(e).lower():
                        continue
                    raise

                # Parse frame packets
                while len(stream_buf) >= 4:
                    pkt_type = stream_buf[0]
                    payload_len = stream_buf[2]
                    info = stream_buf[3]
                    frame_len = 4 + payload_len
                    if len(stream_buf) < frame_len:
                        break # Incomplete frame, wait for next transfer
                        
                    payload = stream_buf[4:frame_len]
                    del stream_buf[0:frame_len]
                    
                    if pkt_type == PKT_START:
                        active = True
                    elif pkt_type == PKT_DATA and active:
                        if info & 0x01: # INFO_COMPRESSED
                            # RLE logic
                            i = 0
                            while i + 1 < len(payload):
                                val = payload[i]
                                count = payload[i+1] + 1
                                aggregated_payload.extend(bytes([val]) * count)
                                i += 2
                        else:
                            aggregated_payload.extend(payload)
                    elif pkt_type == PKT_STOP:
                        break
                    elif pkt_type == PKT_ERROR:
                        print(f"Firmware rejected stream (info={info})")
                        break

        finally:
            try: dev.write(EP_OUT, bytes([0x00]), timeout=2000)
            except: pass
            try: usb.util.release_interface(dev, LA_INTERFACE)
            except: pass
            
        status = self.hat_la_get_status()
        channels = status.get("channels", 4)
        return self.hat_la_decode(bytes(aggregated_payload), channels=channels)

    def hat_la_log_enable(self, enable: bool = True) -> bool:
        """Enable/disable RP2040 log relay via ESP32 side-channel.

        When enabled, ``bb_la_log()`` messages on the RP2040 are forwarded
        through the HAT UART to the ESP32, which relays them to the host
        as ``BBP_EVT_LA_LOG`` events.  Register a callback with
        :meth:`on_la_log` to receive them.

        :param enable: ``True`` to start relaying, ``False`` to stop.
        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        payload = struct.pack('B', int(enable))
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_LOG_ENABLE, payload)
            return True
        raise NotImplementedError("LA log control is USB-only")

    def hat_la_usb_reset(self) -> bool:
        """Reinitialize the RP2040 vendor bulk endpoint to a clean state.

        Resets all USB transport state and performs a one-shot DCD
        write_clear + read_flush.  Use once per session (e.g. in test
        preflight) to recover from stuck endpoints.

        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_USB_RESET)
            return True
        raise NotImplementedError("LA USB reset is USB-only")

    def hat_la_get_status(self) -> dict:
        """Get logic analyzer capture status.

        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        _STATE_NAMES = {
            0: "idle",
            1: "armed",
            2: "capturing",
            3: "done",
            4: "streaming",
            5: "error",
        }
        _STREAM_STOP_NAMES = {
            0: "none",
            1: "host_stop",
            2: "usb_short_write",
            3: "dma_overrun",
        }
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_LA_STATUS)
            off = 0
            state = resp[off]; off += 1
            channels = resp[off]; off += 1
            captured = struct.unpack_from('<I', resp, off)[0]; off += 4
            total = struct.unpack_from('<I', resp, off)[0]; off += 4
            rate = struct.unpack_from('<I', resp, off)[0]; off += 4
            result = {
                "state": state,
                "state_name": _STATE_NAMES.get(state, "unknown"),
                "channels": channels,
                "samples_captured": captured,
                "total_samples": total,
                "actual_rate_hz": rate,
            }
            if len(resp) >= off + 2:
                result["usb_connected"] = bool(resp[off]); off += 1
                result["usb_mounted"] = bool(resp[off]); off += 1
            if len(resp) >= off + 1:
                stop_reason = resp[off]; off += 1
                result["stream_stop_reason"] = stop_reason
                result["stream_stop_reason_name"] = _STREAM_STOP_NAMES.get(stop_reason, "unknown")
            if len(resp) >= off + 4:
                result["stream_overrun_count"] = struct.unpack_from('<I', resp, off)[0]
                off += 4
            if len(resp) >= off + 4:
                result["stream_short_write_count"] = struct.unpack_from('<I', resp, off)[0]
                off += 4
            if len(resp) >= off + 1:
                result["usb_rearm_pending"] = bool(resp[off])
                off += 1
            if len(resp) >= off + 1:
                result["usb_rearm_request_count"] = resp[off]
                off += 1
            if len(resp) >= off + 1:
                result["usb_rearm_complete_count"] = resp[off]
                off += 1
            return result
        else:
            # Phase 0 HTTP schema mapping
            st = self._http_get("/hat/la/status")
            state_name = _first_present(st, "stateName", "state_name", default="IDLE").lower()
            _S_NAME_TO_CODE = {"idle": 0, "armed": 1, "capturing": 2, "done": 3, "streaming": 4, "error": 5}
            return {
                "state": _S_NAME_TO_CODE.get(state_name, 0),
                "state_name": state_name,
                "channels": _parse_int_maybe_hex(_first_present(st, "channels"), 4),
                "samples_captured": _parse_int_maybe_hex(_first_present(st, "samplesCaptured", "samples_captured"), 0),
                "total_samples": _parse_int_maybe_hex(_first_present(st, "maxSamples", "total_samples"), 0),
                "actual_rate_hz": _parse_int_maybe_hex(_first_present(st, "actualRateHz", "clockHz", "rate"), 0),
                "active": bool(_first_present(st, "active", default=False)),
                "trigger_armed": bool(_first_present(st, "triggerArmed", default=False)),
                "usb_rearm_pending": bool(_first_present(st, "usbRearmPending", "usb_rearm_pending", default=False)),
                "usb_rearm_request_count": _parse_int_maybe_hex(_first_present(st, "usbRearmRequestCount", "usb_rearm_request_count"), 0),
                "usb_rearm_complete_count": _parse_int_maybe_hex(_first_present(st, "usbRearmCompleteCount", "usb_rearm_complete_count"), 0),
            }

    def hat_la_read_all(self) -> bytes:
        """
        Read all captured LA data from the buffer. Blocks until complete.
        Returns raw bytes — use hat_la_decode() to convert to channel arrays.

        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        status = self.hat_la_get_status()
        if status["state"] not in (3,):  # LA_STATE_DONE
            raise RuntimeError(f"LA not in DONE state (state={status['state_name']})")

        channels = status["channels"]
        samples = status["samples_captured"]
        samples_per_word = 32 // channels
        total_bytes = ((samples + samples_per_word - 1) // samples_per_word) * 4

        data = bytearray()
        chunk_size = 28  # Max per HAT frame
        offset = 0
        while offset < total_bytes:
            remaining = total_bytes - offset
            req_len = min(remaining, chunk_size)
            payload = struct.pack('<IH', offset, req_len)
            resp = self._usb_cmd(CmdId.HAT_LA_READ, payload)
            # Response: [offset:u32, actual_len:u8, data...]
            actual = resp[4]
            if actual == 0:
                break
            data.extend(resp[5:5 + actual])
            offset += actual

        return bytes(data)

    @staticmethod
    def hat_la_decode(raw: bytes, channels: int = 4) -> list:
        """
        Decode raw LA capture data into per-channel sample arrays.

        :param raw: Raw bytes from hat_la_read_all()
        :param channels: Number of channels (1, 2, or 4)
        :return: List of channels, each a list of 0/1 values
        """
        result = [[] for _ in range(channels)]
        bits_per_sample = channels

        for byte_val in raw:
            for bit_pos in range(0, 8, bits_per_sample):
                for ch in range(channels):
                    result[ch].append((byte_val >> (bit_pos + ch)) & 1)

        return result

    # ------------------------------------------------------------------
    # ── AD74416H Watchdog ────────────────────────────────────────────
    # ------------------------------------------------------------------

    def set_watchdog(self, enable: bool = False, timeout_code: int = 9) -> None:
        """
        Enable or disable the AD74416H hardware watchdog timer.
        If enabled and no SPI transaction occurs within the timeout,
        the device resets all channels to HIGH_IMP.

        :param enable: True to enable, False to disable.
        :param timeout_code: 0=1ms, 1=5ms, 2=10ms, 3=25ms, 4=50ms,
                             5=100ms, 6=250ms, 7=500ms, 8=750ms, 9=1000ms, 10=2000ms.
                             Firmware safety policy clamps enabled watchdogs to >=500ms.
        """
        if self._usb:
            timeout_code = min(timeout_code, 10)
            if enable and timeout_code < 7:
                timeout_code = 7
            payload = struct.pack('<BB', int(enable), timeout_code)
            self._usb_cmd(CmdId.SET_WATCHDOG, payload)
        else:
            raise NotImplementedError("Watchdog control is USB-only (requires SPI)")

    # ------------------------------------------------------------------
    # ── Waveform generator ───────────────────────────────────────────
    # ------------------------------------------------------------------

    def start_waveform(
        self,
        channel:   int,
        waveform:  WaveformType,
        freq_hz:   float,
        amplitude: float,
        offset:    float      = 0.0,
        mode:      OutputMode = OutputMode.VOLTAGE,
    ) -> None:
        """
        Start the software waveform generator on a channel.

        The firmware continuously updates the DAC at the requested frequency.
        Only one waveform can run at a time; call :meth:`stop_waveform` before
        starting a new one.

        *channel*   — output channel (0–3).
        *waveform*  — :class:`WaveformType` (SINE, SQUARE, TRIANGLE, SAWTOOTH).
        *freq_hz*   — frequency in Hz (0.01–100 Hz supported).
        *amplitude* — peak amplitude in volts or milliamps depending on *mode*.
        *offset*    — DC offset in the same units as *amplitude*.
        *mode*      — :class:`OutputMode` (VOLTAGE or CURRENT).

        Example — generate a 1 Hz sine wave between 0 V and 5 V on channel 0::

            bb.set_channel_function(0, ChannelFunction.VOUT)
            bb.start_waveform(0, WaveformType.SINE, freq_hz=1.0, amplitude=2.5, offset=2.5)
        """
        if self._usb:
            payload = struct.pack(
                '<BBfffB',
                channel, int(waveform), float(freq_hz),
                float(amplitude), float(offset), int(mode),
            )
            self._usb_cmd(CmdId.START_WAVEGEN, payload)
        else:
            self._http_post("/wavegen/start", {
                "ch": channel, "waveform": int(waveform),
                "freq_hz": float(freq_hz), "amplitude": float(amplitude),
                "offset": float(offset), "mode": int(mode),
            })

    def stop_waveform(self) -> None:
        """Stop the waveform generator.  The DAC output stays at the last value."""
        if self._usb:
            self._usb_cmd(CmdId.STOP_WAVEGEN)
        else:
            self._http_post("/wavegen/stop")

    # ------------------------------------------------------------------
    # ── ADC streaming  (USB only) ────────────────────────────────────
    # ------------------------------------------------------------------

    def start_adc_stream(
        self,
        channels: list[int],
        divider:  int = 1,
        callback: Optional[Callable[[int, list[int]], None]] = None,
    ) -> None:
        """
        Start continuous ADC data streaming. **USB only.**

        The device pushes batches of raw 24-bit ADC codes at up to 9.6 kSPS
        per channel.

        *channels* — list of channel indices to stream (e.g. ``[0, 1, 2, 3]``).
        *divider*  — sample rate divisor (1 = full rate, 2 = half rate, …).
        *callback* — optional ``callback(channel_mask: int, samples: list[int])``
                     called for every incoming ADC_DATA event.

        Example::

            def on_data(mask, samples):
                print(f"Received {len(samples)//bin(mask).count('1')} samples/channel")

            bb.start_adc_stream([0, 1], callback=on_data)
            time.sleep(2)
            bb.stop_adc_stream()
        """
        self._require_usb("start_adc_stream")

        mask = 0
        for ch in channels:
            mask |= (1 << ch)

        if callback:
            def _handler(payload: bytes):
                ch_mask  = payload[0]
                # samples follow: N groups of active-channel readings
                # each sample is u24 (3 bytes LE)
                n_active = bin(ch_mask).count('1')
                n_samples_total = (len(payload) - 7) // (3 * n_active)
                raw_samples = []
                off = 7  # skip mask(1) + base_ts(4) + count(2)
                while off + 3 <= len(payload):
                    raw_samples.append(int.from_bytes(payload[off:off+3], 'little'))
                    off += 3
                callback(ch_mask, raw_samples)
            self._t.on_event(CmdId.ADC_DATA_EVT, _handler)

        payload = struct.pack('<BB', mask & 0xFF, divider & 0xFF)
        self._usb_cmd(CmdId.START_ADC_STREAM, payload)

    def stop_adc_stream(self) -> None:
        """Stop continuous ADC streaming. **USB only.**"""
        self._require_usb("stop_adc_stream")
        self._usb_cmd(CmdId.STOP_ADC_STREAM)
        self._t.remove_event(CmdId.ADC_DATA_EVT)

    def on_scope_data(self, callback: Callable[[dict], None]) -> None:
        """
        Register a callback for 10 ms oscilloscope buckets. **USB only.**

        The device streams pre-computed min/max/avg per channel every 10 ms.
        The callback receives a dict with keys: ``seq``, ``timestamp_ms``,
        ``count``, ``channels`` (list of 4 dicts with ``avg``, ``min``, ``max``).
        """
        self._require_usb("on_scope_data")

        def _handler(payload: bytes):
            seq, ts_ms, count = struct.unpack_from('<IIH', payload)
            channels = []
            off = 10
            for _ in range(4):
                avg, mn, mx = struct.unpack_from('<fff', payload, off)
                off += 12
                channels.append({"avg": avg, "min": mn, "max": mx})
            callback({"seq": seq, "timestamp_ms": ts_ms, "count": count, "channels": channels})

        self._t.on_event(CmdId.SCOPE_DATA_EVT, _handler)
        self._usb_cmd(CmdId.START_SCOPE_STREAM)

    def on_alert(self, callback: Callable[[dict], None]) -> None:
        """
        Register a callback for hardware alert events. **USB only.**

        The callback receives a dict with ``alert_status`` and
        ``supply_alert_status`` fields.
        """
        self._require_usb("on_alert")

        def _handler(payload: bytes):
            alert, supply = struct.unpack_from('<HH', payload)
            callback({"alert_status": alert, "supply_alert_status": supply})

        self._t.on_event(CmdId.ALERT_EVT, _handler)

    def on_la_log(self, callback: 'Callable[[str], None] | None') -> None:
        """Register callback for RP2040 log messages relayed via ESP32.

        The callback receives a UTF-8 decoded string for each log message.
        Pass ``None`` to unregister.  Requires :meth:`hat_la_log_enable`
        to be called first.
        """
        self._require_usb("on_la_log")
        if callback is None:
            self._t.remove_event(CmdId.HAT_LA_LOG_EVT)
            return

        def _handler(data: bytes) -> None:
            callback(data.decode('utf-8', errors='replace'))

        self._t.on_event(CmdId.HAT_LA_LOG_EVT, _handler)

    def on_din_event(self, callback: Callable[[int, bool, int], None]) -> None:
        """
        Register a callback for digital input edge events. **USB only.**

        The callback receives ``(channel, state, counter)`` where *counter*
        is the cumulative DIN transition count.
        """
        self._require_usb("on_din_event")

        def _handler(payload: bytes):
            ch, state, counter = struct.unpack_from('<BBI', payload)
            callback(ch, bool(state), counter)

        self._t.on_event(CmdId.DIN_EVT, _handler)

    # ------------------------------------------------------------------
    # ── Fault management ─────────────────────────────────────────────
    # ------------------------------------------------------------------

    def clear_alerts(self, channel: int | None = None) -> None:
        """
        Clear alert flags.  Pass a *channel* index to clear only that channel;
        omit or pass ``None`` to clear all channels at once.
        """
        if channel is None:
            if self._usb:
                self._usb_cmd(CmdId.CLEAR_ALL_ALERTS)
            else:
                self._http_post("/faults/clear")
        else:
            if self._usb:
                self._usb_cmd(CmdId.CLEAR_CHAN_ALERT, struct.pack('<B', channel))
            else:
                self._http_post(f"/faults/clear/{channel}")

    def set_channel_alert_mask(self, channel: int, mask: int) -> None:
        """
        Set the per-channel alert mask for a single channel.

        *channel* — channel index (0–3).
        *mask*    — 16-bit alert mask (bit = 1 enables the corresponding alert).
                    0xFFFF enables all alerts, 0x0000 disables all.
        """
        if self._usb:
            payload = struct.pack('<BH', channel, mask & 0xFFFF)
            self._usb_cmd(CmdId.SET_CH_ALERT_MASK, payload)
        else:
            # Firmware route: POST /api/faults/mask/<ch> (registered in webserver.cpp).
            self._http_post(f"/faults/mask/{channel}", {"mask": mask & 0xFFFF})

    def set_alert_mask(self, alert_mask: int, supply_mask: int) -> None:
        """Set the global alert and supply alert masks."""
        if self._usb:
            payload = struct.pack('<HH', alert_mask & 0xFFFF, supply_mask & 0xFFFF)
            self._usb_cmd(CmdId.SET_ALERT_MASK, payload)
        else:
            self._http_post("/faults/mask", {
                "alert_mask": alert_mask, "supply_mask": supply_mask,
            })

    def check_faults(self) -> list[dict]:
        """
        Poll for active faults and return a list of human-readable fault dicts.

        Each dict has keys: ``source`` (str), ``channel`` (int or None),
        ``code`` (int), ``description`` (str).

        Returns an empty list if no faults are active.  Faults are NOT cleared
        automatically — call :meth:`clear_alerts` after handling.

        Example::

            faults = bb.check_faults()
            for f in faults:
                print(f"Fault on {f['source']}: {f['description']}")
                if f['channel'] is not None:
                    bb.clear_alerts(f['channel'])
                else:
                    bb.clear_alerts()
        """
        raw    = self.get_faults()
        result = []

        # Global alert bits (from AD74416H ALERT_STATUS register)
        _GLOBAL_ALERT_BITS = {
            0:  "AVDD_HI supply fault (high)",
            1:  "AVDD_LO supply fault (low)",
            2:  "ALDO1V8 supply fault",
            3:  "DVCC supply fault",
            4:  "AVSS supply fault",
            5:  "REFOUT voltage out of range",
            6:  "Temperature alert (die too hot)",
            7:  "SPI CRC error",
            8:  "Clock source error",
            9:  "RESET detected",
            14: "Supply alert (see supply_alert_status)",
            15: "Channel alert (see per-channel bits)",
        }
        alert_status = raw.get("alert_status", 0)
        for bit, desc in _GLOBAL_ALERT_BITS.items():
            if alert_status & (1 << bit):
                result.append({
                    "source":      "device",
                    "channel":     None,
                    "code":        bit,
                    "description": desc,
                })

        # Per-channel alert bits (from CHANNEL_ALERT_STATUS register)
        _CHANNEL_ALERT_BITS = {
            0:  "Open-circuit detected",
            1:  "Short-circuit / overcurrent",
            2:  "Output voltage error",
            3:  "Output current error",
            4:  "ADC conversion error",
            5:  "DIN open-circuit",
            6:  "DIN short-circuit",
            14: "Channel error",
        }
        for ch in raw.get("channels", []):
            ch_id  = ch.get("id", "?")
            ch_alrt = ch.get("alert", 0)
            for bit, desc in _CHANNEL_ALERT_BITS.items():
                if ch_alrt & (1 << bit):
                    result.append({
                        "source":      f"channel_{ch_id}",
                        "channel":     ch_id,
                        "code":        bit,
                        "description": desc,
                    })

        return result

    def on_fault(self, callback: Callable[[list[dict]], None]) -> None:
        """
        Register a callback that is invoked when the device raises a hardware
        alert. **USB only.**

        The callback receives the result of :meth:`check_faults` — a list of
        fault dicts — so the application can respond immediately without
        polling.

        When the callback fires, the faults are NOT cleared automatically.
        Call :meth:`clear_alerts` inside the callback after handling.

        Example::

            def handle_fault(faults):
                for f in faults:
                    print(f"[ALERT] {f['source']}: {f['description']}")
                bb.clear_alerts()

            bb.on_fault(handle_fault)
        """
        self._require_usb("on_fault")

        def _handler(payload: bytes):
            # Fetch full fault detail — the EVT payload only has status words,
            # but check_faults() gives the human-readable breakdown.
            try:
                faults = self.check_faults()
                if faults:
                    callback(faults)
            except Exception as exc:
                log.error("on_fault callback error: %s", exc)

        self._t.on_event(CmdId.ALERT_EVT, _handler)

    # ------------------------------------------------------------------
    # ── USB Power Delivery ───────────────────────────────────────────
    # ------------------------------------------------------------------

    def usbpd_get_status(self) -> dict:
        """
        Return the USB PD contract status.

        Useful keys: ``attached``, ``voltage_v``, ``current_a``, ``power_w``,
        ``pdos`` (list of 6 available voltage levels).
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.USBPD_GET_STATUS)
            return _parse_usbpd_status(resp)
        return _normalize_http_usbpd(self._http_get("/usbpd"))

    def usbpd_select_voltage(self, voltage_v: int) -> None:
        """
        Request a specific USB PD supply voltage.

        *voltage_v* — one of 5, 9, 12, 15, 18, 20 (volts).
        The source must advertise the requested PDO.
        """
        _V_TO_CODE = {5: 1, 9: 2, 12: 3, 15: 4, 18: 5, 20: 6}
        code = _V_TO_CODE.get(int(voltage_v))
        if code is None:
            raise ValueError(f"voltage_v must be one of {list(_V_TO_CODE)}")
        if self._usb:
            self._usb_cmd(CmdId.USBPD_SELECT_PDO, struct.pack('<B', code))
        else:
            self._http_post("/usbpd/select", {"voltage": int(voltage_v)})

    # ------------------------------------------------------------------
    # ── WiFi management ──────────────────────────────────────────────
    # ------------------------------------------------------------------

    def wifi_get_status(self) -> dict:
        """Return WiFi STA/AP connection status."""
        if self._usb:
            resp = self._usb_cmd(CmdId.WIFI_GET_STATUS)
            return _parse_wifi_status(resp)
        else:
            return _normalize_http_wifi_status(self._http_get("/wifi"))

    def wifi_connect(self, ssid: str, password: str) -> bool:
        """
        Connect the device to a WiFi network.
        Returns ``True`` on success, ``False`` on failure.
        """
        if self._usb:
            ssid_b = ssid.encode()
            pass_b = password.encode()
            payload = (
                struct.pack('<B', len(ssid_b)) + ssid_b +
                struct.pack('<B', len(pass_b)) + pass_b
            )
            resp = self._usb_cmd(CmdId.WIFI_CONNECT, payload)
            return bool(resp[0])
        else:
            return self._http_post("/wifi/connect", {"ssid": ssid, "password": password})

    def wifi_scan(self) -> list[dict]:
        """Scan for nearby WiFi networks. Returns list of {ssid, rssi, auth}."""
        if self._usb:
            resp = self._usb_cmd(CmdId.WIFI_SCAN)
            return _parse_wifi_scan(resp)
        else:
            raw = self._http_get("/wifi/scan")
            return raw.get("networks", raw) if isinstance(raw, dict) else raw

    # ------------------------------------------------------------------
    # ── Raw register access  (USB only, debug/testing) ───────────────
    # ------------------------------------------------------------------

    def register_read(self, address: int) -> int:
        """Read a 16-bit AD74416H SPI register by address. **USB only.**"""
        self._require_usb("register_read")
        resp = self._usb_cmd(CmdId.REGISTER_READ, struct.pack('<B', address & 0xFF))
        return struct.unpack_from('<H', resp, 1)[0]

    def register_write(self, address: int, value: int) -> None:
        """Write a 16-bit value to an AD74416H SPI register. **USB only.**"""
        self._require_usb("register_write")
        self._usb_cmd(CmdId.REGISTER_WRITE, struct.pack('<BH', address & 0xFF, value & 0xFFFF))

    def set_spi_clock(self, clock_hz: int) -> None:
        """
        Adjust the AD74416H SPI clock speed at runtime. **USB only.**
        Valid range: 100 kHz to 20 MHz.
        """
        self._require_usb("set_spi_clock")
        self._usb_cmd(CmdId.SET_SPI_CLOCK, struct.pack('<I', clock_hz))

    def set_level_shifter_oe(self, on: bool) -> None:
        """Enable or disable the TXS0108E level-shifter output enable."""
        if self._usb:
            self._usb_cmd(CmdId.SET_LSHIFT_OE, struct.pack('<B', int(on)))
        else:
            self._http_post("/lshift/oe", {"on": on})


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

    # MUX state: 4 bytes at offset 163 (BugBusterProtocol.md §GET_STATUS).
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


def _parse_pca_status(resp: bytes) -> dict:
    present        = bool(resp[0])
    input0, input1 = resp[1], resp[2]
    out0, out1     = resp[3], resp[4]
    logic_pg       = bool(resp[5])
    vadj1_pg       = bool(resp[6])
    vadj2_pg       = bool(resp[7])
    efuse_faults   = [bool(resp[8 + i]) for i in range(4)]
    return {
        "present": present, "logic_pg": logic_pg,
        "vadj1_pg": vadj1_pg, "vadj2_pg": vadj2_pg,
        "efuse_faults": efuse_faults,
        "input0": input0, "input1": input1,
        "output0": out0,  "output1": out1,
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
    host:    str,
    port:    int   = 80,
    timeout: float = 5.0,
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
    t = HTTPTransport(host, port, timeout)
    bb = BugBuster(t)
    bb.connect()
    return bb
