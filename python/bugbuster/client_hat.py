"""BugBuster client methods: HAT expansion board and logic analyzer.

One layer of :class:`bugbuster.client.BugBuster` (core -> system -> scripting ->
channels -> bus -> power -> hat). ``bugbuster.client`` imports the chain at its
end, so import ``bugbuster.client`` (or ``bugbuster``) first.
"""

import struct

from .constants import CmdId
from .protocol import ProtocolError
from .transport.usb import DeviceError
from .client import (
    HatNotPresentError,
    HatPinFunctionError,
    _first_present,
    _normalize_http_hat_status,
    _parse_int_maybe_hex,
    _require_resp_len,
    log,
)
from .client_power import _ClientPower


class _ClientHat(_ClientPower):
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
            except (TimeoutError, ProtocolError, DeviceError) as exc:
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

    # TR-9: how long a positive HAT status answers presence/type checks.
    HAT_CACHE_TTL_S = 30.0

    def hat_status_cached(self) -> dict:
        """HAT status for presence/type guards, cached per connection.

        Only a *detected* status is cached (a HAT plugged in later is noticed
        on the next call); it expires after HAT_CACHE_TTL_S, on reconnect
        (transport ``connect_gen``) and on hat_detect()/hat_reset().
        """
        import time as _time
        gen = getattr(self._t, "connect_gen", 0)
        c = getattr(self, "_hat_status_cache", None)
        now = _time.monotonic()
        if c is not None and c[0] == gen and now - c[1] < self.HAT_CACHE_TTL_S:
            return c[2]
        status = self.hat_get_status()
        self._hat_status_cache = (gen, now, status) if status.get("detected") else None
        return status

    def hat_invalidate_cache(self) -> None:
        """Forget the cached HAT status (TR-9)."""
        self._hat_status_cache = None
        self._hat_present_cache = None

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
                for _ in range(4):
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
        self._hat_status_cache = None
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
        self._hat_status_cache = None
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
            self._http_post("/hat/power", {"connector": connector, "enable": enable})
            return True

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
            return result
        else:
            j = self._http_get("/hat/power")
            connectors = [
                {
                    "enabled": j["connA"]["enabled"],
                    "current_ma": j["connA"]["currentMa"],
                    "fault": j["connA"]["fault"],
                },
                {
                    "enabled": j["connB"]["enabled"],
                    "current_ma": j["connB"]["currentMa"],
                    "fault": j["connB"]["fault"],
                },
            ]
            return {"connectors": connectors, "io_voltage_mv": j.get("ioVoltageMv", 0)}

    def hat_get_caps(self) -> dict:
        """Get HAT v2 capability metadata."""
        self._require_hat_present()
        if not self._usb:
            j = self._http_get("/hat/v2/caps")
            return {
                "hw_revision": j.get("hwRevision", 0),
                "flags": j.get("flags", 0),
                "rail_count": j.get("railCount", 0),
                "led_count": j.get("ledCount", 0),
                "shifted_io_count": j.get("shiftedIoCount", 0),
                "la_routes": j.get("laRouteCount", 0),
                "fw_version": f"{j.get('fwMajor', 0)}.{j.get('fwMinor', 0)}",
            }

        resp = self._usb_cmd(CmdId.HAT_GET_CAPS)
        _require_resp_len(resp, 11, "HAT_GET_CAPS")
        off = 0
        hw_revision = resp[off]; off += 1
        flags = struct.unpack_from('<I', resp, off)[0]; off += 4
        rail_count = resp[off]; off += 1
        led_count = resp[off]; off += 1
        shifted_io_count = resp[off]; off += 1
        la_routes = resp[off]; off += 1
        fw_major = resp[off]; off += 1
        fw_minor = resp[off]; off += 1
        return {
            "hw_revision": hw_revision,
            "flags": flags,
            "rail_count": rail_count,
            "led_count": led_count,
            "shifted_io_count": shifted_io_count,
            "la_routes": la_routes,
            "fw_version": f"{fw_major}.{fw_minor}",
        }

    def hat_get_rail_status(self) -> dict:
        """Get HAT v2 rail status for 3V3_ADJ, VADJ3, and VADJ4."""
        self._require_hat_present()
        if not self._usb:
            j = self._http_get("/hat/v2/rails")
            if j.get("ok") is False:
                raise RuntimeError(f"HAT rail status unavailable: {j.get('error', 'unknown error')}")
            rails = [
                {
                    "rail_id": r.get("railId", i),
                    "enabled": r.get("enabled", False),
                    "voltage_mv": r.get("voltageMv", 0),
                    "current_ma": r.get("currentMa", 0),
                    "status": r.get("status", 0),
                    "target_mv": r.get("targetVoltageMv", r.get("voltageMv", 0)),
                }
                for i, r in enumerate(j.get("rails", []))
            ]
            return {"count": j.get("railCount", len(rails)), "rails": rails}

        resp = self._usb_cmd(CmdId.HAT_GET_RAIL_STATUS)
        _require_resp_len(resp, 1, "HAT_GET_RAIL_STATUS")
        off = 0
        count = resp[off]; off += 1
        rails = []
        for _ in range(count):
            _require_resp_len(resp[off:], 9, "HAT_GET_RAIL_STATUS rail")
            rail_id = resp[off]; off += 1
            enabled = bool(resp[off]); off += 1
            voltage_mv = struct.unpack_from('<H', resp, off)[0]; off += 2
            current_ma = struct.unpack_from('<H', resp, off)[0]; off += 2
            status = resp[off]; off += 1
            target_mv = struct.unpack_from('<H', resp, off)[0]; off += 2
            rails.append({
                "rail_id": rail_id,
                "enabled": enabled,
                "voltage_mv": voltage_mv,
                "current_ma": current_ma,
                "status": status,
                "target_mv": target_mv,
            })
        return {"count": count, "rails": rails}

    def hat_set_rail_enable(self, rail_id: int, enable: bool) -> dict:
        """Enable or disable a HAT v2 rail, then return refreshed rail status."""
        self._require_hat_present()
        if not self._usb:
            self._http_post("/hat/v2/rail/enable", {"railId": rail_id, "enable": bool(enable)})
            return self.hat_get_rail_status()

        payload = struct.pack('<BB', rail_id & 0xFF, int(enable))
        self._usb_cmd(CmdId.HAT_SET_RAIL_ENABLE, payload)
        return self.hat_get_rail_status()

    def hat_set_rail_voltage(self, rail_id: int, voltage_mv: int) -> dict:
        """Set HAT v2 adjustable rail voltage (VADJ3=1, VADJ4=2), then return refreshed rail status.

        :param rail_id: HAT rail ID — 1 for VADJ3, 2 for VADJ4.
        :param voltage_mv: Target voltage in millivolts (e.g. 27100 for 27.1 V).
        """
        self._require_hat_present()
        if not 0 <= rail_id <= 0xFF:
            raise ValueError(f"rail_id must be 0-255, got {rail_id}")
        if not 0 <= int(voltage_mv) <= 0xFFFF:
            raise ValueError(f"voltage_mv must be 0-65535, got {voltage_mv}")
        if not self._usb:
            self._http_post("/hat/v2/rail/voltage", {"railId": rail_id, "voltageMv": int(voltage_mv)})
            return self.hat_get_rail_status()

        payload = struct.pack('<BH', rail_id, int(voltage_mv))
        self._usb_cmd(CmdId.HAT_SET_RAIL_VOLTAGE, payload)
        return self.hat_get_rail_status()

    def hat_set_led_state(self, led_index: int, color_code: int) -> bool:
        """Set a HAT status LED state. LED indexes are 1..8."""
        self._require_hat_present()
        if not self._usb:
            self._http_post("/hat/v2/led", {"ledId": led_index, "colorCode": color_code})
            return True

        payload = struct.pack('<BB', led_index & 0xFF, color_code & 0xFF)
        self._usb_cmd(CmdId.HAT_SET_LED_STATE, payload)
        return True

    def hat_set_io_voltage(self, voltage_mv: int) -> bool:
        """
        Set the I/O level shift voltage.

        :param voltage_mv: Target I/O voltage in millivolts (1200-5500)
        """
        self._require_hat_present()
        if self._usb:
            payload = struct.pack('<H', voltage_mv)
            self._usb_cmd(CmdId.HAT_SET_IO_VOLT, payload)
            return True
        else:
            self._http_post("/hat/v2/io_voltage", {"voltageMv": int(voltage_mv)})
            return True

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
            return self._http_post("/hat/v2/swd/setup", {
                "targetVoltageMv": target_voltage_mv,
                "connector": connector,
            }).get("ok", False)

    def hat_detect_target(self) -> dict:
        """Actively probe the SWD target (line reset + DPIDR read).

        Enable the target rail (VADJ4), VLOGIC, and the level-shifter OE first.

        :return: ``{"detected": bool, "dpidr": int}``.
        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_DETECT_TARGET)
            _require_resp_len(resp, 5, "HAT_DETECT_TARGET")
            detected = bool(resp[0])
            dpidr = struct.unpack_from('<I', resp, 1)[0]
            return {"detected": detected, "dpidr": dpidr}
        else:
            j = self._http_post("/hat/v2/swd/detect", {})
            return {
                "detected": bool(j.get("detected", False)),
                "dpidr": int(j.get("dpidr", 0)),
            }

    # ------------------------------------------------------------------
    # ── HAT Logic Analyzer ───────────────────────────────────────────
    # ------------------------------------------------------------------

    def hat_la_configure(self, channels: int = 4, rate_hz: int = 1000000, depth: int = 100000, rle_enabled: bool = False) -> bool:
        """
        Configure the HAT logic analyzer.

        :param channels: Number of channels (1, 2, or 4)
        :param rate_hz:  Sample rate in Hz (max ~100MHz for 1ch, ~25MHz for 4ch)
        :param depth:    Total samples to capture
        :param rle_enabled: Enable run-length encoding of the on-HAT capture
            buffer. Note this is the 32-bit memory-mode format
            (``[value:4][count:28]``, see ``Firmware/RP2040/src/bb_la_rle.h``),
            which is NOT the 8-bit per-segment compression the USB stream
            applies on its own. Decode it with :meth:`hat_la_decode_rle`, not
            :meth:`hat_la_decode`.
        :raises HatNotPresentError: If no HAT is detected on this device.
        """
        self._require_hat_present()
        payload = struct.pack('<BIIB', channels, rate_hz, depth, 1 if rle_enabled else 0)
        if self._usb:
            self._usb_cmd(CmdId.HAT_LA_CONFIG, payload)
            self._la_rle_enabled = bool(rle_enabled)
            return True
        else:
            raise NotImplementedError("LA control is USB-only")

    def hat_la_set_route(self, route_id: int) -> bool:
        """Select LA capture route: 0=low-speed (GPIO2-5), 1=high-speed (Conn1)."""
        self._require_hat_present()
        if self._usb:
            payload = struct.pack('<B', route_id & 0xFF)
            self._usb_cmd(CmdId.HAT_LA_SET_ROUTE, payload)
            return True
        else:
            resp = self._http_post("/hat/v2/la/route", {"route": route_id})
            return resp.get("ok", False)

    def hat_calibrate_start(self, rail_id: int) -> int:
        """Start HAT auto-calibration on a specific rail. Returns starting state."""
        self._require_hat_present()
        if self._usb:
            payload = struct.pack('<B', rail_id)
            resp = self._usb_cmd(CmdId.HAT_CALIBRATE_START, payload)
            _require_resp_len(resp, 1, "HAT_CALIBRATE_START")
            return resp[0]
        else:
            http_resp = self._http_post("/hat/v2/calibrate/start", {"railId": rail_id})
            return http_resp.get("status", 0)

    def hat_calibrate_status(self) -> dict:
        """Get the status of the current HAT calibration sweep."""
        self._require_hat_present()
        if self._usb:
            resp = self._usb_cmd(CmdId.HAT_CALIBRATE_STATUS)
            _require_resp_len(resp, 4, "HAT_CALIBRATE_STATUS")
            status = {
                "state": resp[0],
                "progress": resp[1],
                "rail_id": resp[2],
                "last_error": resp[3],
            }
            if len(resp) >= 12:
                status.update({
                    "persist_state": resp[4],
                    "stage": resp[5],
                    "point": resp[6],
                    "code": struct.unpack_from('<b', resp, 7)[0],
                    "measured_mv": struct.unpack_from('<i', resp, 8)[0],
                })
            if len(resp) >= 30:
                status.update({
                    "min_mv": struct.unpack_from('<i', resp, 12)[0],
                    "max_mv": struct.unpack_from('<i', resp, 16)[0],
                    "max_gap_mv": struct.unpack_from('<i', resp, 20)[0],
                    "max_error_mv": struct.unpack_from('<i', resp, 24)[0],
                    "validation_flags": struct.unpack_from('<H', resp, 28)[0],
                })
            return status
        else:
            http_resp = self._http_get("/hat/v2/calibrate/status")
            return {
                "state": http_resp.get("state", 0),
                "progress": http_resp.get("progress", 0),
                "rail_id": http_resp.get("railId", 0),
                "last_error": http_resp.get("lastError", 0),
                "persist_state": http_resp.get("persistState", 0),
                "stage": http_resp.get("stage", 0),
                "point": http_resp.get("point", 0),
                "code": http_resp.get("code", 0),
                "measured_mv": http_resp.get("measuredMv", -1),
                "min_mv": http_resp.get("minMv", -1),
                "max_mv": http_resp.get("maxMv", -1),
                "max_gap_mv": http_resp.get("maxGapMv", -1),
                "max_error_mv": http_resp.get("maxErrorMv", -1),
                "validation_flags": http_resp.get("validationFlags", 0),
            }

    def hat_calibrate_import(self, rail_id: int, points: list[dict]) -> bool:
        """Import calibration data to the HAT."""
        self._require_hat_present()
        if len(points) > 6:
            raise ValueError("Maximum 6 calibration points can be imported at once due to frame size limits.")

        if self._usb:
            payload = bytearray([rail_id, len(points)])
            for pt in points:
                payload += struct.pack('<b', pt["dac_code"])
                payload += struct.pack('<f', pt["measured_v"])
            self._usb_cmd(CmdId.HAT_CALIBRATE_IMPORT, bytes(payload))
            return True
        else:
            pts_http = [{"dacCode": pt["dac_code"], "measuredV": pt["measured_v"]} for pt in points]
            resp = self._http_post("/hat/v2/calibrate/import", {
                "railId": rail_id,
                "points": pts_http
            })
            return resp.get("ok", False)

    def hat_set_io_bank(self, dirs: int, ups: int, dns: int, vals: int = 0) -> bool:
        """Configure directions, pullups/pulldowns, and output values for the shifted IO bank.

        Args:
            dirs: Bitmask - 1 = output, 0 = input (bit 0 = GPIO10 … bit 7 = GPIO21).
            ups:  Bitmask - 1 = pull-up enabled.
            dns:  Bitmask - 1 = pull-down enabled.
            vals: Bitmask - output latch value for pins set as output (default 0 = LOW).
        """
        self._require_hat_present()
        if self._usb:
            payload = struct.pack('<BBBB', dirs & 0xFF, ups & 0xFF, dns & 0xFF, vals & 0xFF)
            self._usb_cmd(CmdId.HAT_SET_IO_BANK, payload)
            return True
        else:
            resp = self._http_post("/hat/v2/io_bank", {
                "dirs": dirs,
                "ups":  ups,
                "dns":  dns,
                "vals": vals,
            })
            return resp.get("ok", False)

    def hat_set_level_shift(self, oe: bool, direction: bool) -> dict:
        """Override output enable (oe) and direction (dir) of level shifters."""
        self._require_hat_present()
        if self._usb:
            payload = struct.pack('<BB', int(oe), int(direction))
            resp = self._usb_cmd(CmdId.HAT_SET_LEVEL_SHIFT, payload)
            _require_resp_len(resp, 2, "HAT_SET_LEVEL_SHIFT")
            return {
                "oe": bool(resp[0]),
                "dir": bool(resp[1])
            }
        else:
            http_resp = self._http_post("/hat/v2/level_shift", {
                "oe": oe,
                "dir": direction
            })
            return {
                "oe": http_resp.get("oe", False),
                "dir": http_resp.get("dir", False)
            }

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
        except ImportError as exc:
            raise ImportError(
                "pyusb is required for high-speed streaming capability: pip install pyusb"
            ) from exc

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
                except (usb.core.USBError, OSError) as e: log.debug("USB teardown ignored: %r", e)
            
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
            except (usb.core.USBError, OSError) as e: log.debug("USB teardown ignored: %r", e)
            try: usb.util.release_interface(dev, LA_INTERFACE)
            except (usb.core.USBError, OSError) as e: log.debug("USB teardown ignored: %r", e)
            
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
        self._http_post("/hat/v2/la/log/enable", {"enable": enable})
        return True

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

        # An RLE capture stores a variable number of 32-bit words, and
        # HAT_LA_STATUS reports samples, not entries - so there is no way to
        # know how many bytes to read. Reading the raw-packed length instead
        # returns a truncated word stream that hat_la_decode() will happily turn
        # into plausible, wrong waveforms. Fail instead of guessing.
        if getattr(self, "_la_rle_enabled", False):
            raise NotImplementedError(
                "hat_la_read_all() cannot read an RLE capture: HAT_LA_STATUS "
                "reports sample count, not RLE entry count, so the transfer "
                "length is unknown. Configure with rle_enabled=False for "
                "memory-mode capture, or use the USB stream "
                "(hat_la_stream_usb_cycle), which compresses per segment and "
                "carries its own lengths."
            )

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
        result: list[list[int]] = [[] for _ in range(channels)]
        bits_per_sample = channels

        for byte_val in raw:
            for bit_pos in range(0, 8, bits_per_sample):
                for ch in range(channels):
                    result[ch].append((byte_val >> (bit_pos + ch)) & 1)

        return result

    @staticmethod
    def hat_la_decode_rle(raw: bytes, channels: int = 4, max_samples: int = 1 << 24) -> list:
        """
        Decode a 32-bit memory-mode RLE capture into per-channel sample arrays.

        Wire format, from ``Firmware/RP2040/src/bb_la_rle.h``::

            word = (value << 28) | (count & 0x0FFFFFFF)   # little-endian u32

        ``value`` holds one sample for every channel packed into its low
        ``channels`` bits. A run longer than 0x0FFFFFFF is emitted by the
        firmware as several consecutive words with the same value, so a decoder
        that simply concatenates runs is correct and needs no special case.

        This is a different format from the 8-bit ``[value][count-1]`` pairs the
        USB stream uses per segment - do not cross the two.

        :param raw: Raw bytes from the HAT capture buffer, a whole number of
            32-bit words.
        :param channels: Number of channels (1, 2, or 4)
        :param max_samples: Refuse to expand beyond this many samples per
            channel. The count field is 28 bits, so one corrupt word can ask for
            268 million samples and exhaust host memory; the HAT's own buffer is
            76 KB, so anything near the cap is corruption rather than data.
        :return: List of channels, each a list of 0/1 values
        :raises ValueError: If ``raw`` is not a whole number of 32-bit words, or
            the run lengths sum past ``max_samples``.
        """
        if len(raw) % 4 != 0:
            raise ValueError(
                f"RLE capture must be a whole number of 32-bit words, got {len(raw)} bytes"
            )
        result: list[list[int]] = [[] for _ in range(channels)]
        total = 0
        for (word,) in struct.iter_unpack("<I", raw):
            value = (word >> 28) & 0x0F
            count = word & 0x0FFFFFFF
            total += count
            if total > max_samples:
                raise ValueError(
                    f"RLE run lengths exceed max_samples ({max_samples}); "
                    f"the capture is corrupt or truncated"
                )
            for ch in range(channels):
                result[ch].extend([(value >> ch) & 1] * count)
        return result
