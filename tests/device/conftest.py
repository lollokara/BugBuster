"""Fixtures shared by the device tier.

``battsim_guard`` is the safety interlock for every ``--daq`` test that mutates
battery-simulator or DUT-supply state: if the board has a run loaded that this
suite did not create (name not starting with ``bbt_``) the test is SKIPPED and
nothing is stopped, paused, started, unloaded or deleted. That run is the
user's. ``tests/unit/test_hw_daq_safety_static.py`` fails the build if a
mutating device test lacks ``requires_daq`` / this fixture.
"""
import base64

import pytest

RUN_PREFIX = "bbt_"


def loaded_run_name(session, base):
    """(run_id, state, name) of the battery-sim run loaded on the board.

    Read-only: POST /api/daq/bs op 0 (STATUS) plus a META read of the loaded run.
    ``run_id`` is 0 / ``name`` None when nothing is loaded. If a run is loaded
    but its name cannot be read, name is "?" so the caller treats it as foreign.
    """
    from bugbuster.battsim import BsFile, BsOp, BsState, parse_meta, parse_status

    r = session.post(base + "/api/daq/bs", json={"op": int(BsOp.STATUS), "args": ""}, timeout=10)
    j = r.json()
    if r.status_code != 200 or not j.get("ok", False):
        raise RuntimeError("battsim status failed: HTTP %d %s" % (r.status_code, r.text[:160]))
    st = parse_status(base64.b64decode(j.get("data", "")))
    if st.run_id == 0 or st.state == BsState.NONE:
        return 0, st.state, None
    try:
        rr = session.post(base + "/api/daq/bs/read",
                          json={"run": st.run_id, "file": int(BsFile.META), "off": 0, "len": 256},
                          timeout=10)
        meta = parse_meta(base64.b64decode(rr.json().get("data", "")))
        return st.run_id, st.state, meta.name
    except Exception:  # noqa: BLE001 - unknown name == not ours
        return st.run_id, st.state, "?"


@pytest.fixture(scope="session")
def battsim_guard(daq_http, daq_http_base):
    """Skip unless the board holds no run, or only a ``bbt_`` test run."""
    try:
        run_id, state, name = loaded_run_name(daq_http, daq_http_base)
    except Exception as exc:  # noqa: BLE001
        pytest.skip("cannot read battery-sim status, refusing to mutate DAQ state: %s" % exc)
    if run_id and not (name or "").startswith(RUN_PREFIX):
        pytest.skip("battery-sim run %d (%r, state %s) is loaded and is not a %s* test run; "
                    "refusing to touch the user's run" % (run_id, name, state, RUN_PREFIX))
    return run_id
