"""
BugBuster MCP — DAQ HAT SMU calibration tools.

Tools: daq_cal_start, daq_cal_ack, daq_cal_abort, daq_cal_status

Calibrates the DAQ HAT onboard supply (LTM8056 + DS4424). Two interactive
routines run on the HAT:

  * voltage — disconnect the DUT load, then ack; sweeps the DAC reading V_DUT.
  * current — short the output, then ack; forces the 50 mohm shunt and sweeps
    the current limit up to 2.5 A.

Typical flow::

    daq_cal_start(mode="voltage")     # -> prompt: disconnect_load
    # operator disconnects the load
    daq_cal_ack()
    daq_cal_status()                  # poll until phase == success/failed
"""

from __future__ import annotations
from .. import session


_CAL_MODE_MAP = {
    "voltage": 0,
    "v":       0,
    "current": 1,
    "i":       1,
}


def register(mcp) -> None:

    @mcp.tool()
    def daq_cal_start(mode: str) -> dict:
        """
        Start an SMU factory-calibration run on the DAQ HAT.

        The run pauses on an operator prompt before touching the supply:
          - mode="voltage": you must DISCONNECT the DUT load, then call
            daq_cal_ack(). The HAT sweeps the DS4424 voltage channel and reads
            V_DUT back through the ADC, storing a code->volts table to NVM.
          - mode="current": you must SHORT the output, then call daq_cal_ack().
            The HAT forces the 50 mohm shunt, ramps the current limit up to
            2.5 A, and stores a code->amps table to NVM.

        Calibrate voltage first, then current.

        Parameters:
        - mode: "voltage" or "current".

        Returns: success, mode, message. Poll daq_cal_status() for progress.
        """
        key = mode.strip().lower()
        if key not in _CAL_MODE_MAP:
            raise ValueError("mode must be 'voltage' or 'current'")
        from bugbuster.daq_config import DaqCalMode
        bb = session.get_client()
        bb.daq.cal_start(DaqCalMode(_CAL_MODE_MAP[key]))
        return {
            "success": True,
            "mode":    key,
            "message": "Calibration started. Poll daq_cal_status(); when the "
                       "prompt asks, perform the action and call daq_cal_ack().",
        }

    @mcp.tool()
    def daq_cal_ack() -> dict:
        """
        Acknowledge the current calibration prompt so the run can proceed.

        Call this only after performing the action the prompt requested
        (disconnect the load for voltage cal, short the output for current cal).

        Returns: success, message.
        """
        bb = session.get_client()
        bb.daq.cal_ack()
        return {"success": True, "message": "Prompt acknowledged."}

    @mcp.tool()
    def daq_cal_abort() -> dict:
        """
        Abort the in-progress calibration and restore a safe SMU state
        (output off, autorange released, DAC neutral).

        Returns: success, message.
        """
        bb = session.get_client()
        bb.daq.cal_abort()
        return {"success": True, "message": "Calibration aborted."}

    @mcp.tool()
    def daq_cal_status() -> dict:
        """
        Return the live SMU calibration status.

        Returns a dict with:
          - phase: idle | prompt | running | success | failed
          - prompt: none | disconnect_load | short_output (action required while
            phase == prompt)
          - mode: voltage | current
          - progress: 0..100
          - point, code: current sweep point index and DS4424 code
          - measured, min, max: last stable reading and span (V or A)
          - flags: validation bitfield (0 = clean)
          - vcount, icount: stored calibration points per channel
          - persist: ram | saving | saved | failed
        """
        bb = session.get_client()
        st = bb.daq.cal_status()
        # Render enum members as their lowercase names for readability.
        out = dict(st)
        for k in ("phase", "prompt", "mode", "persist"):
            v = out.get(k)
            if hasattr(v, "name"):
                out[k] = v.name.lower()
        return out
