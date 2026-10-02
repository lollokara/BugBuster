"""FEAT-4: every transport member the client touches is declared by a
Protocol, and both transports conform, so mypy can check the client against
the protocols instead of two concrete classes.

A: the shared Transport protocol declared only connect/disconnect/fw_version;
send_command, on_event, get/post/delete, ... were undeclared.
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / "python" / "bugbuster" / "client.py"


def _declared(proto) -> set:
    names = set()
    for klass in proto.__mro__:
        if klass.__module__ != "bugbuster.transport.protocol":
            continue   # typing.Protocol / Generic internals
        names |= {n for n in vars(klass) if not n.startswith("__") and not n.startswith("_is_")
                  and not n.startswith("_abc")}
        names |= set(getattr(klass, "__annotations__", {}))
    return names


def test_every_transport_member_used_by_the_client_is_declared():
    from bugbuster.transport import protocol as p

    used = set(re.findall(r"self\._t\.(\w+)", CLIENT.read_text(encoding="utf-8")))
    declared = _declared(p.Transport) | _declared(p.UsbTransportProtocol) | _declared(p.HttpTransportProtocol)
    assert sorted(used - declared) == []


def test_concrete_transports_conform():
    from bugbuster.transport import protocol as p
    from bugbuster.transport.http import HTTPTransport
    from bugbuster.transport.usb import USBTransport

    for proto, cls in ((p.UsbTransportProtocol, USBTransport), (p.HttpTransportProtocol, HTTPTransport)):
        missing = [n for n in _declared(proto) if not hasattr(cls, n) and n not in cls.__init__.__code__.co_names
                   and n not in getattr(cls, "__annotations__", {})]
        # attributes assigned in __init__ show up as instance attributes only
        src = Path(__import__(cls.__module__, fromlist=["x"]).__file__).read_text(encoding="utf-8")
        missing = [n for n in missing if f"self.{n}" not in src]
        assert missing == [], f"{cls.__name__} lacks {missing}"
