"""
BugBuster MCP — On-device scripting tools (V2-H).

Tools: run_device_script, script_list, script_get, script_put, script_delete,
script_autorun
"""

from __future__ import annotations

import time

from .. import session
from bugbuster.client import ScriptBusyError

_LOG_READS_MAX = 64   # bounded drain per poll


def _drain(bb) -> str:
    chunks = []
    for _ in range(_LOG_READS_MAX):
        chunk = bb.script_logs()
        if not chunk:
            break
        chunks.append(chunk)
    return "".join(chunks)


def _status_dict(st) -> dict:
    return {
        "is_running":        st.is_running,
        "script_id":         st.script_id,
        "total_runs":        st.total_runs,
        "total_errors":      st.total_errors,
        "last_error":        st.last_error,
        "mode":              st.mode,
        "globals_bytes_est": st.globals_bytes_est,
        "globals_count":     st.globals_count,
        "auto_reset_count":  st.auto_reset_count,
        "last_eval_at_ms":   st.last_eval_at_ms,
        "idle_for_ms":       st.idle_for_ms,
        "watermark_soft_hit": st.watermark_soft_hit,
        "name":              getattr(st, "name", ""),
        "source":            getattr(st, "source", ""),
        "state":             getattr(st, "state", ""),
        "last_exit":         getattr(st, "last_exit", ""),
        "started_at":        getattr(st, "started_at", 0),
        "file_slot_id":      getattr(st, "file_slot_id", 0),
    }


def register(mcp) -> None:

    @mcp.tool()
    def run_device_script(
        src: str,
        persist: bool = False,
        drain_logs: bool = True,
        wait: bool = True,
        timeout_s: float = 30.0,
    ) -> dict:
        """
        Evaluate a Python script on the BugBuster's embedded MicroPython engine.

        The script runs directly on the ESP32 firmware.  Use this to read
        sensors, toggle IOs, run calibration routines, or any other task that
        benefits from low-latency on-device execution.

        Return-channel convention: to return a value to the host, have the
        script call ``print(repr(value))`` as its last output line.  The MCP
        response includes the raw log text so the caller can parse it.

        Parameters:
        - src: Python source code to evaluate on-device (max 32 KB).
        - persist: If True, globals are preserved between calls (persistent
          VM mode).  Once enabled, the mode is sticky until script_reset().
        - wait: If True (default), poll until the script finishes or
          ``timeout_s`` passes, collecting all log output meanwhile. A script
          still running at the timeout keeps running (``timed_out`` true).
        - timeout_s: Upper bound for ``wait``, seconds.
        - drain_logs: With wait=False, read the log output available now.

        Returns: success (bool), id (int), logs (str), status (dict),
                 timed_out (bool), error (str or null).
        """
        bb = session.get_client()

        logs = ""
        script_id = 0
        timed_out = False

        try:
            before = bb.script_status() if wait else None
            result = bb.script_eval(src, persist=persist)
            script_id = result.script_id

            if wait:
                parts = []
                deadline = time.monotonic() + max(0.0, timeout_s)
                while True:
                    parts.append(_drain(bb))
                    st = bb.script_status()
                    # The eval is queued: until the engine reports OUR id the
                    # script may not have started yet (bench: is_running was
                    # false right after eval and every log line was lost).
                    started = (not script_id or st.script_id == script_id or
                               st.total_runs > before.total_runs)
                    if started and not st.is_running:
                        parts.append(_drain(bb))
                        break
                    if time.monotonic() >= deadline:
                        timed_out = True
                        break
                    time.sleep(0.2)
                logs = "".join(parts)
            elif drain_logs:
                logs = _drain(bb)

            st = bb.script_status()
            return {
                "success":   True,
                "id":        script_id,
                "logs":      logs,
                "status":    _status_dict(st),
                "timed_out": timed_out,
                "error":     None,
            }

        except Exception as exc:
            return {
                "success":   False,
                "id":        script_id,
                "logs":      logs,
                "status":    {},
                "timed_out": timed_out,
                "error":     str(exc),
            }

    # ---- PLT-08: stored scripts on the device's SPIFFS ---------------------

    @mcp.tool()
    def script_list() -> dict:
        """List the script files stored on the device."""
        return {"success": True, "scripts": list(session.get_client().script_list())}

    @mcp.tool()
    def script_get(name: str) -> dict:
        """Return the source of a stored script."""
        return {"success": True, "name": name, "src": session.get_client().script_get(name)}

    @mcp.tool()
    def script_put(name: str, src: str) -> dict:
        """Store (create or overwrite) a script file on the device."""
        session.get_client().script_upload(name, src)
        return {"success": True, "name": name, "bytes": len(src.encode("utf-8"))}

    @mcp.tool()
    def script_delete(name: str) -> dict:
        """Delete a stored script file."""
        session.get_client().script_delete(name)
        return {"success": True, "name": name}

    @mcp.tool()
    def script_run_file(name: str, replace: bool = False) -> dict:
        """
        Start a stored script as a background job on the device and return at
        once; it keeps running after this call. Poll run_device_script-style
        with script_autorun(action="status") / the engine status for progress.

        Only one stored script runs at a time. If another holds the slot the
        device refuses unless ``replace`` is True, which stops it (3 s
        cooperative stop, then a VM reset) and starts this one.

        Returns: success, id, name — or success False with ``running``/``id``
        naming the script that holds the slot.
        """
        bb = session.get_client()
        try:
            r = bb.script_run_file(name, background=True, replace=replace)
        except ScriptBusyError as exc:
            return {"success": False, "error": str(exc), "running": exc.running,
                    "id": exc.script_id}
        return {"success": True, "id": r.script_id, "name": name}

    @mcp.tool()
    def script_autorun(action: str = "status", name: str | None = None) -> dict:
        """
        Boot-time autorun of a stored script.

        action: "status" (default), "enable" (needs ``name``), "disable",
        "run_now" (runs the configured autorun script once, now).
        """
        bb = session.get_client()
        if action == "status":
            st = bb.script_autorun_status()
            engine = bb.script_status()
            return {"success": True,
                    "autorun": st._asdict() if hasattr(st, "_asdict") else vars(st),
                    "engine": _status_dict(engine)}
        if action == "enable":
            if not name:
                raise ValueError("enable needs a script name")
            bb.script_autorun_enable(name)
            return {"success": True, "enabled": name}
        if action == "disable":
            bb.script_autorun_disable()
            return {"success": True, "enabled": None}
        if action == "run_now":
            return {"success": True, "id": bb.script_autorun_run_now()}
        raise ValueError("action must be status, enable, disable or run_now")
