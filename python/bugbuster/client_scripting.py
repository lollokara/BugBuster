"""BugBuster client methods: on-device scripting.

One layer of :class:`bugbuster.client.BugBuster` (core -> system -> scripting ->
channels -> bus -> power -> hat). ``bugbuster.client`` imports the chain at its
end, so import ``bugbuster.client`` (or ``bugbuster``) first.
"""

import struct

from typing import TYPE_CHECKING

from .constants import CmdId
from .transport.http import HTTPLogicalError
from .client import (
    AutorunStatus,
    ScriptStatusResult,
    _require_resp_len,
    _script_busy_error,
)
from .client_system import _ClientSystem

if TYPE_CHECKING:
    from .script import ScriptSession


class _ClientScripting(_ClientSystem):
    # ------------------------------------------------------------------
    # ── On-Device Scripting (USB only) ──────────────────────────────────
    # ------------------------------------------------------------------

    def script_session(self) -> "ScriptSession":
        """
        Return a :class:`~bugbuster.script.ScriptSession` bound to this client.

        Use as a context manager to open a persistent MicroPython VM, run
        multiple evals with shared state, and tear down cleanly::

            with bb.script_session() as s:
                s.eval("x = 5")
                logs = s.eval("print(x)")
        """
        from .script import ScriptSession
        return ScriptSession(client=self)  # type: ignore[arg-type]  # self is always a BugBuster

    def script_eval(self, src: str, persist: bool = False) -> "ScriptStatusResult":
        """
        Submit *src* (a Python string) to the on-device MicroPython engine.

        USB: prepends a flags byte (bit0=persist) before the u16 src_len.
        HTTP: appends ``?persist=true`` when *persist* is True.
        Returns a :class:`ScriptStatusResult` reflecting the engine state
        immediately after enqueue.  Raises ``ValueError`` if *src* exceeds
        32 KB.  Raises ``RuntimeError`` if the queue is full.
        """
        encoded = src.encode("utf-8")
        if len(encoded) > 32 * 1024:
            raise ValueError(f"script source too large: {len(encoded)} bytes (max 32768)")

        if self._usb:
            flags = 0x01 if persist else 0x00
            payload = struct.pack('<B', flags) + struct.pack('<H', len(encoded)) + encoded
            if len(payload) > 1018:
                resp = self._script_transfer(encoded, mode=2 if persist else 1)
            else:
                resp = self._usb_cmd(CmdId.SCRIPT_EVAL, payload)
            enqueued = bool(resp[0])
            script_id, = struct.unpack_from('<I', resp, 1)
            if not enqueued:
                raise RuntimeError("script_eval: device queue full or busy")
            return ScriptStatusResult(
                is_running=True,
                script_id=script_id,
                total_runs=0,
                total_errors=0,
                last_error="",
            )
        else:
            qs = "?persist=true" if persist else ""
            headers = {"Content-Type": "text/plain; charset=utf-8"}
            if self._admin_token:
                headers["X-BugBuster-Admin-Token"] = self._admin_token
            try:
                data = self._t.post(f"/scripts/eval{qs}", encoded, headers=headers)
            except HTTPLogicalError as exc:
                busy = _script_busy_error(exc)
                if busy:
                    raise busy from exc
                raise
            if not data.get("ok"):
                raise RuntimeError(f"script_eval: {data.get('err', 'unknown error')}")
            script_id = int(data.get("id", 0))
            return ScriptStatusResult(
                is_running=True,
                script_id=script_id,
                total_runs=0,
                total_errors=0,
                last_error="",
            )

    def script_status(self) -> "ScriptStatusResult":
        """
        Return the current script engine status.
        """
        if not self._usb:
            data = self._http_get("/scripts/status")
            return ScriptStatusResult(
                is_running=bool(data.get("running", False)),
                script_id=int(data.get("currentScriptId", 0)),
                total_runs=int(data.get("totalRuns", 0)),
                total_errors=int(data.get("totalErrors", 0)),
                last_error=str(data.get("lastError", "")),
                mode=1 if data.get("mode") == "PERSISTENT" else 0,
                globals_bytes_est=int(data.get("globalsBytes", 0)),
                globals_count=int(data.get("globalsCount", 0)),
                auto_reset_count=int(data.get("autoResetCount", 0)),
                last_eval_at_ms=int(data.get("lastEvalAtMs", 0)),
                idle_for_ms=int(data.get("idleForMs", 0)),
                watermark_soft_hit=bool(data.get("watermarkSoftHit", False)),
                name=str(data.get("name", "")),
                source=str(data.get("source", "")),
                state=str(data.get("state", "")),
                last_exit=str(data.get("lastExit", "")),
                started_at=int(data.get("startedAt", 0)),
                file_slot_id=int(data.get("fileSlotId", 0)),
            )
        resp = self._usb_cmd(CmdId.SCRIPT_STATUS)
        pos = 0
        is_running = bool(resp[pos]); pos += 1
        script_id, = struct.unpack_from('<I', resp, pos); pos += 4
        total_runs, = struct.unpack_from('<I', resp, pos); pos += 4
        total_errors, = struct.unpack_from('<I', resp, pos); pos += 4
        err_len = resp[pos]; pos += 1
        last_error = resp[pos:pos + err_len].decode("utf-8", errors="replace")
        pos += err_len

        # V2-A extended fields (length-gated for backward compat with older firmware)
        # Format: u8 mode, u32 globals_bytes_est, u32 globals_count,
        #         u32 auto_reset_count, u32 last_eval_at_ms, u32 idle_for_ms,
        #         u8 watermark_soft_hit
        mode = 0; globals_bytes_est = 0; globals_count = 0
        auto_reset_count = 0; last_eval_at_ms = 0; idle_for_ms = 0
        watermark_soft_hit = False
        if len(resp) >= pos + 1 + 4 + 4 + 4 + 4 + 4 + 1:
            mode = resp[pos]; pos += 1
            globals_bytes_est, = struct.unpack_from('<I', resp, pos); pos += 4
            globals_count, = struct.unpack_from('<I', resp, pos); pos += 4
            auto_reset_count, = struct.unpack_from('<I', resp, pos); pos += 4
            last_eval_at_ms, = struct.unpack_from('<I', resp, pos); pos += 4
            idle_for_ms, = struct.unpack_from('<I', resp, pos); pos += 4
            watermark_soft_hit = bool(resp[pos]); pos += 1

        return ScriptStatusResult(
            is_running=is_running,
            script_id=script_id,
            total_runs=total_runs,
            total_errors=total_errors,
            last_error=last_error,
            mode=mode,
            globals_bytes_est=globals_bytes_est,
            globals_count=globals_count,
            auto_reset_count=auto_reset_count,
            last_eval_at_ms=last_eval_at_ms,
            idle_for_ms=idle_for_ms,
            watermark_soft_hit=watermark_soft_hit,
        )

    def script_reset(self) -> None:
        """
        Request an immediate VM teardown (persistent mode only).

        USB: sends BBP_CMD_SCRIPT_AUTORUN sub=4.
        HTTP: POST /api/scripts/reset.
        No-op if the VM is already in EPHEMERAL mode.
        """
        if self._usb:
            payload = struct.pack('<B', 4)  # sub=4 RESET_VM
            self._usb_cmd(CmdId.SCRIPT_AUTORUN, payload)
        else:
            self._http_post("/scripts/reset")

    def script_logs(self) -> str:
        """
        Drain up to 1016 bytes from the on-device script log ring.

        HTTP drains the available log ring in one read. Returns a ``str``.
        Call repeatedly until the returned string is empty to drain fully.
        """
        if not self._usb:
            return str(self._http_get("/scripts/logs"))
        resp = self._usb_cmd(CmdId.SCRIPT_LOGS)
        _require_resp_len(resp, 2, "SCRIPT_LOGS")
        count, = struct.unpack_from('<H', resp, 0)
        return resp[2:2 + count].decode("utf-8", errors="replace")

    def script_stop(self) -> None:
        """
        Request the running script to stop cooperatively.
        """
        if self._usb:
            self._usb_cmd(CmdId.SCRIPT_STOP)
        else:
            self._http_post("/scripts/stop")

    def script_upload(self, name: str, src: str) -> None:
        """
        Upload *src* (Python source string) as a script named *name* to SPIFFS.

        USB: uses BBP_CMD_SCRIPT_UPLOAD (0xF9).
        HTTP: POST /api/scripts/files?name=<name> with raw body.
        Raises ``ValueError`` on invalid name or oversized source.
        Raises ``RuntimeError`` if the device reports an error.
        """
        if len(name) > 32 or not name.endswith(".py"):
            raise ValueError(f"invalid script name: {name!r}")
        encoded = src.encode("utf-8")
        if len(encoded) > 32 * 1024:
            raise ValueError(f"script source too large: {len(encoded)} bytes (max 32768)")

        if self._usb:
            name_b = name.encode("utf-8")
            payload = (bytes([len(name_b)]) + name_b +
                       struct.pack('<H', len(encoded)) + encoded)
            if len(payload) > 1018:
                resp = self._script_transfer(encoded, mode=0, name=name_b)
            else:
                resp = self._usb_cmd(CmdId.SCRIPT_UPLOAD, payload)
            ok = bool(resp[0])
            err_len = resp[1]
            err_msg = resp[2:2 + err_len].decode("utf-8", errors="replace")
            if not ok:
                raise RuntimeError(f"script_upload failed: {err_msg}")
        else:
            import urllib.parse
            qs = urllib.parse.urlencode({"name": name})
            headers = {"Content-Type": "text/x-python; charset=utf-8"}
            if self._admin_token:
                headers["X-BugBuster-Admin-Token"] = self._admin_token
            result = self._t.post(f"/scripts/files?{qs}", encoded, headers=headers)
            if not result.get("ok"):
                raise RuntimeError(f"script_upload failed: {result.get('err', 'unknown')}")

    def _script_transfer(self, encoded: bytes, mode: int, name: bytes = b"") -> bytes:
        begin = bytes((0, 0, mode, len(name))) + name + struct.pack('<H', len(encoded))
        self._usb_cmd(CmdId.SCRIPT_UPLOAD, begin)
        try:
            for offset in range(0, len(encoded), 900):
                chunk = encoded[offset:offset + 900]
                self._usb_cmd(CmdId.SCRIPT_UPLOAD,
                              b'\0\1' + struct.pack('<H', offset) + chunk)
            return self._usb_cmd(CmdId.SCRIPT_UPLOAD, b'\0\2')
        except Exception:
            try:
                self._usb_cmd(CmdId.SCRIPT_UPLOAD, b'\0\3')
            except Exception:
                pass
            raise

    def script_list(self) -> list:
        """
        Return a list of script names stored on SPIFFS.

        USB: uses BBP_CMD_SCRIPT_LIST (0xFA).
        HTTP: GET /api/scripts/files.
        Returns a list of strings.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.SCRIPT_LIST)
            count = resp[0]
            pos = 1
            names = []
            for _ in range(count):
                nl = resp[pos]; pos += 1
                names.append(resp[pos:pos + nl].decode("utf-8", errors="replace"))
                pos += nl
            return names
        else:
            result = self._http_get("/scripts/files")
            return result.get("files", [])

    def script_get(self, name: str) -> str:
        """
        Download the source of the script named *name* from SPIFFS.

        HTTP only (no BBP equivalent for raw file download).
        Returns the script source as a string.
        Raises ``RuntimeError`` if not found.
        """
        import urllib.parse
        qs = urllib.parse.urlencode({"name": name})
        # Use transport GET directly for text response
        headers = {}
        if self._admin_token:
            headers["X-BugBuster-Admin-Token"] = self._admin_token
        result = self._t.get(f"/scripts/files/get?{qs}", headers=headers)
        if isinstance(result, str):
            return result
        raise RuntimeError(f"script_get failed for {name!r}")

    def script_delete(self, name: str) -> None:
        """
        Delete the script named *name* from SPIFFS.

        USB: uses BBP_CMD_SCRIPT_DELETE (0xFC).
        HTTP: DELETE /api/scripts/files?name=<name>.
        Raises ``RuntimeError`` if the device reports an error.
        """
        if self._usb:
            name_b = name.encode("utf-8")
            payload = bytes([len(name_b)]) + name_b
            resp = self._usb_cmd(CmdId.SCRIPT_DELETE, payload)
            ok = bool(resp[0])
            err_len = resp[1]
            err_msg = resp[2:2 + err_len].decode("utf-8", errors="replace")
            if not ok:
                raise RuntimeError(f"script_delete failed: {err_msg}")
        else:
            import urllib.parse
            qs = urllib.parse.urlencode({"name": name})
            result = self._t.delete(f"/scripts/files?{qs}")
            if not result.get("ok"):
                raise RuntimeError(f"script_delete failed: {result.get('err', 'unknown')}")

    def script_run_file(self, name: str, background: bool = False,
                        replace: bool = False) -> "ScriptStatusResult":
        """
        Run the stored script named *name* from SPIFFS. Returns as soon as it
        is queued; the script keeps running on the device.

        USB: BBP_CMD_SCRIPT_RUN_FILE (0xFB); ``replace`` needs HTTP.
        HTTP: POST /api/scripts/run-file?name=<name>[&background=1][&replace=1].
        ``background`` only tells the device the caller will not watch the
        logs (it is echoed back); ``replace`` stops the running script first
        (3 s cooperative stop, then a VM reset).
        Raises :class:`ScriptBusyError` if another script holds the slot.
        """
        if self._usb:
            if replace:
                raise NotImplementedError("script_run_file(replace=True) needs the HTTP transport")
            name_b = name.encode("utf-8")
            payload = bytes([len(name_b)]) + name_b
            resp = self._usb_cmd(CmdId.SCRIPT_RUN_FILE, payload)
            ok = bool(resp[0])
            script_id, = struct.unpack_from('<I', resp, 1)
            if not ok:
                raise RuntimeError("script_run_file: busy, queue full or file not found")
            return ScriptStatusResult(is_running=True, script_id=script_id, total_runs=0,
                                      total_errors=0, last_error="")
        import urllib.parse
        params = {"name": name}
        if background:
            params["background"] = "1"
        if replace:
            params["replace"] = "1"
        qs = urllib.parse.urlencode(params)
        try:
            result = self._http_post(f"/scripts/run-file?{qs}")
        except HTTPLogicalError as exc:
            busy = _script_busy_error(exc)
            if busy:
                raise busy from exc
            raise
        if not result.get("ok"):
            raise RuntimeError(f"script_run_file failed: {result.get('error', result.get('err', 'unknown'))}")
        return ScriptStatusResult(is_running=True, script_id=int(result.get("id", 0)),
                                  total_runs=0, total_errors=0, last_error="",
                                  name=str(result.get("name", name)))

    # ── Autorun (Phase 6b) ──────────────────────────────────────────────────

    def script_autorun_status(self) -> "AutorunStatus":
        """
        Return the current autorun configuration and last-run outcome.

        USB: uses BBP_CMD_SCRIPT_AUTORUN (0xFD) sub=0.
        HTTP: GET /api/scripts/autorun/status.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.SCRIPT_AUTORUN, bytes([0]))
            enabled     = bool(resp[0])
            has_script  = bool(resp[1])
            io12_high   = bool(resp[2])
            last_run_ok = bool(resp[3])
            last_run_id, = struct.unpack_from('<I', resp, 4)
            return AutorunStatus(enabled, has_script, io12_high, last_run_ok, last_run_id)
        else:
            d = self._http_get("/scripts/autorun/status")
            return AutorunStatus(
                enabled     = bool(d.get("enabled",     False)),
                has_script  = bool(d.get("has_script",  False)),
                io12_high   = bool(d.get("io12_high",   False)),
                last_run_ok = bool(d.get("last_run_ok", False)),
                last_run_id = int(d.get("last_run_id",  0)),
            )

    def script_autorun_enable(self, name: str) -> None:
        """
        Enable autorun: copy script *name* → /spiffs/autorun.py and create sentinel.

        USB: uses BBP_CMD_SCRIPT_AUTORUN (0xFD) sub=1.
        HTTP: POST /api/scripts/autorun/enable?name=<name>.
        Raises ``RuntimeError`` on failure.
        """
        if self._usb:
            name_b  = name.encode("utf-8")
            payload = bytes([1, len(name_b)]) + name_b
            resp    = self._usb_cmd(CmdId.SCRIPT_AUTORUN, payload)
            ok      = bool(resp[0])
            if not ok:
                err_len = resp[1]
                err_msg = resp[2:2 + err_len].decode("utf-8", errors="replace")
                raise RuntimeError(f"script_autorun_enable failed: {err_msg}")
        else:
            import urllib.parse
            qs     = urllib.parse.urlencode({"name": name})
            result = self._http_post(f"/scripts/autorun/enable?{qs}")
            if not result.get("ok"):
                raise RuntimeError(f"script_autorun_enable failed: {result.get('err', 'unknown')}")

    def script_autorun_disable(self) -> None:
        """
        Disable autorun: remove sentinel (autorun.py is left in place).

        USB: uses BBP_CMD_SCRIPT_AUTORUN (0xFD) sub=2.
        HTTP: POST /api/scripts/autorun/disable.
        Raises ``RuntimeError`` on failure.
        """
        if self._usb:
            resp = self._usb_cmd(CmdId.SCRIPT_AUTORUN, bytes([2]))
            ok   = bool(resp[0])
            if not ok:
                err_len = resp[1]
                err_msg = resp[2:2 + err_len].decode("utf-8", errors="replace")
                raise RuntimeError(f"script_autorun_disable failed: {err_msg}")
        else:
            result = self._http_post("/scripts/autorun/disable")
            if not result.get("ok"):
                raise RuntimeError(f"script_autorun_disable failed: {result.get('err', 'unknown')}")

    def script_autorun_run_now(self) -> int:
        """
        Immediately run /spiffs/autorun.py, bypassing all boot gates.
        USB only — raises ``NotImplementedError`` over HTTP.

        Returns the assigned script_id.
        Raises ``RuntimeError`` on failure.
        """
        self._require_usb("script_autorun_run_now")
        resp = self._usb_cmd(CmdId.SCRIPT_AUTORUN, bytes([3]))
        ok   = bool(resp[0])
        script_id, = struct.unpack_from('<I', resp, 1)
        if not ok:
            err_len = resp[5]
            err_msg = resp[6:6 + err_len].decode("utf-8", errors="replace")
            raise RuntimeError(f"script_autorun_run_now failed: {err_msg}")
        return script_id
