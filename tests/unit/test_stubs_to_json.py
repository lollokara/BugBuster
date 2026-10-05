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


# --- documentation depth (spec: docs deep dive) -------------------------------

STUB_ONLY = {"Response"}  # documents the attrtuple returned by http_get/http_post


def _all_callables(doc):
    for mod in doc["modules"]:
        for fn in mod["functions"]:
            yield f"{mod['name']}.{fn['name']}", fn
        for cls in mod["classes"]:
            for fn in cls["methods"]:
                yield f"{mod['name']}.{cls['name']}.{fn['name']}", fn


def test_parse_doc_sections():
    parsed = _tool().parse_doc(
        "Do a thing.\n\nLonger text.\n\nArgs:\n    a: first\n        continued\n    b: second\n"
        "Returns:\n    a dict\nKeys:\n    k: key doc\nRaises:\n    ValueError: bad a\n"
        "Safety:\n    Drives outputs.\nExample:\n    x = 1\n    print(x)\n")
    assert parsed["summary"] == "Do a thing."
    assert parsed["description"] == "Longer text."
    assert parsed["param_docs"] == {"a": "first continued", "b": "second"}
    assert parsed["returns_doc"] == "a dict"
    assert parsed["return_keys"] == [{"name": "k", "doc": "key doc"}]
    assert parsed["raises"] == [{"type": "ValueError", "doc": "bad a"}]
    assert parsed["notes"] == "Drives outputs."
    assert parsed["examples"] == ["x = 1\nprint(x)"]


def test_every_callable_is_fully_documented():
    doc = _tool().build()
    missing = []
    for path, fn in _all_callables(doc):
        if not fn["summary"]:
            missing.append(f"{path}: summary")
        if not fn["examples"]:
            missing.append(f"{path}: example")
        for p in fn["params"]:
            if not p["doc"]:
                missing.append(f"{path}: param {p['name']}")
        if fn["returns"] not in (None, "None") and fn["name"] != "__enter__" \
                and not fn["returns_doc"] and not fn["description"]:
            missing.append(f"{path}: returns")
    assert not missing, "\n".join(missing)


def test_every_class_and_constant_is_documented():
    doc = _tool().build()
    missing = []
    for mod in doc["modules"]:
        if not mod["summary"]:
            missing.append(mod["name"])
        for c in mod["constants"]:
            if not c["doc"]:
                missing.append(f"{mod['name']}.{c['name']}")
        for cls in mod["classes"]:
            if not cls["summary"] or not cls["examples"]:
                missing.append(f"{mod['name']}.{cls['name']}")
            missing += [f"{mod['name']}.{cls['name']}.{c['name']}"
                        for c in cls["constants"] if not c["doc"]]
    assert not missing, missing


def test_dict_returning_functions_document_their_keys():
    doc = _tool().build()
    for path, fn in _all_callables(doc):
        if fn["returns"] == "dict" or fn["returns"] == "list[dict]":
            assert fn["return_keys"], f"{path} returns a dict but has no Keys: section"


def _c_table(src, name):
    block = src[src.index(f"{name}[] = {{"):]
    block = block[:block.index("};")]
    return re.findall(r"\{\s*MP_ROM_QSTR\(MP_QSTR_(\w+)\)", block)


def test_bugbuster_stub_matches_firmware_tables():
    doc = _tool().build()
    bb = _module(doc, "bugbuster")
    src = read_source("Firmware/ESP32/src/mp/modbugbuster.c")
    fw = {q for q in _c_table(src, "bugbuster_module_globals_table") if q != "__name__"}
    stub = ({f["name"] for f in bb["functions"]} | {c["name"] for c in bb["classes"]}
            | {c["name"] for c in bb["constants"]}) - STUB_ONLY
    assert stub == fw, f"stub-only {sorted(stub - fw)}, firmware-only {sorted(fw - stub)}"
    for cls_name, c_file, table in (
            ("Channel", "modbugbuster_channel.c", "bugbuster_channel_locals_table"),
            ("I2C", "modbugbuster_i2c.c", "bugbuster_i2c_locals_table"),
            ("SPI", "modbugbuster_spi.c", "bugbuster_spi_locals_table"),
            ("Claim", "modbugbuster_owner.c", "bugbuster_claim_locals_table")):
        cls = next(c for c in bb["classes"] if c["name"] == cls_name)
        stub_methods = {m["name"] for m in cls["methods"] if m["name"] != "__init__"}
        fw_methods = set(_c_table(read_source(f"Firmware/ESP32/src/mp/{c_file}"), table))
        assert stub_methods == fw_methods, f"{cls_name}: {stub_methods ^ fw_methods}"


def test_hat_constants_match_firmware_values():
    doc = _tool().build()
    consts = {c["name"]: int(c["value"]) for c in _module(doc, "bugbuster")["constants"]}
    src = read_source("Firmware/ESP32/src/mp/modbugbuster.c")
    for name, value in re.findall(r"MP_QSTR_(HAT_\w+)\),\s*MP_ROM_INT\((\d+)\)", src):
        assert consts[name] == int(value), name
    regs = read_source("Firmware/ESP32/src/hal/ad74416h_regs.h")
    for name, value in re.findall(r"CH_FUNC_(\w+?)\s*=\s*(\d+),", regs):
        assert consts[f"FUNC_{name}"] == int(value), name


def test_examples_are_valid_and_use_only_documented_api():
    import ast

    doc = _tool().build()
    api = {m["name"]: {f["name"] for f in m["functions"]} | {c["name"] for c in m["classes"]}
           | {c["name"] for c in m["constants"]} for m in doc["modules"]}
    run_methods = {m["name"] for m in next(c for c in _module(doc, "daq")["classes"]
                                           if c["name"] == "run")["methods"]}
    assert len(doc["examples"]) >= 20
    for ex in doc["examples"]:
        tree = ast.parse(ex["source"], filename=ex["name"])
        assert ast.get_docstring(tree), f"{ex['name']}: needs a docstring (its title)"
        imported = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported |= {a.name for a in node.names}
        assert imported <= set(api), f"{ex['name']}: unknown module {imported - set(api)}"
        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) \
                    and node.value.id in api:
                assert node.attr in api[node.value.id], f"{ex['name']}: {node.value.id}.{node.attr}"
            if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Attribute) \
                    and isinstance(node.value.value, ast.Name) \
                    and (node.value.value.id, node.value.attr) == ("daq", "run"):
                assert node.attr in run_methods, f"{ex['name']}: daq.run.{node.attr}"


def test_examples_do_not_run_the_battery_or_enable_the_vdut_unattended():
    for ex in _tool().build()["examples"]:
        if ex["name"] in ("daq_battery_run.py", "daq_vdut_sweep.py"):
            continue  # the two original DAQ examples, kept as shipped
        assert "daq.run.start" not in ex["source"], ex["name"]
        assert "daq.vdut(True" not in ex["source"], ex["name"]
        assert "rail_power_up" not in ex["source"], ex["name"]
