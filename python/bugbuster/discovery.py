"""
bugbuster.discovery — mDNS-based discovery for BugBuster boards on the LAN.

The ESP32-S3 firmware advertises itself via the `_bugbuster._tcp` mDNS service
(plus a generic `_http._tcp` advert for Bonjour browsers). This module wraps
the `zeroconf` package so callers can find boards without typing IPs.

Install the optional dependency to enable discovery::

    pip install "bugbuster[network]"

Quick start::

    import bugbuster as bb
    devices = bb.discover(timeout=2.0)
    for d in devices:
        print(f"{d.hostname:30s} {d.ip:15s} fw={d.firmware} mac={d.mac}")

    # Connect to the first discovered board over HTTP:
    if devices:
        client = bb.connect_http(f"http://{devices[0].ip}", admin_token="...")
"""

from __future__ import annotations

import socket
from dataclasses import dataclass, field
from typing import List, Optional


@dataclass(frozen=True)
class DiscoveredDevice:
    """A BugBuster board found via mDNS.

    Attributes are populated from the SRV record (hostname, port) and TXT
    records the firmware advertises (`version`, `mac`, `proto`, `model`).
    """

    hostname: str          # e.g. "bugbuster-a1b2c3" (no .local suffix)
    ip: str                # IPv4 dotted-quad
    port: int              # always 80 today, but read from SRV
    firmware: str = ""     # "3.1.0" — matches /api/device/version `version`
    mac: str = ""          # "AA:BB:CC:DD:EE:FF" — pairs HTTP & USB transports
    proto: str = ""        # BBP wire-protocol version, e.g. "4"
    model: str = ""        # "bugbuster-s3"
    instance: str = ""     # mDNS service-instance name (typically "BugBuster")
    extra: dict = field(default_factory=dict)  # any unknown TXT keys

    @property
    def fqdn(self) -> str:
        """`<hostname>.local` — useful for HTTP base URLs once mDNS resolves."""
        return f"{self.hostname}.local"

    @property
    def http_base(self) -> str:
        """`http://<hostname>.local` (or :port when not 80)."""
        host = self.fqdn
        return f"http://{host}" if self.port == 80 else f"http://{host}:{self.port}"


# Service type the firmware advertises. Both `_bugbuster._tcp` and `_http._tcp`
# carry identical TXT records; we browse the BugBuster-specific one to filter
# out other HTTP devices on the LAN.
_SERVICE_TYPE = "_bugbuster._tcp.local."


def discover_mdns(
    timeout: float = 2.0,
    *,
    iface: Optional[str] = None,
) -> List[DiscoveredDevice]:
    """Browse the LAN for BugBuster boards via mDNS.

    :param timeout: seconds to wait for responses. The browser is non-blocking;
        this is the time budget you allow boards to answer. 1-3 s is typical.
    :param iface: optional interface IP address to bind to (e.g. ``"192.168.1.42"``).
        Useful when the host has multiple active interfaces and the default
        (all-interfaces) browser misses the right LAN. Pass ``None`` to let
        zeroconf pick.
    :returns: list of unique discovered devices (deduplicated by MAC, falling
        back to hostname when MAC TXT is missing).
    :raises ImportError: if the optional `zeroconf` dependency is not installed.
        Install with ``pip install "bugbuster[network]"``.
    """
    try:
        from zeroconf import IPVersion, ServiceBrowser, ServiceListener, Zeroconf
    except ImportError as e:  # pragma: no cover — import-guard branch
        raise ImportError(
            "discover_mdns() requires the 'zeroconf' package. "
            'Install with: pip install "bugbuster[network]"'
        ) from e

    found: dict[str, DiscoveredDevice] = {}

    def _txt_decode(props: dict) -> dict[str, str]:
        out: dict[str, str] = {}
        for k, v in (props or {}).items():
            try:
                key = k.decode("utf-8") if isinstance(k, bytes) else str(k)
                val = v.decode("utf-8") if isinstance(v, bytes) else (
                    "" if v is None else str(v)
                )
                out[key] = val
            except UnicodeDecodeError:
                continue
        return out

    class _Listener(ServiceListener):
        def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:  # noqa: D401
            info = zc.get_service_info(type_, name, timeout=int(timeout * 1000))
            if not info:
                return
            addrs = info.parsed_addresses(IPVersion.V4Only)
            if not addrs:
                return
            txt = _txt_decode(info.properties)
            # Hostname: prefer the SRV server (always ends with .local.).
            srv = (info.server or "").rstrip(".")
            host = srv[:-len(".local")] if srv.endswith(".local") else srv
            if not host:
                # Fall back to the service-instance name.
                host = name.split(".")[0]
            mac = txt.pop("mac", "")
            instance_label = name.split("._bugbuster._tcp")[0]
            dev = DiscoveredDevice(
                hostname=host,
                ip=addrs[0],
                port=info.port or 80,
                firmware=txt.pop("version", ""),
                mac=mac,
                proto=txt.pop("proto", ""),
                model=txt.pop("model", ""),
                instance=instance_label,
                extra=txt,
            )
            key = mac or host
            found[key] = dev

        def update_service(self, zc, type_, name) -> None:  # noqa: D401
            self.add_service(zc, type_, name)

        def remove_service(self, zc, type_, name) -> None:  # noqa: D401
            return

    kwargs = {}
    if iface:
        kwargs["interfaces"] = [iface]
    zc = Zeroconf(**kwargs)
    try:
        ServiceBrowser(zc, _SERVICE_TYPE, _Listener())
        # Block the calling thread for the time budget. ServiceBrowser fires
        # the listener callbacks on a background thread.
        import time
        deadline = time.monotonic() + max(0.05, float(timeout))
        while time.monotonic() < deadline:
            time.sleep(0.05)
    finally:
        zc.close()

    # Sort for stable output: MAC ascending, then hostname.
    return sorted(found.values(), key=lambda d: (d.mac, d.hostname))


# Public convenience — re-exported via bugbuster/__init__.py.
discover = discover_mdns


def resolve_local(hostname: str, *, timeout: float = 2.0) -> Optional[str]:
    """Resolve a `*.local` hostname to an IPv4 address using the OS resolver.

    Returns ``None`` if resolution fails (no Bonjour, hostname not on LAN).
    Useful when the caller already knows the hostname (e.g. from a saved
    pairing) and wants to avoid a full mDNS browse.
    """
    name = hostname if hostname.endswith(".local") else f"{hostname}.local"
    try:
        socket.setdefaulttimeout(timeout)
        return socket.gethostbyname(name)
    except (socket.gaierror, socket.herror, OSError):
        return None
    finally:
        socket.setdefaulttimeout(None)


__all__ = ["DiscoveredDevice", "discover_mdns", "discover", "resolve_local"]
