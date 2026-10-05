"""Static guard: device tests that change DAQ state must be gated by --daq.

Incident: a hardware run WITHOUT --daq finalised a user's paused battery-sim run
(daq.run.* probes in an "ENODEV when no HAT" test, and /api/daq/vdut writes behind
a marker that only needed --device-http). This scans tests/device/*.py and fails
if a test that can mutate DAQ state lacks ``requires_daq`` (or ``requires_daq_bbp``,
which conftest also skips without --daq), or, for battery-sim mutations, the
``battsim_guard`` / ``daq_restore`` fixture that skips on a user's run.
"""
import ast
import re
from pathlib import Path

DEVICE_DIR = Path(__file__).resolve().parents[1] / "device"

# Anything that changes DAQ state. Matched against a test's source plus the
# module-level string constants (probe scripts) it references.
MUTATORS = [re.compile(p) for p in (
    r"daq\.run\.(new|start|pause|stop|edit|delete)\(",
    r"daq\.vdut\(\s*[^\s)]",                          # vdut(...) WITH arguments
    r"/api/daq/vdut/(enable|setpoint)",
    r"/api/daq/config",                               # op 1/4 are writes; treat the route as one
    r"\.(new_run|unload|load_run|delete_run|chem_defaults|save_profile|load_profile|delete_profile)\(",
    r"\bbattsim\(\)\.(start|pause|stop|configure)\(",
)]
BATTSIM = [re.compile(p) for p in (
    r"daq\.run\.(new|start|pause|stop|edit|delete)\(",
    r"\.(new_run|unload|load_run|delete_run|chem_defaults|save_profile|load_profile|delete_profile)\(",
    r"\bbattsim\(\)\.(start|pause|stop|configure)\(",
)]
GATE = re.compile(r"requires_daq(_bbp)?\b")
# Runs its probes WITHOUT --daq on purpose (it verifies the no-HAT contract), so it is
# exempt from the marker rule only because it first reads daq.present() in a separate
# read-only script and skips when a HAT is fitted; that ordering is pinned by
# test_enodev_probe_checks_presence_before_probing below.
RUNTIME_GUARDED = {("test_hw_daq_module.py", "test_without_a_daq_hat_every_call_raises_enodev")}
GUARD_FIXTURES = {"battsim_guard", "daq_restore"}   # daq_restore depends on battsim_guard


def _src(path):
    text = path.read_text()
    return text, ast.parse(text)


def _constants(tree):
    out = {}
    for n in tree.body:
        if isinstance(n, ast.Assign) and isinstance(n.value, ast.Constant) and isinstance(n.value.value, str):
            for t in n.targets:
                if isinstance(t, ast.Name):
                    out[t.id] = n.value.value
    return out


def _gated(text, tree, fn):
    """True if the module pytestmark or the test's decorators carry the gate."""
    module_marks = "".join(ast.get_source_segment(text, n) or ""
                           for n in tree.body
                           if isinstance(n, ast.Assign)
                           and any(isinstance(t, ast.Name) and t.id == "pytestmark" for t in n.targets))
    if GATE.search(module_marks):
        return True
    helpers = {n.name: ast.get_source_segment(text, n) or ""
               for n in tree.body if isinstance(n, ast.FunctionDef)}
    for d in fn.decorator_list:
        seg = ast.get_source_segment(text, d) or ""
        if GATE.search(seg):
            return True
        if isinstance(d, ast.Name) and GATE.search(helpers.get(d.id, "")):   # e.g. @daq_hw
            return True
        if isinstance(d, ast.Name):                                          # alias: requires_daq = pytest.mark.requires_daq
            for n in tree.body:
                if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == d.id for t in n.targets):
                    if GATE.search(ast.get_source_segment(text, n) or ""):
                        return True
    return False


def _tests():
    for path in sorted(DEVICE_DIR.glob("*.py")):
        text, tree = _src(path)
        consts = _constants(tree)
        for fn in tree.body:
            if isinstance(fn, ast.FunctionDef) and fn.name.startswith("test_"):
                body = ast.get_source_segment(text, fn) or ""
                refs = "\n".join(consts[n.id] for n in ast.walk(fn)
                                 if isinstance(n, ast.Name) and n.id in consts)
                yield path, text, tree, fn, body + "\n" + refs


def test_scan_actually_sees_the_device_tests():
    names = [fn.name for _, _, _, fn, _ in _tests()]
    assert len(names) > 50, "scan found suspiciously few device tests: %d" % len(names)
    assert "test_without_a_daq_hat_every_call_raises_enodev" in names


def test_every_daq_mutating_device_test_requires_daq_and_the_battsim_guard():
    problems = []
    for path, text, tree, fn, blob in _tests():
        if not any(p.search(blob) for p in MUTATORS) or (path.name, fn.name) in RUNTIME_GUARDED:
            continue
        if not _gated(text, tree, fn):
            problems.append("%s::%s mutates DAQ state but lacks requires_daq" % (path.name, fn.name))
        if any(p.search(blob) for p in BATTSIM):
            args = {a.arg for a in fn.args.args}
            if not (args & GUARD_FIXTURES):
                problems.append("%s::%s mutates the battery sim but lacks the battsim_guard fixture"
                                % (path.name, fn.name))
    assert not problems, "\n".join(problems)


def test_enodev_probe_checks_presence_before_probing():
    text = (DEVICE_DIR / "test_hw_daq_module.py").read_text()
    body = text[text.index("def test_without_a_daq_hat_every_call_raises_enodev"):]
    assert body.index("PRESENT_SRC") < body.index("pytest.skip") < body.index("ENODEV_SRC")


def test_cleanup_script_never_stops_a_foreign_run():
    text = (DEVICE_DIR / "test_hw_daq_module.py").read_text()
    cleanup = text[text.index('CLEANUP_SRC = """'):]
    cleanup = cleanup[:cleanup.index('"""', 20)]
    assert cleanup.index('startswith("bbt_")') < cleanup.index("daq.run.stop()")


def test_autorun_status_falls_back_to_the_runtime_script_name():
    src = (Path(__file__).resolve().parents[2] / "Firmware/ESP32/src/mp/autorun.cpp").read_text()
    body = src[src.index("void autorun_get_status"):]
    body = body[:body.index("\n}\n")]
    assert '"autorun.py"' in body and "has_script" in body.split('"autorun.py"')[0].split("script_name[0]")[-1]
