"""Helper client and fixture utilities for hub streaming hardware tests.

Encapsulates interactions with the ESPFleet hub REST API (http://<hub_ip>:8080/api/v1)
and the board's read-only endpoints (hub status, device info, P4 run queries).
"""

from __future__ import annotations

import base64
import struct
from typing import Any, Optional
import requests
import pytest

from bugbuster.battsim import BsFile, BsOp, RunMeta, parse_meta


def normalize_mac(mac: str) -> str:
    """Normalize a MAC address to 12 lowercase hexadecimal characters without delimiters."""
    return mac.replace(":", "").replace("-", "").strip().lower()


class HubClient:
    """REST client for the ESPFleet hub /api/v1 endpoints."""

    def __init__(self, base_url: str, session: Optional[requests.Session] = None, timeout: float = 8.0):
        self.base_url = base_url.rstrip("/")
        self.api_url = f"{self.base_url}/api/v1"
        self.s = session or requests.Session()
        self.timeout = timeout

    def get_devices(self) -> list[dict[str, Any]]:
        """GET /api/v1/devices -> list of registered devices."""
        r = self.s.get(f"{self.api_url}/devices", timeout=self.timeout)
        r.raise_for_status()
        j = r.json()
        return j.get("devices", []) if isinstance(j, dict) else j

    def get_device(self, device_id: str) -> Optional[dict[str, Any]]:
        """GET /api/v1/devices/{device_id}."""
        r = self.s.get(f"{self.api_url}/devices/{device_id}", timeout=self.timeout)
        if r.status_code == 404:
            return None
        r.raise_for_status()
        return r.json()

    def get_logs(
        self,
        device: Optional[str] = None,
        source: Optional[str] = None,
        level: Optional[str] = None,
        q: Optional[str] = None,
        from_ts: Optional[float] = None,
        to_ts: Optional[float] = None,
        limit: int = 200,
    ) -> list[dict[str, Any]]:
        """GET /api/v1/logs."""
        params: dict[str, Any] = {"limit": limit}
        if device:
            params["device"] = device
        if source:
            params["source"] = source
        if level:
            params["level"] = level
        if q:
            params["q"] = q
        if from_ts is not None:
            params["from"] = from_ts
        if to_ts is not None:
            params["to"] = to_ts
        r = self.s.get(f"{self.api_url}/logs", params=params, timeout=self.timeout)
        r.raise_for_status()
        j = r.json()
        return j.get("logs", []) if isinstance(j, dict) else j

    def get_runs(self, device: Optional[str] = None, limit: int = 100) -> list[dict[str, Any]]:
        """GET /api/v1/runs."""
        params: dict[str, Any] = {"limit": limit}
        if device:
            params["device"] = device
        r = self.s.get(f"{self.api_url}/runs", params=params, timeout=self.timeout)
        r.raise_for_status()
        j = r.json()
        return j.get("runs", []) if isinstance(j, dict) else j

    def get_run(self, run_uid: str) -> Optional[dict[str, Any]]:
        """GET /api/v1/runs/{run_uid}."""
        r = self.s.get(f"{self.api_url}/runs/{run_uid}", timeout=self.timeout)
        if r.status_code == 404:
            return None
        r.raise_for_status()
        return r.json()

    def get_coverage(self, run_uid: str) -> dict[str, Any]:
        """GET /api/v1/runs/{run_uid}/coverage."""
        r = self.s.get(f"{self.api_url}/runs/{run_uid}/coverage", timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def get_samples(
        self,
        run_uid: str,
        from_ts: Optional[float] = None,
        to_ts: Optional[float] = None,
        bucket: Optional[int] = None,
    ) -> dict[str, Any]:
        """GET /api/v1/runs/{run_uid}/samples."""
        params: dict[str, Any] = {}
        if from_ts is not None:
            params["from"] = from_ts
        if to_ts is not None:
            params["to"] = to_ts
        if bucket is not None:
            params["bucket"] = bucket
        r = self.s.get(f"{self.api_url}/runs/{run_uid}/samples", params=params, timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def is_healthy(self) -> bool:
        """Check if the hub is reachable."""
        try:
            r = self.s.get(f"{self.api_url}/devices", timeout=3.0)
            return r.status_code < 500
        except Exception:
            return False


def get_board_mac_id(session: requests.Session, device_base: str) -> str:
    """Read the MAC address from /api/device/info and return normalized 12-hex string."""
    r = session.get(f"{device_base}/api/device/info", timeout=8)
    r.raise_for_status()
    j = r.json()
    mac = j.get("macAddress") or j.get("mac_address") or j.get("mac")
    if not mac:
        raise ValueError(f"No MAC address in /api/device/info: {j}")
    return normalize_mac(mac)


def get_board_hub_status(session: requests.Session, device_base: str) -> dict[str, Any]:
    """Query GET /api/hub/status on the board."""
    r = session.get(f"{device_base}/api/hub/status", timeout=8)
    r.raise_for_status()
    return r.json()


def list_p4_runs(session: requests.Session, device_base: str) -> tuple[list[int], int]:
    """Query the board's P4 run list via read-only POST /api/daq/bs op 1 (LIST_RUNS)."""
    r = session.post(
        f"{device_base}/api/daq/bs",
        json={"op": int(BsOp.LIST_RUNS), "args": "0000"},
        timeout=10,
    )
    r.raise_for_status()
    j = r.json()
    if not j.get("ok", False):
        raise RuntimeError(f"battsim LIST_RUNS failed: {j}")
    raw = base64.b64decode(j.get("data", ""))
    if len(raw) < 4:
        raise ValueError(f"short LIST_RUNS reply: {len(raw)} bytes")
    total, active = struct.unpack_from("<HH", raw, 0)
    page_count = (len(raw) - 4) // 2
    ids = [struct.unpack_from("<H", raw, 4 + 2 * i)[0] for i in range(page_count)]
    return ids, active


def read_p4_run_meta(session: requests.Session, device_base: str, run_id: int) -> Optional[RunMeta]:
    """Read META file (file 0) for a run via read-only POST /api/daq/bs/read."""
    r = session.post(
        f"{device_base}/api/daq/bs/read",
        json={"run": run_id, "file": int(BsFile.META), "off": 0, "len": 68},
        timeout=10,
    )
    if r.status_code != 200:
        return None
    j = r.json()
    if not j.get("ok", False):
        return None
    raw = base64.b64decode(j.get("data", ""))
    if len(raw) < 68:
        return None
    try:
        return parse_meta(raw)
    except Exception:
        return None


def setup_hub_test_env(request, hub_url: str) -> tuple[requests.Session, str, HubClient, str]:
    """Verify preconditions (non-sim, board reachable, hub reachable) or skip cleanly.

    Returns:
        (board_session, device_base_url, hub_client, mac_id)
    """
    cfg = request.config
    if cfg.getoption("--sim", default=False) or cfg.getoption("--sim-full", default=False):
        pytest.skip("hardware test: skipped under --sim (hub streaming requires real board and hub)")

    host = cfg.getoption("--device-http", default=None)
    if not host:
        pytest.skip("needs --device-http <ip> to reach the device")

    hub = HubClient(hub_url)
    if not hub.is_healthy():
        pytest.skip(f"hub at {hub_url} is unreachable or unhealthy")

    token = cfg.getoption("--admin-token", default=None)
    if not token:
        port = cfg.getoption("--device-usb", default=None)
        if port:
            try:
                import bugbuster as bb
                from bugbuster.transport.usb import USBTransport

                dev = bb.BugBuster(USBTransport(port))
                dev.connect()
                token = dev.get_admin_token()
                dev.disconnect()
            except Exception as exc:
                pytest.skip(f"cannot read admin token over USB: {exc}")

    session = requests.Session()
    if token:
        session.headers.update({"X-BugBuster-Admin-Token": token})

    device_base = f"http://{host}"
    try:
        r = session.get(f"{device_base}/api/status", timeout=5)
        if r.status_code != 200:
            pytest.skip(f"device HTTP at {device_base} returned status {r.status_code}")
    except Exception as exc:
        pytest.skip(f"device HTTP at {device_base} unreachable: {exc}")

    try:
        mac_id = get_board_mac_id(session, device_base)
    except Exception as exc:
        pytest.skip(f"cannot determine board MAC ID: {exc}")

    return session, device_base, hub, mac_id
