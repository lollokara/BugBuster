"""
bugbuster.ota — Over-the-air firmware + filesystem updates over WiFi.

Wraps the firmware's ``/api/ota/*`` endpoints with a host-side helper that:

* computes SHA-256 client-side and passes it to the firmware via
  ``?sha256=<hex>`` so the device can reject a corrupted upload before
  switching the boot partition (defense-in-depth on top of the bootloader's
  built-in image checksum);
* streams the binary in chunks with a progress callback so callers can
  drive a progress bar without buffering the full image;
* surfaces the firmware's ``/api/ota/info`` snapshot (running/next
  partition, ota_state, rollback availability) so callers can show a
  meaningful pre-flight UI;
* exposes a manual ``rollback()`` that maps to the firmware's
  ``/api/ota/rollback`` endpoint — the firmware auto-marks an image valid
  on the first successful boot, but operators sometimes want to revert
  immediately (e.g. wrong build flashed by mistake).

Quick start::

    from bugbuster.transport import HTTPTransport
    from bugbuster.ota import OTAClient

    t = HTTPTransport("192.168.4.1", admin_token="<token>")
    ota = OTAClient(t)

    print(ota.get_info())          # {"running": {...}, "next": {...}, "canRollback": true}
    ota.upload_firmware("build/firmware.bin",
                        on_progress=lambda done, total: print(f"{done}/{total}"))
    # Device reboots into the new image automatically.
"""

from __future__ import annotations

import hashlib
import os
from dataclasses import dataclass
from typing import Callable, Optional

import requests

# Re-export the header name so callers don't need a second import.
ADMIN_TOKEN_HEADER = "X-BugBuster-Admin-Token"

# Type alias for the progress callback: (bytes_sent, total_bytes) -> None.
ProgressCallback = Callable[[int, int], None]


@dataclass(frozen=True)
class PartitionInfo:
    label: str
    address: int
    size: int
    state: str = ""    # only populated for the running partition


@dataclass(frozen=True)
class OTAInfo:
    """Decoded ``GET /api/ota/info`` response.

    ``can_rollback`` is the firmware's verdict from
    ``esp_ota_check_rollback_is_possible()`` — true only when there is a
    distinct previously-valid OTA slot to fall back to. Use this to gate the
    rollback button in a UI.
    """
    running: Optional[PartitionInfo]
    next: Optional[PartitionInfo]
    last_invalid: Optional[PartitionInfo]
    can_rollback: bool
    fw_version: str           # "<major>.<minor>.<patch>"


def _sha256_file(path: str, *, chunk: int = 1024 * 1024) -> str:
    """Compute the lowercase hex SHA-256 of @p path. Streams the file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(chunk), b""):
            h.update(block)
    return h.hexdigest()


class OTAError(RuntimeError):
    """Raised when the firmware rejects an OTA operation."""


class OTAClient:
    """Thin OTA helper bound to an :class:`HTTPTransport`.

    The transport must already have an admin token set (via USB pairing or
    ``HTTPTransport(admin_token=...)``); OTA endpoints are admin-gated and
    will return 401 otherwise.
    """

    def __init__(self, transport) -> None:
        # Re-use the transport's session, base URL, and admin token. Going
        # through HTTPTransport.post() would force a JSON body, but OTA needs
        # a raw binary body and a content-length header — so we touch the
        # underlying requests session directly.
        self._session = transport._session
        self._base = transport._base
        self._token = transport._admin_token
        if not self._token:
            raise OTAError(
                "OTAClient requires an admin token on the transport. "
                "Pair the board (HTTPTransport.try_pair) before calling OTA."
            )

    # -------------------------------------------------------------------- read

    def get_info(self) -> OTAInfo:
        """Fetch ``/api/ota/info`` and decode it into an :class:`OTAInfo`."""
        r = self._session.get(f"{self._base}/ota/info", timeout=10)
        r.raise_for_status()
        data = r.json()

        def _part(d) -> Optional[PartitionInfo]:
            if not d:
                return None
            return PartitionInfo(
                label=str(d.get("label", "")),
                address=int(d.get("address", 0)),
                size=int(d.get("size", 0)),
                state=str(d.get("state", "")),
            )

        return OTAInfo(
            running=_part(data.get("running")),
            next=_part(data.get("next")),
            last_invalid=_part(data.get("lastInvalid")),
            can_rollback=bool(data.get("canRollback", False)),
            fw_version=f"{data.get('fwMajor', 0)}.{data.get('fwMinor', 0)}.{data.get('fwPatch', 0)}",
        )

    # ------------------------------------------------------------------ upload

    def upload_firmware(
        self,
        path: str,
        *,
        sha256: Optional[str] = None,
        on_progress: Optional[ProgressCallback] = None,
        chunk_size: int = 64 * 1024,
        timeout: float = 120.0,
    ) -> dict:
        """Stream a firmware image to ``/api/ota/upload``.

        :param path: local path to ``firmware.bin``.
        :param sha256: lowercase hex SHA-256. Pass ``None`` to compute it from
            the file before uploading (recommended). Passing a wrong hash
            forces the firmware to reject the image without changing the
            boot target — useful for negative testing.
        :param on_progress: optional ``fn(bytes_sent, total)`` called after
            each chunk.
        :param chunk_size: bytes per HTTP chunk. Default 64 KB matches the
            firmware ring sizing in ``esp_ota_write``.
        :param timeout: per-request timeout in seconds. The default 120 s
            covers a worst-case 2 MB upload over slow WiFi.
        :returns: parsed JSON from the firmware response,
            e.g. ``{"success": true, "bytesWritten": 1234567,
            "partition": "app1", "sha256Verified": true}``.
        :raises OTAError: when the firmware returns a non-2xx status (the
            response body is included in the message).
        :note: On success the device reboots after a 1 s grace period — the
            transport's persistent connection will drop. Callers polling
            ``/api/device/version`` should expect ~5–10 s of unavailability.
        """
        return self._upload("/ota/upload", path,
                            sha256=sha256, on_progress=on_progress,
                            chunk_size=chunk_size, timeout=timeout)

    def upload_spiffs(
        self,
        path: str,
        *,
        on_progress: Optional[ProgressCallback] = None,
        chunk_size: int = 64 * 1024,
        timeout: float = 180.0,
    ) -> dict:
        """Stream a SPIFFS filesystem image to ``/api/ota/uploadfs``.

        Same semantics as :meth:`upload_firmware`, but the firmware does not
        currently SHA-verify SPIFFS uploads. Erase + write of a 4 MB partition
        can take ~30 s, hence the larger default timeout.
        """
        return self._upload("/ota/uploadfs", path,
                            sha256=None, on_progress=on_progress,
                            chunk_size=chunk_size, timeout=timeout)

    def _upload(
        self,
        endpoint: str,
        path: str,
        *,
        sha256: Optional[str],
        on_progress: Optional[ProgressCallback],
        chunk_size: int,
        timeout: float,
    ) -> dict:
        size = os.path.getsize(path)
        if size <= 0:
            raise OTAError(f"{path} is empty")

        params: dict[str, str] = {}
        if endpoint.endswith("/upload"):
            sha_hex = sha256 or _sha256_file(path)
            if len(sha_hex) != 64 or any(c not in "0123456789abcdef" for c in sha_hex.lower()):
                raise OTAError(f"sha256 must be 64 lowercase hex chars (got {sha_hex!r})")
            params["sha256"] = sha_hex.lower()

        def _gen():
            sent = 0
            with open(path, "rb") as f:
                while True:
                    block = f.read(chunk_size)
                    if not block:
                        break
                    sent += len(block)
                    yield block
                    if on_progress:
                        try:
                            on_progress(sent, size)
                        except Exception:  # callback errors must not abort upload
                            pass

        headers = {
            ADMIN_TOKEN_HEADER: self._token,
            "Content-Type": "application/octet-stream",
            "Content-Length": str(size),
        }

        r = self._session.post(
            f"{self._base}{endpoint}",
            params=params or None,
            data=_gen(),
            headers=headers,
            timeout=timeout,
        )
        if not r.ok:
            raise OTAError(f"{endpoint} -> HTTP {r.status_code}: {r.text[:300]}")
        try:
            return r.json()
        except ValueError:
            return {"raw": r.text}

    # --------------------------------------------------------------- rollback

    def rollback(self) -> dict:
        """Trigger a manual rollback to the previous OTA slot.

        Returns the firmware's response immediately; the device reboots ~500 ms
        later. Raises :class:`OTAError` (HTTP 409) when no rollback target
        exists — check :meth:`get_info` ``can_rollback`` first.
        """
        r = self._session.post(
            f"{self._base}/ota/rollback",
            headers={ADMIN_TOKEN_HEADER: self._token},
            timeout=15,
        )
        if r.status_code == 409:
            raise OTAError("No rollback target available (canRollback=false)")
        if not r.ok:
            raise OTAError(f"/api/ota/rollback -> HTTP {r.status_code}: {r.text[:300]}")
        return r.json()


__all__ = [
    "OTAClient",
    "OTAError",
    "OTAInfo",
    "PartitionInfo",
    "ProgressCallback",
]
