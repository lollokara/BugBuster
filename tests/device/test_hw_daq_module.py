"""Hardware tests: the MicroPython ``daq`` module (spec 2026-10-03 §3).

Each test uploads a tiny script with the chunked API, runs it in the background on
the board and reads back what it printed (``print("R", repr(value))``) - so the real
VM, the real ``moddaq.c`` and, for the DAQ tier, the real P4 are in the loop.

Two tiers:

* **No HAT needed** (always runs against a real board): the module imports, exposes
  the spec surface, ``present()`` is a bool, and - when no DAQ HAT is fitted - every
  hardware call raises ``OSError(ENODEV)``.
* **DAQ HAT tier** (``@requires_daq`` + ``@requires_daq_http``, so it needs ``--daq``
  and ``--device-http``; skipped otherwise, and skipped at runtime if the board
  reports no HAT): VDUT set / read-back, a tiny battery run (new / start / pause /
  stop / edit / delete / list / status), 1 s samples, argument validation.

The DAQ tier ENERGISES THE DUT TERMINALS (VDUT up to 6 V, battery-sim output while a
run is active). Teardown restores the VDUT state it found, stops the run and deletes
every run named ``bbt_*``.

    PYTHONPATH=python pytest tests/device/test_hw_daq_module.py -v \
        --device-http=<ip> --device-usb=<port> --daq
"""

import errno
import time

import pytest

from tests.device._hw_scripts import (  # noqa: F401  (fixtures are used by name)
    ScriptsApi, hw_scripts_session, script_name, scripts,
)

pytestmark = [pytest.mark.timeout(180), pytest.mark.http_only]



def daq_hw(fn):
    """DAQ-HAT tier: needs --daq (energises the DUT terminals) and --device-http."""
    for mark in (pytest.mark.requires_daq, pytest.mark.requires_daq_http, pytest.mark.destructive):
        fn = mark(fn)
    return fn


RUN_PREFIX = "bbt_"                      # battery runs created here; 23-byte device cap


def _py(scripts, tag, src, timeout=45.0):
    """Run *src*; fail loudly with the log if the script itself errored."""
    st, text = scripts.run_source(script_name(tag), src, timeout=timeout)
    return st, text, ScriptsApi.results(text)


def _ok(st, text):
    assert st["lastExit"] == "ok", "script failed (lastExit=%r). Log:\n%s" % (st.get("lastExit"), text)


# ---------------------------------------------------------------------------
# tier 1: no DAQ HAT required
# ---------------------------------------------------------------------------

SURFACE_SRC = """\
import daq
missing = []
for n in ("present", "vdut", "read", "samples", "run"):
    if not hasattr(daq, n):
        missing.append(n)
for n in ("status", "list", "new", "start", "pause", "stop", "edit", "delete"):
    if not hasattr(daq.run, n):
        missing.append("run." + n)
print("R", repr(missing))
print("R", repr(daq.present()))
"""


def test_daq_module_imports_and_exposes_the_spec_surface(scripts):
    st, text, res = _py(scripts, "daqsurf", SURFACE_SRC)
    _ok(st, text)
    missing, present = res
    assert missing == [], "daq module lacks %r" % missing
    assert present in (True, False) and isinstance(present, bool), present


ENODEV_SRC = """\
import daq
import errno
out = {}
def probe(label, fn):
    try:
        fn()
        out[label] = "no-error"
    except OSError as e:
        out[label] = e.args[0] if e.args else -1
    except Exception as e:
        out[label] = type(e).__name__
print("R", repr(daq.present()))
probe("read", lambda: daq.read())
probe("vdut", lambda: daq.vdut())
probe("vdut_set", lambda: daq.vdut(True, volts=3.3))
probe("status", lambda: daq.run.status())
probe("list", lambda: daq.run.list())
probe("new", lambda: daq.run.new("bbt_x", "lipo", 1, 100))
probe("start", lambda: daq.run.start())
probe("pause", lambda: daq.run.pause())
probe("stop", lambda: daq.run.stop())
probe("edit", lambda: daq.run.edit(capacity_mah=10))
probe("delete", lambda: daq.run.delete(1))
probe("samples", lambda: daq.samples(1))
print("R", repr(out))
"""


def test_without_a_daq_hat_every_call_raises_enodev(scripts):
    st, text, res = _py(scripts, "daqnodev", ENODEV_SRC)
    _ok(st, text)
    present, out = res
    if present:
        pytest.skip("a DAQ HAT is connected; the ENODEV contract only applies to a board without one")
    wrong = {k: v for k, v in out.items() if v != errno.ENODEV}
    assert not wrong, "calls that did not raise OSError(ENODEV=%d): %r" % (errno.ENODEV, wrong)


# ---------------------------------------------------------------------------
# tier 2: DAQ HAT
# ---------------------------------------------------------------------------

@pytest.fixture
def hat_or_skip(scripts):
    st, text, res = _py(scripts, "daqhat", "import daq\nprint('R', repr(daq.present()))\n")
    _ok(st, text)
    if res != [True]:
        pytest.skip("no DAQ HAT connected to this board (daq.present() is False)")


@pytest.fixture
def daq_restore(scripts, hat_or_skip):
    """Snapshot VDUT over the REST status route; restore it and delete test runs after."""
    r = scripts.get("/api/daq/vdut/status")
    assert r.status_code == 200, r.text
    before = r.json()
    yield before
    # stop scripts first so nothing re-energises the output behind our back
    try:
        scripts.ensure_idle()
    except Exception:
        pass
    try:
        scripts.run_source(script_name("daqclean"), CLEANUP_SRC, timeout=45.0)
    except Exception as exc:  # noqa: BLE001
        print("[test_hw_daq_module] run cleanup script failed: %s" % exc)
    try:
        scripts.post("/api/daq/vdut/setpoint", json={
            "voltageV": before["voltageSetpointV"], "currentLimitMa": before["currentLimitMa"]})
        scripts.post("/api/daq/vdut/enable", json={"enabled": bool(before["enabled"])})
    except Exception as exc:  # noqa: BLE001
        print("[test_hw_daq_module] VDUT restore failed: %s" % exc)


CLEANUP_SRC = """\
import daq
try:
    daq.run.stop()
except Exception:
    pass
n = 0
for r in daq.run.list():
    nm = r["name"]
    if nm and nm.startswith("bbt_"):
        try:
            daq.run.delete(r["run_id"])
            n += 1
        except Exception as e:
            print("cleanup failed", r["run_id"], e)
print("R", repr(n))
"""

VDUT_SRC = """\
import daq, time
out = {}
a = daq.vdut(True, volts=3.3, amps_limit=0.2)
time.sleep_ms(800)
out["a_state"] = a
out["a_read"] = daq.read()
b = daq.vdut(volts=6.0)
time.sleep_ms(800)
out["b_state"] = b
out["b_read"] = daq.read()
out["after_state"] = daq.vdut()
c = daq.vdut(False)
out["off_state"] = c
out["off_read"] = daq.read()
print("R", repr(out))
"""


@daq_hw
def test_vdut_set_read_back_and_enable(scripts, daq_restore):
    st, text, res = _py(scripts, "daqvdut", VDUT_SRC)
    _ok(st, text)
    out = res[0]

    for k in ("present", "enabled", "fault", "volts", "amps_limit", "v", "i"):
        assert k in out["a_state"], "daq.vdut() dict missing %r: %r" % (k, out["a_state"])
    assert abs(out["a_state"]["volts"] - 3.3) < 0.3, out["a_state"]
    assert abs(out["a_state"]["amps_limit"] - 0.2) < 0.05, "amps_limit not applied: %r" % out["a_state"]
    assert out["a_state"]["enabled"] is True and out["a_state"]["fault"] is False, out["a_state"]
    assert out["after_state"]["volts"] != out["a_state"]["volts"], "second setpoint not retained"
    assert abs(out["after_state"]["volts"] - 6.0) < 0.3, out["after_state"]
    # keeping amps_limit when only volts is given
    assert abs(out["b_state"]["amps_limit"] - 0.2) < 0.05, out["b_state"]

    for key in ("a_read", "b_read", "off_read"):
        r = out[key]
        assert set(r) >= {"v", "i", "p", "enabled"}, r
        assert -1.0 < r["v"] < 25.0 and -5.0 < r["i"] < 5.0, "implausible reading %r" % r
        exp = r["v"] * r["i"]
        assert abs(r["p"] - exp) <= max(abs(exp) * 0.05, 1e-6), "p != v*i in %r" % r
    # relational, not absolute: a higher setpoint must not read lower
    assert out["b_read"]["v"] > out["a_read"]["v"], (out["a_read"], out["b_read"])
    assert out["off_state"]["enabled"] is False and out["off_read"]["enabled"] is False, out

    # the script's setpoint is what the REST route (and the iOS app) see: same current limit
    j = scripts.get("/api/daq/vdut/status").json()
    assert abs(j["voltageSetpointV"] - 6.0) < 0.3, j
    assert abs(j["currentLimitMa"] - 200.0) < 50.0, "currentLimitMa disagrees with daq.vdut(amps_limit=0.2): %r" % j


VDUT_RANGE_SRC = """\
import daq
before = daq.vdut()
res = {}
for label, kw in (("v_low", dict(volts=0.5)), ("v_high", dict(volts=25.0)),
                  ("i_high", dict(amps_limit=5.0)), ("i_low", dict(amps_limit=0.001))):
    try:
        daq.vdut(**kw)
        res[label] = "accepted"
    except ValueError:
        res[label] = "ValueError"
after = daq.vdut()
res["unchanged"] = (before["volts"], before["amps_limit"]) == (after["volts"], after["amps_limit"])
print("R", repr(res))
"""


@daq_hw
def test_vdut_out_of_range_raises_valueerror_and_keeps_the_setpoint(scripts, daq_restore):
    st, text, res = _py(scripts, "daqrange", VDUT_RANGE_SRC)
    _ok(st, text)
    out = res[0]
    assert out == {"v_low": "ValueError", "v_high": "ValueError", "i_high": "ValueError",
                   "i_low": "ValueError", "unchanged": True}, out


BATTERY_SRC = """\
import daq, time
out = {}
run_id = None
try:
    run_id = daq.run.new("bbt_life", "lipo", 1, 100, soc=50)
    out["run_id"] = run_id
    out["after_new"] = daq.run.status()
    out["listed"] = [r for r in daq.run.list() if r["run_id"] == run_id]
    daq.run.start()
    time.sleep_ms(4500)
    out["running"] = daq.run.status()
    s = daq.samples(run_id)
    out["samples"] = s
    last_t = s[-1][0] if s else 0
    out["newer"] = daq.samples(run_id, since_s=last_t)
    out["limited"] = daq.samples(run_id, max=1)
    daq.run.pause()
    out["paused"] = daq.run.status()
    daq.run.stop()
    out["stopped"] = daq.run.status()
finally:
    if run_id is not None:
        try:
            daq.run.stop()
        except Exception:
            pass
        daq.run.delete(run_id)
out["gone"] = [r for r in daq.run.list() if r["run_id"] == run_id] == []
print("R", repr(out))
"""


@daq_hw
def test_battery_run_lifecycle_samples_and_delete(scripts, daq_restore):
    st, text, res = _py(scripts, "daqbatt", BATTERY_SRC, timeout=60.0)
    _ok(st, text)
    out = res[0]
    rid = out["run_id"]
    assert isinstance(rid, int) and 1 <= rid <= 65535

    s0 = out["after_new"]
    assert s0["run_id"] == rid and s0["name"] == "bbt_life", s0
    assert s0["state"] != "active", "a freshly created run must not be running: %r" % s0
    assert s0["params"]["chem"] == "lipo" and s0["params"]["cells"] == 1 and s0["params"]["capacity_mah"] == 100, s0
    assert abs(s0["params"]["soc"] - 50.0) < 1.0, s0

    assert len(out["listed"]) == 1 and out["listed"][0]["name"] == "bbt_life", out["listed"]
    assert out["listed"][0]["chem"] == "lipo" and out["listed"][0]["capacity_mah"] == 100

    assert out["running"]["state"] == "active", "run did not start: %r" % out["running"]
    assert out["running"]["elapsed_s"] >= 2, out["running"]

    samples = out["samples"]
    assert len(samples) >= 2, "expected ~4 one-second samples after 4.5 s, got %r" % samples
    for t_s, v, i, soc, flags in samples:
        assert isinstance(t_s, int) and isinstance(flags, int)
        assert -1.0 < v < 25.0 and -5.0 < i < 5.0 and 0.0 <= soc <= 100.0, (t_s, v, i, soc, flags)
    ts = [t[0] for t in samples]
    assert ts == sorted(ts) and len(set(ts)) == len(ts), "sample times must be strictly increasing: %r" % ts
    assert all(x[0] > samples[-1][0] for x in out["newer"]), (samples[-1], out["newer"])
    assert len(out["limited"]) == 1, "max=1 must return one sample: %r" % out["limited"]

    assert out["paused"]["state"] == "paused", out["paused"]
    assert out["stopped"]["state"] in ("stopped", "none"), out["stopped"]
    assert out["gone"] is True, "run %d still listed after daq.run.delete()" % rid


EDIT_SRC = """\
import daq
out = {}
rid = daq.run.new("bbt_edit", "lifepo4", 1, 100)
try:
    s = daq.run.edit(rid, capacity_mah=250, name="bbt_edit2", soc=80)
    out["edited"] = s
    try:
        daq.run.edit(rid, bogus_param=1)
        out["bogus"] = "accepted"
    except TypeError:
        out["bogus"] = "TypeError"
    try:
        daq.run.new("bbt_badchem", "plutonium", 1, 100)
        out["badchem"] = "accepted"
    except ValueError:
        out["badchem"] = "ValueError"
finally:
    daq.run.delete(rid)
print("R", repr(out))
"""


@daq_hw
def test_run_edit_changes_params_live_and_rejects_bad_input(scripts, daq_restore):
    st, text, res = _py(scripts, "daqedit", EDIT_SRC)
    _ok(st, text)
    out = res[0]
    e = out["edited"]
    assert e["name"] == "bbt_edit2" and e["params"]["capacity_mah"] == 250, e
    assert e["params"]["chem"] == "lifepo4" and abs(e["params"]["soc"] - 80.0) < 1.0, e
    assert out["bogus"] == "TypeError", "unknown edit key must raise TypeError: %r" % out
    assert out["badchem"] == "ValueError", out


NAME_SRC = """\
import daq
n_before = len(daq.run.list())
out = {}
try:
    daq.run.new("bbt_" + "n" * 21, "lipo", 1, 100)       # 25 bytes
    out["long"] = "accepted"
except ValueError:
    out["long"] = "ValueError"
out["count_after_reject"] = len(daq.run.list()) - n_before
rid = daq.run.new("bbt_" + "n" * 19, "lipo", 1, 100)     # exactly 23 bytes
out["max_len_ok"] = True
out["max_len_name"] = daq.run.status()["name"]
daq.run.delete(rid)
print("R", repr(out))
"""


@daq_hw
def test_run_name_length_is_checked(scripts, daq_restore):
    st, text, res = _py(scripts, "daqname", NAME_SRC)
    _ok(st, text)
    out = res[0]
    assert out["long"] == "ValueError", "a 25-byte name must raise ValueError: %r" % out
    assert out["count_after_reject"] == 0, "the rejected name must not create a run: %r" % out
    assert out["max_len_ok"] and out["max_len_name"] == "bbt_" + "n" * 19, out


SAMPLES_ARGS_SRC = """\
import daq
st = daq.run.status()
loaded = st["run_id"]
other = 65000 if loaded != 65000 else 64999
out = {}
def probe(label, fn):
    try:
        fn()
        out[label] = "no-error"
    except OSError as e:
        out[label] = ("OSError", e.args[0] if e.args else -1)
    except ValueError:
        out[label] = "ValueError"
probe("not_loaded", lambda: daq.samples(other))
probe("run0", lambda: daq.samples(0))
probe("max0", lambda: daq.samples(1, max=0))
probe("max_big", lambda: daq.samples(1, max=3601))
probe("delete0", lambda: daq.run.delete(0))
print("R", repr(out))
"""


@daq_hw
def test_samples_and_run_id_argument_validation(scripts, daq_restore):
    st, text, res = _py(scripts, "daqargs", SAMPLES_ARGS_SRC)
    _ok(st, text)
    out = res[0]
    assert out["not_loaded"] == ("OSError", errno.ENOENT), \
        "samples() of a run that is not loaded must be OSError(ENOENT): %r" % out
    assert out["run0"] == "ValueError" and out["delete0"] == "ValueError", out
    assert out["max0"] == "ValueError" and out["max_big"] == "ValueError", out


@daq_hw
def test_script_stop_interrupts_a_long_samples_loop_and_leaves_hat_usable(scripts, daq_restore):
    """A stop request must land even while a script is polling the HAT."""
    src = (
        "import daq, time\n"
        "print('poll-start')\n"
        "for _ in range(2000):\n"
        "    daq.read()\n"
        "    time.sleep_ms(50)\n"
    )
    name = script_name("daqpoll")
    scripts.upload(name, src)
    scripts.run_file(name)
    scripts.wait_state("running", timeout=5.0)
    time.sleep(1.0)
    scripts.stop()
    st = scripts.wait_not_running(timeout=20.0)
    assert st["lastExit"] == "stopped", st
    # HAT link still healthy afterwards
    st2, text, res = _py(scripts, "daqafter", "import daq\nprint('R', repr(sorted(daq.read())))\n")
    _ok(st2, text)
    assert res == [["enabled", "i", "p", "v"]], res


START_RUN_SRC = """\
import daq
rid = daq.run.new("bbt_s1", "lipo", 1, 100, soc=60)
daq.run.start()
print("R", repr(rid))
"""

SAMPLES_SRC = """\
import daq
print("R", repr(daq.samples(%d)))
"""


@daq_hw
def test_p4_s1_since_over_the_bbp_path_agrees_with_daq_samples(scripts, daq_restore, http_device):
    """BS_HOP_S1_SINCE end to end, from both clients.

    The MicroPython module and the Python ``BattSim.samples_since`` (HTTP /daq/bs ->
    S3 -> UART -> P4) ask the P4 the same question; for the loaded run they must see
    the same 1 s records. A record lost or re-ordered in the new P4 ring window, or a
    framing difference between the two S3 paths, shows up here.
    """
    st, text, res = _py(scripts, "daqs1a", START_RUN_SRC)
    _ok(st, text)
    rid = res[0]
    time.sleep(4.5)

    host = http_device.battsim().samples_since(rid, 0, 600)
    assert len(host) >= 2, "no live samples from the P4 ring after 4.5 s: %r" % host
    assert [s.t_s for s in host] == sorted({s.t_s for s in host}), "S1 records not strictly increasing in t_s"
    assert all(0.0 <= s.soc_pct <= 100.0 and -1.0 < s.v < 25.0 for s in host), host

    cut = host[0].t_s
    newer = http_device.battsim().samples_since(rid, cut, 600)
    assert all(s.t_s > cut for s in newer) and [s.t_s for s in newer] == [s.t_s for s in host[1:]][:len(newer)], \
        "since_s must return exactly the records after it"

    st, text, res = _py(scripts, "daqs1b", SAMPLES_SRC % rid)
    _ok(st, text)
    mp = {t[0]: t for t in res[0]}
    common = [s for s in host if s.t_s in mp]
    assert common, "daq.samples() and BattSim.samples_since() share no timestamp: %r vs %r" % (sorted(mp), [s.t_s for s in host])
    for s in common:
        assert abs(mp[s.t_s][1] - s.v) < 0.05 and abs(mp[s.t_s][3] - s.soc_pct) < 0.5, (mp[s.t_s], s)
