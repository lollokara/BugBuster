"""
BugBuster MCP — On-device scripting tools (V2-H).

Tools: run_device_script
"""

from __future__ import annotations
from .. import session


def register(mcp) -> None:

    @mcp.tool()
    def run_device_script(
        src: str,
        persist: bool = False,
        drain_logs: bool = True,
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
        - drain_logs: If True (default), wait for and return log output.

        Returns: success (bool), id (int), logs (str), status (dict),
                 error (str or null).
        """
        bb = session.get_client()

        error: str | None = None
        logs = ""
        script_id = 0

        try:
            result = bb.script_eval(src, persist=persist)
            script_id = result.script_id

            if drain_logs:
                chunks = []
                while True:
                    chunk = bb.script_logs()
                    if not chunk:
                        break
                    chunks.append(chunk)
                logs = "".join(chunks)

            st = bb.script_status()
            status = {
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
            }

            return {
                "success": True,
                "id":      script_id,
                "logs":    logs,
                "status":  status,
                "error":   None,
            }

        except Exception as exc:
            error = str(exc)
            return {
                "success": False,
                "id":      script_id,
                "logs":    logs,
                "status":  {},
                "error":   error,
            }
