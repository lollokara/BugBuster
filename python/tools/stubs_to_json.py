#!/usr/bin/env python3
"""Build the firmware scripting API catalogue from the .pyi stubs.

The stubs under python/firmware_modules/stubs/ are the single source for iOS
autocomplete, the iOS docs browser and the ESPFleet portal docs page
(spec 2026-10-03 §3). This tool turns them, plus stubs/examples/*.py, into one
JSON document that is committed next to them:

    python python/tools/stubs_to_json.py            # rewrite firmware_api.json
    python python/tools/stubs_to_json.py --check    # exit 1 if it is stale

A class whose methods are all @staticmethod is a namespace (``daq.run``) and is
emitted with ``"kind": "namespace"``.
"""
from __future__ import annotations

import argparse
import ast
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STUBS = ROOT / "python" / "firmware_modules" / "stubs"
EXAMPLES = STUBS / "examples"
OUT = STUBS / "firmware_api.json"
SCHEMA = "bugbuster.firmware-api/1"
MODULE_ORDER = ["bugbuster", "daq", "machine", "bb_helpers", "bb_devices", "bb_logging"]


def _ann(node: ast.expr | None) -> str | None:
    return ast.unparse(node) if node is not None else None


def _params(args: ast.arguments, drop_self: bool) -> list[dict]:
    out = []
    pos = list(args.posonlyargs) + list(args.args)
    defaults = [None] * (len(pos) - len(args.defaults)) + list(args.defaults)
    for a, d in zip(pos, defaults):
        out.append({"name": a.arg, "kind": "positional", "annotation": _ann(a.annotation),
                    "default": _ann(d)})
    if args.vararg:
        out.append({"name": args.vararg.arg, "kind": "var_positional",
                    "annotation": _ann(args.vararg.annotation), "default": None})
    for a, d in zip(args.kwonlyargs, args.kw_defaults):
        out.append({"name": a.arg, "kind": "keyword_only", "annotation": _ann(a.annotation),
                    "default": _ann(d)})
    if args.kwarg:
        out.append({"name": args.kwarg.arg, "kind": "var_keyword",
                    "annotation": _ann(args.kwarg.annotation), "default": None})
    if drop_self and out and out[0]["kind"] == "positional" and out[0]["name"] in ("self", "cls"):
        out = out[1:]
    return out


def _signature(name: str, params: list[dict], returns: str | None) -> str:
    parts: list[str] = []
    star = False
    for p in params:
        if p["kind"] == "keyword_only" and not star:
            parts.append("*")
            star = True
        if p["kind"] == "var_positional":
            star = True
            text = "*" + p["name"]
        elif p["kind"] == "var_keyword":
            text = "**" + p["name"]
        else:
            text = p["name"]
        if p["annotation"]:
            text += ": " + p["annotation"]
        if p["default"] is not None:
            text += (" = " if p["annotation"] else "=") + p["default"]
        parts.append(text)
    sig = f"{name}({', '.join(parts)})"
    return f"{sig} -> {returns}" if returns else sig


def _function(node: ast.FunctionDef, drop_self: bool = False) -> dict:
    params = _params(node.args, drop_self)
    returns = _ann(node.returns)
    return {"name": node.name, "signature": _signature(node.name, params, returns),
            "params": params, "returns": returns, "doc": ast.get_docstring(node) or ""}


def _constants(body: list[ast.stmt]) -> list[dict]:
    out = []
    for n in body:
        if isinstance(n, ast.AnnAssign) and isinstance(n.target, ast.Name) \
                and not n.target.id.startswith("_"):
            out.append({"name": n.target.id, "annotation": _ann(n.annotation),
                        "value": _ann(n.value)})
    return out


def _is_static(fn: ast.FunctionDef) -> bool:
    return any(isinstance(d, ast.Name) and d.id == "staticmethod" for d in fn.decorator_list)


def _class(node: ast.ClassDef) -> dict:
    funcs = [n for n in node.body if isinstance(n, ast.FunctionDef)
             and (not n.name.startswith("_") or n.name in ("__init__", "__enter__", "__exit__"))]
    namespace = bool(funcs) and all(_is_static(f) for f in funcs)
    return {"name": node.name, "kind": "namespace" if namespace else "class",
            "doc": ast.get_docstring(node) or "",
            "methods": [_function(f, drop_self=not namespace) for f in funcs],
            "constants": _constants(node.body)}


def module_doc(path: Path) -> dict:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    return {
        "name": path.stem,
        "doc": ast.get_docstring(tree) or "",
        "functions": [_function(n) for n in tree.body
                      if isinstance(n, ast.FunctionDef) and not n.name.startswith("_")],
        "classes": [_class(n) for n in tree.body
                    if isinstance(n, ast.ClassDef) and not n.name.startswith("_")],
        "constants": _constants(tree.body),
    }


def examples() -> list[dict]:
    out = []
    for p in sorted(EXAMPLES.glob("*.py")):
        src = p.read_text(encoding="utf-8")
        doc = ast.get_docstring(ast.parse(src)) or ""
        out.append({"name": p.name, "title": doc.splitlines()[0] if doc else p.stem,
                    "doc": doc, "source": src})
    return out


def build() -> dict:
    mods = []
    for name in MODULE_ORDER:
        path = STUBS / f"{name}.pyi"
        if not path.is_file():
            raise SystemExit(f"missing stub {path}")
        mods.append(module_doc(path))
    return {"schema": SCHEMA, "modules": mods, "examples": examples()}


def render(doc: dict) -> str:
    return json.dumps(doc, indent=1, ensure_ascii=False) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if --out is stale")
    ap.add_argument("--out", type=Path, default=OUT)
    a = ap.parse_args(argv)
    text = render(build())
    if a.check:
        current = a.out.read_text(encoding="utf-8") if a.out.is_file() else ""
        if current != text:
            print(f"{a.out} is stale; run: python python/tools/stubs_to_json.py", file=sys.stderr)
            return 1
        return 0
    a.out.write_text(text, encoding="utf-8")
    print(f"wrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
