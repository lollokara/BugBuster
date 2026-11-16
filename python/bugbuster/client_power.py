"""BugBuster client methods: self-test, e-fuse monitoring, MUX matrix, IDAC supplies, PCA9535 power management.

One layer of :class:`bugbuster.client.BugBuster` (core -> system -> scripting ->
channels -> bus -> power -> hat). ``bugbuster.client`` imports the chain at its
end, so import ``bugbuster.client`` (or ``bugbuster``) first.
"""

from typing import Optional
import struct
import warnings

from .constants import CmdId
from .constants import PowerControl
from .protocol import ProtocolError
from .client import (
    BugBusterWarning,
    EfuseImonConfirmRequired,
    EfuseImonStatus,
    IdacChannel,
    PD_LIMITED_VADJ_MAX_V,
    PD_LIMITED_VADJ_TOLERANCE_V,
    _EFUSE_IMON_BLOCK_FMT,
    _EFUSE_IMON_BLOCK_LEN,
    _EFUSE_IMON_RESULTS,
    _IDAC_CH_FMT,
    _IDAC_CH_LEN,
    _first_present,
    _normalize_http_pca_status,
    _parse_int_maybe_hex,
    _parse_pca_status,
    _pd_limited_vadj_warning,
    _require_resp_len,
    log,
)
from .client_bus import _ClientBus


class _ClientPower(_ClientBus):
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
            worker_enabled = bool(resp[off]) if len(resp) > off else False; off += 1
            supply_monitor = bool(resp[off]) if len(resp) > off else False; off += 1
            return {
                "boot": {"ran": boot_ran, "passed": boot_passed,
                         "vadj1_v": vadj1, "vadj2_v": vadj2, "vlogic_v": vlogic},
                "cal":  {"status": cal_status, "channel": cal_ch,
                         "points": cal_pts, "error_mv": cal_err},
                "worker_enabled": worker_enabled,
                "supply_monitor_active": supply_monitor,
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

    def selftest_supplies_cached(self) -> dict:
        """
        Get the cached supply rail voltages from the self-test worker.

        Returns::

            {
                "available": True,
                "timestamp_ms": 12345,
                "rails": [
                    {"rail": 0, "name": "VADJ1", "voltage_v": 12.0},
                    {"rail": 1, "name": "VADJ2", "voltage_v": 5.0},
                    {"rail": 2, "name": "VLOGIC", "voltage_v": 3.3},
                ]
            }

        ``available`` is False when the cached sample is stale or the worker is
        disabled.  ``selftest_efuse_currents()`` remains as a compatibility
        alias.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.SELFTEST_SUPPLY_VOLTAGES_CACHED)
            off = 0
            avail = bool(resp[off]); off += 1
            ts,   = struct.unpack_from('<I', resp, off); off += 4
            rail_names = ["VADJ1", "VADJ2", "VLOGIC"]
            rails = []
            for i, name in enumerate(rail_names):
                v, = struct.unpack_from('<f', resp, off); off += 4
                rails.append({"rail": i, "name": name, "voltage_v": v})
            return {"available": avail, "timestamp_ms": ts, "rails": rails}
        else:
            raw = self._http_get("/selftest/supplies/cached")
            rails = []
            for i, rail in enumerate(raw.get("rails", [])):
                rails.append({
                    "rail": _parse_int_maybe_hex(_first_present(rail, "rail"), i),
                    "name": _first_present(rail, "name", default=["VADJ1", "VADJ2", "VLOGIC"][i] if i < 3 else f"Rail {i}"),
                    "voltage_v": float(_first_present(rail, "voltageV", "voltage_v", default=-1.0)),
                })
            return {
                "available": bool(raw.get("available", False)),
                "timestamp_ms": _parse_int_maybe_hex(_first_present(raw, "timestampMs", "timestamp_ms"), 0),
                "rails": rails,
            }

    def selftest_efuse_currents(self) -> dict:
        """
        Compatibility alias for :meth:`selftest_supplies_cached`.
        """
        return self.selftest_supplies_cached()

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
            return {
                **r,
                "status":   r.get("status", 0),
                "channel":  r.get("channel", idac_channel),
                "points":   r.get("points", 0),
                "error_mv": _first_present(r, "errorMv", "error_mv", default=0.0),
            }

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
            http_resp = self._http_get("/mux")
            if "states" not in http_resp:
                raise ProtocolError("MUX_GET_ALL: response missing 'states' field")
            return list(http_resp["states"])

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
        ``present``, ``channels`` (list of :class:`IdacChannel`).

        IDAC channel mapping:
          - 0 → VLOGIC (LTM8078 Out2, 1.8–5 V logic level for digital IOs)
          - 1 → VADJ1 (LTM8063 #1, feeds IO_Block 1+2, IO 1–6)
          - 2 → VADJ2 (LTM8063 #2, feeds IO_Block 3+4, IO 7–12)

        The DS4424 has a fourth channel, but it is not connected on this board
        and the firmware deliberately omits it from the response, so only the
        three writable channels are returned.
        """
        if self._usb:
            resp    = self._usb_cmd(CmdId.IDAC_GET_STATUS)
            present = bool(resp[0])
            chans   = []
            off     = 1
            # 44 bytes per channel; the count comes from the payload rather
            # than a hardcoded loop bound so a firmware that reports a
            # different number of channels cannot silently desynchronise this
            # parser (it previously read 26 bytes for 4 channels against a
            # 44-byte 3-channel reply and returned garbage for every field).
            while off + _IDAC_CH_LEN <= len(resp):
                (_ch, code, tgt, act, _mid, vmin, vmax, _step_mv,
                 cal, _have_poly, _p0, _p1, _p2, _p3) = struct.unpack_from(
                    _IDAC_CH_FMT, resp, off)
                off += _IDAC_CH_LEN
                chans.append(IdacChannel(code, tgt, act, vmin, vmax, bool(cal)))
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
        warning = self.check_vadj_pd_limit(channel, float(voltage))
        if warning:
            warnings.warn(warning, BugBusterWarning, stacklevel=2)
        if self._usb:
            payload = struct.pack('<Bf', channel, float(voltage))
            self._usb_cmd(CmdId.IDAC_SET_VOLTAGE, payload)
        else:
            self._http_post("/idac/voltage", {"ch": channel, "voltage": float(voltage)})

    def check_vadj_pd_limit(self, rail: int, voltage: float) -> Optional[str]:
        """Return a warning if VADJ1/VADJ2 is requested above the active
        USB-C input/PD contract capability.

        The check is advisory and intentionally fail-open if PD status cannot
        be read, so existing workflows do not break when using older firmware
        or transports that lack USB-PD status.
        """
        if rail not in (1, 2) or float(voltage) <= PD_LIMITED_VADJ_MAX_V + PD_LIMITED_VADJ_TOLERANCE_V:
            return None
        try:
            return _pd_limited_vadj_warning(self.usbpd_get_status(), rail, float(voltage))
        except Exception as exc:
            log.warning("Could not check USB-C PD status before VADJ%d %.2f V request: %s", rail, voltage, exc)
            return (
                f"Requested VADJ{rail}={float(voltage):.2f} V, but USB-C PD status could not be read ({exc}). "
                "If the board is powered only from the data USB port or a 5 V/no-PD source, "
                "VADJ1/VADJ2 are limited to about 5 V."
            )

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

    def idac_cal_add_point(self, channel: int, code: int, measured_v: float) -> dict:
        """
        Record one measured calibration point for an IDAC channel.

        Write *code* with :meth:`idac_set_code`, measure the resulting rail
        with a trusted meter, then pass the reading here. Persist the curve
        with :meth:`idac_cal_save` once enough points are in.

        :return: ``{"channel": int, "count": int, "valid": bool}`` — ``valid``
            turns True once the curve has enough monotonic points to be used.
        """
        if not (-127 <= code <= 127):
            raise ValueError(f"code must be -127..127, got {code}")
        if self._usb:
            resp = self._usb_cmd(CmdId.IDAC_CAL_ADD_POINT,
                                 struct.pack('<Bbf', channel, code, float(measured_v)))
            _require_resp_len(resp, 3, "IDAC_CAL_ADD_POINT")
            return {"channel": resp[0], "count": resp[1], "valid": bool(resp[2])}
        r = self._http_post("/idac/cal/point", {
            "ch": channel, "code": code, "measuredV": float(measured_v)})
        return {
            "channel": channel,
            "count": int(r.get("count", 0)),
            "valid": bool(r.get("valid", False)),
        }

    def idac_cal_clear(self, channel: int) -> None:
        """Discard the in-RAM calibration curve for an IDAC channel.

        Does not touch NVS — call :meth:`idac_cal_save` afterwards to make the
        clear permanent.
        """
        if self._usb:
            self._usb_cmd(CmdId.IDAC_CAL_CLEAR, struct.pack('<B', channel))
        else:
            self._http_post("/idac/cal/clear", {"ch": channel})

    def idac_calibrate(
        self,
        channel:     int,
        adc_channel: int,
        step:        int = 8,
        settle_ms:   int = 50,
    ) -> int:
        """
        Run an automatic IDAC calibration sweep. **USB only.**

        Sweeps the IDAC across its code range, reading each resulting voltage
        back through *adc_channel*, and stores the curve. The result is saved
        to NVS by the firmware, so no :meth:`idac_cal_save` is needed.

        *adc_channel* must already be in :attr:`ChannelFunction.VIN` and wired
        to the rail being calibrated, or the device returns INVALID_STATE.

        :return: number of calibration points captured.
        """
        self._require_usb("idac_calibrate")
        resp = self._usb_cmd(
            CmdId.IDAC_CALIBRATE,
            struct.pack('<BBHB', channel, step & 0xFF,
                        settle_ms & 0xFFFF, adc_channel))
        _require_resp_len(resp, 2, "IDAC_CALIBRATE")
        return resp[1]

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
            return _normalize_http_pca_status(self._http_get("/ioexp"))

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
            PowerControl.USB_HUB: "usb",  # firmware accepter is "usb" (webserver.cpp:2613)
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

    @staticmethod
    def _efuse_imon_from_http(raw: dict) -> EfuseImonStatus:
        if "result" not in raw:
            raise RuntimeError(f"efuse_imon: {raw.get('error', 'unexpected response')}")
        return EfuseImonStatus(
            result=str(raw["result"]),
            efuse=int(raw.get("efuse", 0)),
            valid=bool(raw.get("valid", False)),
            saturated=bool(raw.get("saturated", False)),
            efuse_on=bool(raw.get("efuseOn", False)),
            imon_v=float(raw.get("imonV", 0.0)),
            current_ma=float(raw.get("currentMa", 0.0)),
        )

    @staticmethod
    def _efuse_imon_from_block(result: str, block: bytes, cmd_name: str) -> EfuseImonStatus:
        _require_resp_len(block, _EFUSE_IMON_BLOCK_LEN, cmd_name)
        efuse, flags, imon_v, current_ma = struct.unpack_from(_EFUSE_IMON_BLOCK_FMT, block)
        return EfuseImonStatus(
            result=result, efuse=efuse,
            valid=bool(flags & 0x01), saturated=bool(flags & 0x02),
            efuse_on=bool(flags & 0x04),
            imon_v=imon_v, current_ma=current_ma,
        )

    def efuse_imon_get(self) -> EfuseImonStatus:
        """Read the e-fuse current monitor (``efuse`` 0 means no monitor attached)."""
        if self._usb:
            resp = self._usb_cmd(CmdId.EFUSE_IMON_GET)
            return self._efuse_imon_from_block("ok", resp, "EFUSE_IMON_GET")
        return self._efuse_imon_from_http(self._http_get("/selftest/efuse_imon"))

    def efuse_imon_set(self, efuse: int, confirm_power_cycle: bool = False) -> EfuseImonStatus:
        """
        Route one e-fuse's IMON to AD74416H channel D (0 = stop, 1..4 = EFUSE1..4).

        Only one e-fuse can be monitored at a time.  If the e-fuse being attached,
        or the one being detached, is ON it is power-cycled; without
        ``confirm_power_cycle`` this raises :class:`EfuseImonConfirmRequired`.
        While active, supply measurement, IDAC auto-calibration and raw U23 MUX
        writes are refused and logical channel C (IO9 analog) is held.
        """
        if not 0 <= efuse <= 4:
            raise ValueError(f"efuse must be 0..4, got {efuse}")
        if self._usb:
            resp = self._usb_cmd(CmdId.EFUSE_IMON_SET,
                                 struct.pack('<BB', efuse, 1 if confirm_power_cycle else 0))
            _require_resp_len(resp, 1, "EFUSE_IMON_SET")
            code = resp[0]
            result = _EFUSE_IMON_RESULTS[code] if code < len(_EFUSE_IMON_RESULTS) else f"code_{code}"
            status = self._efuse_imon_from_block(result, resp[1:], "EFUSE_IMON_SET")
        else:
            status = self._efuse_imon_from_http(
                self._http_post("/selftest/efuse_imon",
                                {"efuse": efuse, "confirm": bool(confirm_power_cycle)}))
        if status.result == "ok":
            return status
        if status.result == "needs_confirm":
            raise EfuseImonConfirmRequired(
                f"e-fuse monitor change for EFUSE{efuse} will power-cycle an e-fuse that is "
                "currently ON (off, 200 ms, on); retry with confirm_power_cycle=True"
            )
        raise RuntimeError(f"efuse_imon_set({efuse}) failed: {status.result}")

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

    def power_set_port(self, port: int, value: int, *, confirm: bool = False) -> tuple[int, int]:
        """
        Write a raw PCA9535 output port byte. **USB only.**

        Bypasses the named-control mapping in :meth:`power_set` and drives all
        eight pins of *port* at once. **This is dangerous** - incorrect values
        can drop USB or brick the logic rail, requiring a physical reset.
        
        **Use :meth:`power_set` instead** unless you need to change several
        rails in one I2C transaction. The named API validates each control
        and prevents dangerous combinations.
        
        **PCA9535 Port 0 bit layout** (from ``.mex/context/hardware-pinout.md``):
        
        - Bit 0: LOGIC_EN (main logic enable) - **CRITICAL**, must be 1
        - Bit 1: VADJ1_PG (input, power good)
        - Bit 2: VADJ1_EN (VADJ1 enable)
        - Bit 3: VADJ2_EN (VADJ2 enable)
        - Bit 4: VADJ2_PG (input, power good)
        - Bit 5: EN_15V_A (±15V analog enable)
        - Bit 6: EN_MUX (legacy, unused)
        - Bit 7: EN_USB_HUB (USB hub enable) - **CRITICAL**, must be 1
        
        **PCA9535 Port 1 bit layout:**
        
        - Bit 0: EFUSE_EN_1 (output)
        - Bit 1: EFUSE_FLT_1 (input, active low)
        - Bit 2: EFUSE_EN_2 (output)
        - Bit 3: EFUSE_FLT_2 (input, active low)
        - Bit 4: EFUSE_EN_3 (output, maps to logical port 4)
        - Bit 5: EFUSE_FLT_3 (input, logical port 4 fault)
        - Bit 6: EFUSE_EN_4 (output, maps to logical port 3)
        - Bit 7: EFUSE_FLT_4 (input, logical port 3 fault)
        
        **Safety:** Port 0 bits 0 (LOGIC_EN) and 7 (EN_USB_HUB) must never be
        cleared. Clearing bit 0 disables the logic rail; clearing bit 7 drops
        the USB connection. This method refuses to write such values even with
        ``confirm=True`` - a physical reset would be required to recover.

        :param port: 0 or 1.
        :param value: 8-bit output latch value.
        :param confirm: Must be True to bypass the safety check. Use this when
                        you understand the exact bit consequences.
        :return: ``(port, value)`` echoed by the device.
        :raises ValueError: If port is out of range, or if dangerous bits would
                            be cleared on port 0.
        """
        self._require_usb("power_set_port")
        if port not in (0, 1):
            raise ValueError(f"port must be 0 or 1, got {port}")
        
        if not confirm:
            raise ValueError(
                "power_set_port() requires confirm=True. This bypasses all "
                "safety checks and can drop USB or brick the logic rail. "
                "Use power_set() instead for normal rail control."
            )
        
        # Port 0 bit 0 (LOGIC_EN) and bit 7 (EN_USB_HUB) must NEVER be cleared.
        if port == 0:
            LOGIC_EN_BIT = 0x01   # bit 0
            USB_HUB_BIT = 0x80    # bit 7
            CRITICAL_BITS = LOGIC_EN_BIT | USB_HUB_BIT
            
            if (value & CRITICAL_BITS) != CRITICAL_BITS:
                missing = []
                if not (value & LOGIC_EN_BIT):
                    missing.append("bit 0 (LOGIC_EN)")
                if not (value & USB_HUB_BIT):
                    missing.append("bit 7 (EN_USB_HUB)")
                raise ValueError(
                    f"Port 0 {', '.join(missing)} must be set. Clearing these "
                    f"bits drops USB or disables the logic rail, requiring a "
                    f"physical reset. Requested value: 0x{value:02X}. "
                    f"Use power_set() for safe rail control."
                )
        
        resp = self._usb_cmd(CmdId.PCA_SET_PORT,
                             struct.pack('<BB', port, value & 0xFF))
        _require_resp_len(resp, 2, "PCA_SET_PORT")
        return resp[0], resp[1]

    def rail_power_up(
        self,
        rail: int,
        voltage: float,
        settle_ms: int = 500,
        *,
        confirm: bool = False,
        power_cycle: bool = False,
        efuse_mask: int = 0,
    ) -> dict:
        """
        Power a VADJ rail up as one firmware-sequenced operation.

        Firmware order: rail e-fuses off -> (VADJ off + 200 ms discharge if
        *power_cycle*) -> set *voltage* -> VADJ on -> *settle_ms* -> e-fuses
        armed through the soft-start blackout gate -> power-good / fault read.

        :param rail: 1 = VADJ1 (EFUSE1+2, IO 1-6), 2 = VADJ2 (EFUSE3+4, IO 7-12).
        :param voltage: 3.0-15.0 V. Above 12 V the device refuses unless *confirm*.
        :param settle_ms: wait between rail enable and e-fuse enable (0-5000).
        :param efuse_mask: bit0/bit1 = the rail's first/second e-fuse; 0 = both.
        :return: ``rail``, ``applied_v`` (after DAC clamping), ``clamped``,
                 ``pg``, ``efuse_faults`` (two bools, the rail's e-fuses).
        """
        if rail not in (1, 2):
            raise ValueError(f"rail must be 1 or 2, got {rail!r}")
        if not 0 <= settle_ms <= 5000:
            raise ValueError("settle_ms must be 0-5000")
        if self._usb:
            flags = (0x01 if confirm else 0) | (0x02 if power_cycle else 0)
            payload = struct.pack('<BHHBB', rail, int(round(voltage * 1000)),
                                  settle_ms, flags, efuse_mask & 0x03)
            resp = self._usb_cmd(CmdId.RAIL_POWER_UP, payload)
            r, mv, status = struct.unpack_from('<BHB', resp)
            return {"rail": r, "applied_v": mv / 1000.0, "clamped": bool(status & 0x08),
                    "pg": bool(status & 0x01),
                    "efuse_faults": [bool(status & 0x02), bool(status & 0x04)]}
        body = self._http_post("/ioexp/rail_up", {
            "rail": rail, "voltage": voltage, "settleMs": settle_ms,
            "confirm": confirm, "powerCycle": power_cycle, "efuseMask": efuse_mask & 0x03,
        })
        if "error" in body:
            raise ValueError(body["error"])
        return {"rail": body["rail"], "applied_v": float(body["appliedV"]),
                "clamped": bool(body["clamped"]), "pg": bool(body["pg"]),
                "efuse_faults": [bool(x) for x in body["efuseFaults"]]}

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
