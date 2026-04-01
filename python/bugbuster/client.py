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
    VoutRange, CurrentLimit, DoMode, AvddSelect,
    PowerControl, UsbPdVoltage, ErrorCode,
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

    # ------------------------------------------------------------------
    # Connection lifecycle
    # ------------------------------------------------------------------

    def connect(self):
        """Open the connection and perform the protocol handshake."""
        if not self._connected:
            self._t.connect()
            self._connected = True
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
        return self._t.post(path, body)

    def _require_usb(self, method: str):
        if not self._usb:
            raise NotImplementedError(f"{method} is only available over USB")

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

    def get_device_info(self) -> DeviceInfo:
        """Read silicon identification (revision + ID words)."""
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_DEVICE_INFO)
            spi_ok, rev = struct.unpack_from('<BB', resp)
            id0, id1    = struct.unpack_from('<HH', resp, 2)
            return DeviceInfo(bool(spi_ok), rev, id0, id1)
        else:
            d = self._http_get("/device/info")
            return DeviceInfo(True, d["silicon_rev"], d["silicon_id0"], d["silicon_id1"])

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
            return self._http_get("/status")

    def get_faults(self) -> dict:
        """Return global and per-channel fault/alert registers."""
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_FAULTS)
            return _parse_faults(resp)
        else:
            return self._http_get("/faults")

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
            return self._http_get(f"/channel/{channel}/dac/readback")["code"]

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
                raw=d.get("raw_code", 0),
                value=d.get("value", 0.0),
                range=d.get("range", 0),
                rate=d.get("rate", 0),
                mux=d.get("mux", 0),
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
        """
        if self._usb:
            payload = struct.pack(
                '<BBBBBBB',
                channel, threshold, int(thresh_mode), debounce,
                sink, int(sink_range), (int(oc_detect) | (int(sc_detect) << 1)),
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
            raw  = self._http_get("/gpio")
            return [
                GpioStatus(
                    id=g["id"], mode=GpioMode(g["mode"]),
                    output=g["output"], input=g["input"], pulldown=g["pulldown"],
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
        tx_pin:    int   = 17,
        rx_pin:    int   = 18,
        baudrate:  int   = 115200,
        data_bits: int   = 8,
        parity:    int   = 0,
        stop_bits: int   = 0,
        enabled:   bool  = True,
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
            Activate the bridge immediately (default True).
        """
        self._require_usb("set_uart_config")
        payload = struct.pack(
            '<BBBBIBBBb',
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
            return self._http_get("/mux")["states"]

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
            payload = struct.pack('<bf', channel, float(voltage))
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
            from .constants import CmdId as _C
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

    def clear_alerts(self, channel: int = None) -> None:
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
        else:
            return self._http_get("/usbpd")

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
            return self._http_get("/wifi")

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
            return self._http_get("/wifi/scan")

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
        off = 15 + i * 28
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
        rtd_ua,        = struct.unpack_from('<H', resp, off + 26)
        channels.append({
            "id": ch_id, "function": func,
            "adc_raw": raw, "adc_value": adc_val,
            "adc_range": adc_rng, "adc_rate": adc_rate, "adc_mux": adc_mux,
            "dac_code": dac_code, "dac_value": dac_val,
            "din_state": din_state, "din_counter": din_counter,
            "do_state": do_state, "channel_alert": ch_alert,
            "rtd_excitation_ua": rtd_ua,
        })

    diagnostics = []
    for i in range(4):
        off     = 127 + i * 7
        src, rc = struct.unpack_from('<BH', resp, off)
        val,    = struct.unpack_from('<f', resp, off + 3)
        diagnostics.append({"source": src, "raw_code": rc, "value": val})

    return {
        "spi_ok": spi_ok, "die_temp_c": die_temp,
        "alert_status": alert_status, "alert_mask": alert_mask,
        "supply_alert_status": supply_alert_status,
        "channels": channels, "diagnostics": diagnostics,
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
