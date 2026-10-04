#!/usr/bin/env python3
"""Build the firmware scripting API catalogue from the .pyi stubs.

The stubs under python/firmware_modules/stubs/ are the single source for iOS
autocomplete, the iOS docs browser and the ESPFleet portal docs page
(spec 2026-10-03 §3). This tool turns them, plus stubs/examples/*.py, into one
JSON document that is committed next to them:

    python python/tools/stubs_to_json.py            # rewrite firmware_api.json + the web completions
    python python/tools/stubs_to_json.py --check    # exit 1 if either is stale

A class whose methods are all @staticmethod is a namespace (``daq.run``) and is
emitted with ``"kind": "namespace"``.

Docstring format (Google style). Everything before the first section header is
the summary (first line) and description; the sections below are parsed into
structured fields and also stay in the raw ``doc`` string:

    Args:      one ``name: text`` entry per parameter (indented continuation lines join)
    Returns:   free text
    Keys:      ``name: text`` entries describing the keys of a returned dict
    Raises:    ``ExceptionName: text`` entries
    Safety:    free text (also accepted: Warning, Note, Notes)
    Example:   a code block (Examples is accepted); repeatable

Each function/method/class gains ``summary``, ``description``, ``param_docs``
(a ``{name: text}`` map, also merged into ``params`` as ``doc``), ``returns_doc``,
``return_keys``, ``raises``, ``notes`` and ``examples``. All are additive: the
schema stays ``bugbuster.firmware-api/1`` and ``doc`` is still the full text.
A string literal straight after a constant (``X: int = 1`` then a bare string)
documents it.
"""
from __future__ import annotations

import argparse
import ast
import inspect
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STUBS = ROOT / "python" / "firmware_modules" / "stubs"
EXAMPLES = STUBS / "examples"
OUT = STUBS / "firmware_api.json"
WEB_OUT = ROOT / "Firmware" / "ESP32" / "web" / "src" / "tabs" / "scripts" / "completions.generated.ts"
SCHEMA = "bugbuster.firmware-api/1"
MODULE_ORDER = ["bugbuster", "daq", "machine", "bb_helpers", "bb_devices", "bb_logging"]


_SECTIONS = {"args": "args", "arguments": "args", "returns": "returns", "keys": "keys",
             "raises": "raises", "safety": "notes", "warning": "notes", "note": "notes",
             "notes": "notes", "example": "examples", "examples": "examples"}
_HEADER = re.compile(r"^([A-Za-z]+):\s*$")
_ENTRY = re.compile(r"^(\*{0,2}[A-Za-z_][A-Za-z0-9_.*]*)\s*:\s*(.*)$")


def _dedent(lines: list[str]) -> list[str]:
    body = [ln for ln in lines if ln.strip()]
    if not body:
        return []
    pad = min(len(ln) - len(ln.lstrip()) for ln in body)
    return [ln[pad:] if ln.strip() else "" for ln in lines]


def _trim(lines: list[str]) -> list[str]:
    while lines and not lines[0].strip():
        lines = lines[1:]
    while lines and not lines[-1].strip():
        lines = lines[:-1]
    return lines


def _entries(lines: list[str]) -> list[tuple[str, str]]:
    """``name: text`` entries; deeper-indented or non-matching lines continue the previous one."""
    out: list[list[str]] = []
    for raw in _dedent(lines):
        m = _ENTRY.match(raw) if raw and not raw[0].isspace() else None
        if m:
            out.append([m.group(1), m.group(2).strip()])
        elif raw.strip() and out:
            out[-1][1] = (out[-1][1] + " " + raw.strip()).strip()
    return [(n, t) for n, t in out]


def _paragraphs(lines: list[str]) -> str:
    """Dedented text with each hard-wrapped paragraph joined into one line (blank line = break)."""
    text = "\n".join(_trim(_dedent(lines))).strip()
    return "\n\n".join(" ".join(p.split()) for p in re.split(r"\n\s*\n", text) if p.strip())


def parse_doc(doc: str) -> dict:
    """Split a docstring into summary, description and the structured sections."""
    lines = doc.splitlines()
    head: list[str] = []
    sections: list[tuple[str, list[str]]] = []
    current: list[str] | None = None
    for raw in lines:
        m = _HEADER.match(raw)
        if m and m.group(1).lower() in _SECTIONS and not raw.startswith(" "):
            current = []
            sections.append((_SECTIONS[m.group(1).lower()], current))
        elif current is not None:
            current.append(raw)
        else:
            head.append(raw)
    head = _trim(head)
    summary = head[0].strip() if head else ""
    description = _paragraphs(head[1:])
    out: dict = {"summary": summary, "description": description, "param_docs": {},
                 "returns_doc": "", "return_keys": [], "raises": [], "notes": "", "examples": []}
    for kind, body in sections:
        if kind == "args":
            out["param_docs"].update((n.lstrip("*"), t) for n, t in _entries(body))
        elif kind == "returns":
            out["returns_doc"] = _paragraphs(body)
        elif kind == "keys":
            out["return_keys"] = [{"name": n, "doc": t} for n, t in _entries(body)]
        elif kind == "raises":
            out["raises"] = [{"type": n, "doc": t} for n, t in _entries(body)]
        elif kind == "notes":
            text = _paragraphs(body)
            out["notes"] = (out["notes"] + "\n\n" + text).strip()
        elif kind == "examples":
            code = "\n".join(_trim(_dedent(body)))
            if code:
                out["examples"].append(code)
    return out


def _ann(node: ast.expr | None) -> str | None:
    return ast.unparse(node) if node is not None else None


def _params(args: ast.arguments, drop_self: bool) -> list[dict]:
    out = []
    pos = list(args.posonlyargs) + list(args.args)
    defaults = [None] * (len(pos) - len(args.defaults)) + list(args.defaults)
    for a, d in zip(pos, defaults, strict=True):
        out.append({"name": a.arg, "kind": "positional", "annotation": _ann(a.annotation),
                    "default": _ann(d)})
    if args.vararg:
        out.append({"name": args.vararg.arg, "kind": "var_positional",
                    "annotation": _ann(args.vararg.annotation), "default": None})
    for a, d in zip(args.kwonlyargs, args.kw_defaults, strict=True):
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
    doc = ast.get_docstring(node) or ""
    parsed = parse_doc(doc)
    for p in params:
        p["doc"] = parsed["param_docs"].get(p["name"].lstrip("*"), "")
    return {"name": node.name, "signature": _signature(node.name, params, returns),
            "params": params, "returns": returns, "doc": doc, **parsed}


def _constants(body: list[ast.stmt]) -> list[dict]:
    out = []
    for i, n in enumerate(body):
        if isinstance(n, ast.AnnAssign) and isinstance(n.target, ast.Name) \
                and not n.target.id.startswith("_"):
            doc = ""
            nxt = body[i + 1] if i + 1 < len(body) else None
            if isinstance(nxt, ast.Expr) and isinstance(nxt.value, ast.Constant) \
                    and isinstance(nxt.value.value, str):
                doc = inspect.cleandoc(nxt.value.value)
            out.append({"name": n.target.id, "annotation": _ann(n.annotation),
                        "value": _ann(n.value), "doc": doc})
    return out


def _is_static(fn: ast.FunctionDef) -> bool:
    return any(isinstance(d, ast.Name) and d.id == "staticmethod" for d in fn.decorator_list)


def _class(node: ast.ClassDef) -> dict:
    funcs = [n for n in node.body if isinstance(n, ast.FunctionDef)
             and (not n.name.startswith("_") or n.name in ("__init__", "__enter__", "__exit__"))]
    namespace = bool(funcs) and all(_is_static(f) for f in funcs)
    doc = ast.get_docstring(node) or ""
    return {"name": node.name, "kind": "namespace" if namespace else "class",
            "doc": doc, **parse_doc(doc),
            "methods": [_function(f, drop_self=not namespace) for f in funcs],
            "constants": _constants(node.body)}


def module_doc(path: Path) -> dict:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    doc = ast.get_docstring(tree) or ""
    return {
        "name": path.stem,
        "doc": doc,
        **parse_doc(doc),
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


def _snippet(callee: str, fn: dict | None) -> str:
    """``callee(${req1}, ${kw}=${kw})``: the required arguments as CodeMirror placeholders."""
    args = []
    for p in (fn["params"] if fn else []):
        if p["default"] is None and p["kind"] in ("positional", "keyword_only"):
            args.append(f"{p['name']}=${{{p['name']}}}" if p["kind"] == "keyword_only" else f"${{{p['name']}}}")
    return f"{callee}({', '.join(args)})"


def _info(fn: dict) -> str:
    lines = [fn["summary"]] if fn["summary"] else []
    lines += [f"{p['name']}: {p['doc']}" for p in fn["params"] if p["doc"]]
    if fn["returns_doc"]:
        lines.append(f"returns: {fn['returns_doc']}")
    return "\n".join(lines)


def web_completions(doc: dict) -> list[dict]:
    """Compact completion table for the on-device web editor (CodeMirror), built from the catalogue."""
    out: list[dict] = []
    seen: set[str] = set()

    def add(label: str, snippet: str, kind: str, detail: str, info: str = "") -> None:
        if label in seen:
            return
        seen.add(label)
        out.append({"label": label, "snippet": snippet, "type": kind, "detail": detail[:90], "info": info})

    for mod in doc["modules"]:
        m = mod["name"]
        add(m, m, "namespace", mod["summary"], mod["summary"])
        for fn in mod["functions"]:
            add(f"{m}.{fn['name']}", _snippet(f"{m}.{fn['name']}", fn), "function", fn["summary"], _info(fn))
        for c in mod["constants"]:
            add(f"{m}.{c['name']}", f"{m}.{c['name']}", "constant", c["doc"])
        for cls in mod["classes"]:
            path = f"{m}.{cls['name']}"
            if cls["kind"] == "namespace":
                add(path, path, "namespace", cls["summary"], cls["summary"])
                for fn in cls["methods"]:
                    add(f"{path}.{fn['name']}", _snippet(f"{path}.{fn['name']}", fn), "function",
                        fn["summary"], _info(fn))
                continue
            ctor = next((f for f in cls["methods"] if f["name"] == "__init__"), None)
            add(path, _snippet(path, ctor), "class", cls["summary"], _info(ctor) if ctor else cls["summary"])
            for c in cls["constants"]:
                add(f"{path}.{c['name']}", f"{path}.{c['name']}", "constant", c["doc"])
    # Instance methods complete after any receiver (`ch.`), so they carry the bare name.
    for mod in doc["modules"]:
        for cls in mod["classes"]:
            for fn in cls["methods"]:
                if not fn["name"].startswith("_"):
                    add(fn["name"], _snippet(fn["name"], fn), "method", f"{cls['name']}.{fn['name']}: {fn['summary']}",
                        _info(fn))
    return out


def render_web(doc: dict) -> str:
    rows = json.dumps(web_completions(doc), indent=1, ensure_ascii=False)
    return ("// GENERATED by python/tools/stubs_to_json.py from python/firmware_modules/stubs/*.pyi -- do not edit.\n"
            "// Regenerate: python python/tools/stubs_to_json.py\n"
            "export interface GeneratedCompletion {\n  label: string;\n  snippet: string;\n"
            "  type: \"function\" | \"class\" | \"method\" | \"constant\" | \"namespace\";\n"
            "  detail: string;\n  info: string;\n}\n\n"
            f"export const generatedCompletions: GeneratedCompletion[] = {rows};\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if --out is stale")
    ap.add_argument("--out", type=Path, default=OUT)
    ap.add_argument("--web-out", type=Path, default=WEB_OUT, help="TypeScript completions for the web editor")
    a = ap.parse_args(argv)
    doc = build()
    outputs = [(a.out, render(doc)), (a.web_out, render_web(doc))]
    if a.check:
        stale = [p for p, text in outputs
                 if (p.read_text(encoding="utf-8") if p.is_file() else "") != text]
        for p in stale:
            print(f"{p} is stale; run: python python/tools/stubs_to_json.py", file=sys.stderr)
        return 1 if stale else 0
    for p, text in outputs:
        p.write_text(text, encoding="utf-8")
        print(f"wrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
