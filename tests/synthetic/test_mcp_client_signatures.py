"""TEST-1: every MCP tool call into the client/HAL must bind to the real
signature.

Static check (AST, nothing executed): for each ``bb.<m>(...)`` / ``hal.<m>(...)``
call in ``python/bugbuster_mcp/tools``, bind the call's positional count and
keyword names against ``inspect.signature`` of the real ``BugBuster`` /
``BugBusterHAL`` method. Calls using ``*args``/``**kwargs`` are skipped.
"""

from __future__ import annotations

import ast
import inspect
from pathlib import Path

import pytest

from bugbuster import BugBuster
from bugbuster.bus import BugBusterBusManager
from bugbuster.hal import BugBusterHAL
from tests.lib.srcread import REPO_ROOT, read_source

TOOLS_DIR = REPO_ROOT / "python" / "bugbuster_mcp" / "tools"
RECEIVERS = {"bb": BugBuster, "client": BugBuster, "hal": BugBusterHAL}

# (tool file, method) -> audit ID. Each entry is a known A/B: it must fail
# until its fix lands, then the entry is deleted.
KNOWN_MISMATCHES = {
    ("io_owner.py", "io_claim"): "MCP-20",
}


def _receiver_class(node: ast.expr):
    if isinstance(node, ast.Name):
        return RECEIVERS.get(node.id)
    # bb.bus.method(...)
    if (isinstance(node, ast.Attribute) and node.attr == "bus"
            and RECEIVERS.get(getattr(node.value, "id", None)) is BugBuster):
        return BugBusterBusManager
    # session.get_client().method(...)
    if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name) and node.func.value.id == "session"):
        return {"get_client": BugBuster, "get_hal": BugBusterHAL}.get(node.func.attr)
    return None


def _collect():
    cases = []
    for path in sorted(TOOLS_DIR.glob("*.py")):
        tree = ast.parse(read_source(path), filename=str(path))
        for node in ast.walk(tree):
            if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)):
                continue
            cls = _receiver_class(node.func.value)
            if cls is None:
                continue
            if any(isinstance(a, ast.Starred) for a in node.args) or any(
                    k.arg is None for k in node.keywords):
                continue
            cases.append((path.name, node.lineno, cls, node.func.attr,
                          len(node.args), tuple(k.arg for k in node.keywords)))
    return cases


def _params():
    out = []
    for fname, line, cls, meth, nargs, kwargs in _collect():
        marks = []
        audit_id = KNOWN_MISMATCHES.get((fname, meth))
        if audit_id:
            marks.append(pytest.mark.xfail(strict=True, reason=audit_id))
        out.append(pytest.param(cls, meth, nargs, kwargs, marks=marks,
                                id=f"{fname}:{line}:{cls.__name__}.{meth}"))
    return out


def test_collector_finds_calls():
    assert len(_collect()) > 100, "AST collector found too few client calls"


@pytest.mark.parametrize("cls,meth,nargs,kwargs", _params())
def test_tool_call_binds_to_real_signature(cls, meth, nargs, kwargs):
    raw = inspect.getattr_static(cls, meth, None)
    assert raw is not None, f"{cls.__name__} has no attribute {meth!r}"
    if isinstance(raw, property):
        pytest.fail(f"{cls.__name__}.{meth} is a property but is called")
    sig = inspect.signature(getattr(cls, meth))
    placeholders = [object()] * nargs
    if not isinstance(raw, staticmethod):
        placeholders = [object(), *placeholders]  # self
    try:
        sig.bind(*placeholders, **{k: object() for k in kwargs})
    except TypeError as exc:
        pytest.fail(f"{cls.__name__}.{meth}{sig}: call with {nargs} positional "
                    f"and keywords {list(kwargs)} does not bind: {exc}")


def test_known_mismatch_table_has_no_stale_entries():
    seen = {(f, m) for f, _l, _c, m, _n, _k in _collect()}
    stale = set(KNOWN_MISMATCHES) - seen
    assert not stale, f"KNOWN_MISMATCHES entries no longer called: {stale}"


def test_tools_dir_exists():
    assert Path(TOOLS_DIR).is_dir()
