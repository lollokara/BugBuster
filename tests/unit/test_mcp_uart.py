"""MCP-23: uart_config must map parity names to the firmware's codes.

Firmware uart_bridge.cpp:189-190 maps 1 -> UART_PARITY_ODD, 2 -> UART_PARITY_EVEN.
"""

from unittest.mock import patch

import pytest

from bugbuster_mcp import session
from bugbuster_mcp.tools.debug import register
from tests.unit._mock_client import make_client_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


@pytest.fixture
def tools():
    session.configure(transport="usb", port="/dev/null", vlogic=3.3)
    mcp = _DummyMCP()
    register(mcp)
    return mcp.tools


@pytest.mark.parametrize("name,code", [
    pytest.param("even", 2, marks=pytest.mark.xfail(strict=True, reason="MCP-23")),
    pytest.param("odd", 1, marks=pytest.mark.xfail(strict=True, reason="MCP-23")),
    ("none", 0),
])
def test_parity_name_maps_to_firmware_code(tools, name, code):
    bb = make_client_mock()
    bb.get_uart_config.return_value = [{"baudrate": 115200, "parity": 0}]
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        tools["uart_config"](bridge_id=0, parity=name)
    assert bb.set_uart_config.call_args.kwargs["parity"] == code
