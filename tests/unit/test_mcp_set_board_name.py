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


def _outside_profile(tmp_path) -> str:
    profile_dir = os.path.join(os.path.dirname(discovery.__file__), os.pardir, "board_profiles")
    target = tmp_path / "evil.json"
    target.write_text('{"name": "evil", "description": "outside"}', encoding="utf-8")
    return os.path.splitext(os.path.relpath(target, os.path.abspath(profile_dir)))[0]


@pytest.mark.xfail(strict=True, reason="IO-23")
def test_relative_traversal_is_rejected(set_board, tmp_path):
    name = _outside_profile(tmp_path)
    result = set_board(name)
    assert result.startswith("Error"), result
    assert "outside" not in result


@pytest.mark.xfail(strict=True, reason="IO-23")
def test_absolute_path_is_rejected(set_board, tmp_path):
    target = tmp_path / "evil.json"
    target.write_text('{"name": "evil", "description": "outside"}', encoding="utf-8")
    result = set_board(str(target)[:-5])
    assert result.startswith("Error"), result
