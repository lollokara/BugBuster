"""
tests/unit/test_discovery.py — mDNS discovery unit tests.

We don't actually send mDNS packets here; instead we monkeypatch the
``zeroconf`` import that ``bugbuster.discovery`` reaches for, so the test
exercises the wiring (TXT decode, hostname stripping, dedup, sorting,
fall-back paths) without touching the network.
"""

from __future__ import annotations

import sys
import types
from unittest.mock import patch

import pytest

from bugbuster.discovery import (
    DiscoveredDevice,
    discover_mdns,
    resolve_local,
)


# ---------------------------------------------------------------------------
# Fakes
# ---------------------------------------------------------------------------

class _FakeIPVersion:
    V4Only = "v4"


class _FakeServiceInfo:
    def __init__(self, *, server, addr, port, properties):
        self.server = server
        self._addr = addr
        self.port = port
        self.properties = properties

    def parsed_addresses(self, _ipver):
        return [self._addr]


class _FakeZeroconf:
    """Records construction args; ``get_service_info`` is keyed by name."""

    INSTANCES = []
    SERVICES: dict[str, _FakeServiceInfo] = {}

    def __init__(self, *args, **kwargs):
        _FakeZeroconf.INSTANCES.append((args, kwargs))
        self.closed = False

    def get_service_info(self, _type, name, timeout=None):  # noqa: ARG002
        return _FakeZeroconf.SERVICES.get(name)

    def close(self):
        self.closed = True


class _FakeServiceBrowser:
    """Synchronously fires ``add_service`` for every entry in SERVICES."""

    def __init__(self, zc, type_, listener):
        for name in _FakeZeroconf.SERVICES:
            listener.add_service(zc, type_, name)


class _FakeListener:
    pass


def _install_fake_zeroconf(monkeypatch, services):
    _FakeZeroconf.SERVICES = services
    _FakeZeroconf.INSTANCES = []
    fake = types.SimpleNamespace(
        Zeroconf=_FakeZeroconf,
        ServiceBrowser=_FakeServiceBrowser,
        ServiceListener=_FakeListener,
        IPVersion=_FakeIPVersion,
    )
    monkeypatch.setitem(sys.modules, "zeroconf", fake)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_discover_returns_empty_list_when_no_devices(monkeypatch):
    _install_fake_zeroconf(monkeypatch, services={})
    devs = discover_mdns(timeout=0.05)
    assert devs == []
    assert _FakeZeroconf.INSTANCES, "Zeroconf should have been instantiated"


def test_discover_decodes_txt_and_hostname(monkeypatch):
    info = _FakeServiceInfo(
        server="bugbuster-a1b2c3.local.",
        addr="192.168.4.42",
        port=80,
        properties={
            b"version": b"3.1.0",
            b"mac":     b"AA:BB:CC:DD:EE:FF",
            b"proto":   b"4",
            b"model":   b"bugbuster-s3",
        },
    )
    _install_fake_zeroconf(
        monkeypatch,
        services={"BugBuster._bugbuster._tcp.local.": info},
    )

    devs = discover_mdns(timeout=0.05)
    assert len(devs) == 1
    d = devs[0]
    assert isinstance(d, DiscoveredDevice)
    assert d.hostname == "bugbuster-a1b2c3"
    assert d.fqdn == "bugbuster-a1b2c3.local"
    assert d.ip == "192.168.4.42"
    assert d.port == 80
    assert d.firmware == "3.1.0"
    assert d.mac == "AA:BB:CC:DD:EE:FF"
    assert d.proto == "4"
    assert d.model == "bugbuster-s3"
    assert d.http_base == "http://bugbuster-a1b2c3.local"


def test_discover_dedupes_by_mac(monkeypatch):
    """Two TXT records with the same MAC collapse to one device."""
    info_a = _FakeServiceInfo(
        server="bugbuster-aa.local.", addr="10.0.0.5", port=80,
        properties={b"mac": b"AA:BB:CC:DD:EE:FF", b"version": b"3.1.0"},
    )
    info_b = _FakeServiceInfo(
        server="bugbuster-aa.local.", addr="10.0.0.5", port=80,
        properties={b"mac": b"AA:BB:CC:DD:EE:FF", b"version": b"3.1.0"},
    )
    _install_fake_zeroconf(monkeypatch, services={
        "BugBuster._bugbuster._tcp.local.":   info_a,
        "BugBuster-2._bugbuster._tcp.local.": info_b,
    })

    devs = discover_mdns(timeout=0.05)
    assert len(devs) == 1
    assert devs[0].mac == "AA:BB:CC:DD:EE:FF"


def test_discover_falls_back_to_instance_name_when_server_missing(monkeypatch):
    info = _FakeServiceInfo(
        server="", addr="10.0.0.7", port=80,
        properties={b"version": b"3.1.0"},
    )
    _install_fake_zeroconf(monkeypatch, services={
        "BoardX._bugbuster._tcp.local.": info,
    })

    devs = discover_mdns(timeout=0.05)
    assert len(devs) == 1
    assert devs[0].hostname == "BoardX"


def test_discover_skips_devices_without_addresses(monkeypatch):
    info = _FakeServiceInfo(
        server="ghost.local.", addr=None, port=80,
        properties={b"version": b"3.1.0"},
    )
    # parsed_addresses returns [None] which is non-empty but useless;
    # tweak to return empty list to exercise the address guard:
    info.parsed_addresses = lambda _v: []
    _install_fake_zeroconf(monkeypatch, services={
        "Ghost._bugbuster._tcp.local.": info,
    })

    devs = discover_mdns(timeout=0.05)
    assert devs == []


def test_discover_raises_helpful_message_when_zeroconf_missing(monkeypatch):
    """Importing zeroconf must fail loudly with an install hint."""
    monkeypatch.setitem(sys.modules, "zeroconf", None)
    # Patch __import__ so the lazy import inside discover_mdns raises.
    real_import = __builtins__["__import__"] if isinstance(__builtins__, dict) else __builtins__.__import__
    def _imp(name, *a, **kw):
        if name == "zeroconf":
            raise ImportError("no zeroconf")
        return real_import(name, *a, **kw)
    with patch("builtins.__import__", side_effect=_imp):
        with pytest.raises(ImportError, match=r'pip install "bugbuster\[network\]"'):
            discover_mdns(timeout=0.01)


def test_resolve_local_returns_none_on_failure():
    """`*.local` resolution should not raise even when no Bonjour responder."""
    result = resolve_local("definitely-not-a-real-host-xyzzy", timeout=0.5)
    assert result is None
