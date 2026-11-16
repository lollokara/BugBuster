"""BugBuster client methods: AD74416H channels, GPIO, digital IO, watchdog, waveform generator, ADC streaming.

One layer of :class:`bugbuster.client.BugBuster` (core -> system -> scripting ->
channels -> bus -> power -> hat). ``bugbuster.client`` imports the chain at its
end, so import ``bugbuster.client`` (or ``bugbuster``) first.
"""

from typing import Callable
from typing import Optional
import struct

from .constants import AdcMux
from .constants import AdcRange
from .constants import AdcRate
from .constants import AvddSelect
from .constants import ChannelFunction
from .constants import CmdId
from .constants import CurrentLimit
from .constants import DoMode
from .constants import GpioMode
from .constants import OutputMode
from .constants import RtdCurrent
from .constants import VoutRange
from .constants import WaveformType
from .client import (
    AdcDspWindow,
    AdcResult,
    GpioStatus,
    _first_present,
    _parse_int_maybe_hex,
    _require_resp_len,
)
from .client_scripting import _ClientScripting


class _ClientChannels(_ClientScripting):
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
        def _body():
            if self._usb:
                payload = struct.pack('<BB', channel, int(function))
                self._usb_cmd(CmdId.SET_CHANNEL_FUNC, payload)
            else:
                self._http_post(f"/channel/{channel}/function", {"function": int(function)})
        self._auto_claim_wrap([channel + 12], _body)

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
        def _body():
            if self._usb:
                payload = struct.pack('<BfB', channel, float(voltage), int(bipolar))
                self._usb_cmd(CmdId.SET_DAC_VOLTAGE, payload)
            else:
                self._http_post(
                    f"/channel/{channel}/dac",
                    {"voltage": float(voltage), "bipolar": bipolar},
                )
        self._auto_claim_wrap([channel + 12], _body)

    def set_dac_current(self, channel: int, current_ma: float) -> None:
        """
        Set the DAC output current for a channel in IOUT mode.

        *current_ma* — target current in milliamps (0–25 mA).

        The channel must already be in :attr:`ChannelFunction.IOUT` mode.
        """
        def _body():
            if self._usb:
                payload = struct.pack('<Bf', channel, float(current_ma))
                self._usb_cmd(CmdId.SET_DAC_CURRENT, payload)
            else:
                self._http_post(f"/channel/{channel}/dac", {"current_mA": float(current_ma)})
        self._auto_claim_wrap([channel + 12], _body)

    def set_dac_code(self, channel: int, code: int) -> None:
        """
        Write a raw 16-bit DAC code directly (0–65535).
        Useful for precise calibration — normally prefer :meth:`set_dac_voltage`.
        """
        def _body():
            if self._usb:
                payload = struct.pack('<BH', channel, code & 0xFFFF)
                self._usb_cmd(CmdId.SET_DAC_CODE, payload)
            else:
                self._http_post(f"/channel/{channel}/dac", {"code": code & 0xFFFF})
        self._auto_claim_wrap([channel + 12], _body)

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
        def _body():
            if self._usb:
                payload = struct.pack('<BB', channel, int(bipolar))
                self._usb_cmd(CmdId.SET_VOUT_RANGE, payload)
            else:
                self._http_post(f"/channel/{channel}/vout/range", {"bipolar": bipolar})
        self._auto_claim_wrap([channel + 12], _body)

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
        def _body():
            if self._usb:
                payload = struct.pack('<BB', channel, int(limit))
                self._usb_cmd(CmdId.SET_CURRENT_LIMIT, payload)
            else:
                self._http_post(f"/channel/{channel}/ilimit", {"limit8mA": bool(limit)})
        self._auto_claim_wrap([channel + 12], _body)

    def set_avdd_select(self, channel: int, select: AvddSelect) -> None:
        """
        Choose which analog supply rail feeds a channel's output stage.

        *select* — :class:`AvddSelect`. ``HI`` is required for outputs above
        roughly 10 V; ``LO`` saves power on low-voltage loads.
        """
        def _body():
            if self._usb:
                payload = struct.pack('<BB', channel, int(select))
                self._usb_cmd(CmdId.SET_AVDD_SELECT, payload)
            else:
                self._http_post(f"/channel/{channel}/avdd", {"select": int(select)})
        self._auto_claim_wrap([channel + 12], _body)

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
            _require_resp_len(resp, 11, "GET_ADC_VALUE")
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
        if not 0 <= int(channel) < 4:
            raise ValueError("channel must be 0-3")
        mux = AdcMux(mux)
        range_ = AdcRange(range_)
        rate = AdcRate(rate)

        def _body():
            if self._usb:
                payload = struct.pack('<BBBB', int(channel), int(mux), int(range_), int(rate))
                self._usb_cmd(CmdId.SET_ADC_CONFIG, payload)
            else:
                self._http_post(
                    f"/channel/{channel}/adc/config",
                    {"mux": int(mux), "range": int(range_), "rate": int(rate)},
                )
        self._auto_claim_wrap([channel + 12], _body)

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
        def _body():
            if self._usb:
                payload = struct.pack('<BB', channel, int(current))
                self._usb_cmd(CmdId.SET_RTD_CONFIG, payload)
            else:
                self._http_post(f"/channel/{channel}/rtd/config", {"current": int(current)})
        self._auto_claim_wrap([channel + 12], _body)

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
        def _body():
            if self._usb:
                payload = struct.pack('<BB', channel, int(on))
                self._usb_cmd(CmdId.SET_DO_STATE, payload)
            else:
                self._http_post(f"/channel/{channel}/do/set", {"on": on})
        self._auto_claim_wrap([channel + 12], _body)

    def set_do_config(
        self,
        channel:      int,
        mode:         DoMode,
        src_sel_gpio: bool = False,
        t1:           int  = 0,
        t2:           int  = 0,
    ) -> None:
        """
        Configure a channel's digital-output driver.

        *mode*         — :class:`DoMode` (HIGH_SIDE, LOW_SIDE, PUSH_PULL).
        *src_sel_gpio* — drive the output from a GPIO instead of the DO state
                         register, so an external signal can toggle it.
        *t1*, *t2*     — slew/debounce timing codes passed straight through to
                         the AD74416H.

        Call before :meth:`set_digital_output`, which only sets the level.
        """
        def _body():
            if self._usb:
                payload = struct.pack('<BBBBB', channel, int(mode),
                                      int(bool(src_sel_gpio)), t1 & 0xFF, t2 & 0xFF)
                self._usb_cmd(CmdId.SET_DO_CONFIG, payload)
            else:
                self._http_post(f"/channel/{channel}/do/config", {
                    "mode": int(mode),
                    "srcSelGpio": bool(src_sel_gpio),
                    "t1": t1,
                    "t2": t2,
                })
        self._auto_claim_wrap([channel + 12], _body)

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
        def _body():
            if self._usb:
                payload = struct.pack(
                    '<BBBBBBBB',
                    channel, threshold, int(thresh_mode), debounce,
                    sink, int(sink_range), int(oc_detect), int(sc_detect),
                )
                self._usb_cmd(CmdId.SET_DIN_CONFIG, payload)
            else:
                self._http_post(f"/channel/{channel}/din/config", {
                    "thresh":     threshold,
                    "threshMode": bool(thresh_mode),
                    "debounce":   debounce,
                    "sink":       sink,
                    "sinkRange":  bool(sink_range),
                    "ocDet":      bool(oc_detect),
                    "scDet":      bool(sc_detect),
                })
        self._auto_claim_wrap([channel + 12], _body)

    # ------------------------------------------------------------------
    # ── GPIO (12 logical IOs, indices 0–11 = IO1..IO12) ──────────────
    # Post 2026-04-25 IO remap. Firmware GET_GPIO_STATUS now returns 12
    # 5-byte records (DIO_NUM_IOS=12), not the legacy AD74416H A–F (6).
    # ------------------------------------------------------------------

    GPIO_COUNT = 12

    def get_gpio(self) -> list[GpioStatus]:
        """
        Read the state of all 12 logical IO pins (post 2026-04-25 remap).
        Returns a list of :class:`GpioStatus` namedtuples.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.GET_GPIO_STATUS)
            pins = []
            for i in range(self.GPIO_COUNT):
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

        *gpio* — logical IO index 0–11 (IO1..IO12).
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
        def _body():
            if self._usb:
                self._usb_cmd(CmdId.DIO_CONFIG, struct.pack('<BB', io, mode))
            else:
                self._http_post(f"/dio/{io}/config", {"mode": mode})
        self._auto_claim_wrap([io - 1], _body)

    def dio_write(self, io: int, value: bool) -> None:
        """
        Set a digital IO output level.

        *io* must be configured as output (mode=2) first.
        *value* — ``True`` for HIGH, ``False`` for LOW.
        """
        def _body():
            if self._usb:
                self._usb_cmd(CmdId.DIO_WRITE, struct.pack('<BB', io, int(value)))
            else:
                self._http_post(f"/dio/{io}/set", {"value": value})
        self._auto_claim_wrap([io - 1], _body)

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
        *freq_hz*   — frequency in Hz (0.1–100 Hz; the firmware rejects lower).
        *amplitude* — peak amplitude in volts or milliamps depending on *mode*.
        *offset*    — DC offset in the same units as *amplitude*.
        *mode*      — :class:`OutputMode` (VOLTAGE or CURRENT).

        Example — generate a 1 Hz sine wave between 0 V and 5 V on channel 0::

            bb.set_channel_function(0, ChannelFunction.VOUT)
            bb.start_waveform(0, WaveformType.SINE, freq_hz=1.0, amplitude=2.5, offset=2.5)
        """
        if not 0 <= int(channel) < 4:
            raise ValueError("channel must be 0-3")
        waveform = WaveformType(waveform)
        mode = OutputMode(mode)
        if not 0.1 <= float(freq_hz) <= 100.0:
            raise ValueError("freq_hz must be 0.1-100.0")
        max_out = 25.0 if mode == OutputMode.CURRENT else 12.0
        if not 0.0 <= float(amplitude) <= max_out:
            raise ValueError(f"amplitude must be 0.0-{max_out}")
        if not -max_out <= float(offset) <= max_out:
            raise ValueError(f"offset must be -{max_out}..{max_out}")

        if self._usb:
            payload = struct.pack(
                '<BBfffB',
                int(channel), int(waveform), float(freq_hz),
                float(amplitude), float(offset), int(mode),
            )
            self._usb_cmd(CmdId.START_WAVEGEN, payload)
        else:
            self._http_post("/wavegen/start", {
                "channel": int(channel), "waveform": int(waveform),
                "freq_hz": float(freq_hz), "amplitude": float(amplitude),
                "offset": float(offset), "mode": int(mode),
            })

    def stop_waveform(self) -> None:
        """Stop the waveform generator.  The waveform channel returns to HIGH_IMP (output off)."""
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

        The device pushes batches of raw 24-bit ADC codes. The converter runs
        at up to 9.6 kSPS, but the stream delivers about 1.2 k samples/s at
        most (SPI read cost per sample); the START reply's effective rate
        reports the capped value. *divider* is applied on the device.

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

    # ------------------------------------------------------------------
    # ADC DSP streaming  (USB + HTTP/WebSocket)
    # ------------------------------------------------------------------

    @staticmethod
    def _parse_adc_dsp_evt(payload: bytes) -> "AdcDspWindow":
        off = 0
        channel       = payload[off]; off += 1
        timestamp_us, = struct.unpack_from('<I', payload, off); off += 4
        n_samples,    = struct.unpack_from('<H', payload, off); off += 2
        min_v, max_v, mean_v, rms_v = struct.unpack_from('<ffff', payload, off); off += 16
        n_fft = payload[off]; off += 1
        fft_peaks = []
        for _ in range(n_fft):
            bin_idx = payload[off]; off += 1
            mag,    = struct.unpack_from('<f', payload, off); off += 4
            fft_peaks.append((bin_idx, mag))
        n_spikes = payload[off]; off += 1
        spikes = []
        for _ in range(n_spikes):
            offset_us, = struct.unpack_from('<I', payload, off); off += 4
            val,       = struct.unpack_from('<f', payload, off); off += 4
            spikes.append((offset_us, val))
        return AdcDspWindow(channel, timestamp_us, n_samples,
                            min_v, max_v, mean_v, rms_v,
                            fft_peaks, spikes)

    def start_adc_dsp_stream(
        self,
        channel:         int,
        rate:            Optional["AdcRate"] = None,
        window_samples:  int        = 256,
        spike_threshold: float      = 0.1,
        n_fft_peaks:     int        = 8,
        callback:        Optional[Callable[["AdcDspWindow"], None]] = None,
    ) -> None:
        """
        Start high-rate ADC streaming with on-device DSP processing.

        Works over **USB** (via BBP binary events) and **HTTP/WiFi** (via
        WebSocket ``/api/ws/stream`` with ``"adc-dsp"`` subscription).

        The device samples at *rate*, accumulates *window_samples*-sample
        windows, applies a Hann window + FFT, detects spikes, and pushes a
        compressed :class:`AdcDspWindow` for every window (~89 bytes/window vs
        768 bytes raw).

        Throughput is bounded by the ADC **poll loop**, not the converter:
        adcPoll reads over SPI once per tick, so ~2 windows/s (about 512
        samples/s) is what a 9.6 kSPS request actually delivers with a
        256-sample window — measured on hardware. The device reports its own
        estimate as ``effectiveRateHz``. Requesting a rate slower than
        ``window_samples`` per second simply makes windows rare (20 SPS needs
        ~13 s per window).

        The channel must already be in an analog input function — adcPoll never
        samples a ``HIGH_IMP`` channel, so the device rejects the request with
        ``INVALID_STATE`` rather than streaming zeros. For the duration of the
        stream the channel is moved to *rate* and the ADC diagnostic slots are
        suspended; both are restored by :meth:`stop_adc_dsp_stream`.

        *channel*         — logical AD74416H channel (0–3).
        *rate*            — :class:`AdcRate` enum value; defaults to SPS_9600.
        *window_samples*  — window size (256, only supported value for v1).
        *spike_threshold* — V above mean to classify a sample as a spike.
        *n_fft_peaks*     — dominant FFT bins to include (0 = skip FFT).
        *callback*        — called with each :class:`AdcDspWindow`.
        """
        from .constants import AdcRate as _AdcRate
        if rate is None:
            rate = _AdcRate.SPS_9600

        rate_code = int(rate)

        if self._usb:
            payload = struct.pack('<BBHfB',
                                  channel, rate_code, window_samples,
                                  spike_threshold, n_fft_peaks)
            self._usb_cmd(CmdId.START_ADC_DSP_STREAM, payload)

            if callback:
                def _handler(data: bytes):
                    try:
                        callback(self._parse_adc_dsp_evt(data))
                    except Exception:
                        pass
                self._t.on_event(CmdId.ADC_DSP_EVT, _handler)
        else:
            # HTTP path: POST to start, then open WebSocket for events
            body = {
                "channel":        channel,
                "rate":           rate_code,
                "windowSamples":  window_samples,
                "spikeThreshold": spike_threshold,
                "nFftPeaks":      n_fft_peaks,
            }
            self._t.post("/adc/dsp/start", body)
            if callback:
                self._t.start_dsp_ws_stream(
                    callback=lambda data: callback(self._parse_adc_dsp_evt(data))
                )

    def stop_adc_dsp_stream(self) -> None:
        """Stop ADC DSP streaming (USB and HTTP)."""
        if self._usb:
            self._usb_cmd(CmdId.STOP_ADC_DSP_STREAM)
            self._t.remove_event(CmdId.ADC_DSP_EVT)
        else:
            try:
                self._t.post("/adc/dsp/stop")
            except Exception:
                pass
            self._t.stop_dsp_ws_stream()

    def on_scope_data(self, callback: Callable[[dict], None], ch_mask: int = 0x0F) -> None:
        """
        Register a callback for 10 ms oscilloscope buckets. **USB only.**

        The device streams pre-computed min/max/avg per channel every 10 ms.
        The callback receives a dict with keys: ``seq``, ``timestamp_ms``,
        ``count``, ``channels`` (list of 4 dicts with ``avg``, ``min``, ``max``).

        *ch_mask* is a 4-bit logical channel bitmask (bit 0 = CH A … bit 3 = CH D).
        Only the selected channels are converted while scope streaming is active,
        which maximises sample rate by disabling the 4 ADC diagnostic slots for
        non-selected channels.  Pass 0 or omit to select all four channels (0x0F).
        The diagnostic conversions are restored automatically when the stream stops
        via :meth:`stop_scope_stream` or device reset.
        """
        if not (0 <= ch_mask <= 15):
            raise ValueError(f"ch_mask must be 0–15, got {ch_mask!r}")
        if ch_mask == 0:
            ch_mask = 0x0F
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
        self._usb_cmd(CmdId.START_SCOPE_STREAM, struct.pack("<B", ch_mask))

    def stop_scope_stream(self) -> None:
        """
        Stop the 10 ms oscilloscope stream. **USB only.**

        Also re-enables the ADC diagnostic conversions that
        :meth:`on_scope_data` disabled for non-selected channels.
        """
        self._require_usb("stop_scope_stream")
        self._usb_cmd(CmdId.STOP_SCOPE_STREAM)
        self._t.remove_event(CmdId.SCOPE_DATA_EVT)

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
