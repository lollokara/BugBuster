"""Hardware test: the boot autorun script starts again after a reboot.

Reboots the S3 (``machine.reset()`` via /api/scripts/eval), stays quiet for the 5 s
autorun grace window (any request inside it cancels autorun), then checks the
script slot and the autorun status. Never changes or disables autorun; it must
already be enabled. Needs ``--allow-reboot``. Read-only on the battery sim.

    PYTHONPATH=python pytest tests/device/test_hw_autorun_reboot.py \
        --device-http=<ip> --admin-token=<tok> --allow-reboot
"""

import base64
import subprocess
import time

import pytest

from tests.device._hw_scripts import ScriptsApi

pytestmark = [pytest.mark.timeout(180), pytest.mark.http_only, pytest.mark.destructive]

QUIET_S = 25       # total quiet time after the reset
GRACE_MARGIN_S = 45  # after the first ping reply: HTTP + the 5 s grace window come up well after the network


def _ping(host):
    return subprocess.run(["ping", "-c", "1", "-W", "1", host],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def _wait_quiet_boot(host):
    """Wait for the board to come back WITHOUT touching its HTTP/BBP/CLI.

    ICMP does not count as inbound activity; an HTTP poll that lands inside the
    5 s autorun grace window would cancel autorun. Ping until it answers, then
    stay quiet past the grace window.
    """
    time.sleep(QUIET_S)
    end = time.time() + 90
    while time.time() < end and not _ping(host):
        time.sleep(1)
    time.sleep(GRACE_MARGIN_S)


def _bs_status(session, base):
    """(run_id, state) from a read-only battery-sim STATUS (op 0)."""
    from bugbuster.battsim import BsOp, parse_status
    r = session.post(base + "/api/daq/bs", json={"op": int(BsOp.STATUS), "args": ""}, timeout=10)
    j = r.json()
    if r.status_code != 200 or not j.get("ok", False):
        return None
    st = parse_status(base64.b64decode(j.get("data", "")))
    return st.run_id, st.state


def _get_json(session, base, path):
    r = session.get(base + path, timeout=10)
    assert r.status_code == 200, "%s: HTTP %d %s" % (path, r.status_code, r.text[:200])
    return r.json()


def test_autorun_script_runs_after_reboot(request, hw_scripts_session):
    if not request.config.getoption("--allow-reboot", default=False):
        pytest.skip("reboots the board; pass --allow-reboot")
    session, base = hw_scripts_session
    scripts = ScriptsApi(session, base)    # no ensure_idle(): the running script may be the user's

    before = _get_json(session, base, "/api/scripts/autorun/status")
    if not before.get("enabled") or before.get("scriptName") != "autorun.py":
        pytest.skip("autorun must already be enabled with autorun.py: %r" % before)
    bs_before = _bs_status(session, base)

    # eval is refused while a file script holds the slot. The only thing this test
    # stops is the autorun.py instance itself (the reboot restarts it); autorun
    # stays enabled and any other script means the slot is not free -> skip.
    st = scripts.status()
    if st.get("state") in ("running", "stopping"):
        if st.get("name") != "autorun.py":
            pytest.skip("slot busy with %r; not rebooting" % st.get("name"))
        scripts.stop()
        scripts.wait_not_running(timeout=25.0)
    try:
        scripts.eval("import machine; machine.reset()")
    except Exception:
        pass  # the connection drops as the board resets

    _wait_quiet_boot(base.replace("http://", ""))

    st = scripts.wait_state("running", timeout=30.0)
    assert st.get("name") == "autorun.py", st
    assert st.get("source") == "autorun", st

    after = _get_json(session, base, "/api/scripts/autorun/status")
    assert after.get("ranThisBoot") is True, after
    assert after.get("scriptName"), after

    assert _bs_status(session, base) == bs_before, "battery-sim run changed across the reboot"
