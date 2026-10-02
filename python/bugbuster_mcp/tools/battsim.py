"""
BugBuster MCP - DAQ HAT battery simulator tools.

The P4 drives the DUT supply along a cell discharge curve and logs history to its
`battlog` flash. These tools read status, browse and summarise runs (window
statistics: V/I/P min/avg/max, baseline/peak current, charge, energy, SOC rate,
projected life), export history to JSON/CSV and control runs.

Tools: battsim_status, battsim_list_runs, battsim_run_summary, battsim_export,
       battsim_configure, battsim_control
"""

from __future__ import annotations

import math
from typing import Optional

from .. import session

_ACTIONS = {
    "new": "new_run", "start": "start", "pause": "pause", "stop": "stop",
    "unload": "unload", "load": "load_run", "delete": "delete_run", "defaults": "chem_defaults",
}


def _clean(d: dict) -> dict:
    return {k: (None if isinstance(v, float) and not math.isfinite(v) else v) for k, v in d.items()}


def register(mcp) -> None:

    @mcp.tool()
    def battsim_status() -> dict:
        """
        Live battery-simulator state: run id, state (NONE/PAUSED/ACTIVE/DEPLETED/STOPPED),
        SOC %, measured V/I, model target V, elapsed and remaining simulated time,
        charge split (DUT, virtual external load, self-discharge, Peukert) and
        battlog flash usage. last_error explains the last refused action.
        """
        from dataclasses import asdict
        st = session.get_client().battsim.status()
        d = asdict(st)
        d["state_name"] = st.state_name
        d["error_name"] = st.error_name
        return _clean(d)

    @mcp.tool()
    def battsim_list_runs() -> dict:
        """List runs stored on the device with their battery definition and stored size."""
        from dataclasses import asdict
        from bugbuster.battsim import BsFile, parse_meta
        b = session.get_client().battsim
        ids, active = b.list_runs()
        runs = []
        for rid in ids:
            files = b.run_dir(rid)
            entry: dict = {"run_id": rid, "active": rid == active,
                           "bytes": sum(f.size for f in files), "files": len(files)}
            try:
                m = parse_meta(b.read_file(rid, BsFile.META, 0, 68))
                entry.update(name=m.name, created_epoch=m.created_epoch, format_version=m.version,
                             params=asdict(m.params))
            except (ValueError, RuntimeError, TimeoutError) as exc:
                entry["meta_error"] = str(exc)
            runs.append(entry)
        return {"active": active, "runs": runs}

    @mcp.tool()
    def battsim_run_summary(run_id: int, t_start_s: Optional[float] = None,
                            t_end_s: Optional[float] = None) -> dict:
        """
        Download a run (whole history) and return statistics for a window of
        simulated time (seconds from run start; omit for the whole run), plus events.

        Statistics: V min/avg/max, I min/avg/max, I P10 (baseline / sleep current),
        median and P99, P avg/max, charge (C, mAh), energy (J, mWh; estimated for
        format-1 runs), SOC start/end and %/day, projected battery life.
        """
        h = session.get_client().battsim.history(int(run_id))
        t0 = -math.inf if t_start_s is None else float(t_start_s)
        t1 = math.inf if t_end_s is None else float(t_end_s)
        return {
            "run_id": run_id, "name": h.meta.name, "format_version": h.meta.version,
            "points": len(h.records),
            "span_s": [h.records[0].t_s - h.records[0].dt_s, h.records[-1].t_s] if h.records else None,
            "stats": _clean(h.stats(t0, t1)),
            "events": [{"t_s": e.t_s, "event": e.name, "a": e.a, "b": e.b} for e in h.events],
        }

    @mcp.tool()
    def battsim_export(run_id: int, path: str, fmt: str = "json") -> dict:
        """Download a run and write it to `path` as JSON (schema bugbuster.battsim.run/1) or CSV."""
        from bugbuster.battsim import export_csv, export_json
        h = session.get_client().battsim.history(int(run_id))
        if fmt.lower() == "csv":
            export_csv(h, path)
        elif fmt.lower() == "json":
            export_json(h, path)
        else:
            raise ValueError("fmt must be 'json' or 'csv'")
        return {"path": path, "points": len(h.records)}

    @mcp.tool()
    def battsim_configure(chem: Optional[str] = None, cells: Optional[int] = None,
                          capacity_mah: Optional[int] = None, start_soc_pct: Optional[float] = None,
                          cutoff_mv_cell: Optional[int] = None, rint_uohm_cell: Optional[int] = None,
                          peukert: Optional[float] = None, self_discharge: Optional[bool] = None,
                          self_discharge_pct_month: Optional[float] = None,
                          external_load: Optional[bool] = None, external_load_ua: Optional[int] = None,
                          dither: Optional[bool] = None, name: Optional[str] = None) -> dict:
        """
        Write the battery definition used by the next battsim_control('new').
        chem: lipo | lifepo4 | nimh | lead. Call battsim_control('defaults') after
        chem/cells/capacity to load chemistry defaults, then override fields.
        Identity fields are refused while a run is loaded.
        """
        from bugbuster.battsim import BsChem
        chem_v = None
        if chem is not None:
            try:
                chem_v = int(BsChem[chem.strip().upper()])
            except KeyError as exc:
                raise ValueError("chem must be one of lipo, lifepo4, nimh, lead") from exc
        session.get_client().battsim.configure(
            chem=chem_v, cells=cells, capacity_mah=capacity_mah, start_soc_pct=start_soc_pct,
            cutoff_mv_cell=cutoff_mv_cell, rint_uohm_cell=rint_uohm_cell, peukert=peukert,
            sd_enable=self_discharge, sd_pct_month=self_discharge_pct_month,
            ext_enable=external_load, ext_load_ua=external_load_ua, dither=dither, name=name)
        return {"ok": True}

    @mcp.tool()
    def battsim_control(action: str, run_id: Optional[int] = None, confirm: bool = False) -> dict:
        """
        Run control. action: new (create from the configured definition, PAUSED),
        start (output ON - drives the DUT supply), pause, stop (finalise),
        unload (leave battery-sim mode), load (run_id, PAUSED), delete (run_id,
        needs confirm=True), defaults (chemistry defaults into the definition).
        Returns the status afterwards; check error_name when an action is refused.
        """
        a = action.strip().lower()
        if a not in _ACTIONS:
            raise ValueError(f"action must be one of {', '.join(_ACTIONS)}")
        if a in ("load", "delete") and run_id is None:
            raise ValueError(f"'{a}' needs run_id")
        if a == "delete" and not confirm:
            raise ValueError("delete is permanent: pass confirm=True")
        b = session.get_client().battsim
        fn = getattr(b, _ACTIONS[a])
        fn(int(run_id)) if a in ("load", "delete") else fn()
        return battsim_status()
