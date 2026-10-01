"""Spec'd client/HAL mocks for MCP tool tests.

A bare ``MagicMock()`` accepts any call signature, which is how MCP-20 (an
``io_claim`` tool calling the client with arguments the real method rejects)
survived its own unit tests. ``create_autospec`` enforces the real signatures.
"""

from unittest.mock import create_autospec

from bugbuster import BugBuster
from bugbuster.bus import BugBusterBusManager
from bugbuster.hal import BugBusterHAL


def make_client_mock():
    bb = create_autospec(BugBuster, instance=True)
    # `bus` is a lazy property; autospec cannot see what it returns.
    bb.bus = create_autospec(BugBusterBusManager, instance=True)
    return bb


def make_hal_mock():
    return create_autospec(BugBusterHAL, instance=True)
