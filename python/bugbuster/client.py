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

    def hat_get_status(self) -> dict:
        """
        Get HAT expansion board status: detection, connection, pin config.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_GET_STATUS)
            off = 0
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
            return {
                "detected": detected, "connected": connected,
                "type": hat_type, "detect_voltage": detect_v,
                "fw_version": f"{fw_major}.{fw_minor}",
                "config_confirmed": confirmed,
                "pin_config": pins,
            }
        else:
            return self._http_get("/hat")

    def hat_set_pin(self, ext_pin: int, function) -> bool:
        """
        Set a single EXP_EXT pin function on the HAT.

        :param ext_pin:  Pin index 0-3 (EXP_EXT_1 to EXP_EXT_4)
        :param function: HatPinFunction value
        :return: True if HAT acknowledged
        """
        func_val = int(function)
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
        if self._usb:
            self._usb_cmd(CmdId.HAT_RESET)
            return True
        else:
            resp = self._http_post("/hat/reset", {})
            return resp.get("ok", False)

    def hat_detect(self) -> dict:
        """Re-run HAT detection and return result."""
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_DETECT)
            off = 0
            detected = bool(resp[off]); off += 1
            hat_type = resp[off]; off += 1
            detect_v = struct.unpack_from('<f', resp, off)[0]; off += 4
            connected = bool(resp[off]); off += 1
            return {"detected": detected, "type": hat_type,
                    "detect_voltage": detect_v, "connected": connected}
        else:
            return self._http_post("/hat/detect", {})

    def hat_set_power(self, connector: int, enable: bool) -> bool:
        """
        Enable or disable target power on a HAT connector.

        :param connector: 0 = Connector A (VADJ1), 1 = Connector B (VADJ2)
        :param enable: True to enable power
        """
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
            return {"connectors": connectors, "io_voltage_mv": io_mv}
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
        """
        if self._usb:
            payload = struct.pack('<HB', target_voltage_mv, connector)
            self._usb_cmd(CmdId.HAT_SETUP_SWD, payload)
            return True
        else:
            return self._http_post("/hat/setup_swd", {
                "target_voltage_mv": target_voltage_mv,
                "connector": connector,
            }).get("ok", False)

    # ------------------------------------------------------------------
    # ── HAT Logic Analyzer ───────────────────────────────────────────
    # ------------------------------------------------------------------

    def hat_la_configure(self, channels: int = 4, rate_hz: int = 1000000, depth: int = 100000) -> bool:
        """
        Configure the HAT logic analyzer.

        :param channels: Number of channels (1, 2, or 4)
        :param rate_hz:  Sample rate in Hz (max ~100MHz for 1ch, ~25MHz for 4ch)
        :param depth:    Total samples to capture
        """
        payload = struct.pack('<BII', channels, rate_hz, depth)
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
        """
        payload = struct.pack('<BB', int(trigger_type), channel)
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_TRIGGER, payload)
            return True
        else:
            raise NotImplementedError("LA control is USB-only")

    def hat_la_arm(self) -> bool:
        """Arm the logic analyzer trigger and start waiting for capture."""
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_ARM)
            return True
        raise NotImplementedError("LA control is USB-only")

    def hat_la_force(self) -> bool:
        """Force trigger immediately (bypass trigger condition)."""
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_FORCE)
            return True
        raise NotImplementedError("LA control is USB-only")

    def hat_la_stop(self) -> bool:
        """Stop capture and return to idle."""
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_STOP)
            return True
        raise NotImplementedError("LA control is USB-only")

    def hat_la_get_status(self) -> dict:
        """Get logic analyzer capture status."""
        _STATE_NAMES = {0: "idle", 1: "armed", 2: "capturing", 3: "done", 4: "error"}
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_LA_STATUS)
            off = 0
            state = resp[off]; off += 1
            channels = resp[off]; off += 1
            captured = struct.unpack_from('<I', resp, off)[0]; off += 4
            total = struct.unpack_from('<I', resp, off)[0]; off += 4
            rate = struct.unpack_from('<I', resp, off)[0]; off += 4
            return {
                "state": state,
                "state_name": _STATE_NAMES.get(state, "unknown"),
                "channels": channels,
                "samples_captured": captured,
                "total_samples": total,
                "actual_rate_hz": rate,
            }
        raise NotImplementedError("LA control is USB-only")

    def hat_la_read_all(self) -> bytes:
        """
        Read all captured LA data from the buffer. Blocks until complete.
        Returns raw bytes — use hat_la_decode() to convert to channel arrays.
        """
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
                             5=100ms, 6=250ms, 7=500ms, 8=750ms, 9=1000ms, 10=2000ms
        """
        if self._usb:
            payload = struct.pack('<BB', int(enable), min(timeout_code, 10))
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
