"""TEST-2: MCP tool tests must not stand in for the client with a bare
``MagicMock()``. Use ``tests/unit/_mock_client.make_client_mock()`` (autospec).
"""

from __future__ import annotations

import re

import pytest

from tests.lib.srcread import REPO_ROOT, read_source

UNIT = REPO_ROOT / "tests" / "unit"
BARE = re.compile(r"\b(fake_bb|fake_hal|bb|hal)\s*=\s*MagicMock\(\s*\)")

# file -> audit ID; delete the entry when the file is migrated.
NOT_YET_MIGRATED = {
    "test_mcp_io_owner.py": "TEST-2",
}


def _params():
    out = []
    for path in sorted(UNIT.glob("test_mcp_*.py")):
        marks = []
        if path.name in NOT_YET_MIGRATED:
            marks.append(pytest.mark.xfail(strict=True, reason=NOT_YET_MIGRATED[path.name]))
        out.append(pytest.param(path, marks=marks, id=path.name))
    return out


@pytest.mark.parametrize("path", _params())
def test_no_bare_client_mock(path):
    hits = [f"{path.name}:{i}" for i, line in enumerate(read_source(path).splitlines(), 1)
            if BARE.search(line)]
    assert not hits, "bare MagicMock() client/HAL stand-ins: " + ", ".join(hits)
