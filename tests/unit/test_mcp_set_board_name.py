"""IO-23: set_board must only load profiles from the board_profiles directory.

A: the name was joined into a path unchecked, so ``..``/absolute names loaded
any readable ``.json`` file as the active board profile."""
from __future__ import annotations

import os

import pytest

from bugbuster_mcp import session
from bugbuster_mcp.tools import discovery


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator

    def resource(self, *_a, **_k):
        return lambda func: func


@pytest.fixture
def set_board():
    mcp = _DummyMCP()
    discovery.register(mcp)
    previous = session._active_board
    yield mcp.tools["set_board"]
    session._active_board = previous


def _outside_profile(tmp_path, monkeypatch) -> str:
    # Point the tool's package root into tmp_path so the traversal stays on one
    # drive (CI checks out on D: while tmp_path lives on C:).
    monkeypatch.setattr(discovery, "__file__", str(tmp_path / "pkg" / "tools" / "discovery.py"))
    (tmp_path / "pkg" / "board_profiles").mkdir(parents=True)
    target = tmp_path / "evil.json"
    target.write_text('{"name": "evil", "description": "outside"}', encoding="utf-8")
    return "../../evil"


def test_relative_traversal_is_rejected(set_board, tmp_path, monkeypatch):
    name = _outside_profile(tmp_path, monkeypatch)
    assert os.path.exists(os.path.join(tmp_path, "pkg", "board_profiles", name + ".json"))
    result = set_board(name)
    assert result.startswith("Error"), result
    assert "outside" not in result
    assert session._active_board != name


def test_absolute_path_is_rejected(set_board, tmp_path):
    target = tmp_path / "evil.json"
    target.write_text('{"name": "evil", "description": "outside"}', encoding="utf-8")
    result = set_board(str(target)[:-5])
    assert result.startswith("Error"), result
