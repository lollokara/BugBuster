"""Spec 2026-10-03 §3: the .pyi stubs are the single source for iOS
autocomplete, the iOS docs browser and the portal docs page. They must parse,
cover every exported firmware name, and the pure-Python module stubs must keep
the real parameter lists."""

import ast
import re
from pathlib import Path

from tests.lib.srcread import REPO_ROOT, read_source

STUBS = REPO_ROOT / "python" / "firmware_modules" / "stubs"
MODS = ["bugbuster", "daq", "machine", "bb_helpers", "bb_devices", "bb_logging"]


def _stub(name: str) -> ast.Module:
    return ast.parse((STUBS / f"{name}.pyi").read_text(encoding="utf-8"))


def _names(tree: ast.Module) -> set[str]:
    out = set()
    for n in tree.body:
        if isinstance(n, (ast.FunctionDef, ast.ClassDef)):
            out.add(n.name)
        elif isinstance(n, ast.AnnAssign) and isinstance(n.target, ast.Name):
            out.add(n.target.id)
    return out


def _class(tree: ast.Module, name: str) -> ast.ClassDef:
    return next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == name)


def _methods(cls: ast.ClassDef) -> set[str]:
    return {n.name for n in cls.body if isinstance(n, ast.FunctionDef)} | \
           {n.target.id for n in cls.body if isinstance(n, ast.AnnAssign)}


def _c_table(path: str, table: str) -> list[str]:
    src = read_source(path)
    block = src[src.index(f"{table}[] = {{"):]
    block = block[:block.index("};")]
    return [q for q in re.findall(r"\{\s*MP_ROM_QSTR\(MP_QSTR_(\w+)\)", block) if q != "__name__"]


def test_every_stub_parses_and_has_a_module_docstring():
    for m in MODS:
        assert ast.get_docstring(_stub(m)), f"{m}.pyi needs a module docstring"


def test_bugbuster_stub_covers_the_firmware_module():
    names = _names(_stub("bugbuster"))
    exported = _c_table("Firmware/ESP32/src/mp/modbugbuster.c", "bugbuster_module_globals_table")
    missing = set(exported) - names
    assert not missing, f"bugbuster.pyi lacks {sorted(missing)}"


def test_bugbuster_classes_cover_their_methods():
    tree = _stub("bugbuster")
    for cls, path, table in [
        ("Channel", "Firmware/ESP32/src/mp/modbugbuster_channel.c", "bugbuster_channel_locals_table"),
        ("I2C", "Firmware/ESP32/src/mp/modbugbuster_i2c.c", "bugbuster_i2c_locals_table"),
        ("SPI", "Firmware/ESP32/src/mp/modbugbuster_spi.c", "bugbuster_spi_locals_table"),
    ]:
        missing = set(_c_table(path, table)) - _methods(_class(tree, cls))
        assert not missing, f"{cls} lacks {sorted(missing)}"


def test_machine_pin_stub_covers_firmware():
    pin = _class(_stub("machine"), "Pin")
    missing = set(_c_table("Firmware/ESP32/components/micropython/machine_pin.c",
                           "machine_pin_locals_dict_table")) - _methods(pin)
    assert not missing, sorted(missing)


def _params(fn: ast.FunctionDef) -> list[tuple[str, str | None]]:
    a = fn.args
    pos = a.posonlyargs + a.args
    defaults = [None] * (len(pos) - len(a.defaults)) + [ast.unparse(d) for d in a.defaults]
    return [(p.arg, d) for p, d in zip(pos, defaults)]


def _public_defs(tree: ast.Module) -> dict[str, ast.FunctionDef]:
    out = {}
    for n in tree.body:
        if isinstance(n, ast.FunctionDef) and not n.name.startswith("_"):
            out[n.name] = n
        if isinstance(n, ast.ClassDef) and not n.name.startswith("_"):
            for m in n.body:
                if isinstance(m, ast.FunctionDef) and (not m.name.startswith("_") or m.name == "__init__"):
                    out[f"{n.name}.{m.name}"] = m
    return out


def test_python_module_stubs_keep_the_real_signatures():
    for mod in ("bb_helpers", "bb_devices", "bb_logging"):
        real = _public_defs(ast.parse((REPO_ROOT / "python" / "firmware_modules" / f"{mod}.py")
                                      .read_text(encoding="utf-8")))
        stub = _public_defs(_stub(mod))
        assert set(real) == set(stub), f"{mod}: {sorted(set(real) ^ set(stub))}"
        for name, fn in real.items():
            assert _params(fn) == _params(stub[name]), f"{mod}.{name}"


def test_daq_stub_matches_spec_surface():
    tree = _stub("daq")
    assert {"present", "vdut", "read", "samples", "run"} <= _names(tree)
    run = _class(tree, "run")
    assert _methods(run) == {"status", "list", "new", "start", "pause", "stop", "reopen", "edit", "delete"}
    for fn in run.body:
        if isinstance(fn, ast.FunctionDef):
            assert any(isinstance(d, ast.Name) and d.id == "staticmethod" for d in fn.decorator_list)


def test_examples_compile_and_are_documented():
    ex = sorted((STUBS / "examples").glob("*.py"))
    assert len(ex) >= 4
    for p in ex:
        src = p.read_text(encoding="utf-8")
        compile(src, str(p), "exec")
        assert ast.get_docstring(ast.parse(src)), f"{p.name} needs a docstring (its title)"
