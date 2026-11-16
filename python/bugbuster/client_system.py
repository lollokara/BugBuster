"""BugBuster client methods: standby, crash reports, faults, USB-PD, WiFi, quick setup, raw registers, IO ownership.

One layer of :class:`bugbuster.client.BugBuster` (core -> system -> scripting ->
channels -> bus -> power -> hat). ``bugbuster.client`` imports the chain at its
end, so import ``bugbuster.client`` (or ``bugbuster``) first.
"""

from contextlib import contextmanager
from typing import Callable
from typing import Optional
import struct

from typing import TYPE_CHECKING

from .constants import CmdId
from .client import (
    ApPasswordResult,
    _normalize_http_usbpd,
    _normalize_http_wifi_status,
    _parse_usbpd_status,
    _parse_wifi_scan,
    _parse_wifi_status,
    _require_resp_len,
    log,
    _ClientCore,
)

if TYPE_CHECKING:
    from .crash import BootReport
    from .crash import CrashSummary


class _ClientSystem(_ClientCore):
    # ------------------------------------------------------------------
    # ── Crash dump & boot report (HTTP only) ────────────────────────────
    # ------------------------------------------------------------------

    def _require_http(self, method: str):
        if self._usb:
            raise NotImplementedError(
                f"{method} is only available over HTTP (/api/system/crash needs the admin token)")

    def get_crash_info(self) -> "CrashSummary":
        """
        Reset reason, boot count, consecutive-crash streak and, when a coredump
        is stored, its decoded summary (task, PC, exception name, backtrace,
        registers, whether it matches the running build, and the last RTC
        snapshot before the crash). **HTTP only**, admin token required.
        """
        from .crash import parse_crash_json
        self._require_http("get_crash_info")
        return parse_crash_json(self._http_get("/system/crash"))

    def get_boot_report(self) -> "BootReport":
        """The full diagnostics bundle the firmware logs 30 s after boot, built now. **HTTP only.**"""
        from .crash import parse_boot_report_json
        self._require_http("get_boot_report")
        return parse_boot_report_json(self._http_get("/system/crash", report=1))

    def download_coredump(self, path: Optional[str] = None, *,
                          progress: Optional[Callable[[int, int], None]] = None) -> bytes:
        """
        Download the stored ELF coredump in 768-byte base64 slices and return it.
        With *path* the bytes are also written there. **HTTP only.**

        Raises :class:`FileNotFoundError` when no valid dump is stored and
        :class:`ValueError` if the device reports an error or the size changes
        mid-download. Decode with ``espcoredump.py info_corefile`` against the ELF
        of the build named by ``CrashSummary.dump_elf``.
        """
        from .crash import decode_chunk
        self._require_http("download_coredump")
        out = bytearray()
        total = None
        while total is None or len(out) < total:
            resp = self._http_get("/system/crash", offset=len(out), len=768)
            if not resp.get("ok"):
                if total is None and "no coredump" in str(resp.get("error", "")):
                    raise FileNotFoundError("no coredump stored on the device")
                raise ValueError(resp.get("error", "coredump chunk failed"))
            if total is not None and int(resp["total"]) != total:
                raise ValueError("coredump changed while downloading")
            total = int(resp["total"])
            out += decode_chunk(resp)
            if progress:
                progress(len(out), total)
        if path:
            with open(path, "wb") as fh:
                fh.write(out)
        return bytes(out)

    def clear_coredump(self) -> bool:
        """Erase the stored coredump. Returns whether one existed. **HTTP only**, admin token."""
        self._require_http("clear_coredump")
        resp = self._http_post("/system/crash/clear")
        if not resp.get("ok", True):
            raise RuntimeError(resp.get("error", "clear failed"))
        return bool(resp.get("had_dump", False))

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
    # ── Fault management ─────────────────────────────────────────────
    # ------------------------------------------------------------------

    def clear_alerts(self, channel: Optional[int] = None) -> None:
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
                "alertMask": alert_mask & 0xFFFF, "supplyMask": supply_mask & 0xFFFF,
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
            # BBP SELECT_PDO only stages the PDO; GO 0x01 renegotiates. HTTP
            # /api/usbpd/select does both in one call.
            self._usb_cmd(CmdId.USBPD_SELECT_PDO, struct.pack('<B', code))
            self.usbpd_go(0x01)
        else:
            self._http_post("/usbpd/select", {"voltage": int(voltage_v)})

    def usbpd_go(self, command: int) -> int:
        """
        Send a raw HUSB238 GO command to re-run PD negotiation. **USB only.**

        :meth:`usbpd_select_voltage` stages a PDO; this commits it. Also used
        to request the source capabilities be re-read after a cable swap.

        :return: the command byte the device echoed back.
        """
        self._require_usb("usbpd_go")
        resp = self._usb_cmd(CmdId.USBPD_GO, struct.pack('<B', command & 0xFF))
        _require_resp_len(resp, 1, "USBPD_GO")
        return resp[0]

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
            reply = self._http_post("/wifi/connect", {"ssid": ssid, "password": password})
            return bool(reply.get("success")) if isinstance(reply, dict) else bool(reply)

    def wifi_scan(self) -> list[dict]:
        """Scan for nearby WiFi networks. Returns list of {ssid, rssi, auth}."""
        if self._usb:
            resp = self._usb_cmd(CmdId.WIFI_SCAN)
            return _parse_wifi_scan(resp)
        else:
            raw = self._http_get("/wifi/scan")
            return raw.get("networks", raw) if isinstance(raw, dict) else raw

    def wifi_forget(self) -> bool:
        """
        Erase the saved STA credentials from NVS and disconnect. **USB only.**

        The device drops off the station network until :meth:`wifi_connect`
        is called again; the SoftAP is unaffected.
        """
        self._require_usb("wifi_forget")
        resp = self._usb_cmd(CmdId.WIFI_FORGET)
        _require_resp_len(resp, 1, "WIFI_FORGET")
        return bool(resp[0])

    def wifi_set_ap_password(self, password: str) -> ApPasswordResult:
        """
        Set the SoftAP password. Persists to NVS and applies live (no reboot needed).
        Password must be 8–63 characters (WPA2-PSK requirement).

        Returns an :class:`ApPasswordResult` that is truthy when the password
        was applied. Check ``.persisted``: when False the NVS write failed and
        the old password returns on the next reboot.
        """
        if len(password) < 8 or len(password) > 63:
            raise ValueError(
                f"AP password must be 8-63 characters (WPA2 requirement), got {len(password)}"
            )
        if self._usb:
            pass_b = password.encode()
            payload = struct.pack('<B', len(pass_b)) + pass_b
            resp = self._usb_cmd(CmdId.WIFI_SET_AP_PASSWORD, payload)
            _require_resp_len(resp, 1, "WIFI_SET_AP_PASSWORD")
            # 0x00 applied+persisted, 0x01 applied not persisted, 0x02 failed
            return ApPasswordResult(applied=resp[0] != 0x02, persisted=resp[0] == 0x00)
        else:
            result = self._http_post("/wifi/ap_password", {"password": password})
            if isinstance(result, dict):
                applied = bool(result.get("success", False))
                return ApPasswordResult(applied, applied and bool(result.get("persisted", False)))
            return ApPasswordResult(bool(result), False)

    # ------------------------------------------------------------------
    # ── Quick Setup slots ─────────────────────────────────────────────
    # ------------------------------------------------------------------

    _QS_SLOT_COUNT = 4

    def quicksetup_list(self) -> list:
        """
        Return summary info for all 4 quick-setup slots.

        USB: QS_LIST (0xF0) → bitmap byte + 4 hash bytes.
        HTTP: GET /api/quicksetup.
        Returns a list of 4 dicts: ``{"index", "occupied", "hash"}``.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.QS_LIST)
            bitmap = resp[0] if resp else 0
            slots = []
            for i in range(self._QS_SLOT_COUNT):
                h = resp[1 + i] if len(resp) > 1 + i else 0
                slots.append({"index": i, "occupied": bool(bitmap & (1 << i)), "hash": h})
            return slots
        else:
            result = self._http_get("/quicksetup")
            return result.get("slots", [])

    def quicksetup_get(self, slot: int) -> Optional[dict]:
        """
        Return the JSON payload stored in *slot* (0–3), or ``None`` if empty.

        USB: QS_GET (0xF1) → raw JSON bytes.
        HTTP: GET /api/quicksetup/{slot}.
        """
        import json as _json
        if self._usb:
            resp = self._usb_cmd(CmdId.QS_GET, bytes([slot & 0xFF]))
            if not resp:
                return None
            return _json.loads(resp.decode("utf-8", errors="replace"))
        else:
            try:
                result = self._http_get(f"/quicksetup/{slot}")
                if not isinstance(result, dict) or result.get("ok") is False:
                    return None
                return result
            except Exception:
                return None

    def quicksetup_save(self, slot: int) -> dict:
        """
        Snapshot the current device state into *slot* (0–3).

        USB: QS_SAVE (0xF2) → raw JSON bytes of the saved snapshot.
        HTTP: POST /api/quicksetup/{slot}.
        Returns the saved JSON payload as a dict.
        """
        import json as _json
        if self._usb:
            resp = self._usb_cmd(CmdId.QS_SAVE, bytes([slot & 0xFF]))
            return _json.loads(resp.decode("utf-8", errors="replace")) if resp else {}
        else:
            result = self._http_post(f"/quicksetup/{slot}", {})
            return result if isinstance(result, dict) else {}

    def quicksetup_apply(self, slot: int) -> dict:
        """
        Apply the quick-setup stored in *slot* to the live device state.

        USB: QS_APPLY (0xF3) → u8 result (0=ok, 1=not_found, 2=error).
        HTTP: POST /api/quicksetup/{slot}/apply.
        Returns ``{"ok": bool, "applied": bool}``.
        Raises ``RuntimeError`` on firmware error.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.QS_APPLY, bytes([slot & 0xFF]))
            code = resp[0] if resp else 2
            if code == 2:
                raise RuntimeError(f"quicksetup_apply slot={slot}: firmware error")
            return {"ok": code == 0, "applied": code == 0}
        else:
            result = self._http_post(f"/quicksetup/{slot}/apply", {})
            return {"ok": bool(result.get("ok")), "applied": bool(result.get("applied"))}

    def quicksetup_delete(self, slot: int) -> dict:
        """
        Delete the quick-setup stored in *slot*.

        USB: QS_DELETE (0xF4) → u8 existed (0=existed, 1=not_found).
        HTTP: POST /api/quicksetup/{slot}/delete.
        Returns ``{"ok": bool, "deleted": bool}``.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.QS_DELETE, bytes([slot & 0xFF]))
            existed = (resp[0] == 0) if resp else False
            return {"ok": True, "deleted": existed}
        else:
            result = self._http_post(f"/quicksetup/{slot}/delete", {})
            return {"ok": bool(result.get("ok")), "deleted": bool(result.get("deleted"))}

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

    def set_spi_clock(self, clock_hz: int) -> bool:
        """
        Adjust the AD74416H SPI clock speed at runtime. **USB only.**
        Valid range: 100 kHz to 20 MHz.

        Returns ``True`` if the firmware's scratch-register readback at the new
        clock matched, ``False`` if it did not.
        """
        self._require_usb("set_spi_clock")
        resp = self._usb_cmd(CmdId.SET_SPI_CLOCK, struct.pack('<I', clock_hz))
        _require_resp_len(resp, 5, "SET_SPI_CLOCK")
        return bool(resp[4])

    def set_level_shifter_oe(self, on: bool) -> None:
        """Enable or disable the TXS0108E level-shifter output enable."""
        if self._usb:
            self._usb_cmd(CmdId.SET_LSHIFT_OE, struct.pack('<B', int(on)))
        else:
            self._http_post("/lshift/oe", {"on": on})

    # ------------------------------------------------------------------
    # ── IO Ownership (BBP v5+) ─────────────────────────────────────────
    # ------------------------------------------------------------------

    @staticmethod
    def _fnv1a32(s: str) -> int:
        """FNV-1a 32-bit hash of a UTF-8 string, used as purpose_tag on the wire."""
        h = 0x811C9DC5
        for b in s.encode("utf-8"):
            h ^= b
            h = (h * 0x01000193) & 0xFFFFFFFF
        return h

    def _io_claim_raw(self, slots: list[int], lease_ms: int, purpose: str) -> list[int]:
        """Send IO_CLAIM; return the per-slot status bytes (0 = acquired)."""
        if not self._usb:
            return [0] * len(slots)  # HTTP transport: ownership enforced server-side only
        n = len(slots)
        purpose_tag = self._fnv1a32(purpose)
        payload = struct.pack('<B', n) + bytes(slots) + struct.pack('<II', lease_ms, purpose_tag)
        resp = self._usb_cmd(CmdId.IO_CLAIM, payload)
        return list(resp[1:1 + resp[0]]) if resp else []

    def _io_release_raw(self, slots: list[int]) -> None:
        """Send IO_RELEASE command for the given slot indices (or all if empty)."""
        if not self._usb:
            return  # HTTP transport: ownership enforced server-side only
        n = len(slots)
        payload = struct.pack('<B', n) + bytes(slots)
        self._usb_cmd(CmdId.IO_RELEASE, payload)

    @contextmanager
    def io_claim(self, slots: list[int], *, lease_ms: int = 0, purpose: str = ""):
        """
        Context manager that claims the given IO slot indices for this session
        and releases them on exit (normal or exceptional).

        Slot indices: IO1..IO12 → 0..11, CH0..CH3 → 12..15.

        Example::

            with bb.io_claim([12, 13], purpose="analog_sweep"):
                bb.set_dac_voltage(0, 3.3)
                bb.set_dac_voltage(1, 1.8)
        """
        self._io_claim_raw(slots, lease_ms, purpose)
        prev = self._io_claimed_slots
        self._io_claimed_slots = set(slots)
        try:
            yield
        finally:
            self._io_claimed_slots = prev
            self._io_release_raw(slots)

    def _auto_claim_wrap(self, slots: list[int], fn, *args, **kwargs):
        """
        If not inside an io_claim() context, auto-claim *slots* with a 5 s
        lease, call *fn*, then immediately release.  If already inside a
        context that covers *slots*, just call *fn*.

        This costs 3 frames in the common single-call case (claim → mutate →
        release).  Correctness is preferred over throughput here.
        # TODO: batch claim+write into one frame in v6
        """
        if self._io_claimed_slots is not None and set(slots).issubset(self._io_claimed_slots):
            return fn(*args, **kwargs)
        if self._usb:
            self._io_claim_raw(slots, 5000, "")
            try:
                return fn(*args, **kwargs)
            finally:
                self._io_release_raw(slots)
        else:
            return fn(*args, **kwargs)

    def io_claim_lease(self, slots: list[int], *, lease_ms: int = 0, purpose: str = "") -> None:
        """
        Claim IO slots and keep them until :meth:`io_release` (or the lease
        expires). Unlike the :meth:`io_claim` context manager this does not
        release on return, so a long-lived holder (the MCP server) can span
        several calls.

        Raises ``RuntimeError`` naming the refused slots if the device rejects
        any of them (held by another owner); nothing is kept in that case.
        """
        status = self._io_claim_raw(slots, lease_ms, purpose)
        refused = [s for s, st in zip(slots, status, strict=False) if st != 0]
        if refused:
            self._io_release_raw([s for s in slots if s not in refused])
            raise RuntimeError(f"IO slots {refused} are held by another owner")
        self._io_claimed_slots = (self._io_claimed_slots or set()) | set(slots)

    def io_release(self, slots: Optional[list[int]] = None) -> None:
        """
        Explicitly release IO slot ownership.

        *slots* — list of slot indices to release (0–15).  Pass ``None`` or
        an empty list to release all slots currently owned by this session.
        """
        self._io_release_raw(slots if slots else [])
        if not slots:
            self._io_claimed_slots = None
        elif self._io_claimed_slots is not None:
            self._io_claimed_slots = (self._io_claimed_slots - set(slots)) or None

    def io_owner_status(self) -> list[dict]:
        """
        Read the 16-slot IO ownership table from the device.

        Returns a list of 16 dicts::

            [
                {
                    "slot":          0,
                    "kind":          1,   # IoOwnerKind value
                    "session_id":    0,
                    "token_fp32":    0,
                    "lease_until_ms":0,
                },
                …
            ]

        USB only; raises ``NotImplementedError`` over HTTP.
        """
        self._require_usb("io_owner_status")
        resp = self._usb_cmd(CmdId.IO_OWNER_STATUS)
        _require_resp_len(resp, 160, "IO_OWNER_STATUS")
        result = []
        for i in range(16):
            off = i * 10
            kind, session_id = struct.unpack_from('<BB', resp, off)
            token_fp32,      = struct.unpack_from('<I',  resp, off + 2)
            lease_until_ms,  = struct.unpack_from('<I',  resp, off + 6)
            result.append({
                "slot":          i,
                "kind":          kind,
                "session_id":    session_id,
                "token_fp32":    token_fp32,
                "lease_until_ms": lease_until_ms,
            })
        return result

    def io_force_release(self, slot: int, admin_token: str) -> None:
        """
        Forcibly release the owner of a single IO slot (admin-only).

        *slot* — slot index 0–15, or 255 to release all slots.
        *admin_token* — hardware-derived admin token string.

        USB only; raises ``NotImplementedError`` over HTTP.
        """
        self._require_usb("io_force_release")
        tok_bytes = admin_token.encode("ascii")
        payload = struct.pack('<BB', slot & 0xFF, len(tok_bytes)) + tok_bytes
        self._usb_cmd(CmdId.IO_FORCE_RELEASE, payload)
