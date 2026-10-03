"""python/tools/stubs_to_json.py turns the .pyi stubs into the one JSON
catalogue iOS and the portal consume (spec 2026-10-03 §3). The committed JSON
must be current, and the daq stub must match the firmware module tables."""

import importlib.util
import json
import re

from tests.lib.srcread import REPO_ROOT, read_source

TOOL = REPO_ROOT / "python" / "tools" / "stubs_to_json.py"
OUT = REPO_ROOT / "python" / "firmware_modules" / "stubs" / "firmware_api.json"


def _tool():
    spec = importlib.util.spec_from_file_location("stubs_to_json", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _module(doc, name):
    return next(m for m in doc["modules"] if m["name"] == name)


def test_committed_json_is_current():
    assert _tool().main(["--check"]) == 0, "run: python python/tools/stubs_to_json.py"


def test_check_flags_a_stale_file(tmp_path):
    stale = tmp_path / "api.json"
    stale.write_text("{}\n", encoding="utf-8")
    assert _tool().main(["--check", "--out", str(stale)]) == 1


def test_schema_and_order():
    doc = json.loads(OUT.read_text(encoding="utf-8"))
    assert doc["schema"] == "bugbuster.firmware-api/1"
    assert [m["name"] for m in doc["modules"]] == [
        "bugbuster", "daq", "machine", "bb_helpers", "bb_devices", "bb_logging"]
    assert {e["name"] for e in doc["examples"]} >= {"daq_vdut_sweep.py", "daq_battery_run.py"}


def test_signatures_and_params():
    doc = _tool().build()
    vdut = next(f for f in _module(doc, "daq")["functions"] if f["name"] == "vdut")
    assert vdut["signature"] == ("vdut(enable: bool | None = None, *, volts: float | None = None, "
                                 "amps_limit: float | None = None) -> dict")
    assert [(p["name"], p["kind"]) for p in vdut["params"]] == [
        ("enable", "positional"), ("volts", "keyword_only"), ("amps_limit", "keyword_only")]
    i2c = next(c for c in _module(doc, "bugbuster")["classes"] if c["name"] == "I2C")
    scan = next(m for m in i2c["methods"] if m["name"] == "scan")
    assert scan["params"][0]["name"] == "start", "self must be dropped"


def test_daq_run_is_a_namespace_matching_firmware():
    doc = _tool().build()
    run = next(c for c in _module(doc, "daq")["classes"] if c["name"] == "run")
    assert run["kind"] == "namespace"
    src = read_source("Firmware/ESP32/src/mp/moddaq.c")

    def table(name):
        block = src[src.index(f"{name}[] = {{"):]
        block = block[:block.index("};")]
        return [q for q in re.findall(r"\{\s*MP_ROM_QSTR\(MP_QSTR_(\w+)\)", block) if q != "__name__"]

    assert [m["name"] for m in run["methods"]] == table("daq_run_globals_table")
    daq = _module(doc, "daq")
    top = [f["name"] for f in daq["functions"]] + [c["name"] for c in daq["classes"]]
    assert sorted(top) == sorted(table("daq_module_globals_table"))
