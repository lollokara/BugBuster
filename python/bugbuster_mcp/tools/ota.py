"""
BugBuster MCP — Over-the-air firmware tools.

Tools: ota_get_info, ota_upload_firmware, ota_upload_spiffs, ota_rollback.

Notes
-----
OTA goes over WiFi (HTTP), not USB. The tools accept an explicit ``host`` and
``admin_token`` so they work even when the MCP server is itself attached over
USB (the common case). If those args are omitted, the tools fall back to the
host/token from the active session, which is only useful when the server was
started with ``--transport http``.
"""

from __future__ import annotations
import os
from typing import Optional

from bugbuster.transport import HTTPTransport
from bugbuster.ota import OTAClient, OTAError

from .. import session


def _make_ota(host: Optional[str], admin_token: Optional[str]) -> OTAClient:
    h = host or session._host
    t = admin_token or session._admin_token
    if not h:
        raise ValueError(
            "ota tools need a host (e.g. '192.168.4.1' or 'http://bugbuster-a1b2c3.local'). "
            "Pass it explicitly or start the MCP server with --transport http --host <addr>."
        )
    if not t:
        raise ValueError(
            "ota tools need admin_token (the 64-hex pairing token from USB GET_ADMIN_TOKEN). "
            "Pass it explicitly or start the MCP server with --admin-token <token>."
        )
    return OTAClient(HTTPTransport(h, admin_token=t))


def register(mcp) -> None:

    @mcp.tool()
    def ota_get_info(
        host: Optional[str] = None,
        admin_token: Optional[str] = None,
    ) -> dict:
        """
        Inspect the device's OTA partition state without changing anything.

        Returns the running partition (label, address, size, state — one of
        NEW / PENDING_VERIFY / VALID / INVALID / ABORTED / UNDEFINED), the
        next slot that an upload would target, the last partition the
        bootloader marked invalid (if any), and ``can_rollback``: whether
        a previously-valid OTA slot exists to fall back to.

        Parameters:
        - host: full base URL or bare IP (e.g. '192.168.4.1' or
          'http://bugbuster-a1b2c3.local'). Defaults to the session host.
        - admin_token: 64-hex admin token. Defaults to the session token.

        Returns: running, next, last_invalid, can_rollback, fw_version.
        """
        ota = _make_ota(host, admin_token)
        info = ota.get_info()
        return {
            "running": (info.running and {
                "label": info.running.label,
                "address": info.running.address,
                "size": info.running.size,
                "state": info.running.state,
            }),
            "next": (info.next and {
                "label": info.next.label,
                "address": info.next.address,
                "size": info.next.size,
            }),
            "last_invalid": (info.last_invalid and {
                "label": info.last_invalid.label,
            }),
            "can_rollback": info.can_rollback,
            "fw_version": info.fw_version,
        }

    @mcp.tool()
    def ota_upload_firmware(
        path: str,
        sha256: Optional[str] = None,
        host: Optional[str] = None,
        admin_token: Optional[str] = None,
    ) -> dict:
        """
        Upload a firmware image to the device. The device verifies SHA-256
        before switching the boot partition and reboots on success.

        Parameters:
        - path: local path to the firmware .bin (typically
          ``Firmware/ESP32/.pio/build/esp32s3/firmware.bin``).
        - sha256: optional 64-char hex digest. If omitted, computed from the
          file on the host. Pass an explicit value to force a mismatch test.
        - host, admin_token: as in ota_get_info.

        Returns: success, bytes_written, partition, sha256_verified.

        WARNING: On success the device reboots immediately. Subsequent calls
        to other tools will fail until the device is back up (~5–10 s).
        """
        if not os.path.isfile(path):
            raise ValueError(f"Firmware not found: {path}")
        ota = _make_ota(host, admin_token)
        try:
            r = ota.upload_firmware(path, sha256=sha256)
        except OTAError as e:
            raise RuntimeError(f"OTA upload failed: {e}") from e
        return {
            "success": bool(r.get("success", False)),
            "bytes_written": int(r.get("bytesWritten", 0)),
            "partition": r.get("partition", ""),
            "sha256_verified": bool(r.get("sha256Verified", False)),
        }

    @mcp.tool()
    def ota_upload_spiffs(
        path: str,
        host: Optional[str] = None,
        admin_token: Optional[str] = None,
    ) -> dict:
        """
        Upload a SPIFFS filesystem image (the on-device web UI) to the
        ``spiffs`` partition. Erase + write of a 4 MB partition takes ~30 s.

        Parameters:
        - path: local SPIFFS image, typically built via
          ``pio run -e esp32s3 -t buildfs`` then taken from
          ``.pio/build/esp32s3/spiffs.bin``.
        - host, admin_token: as in ota_get_info.

        Returns: success and any extra fields the firmware reports.
        """
        if not os.path.isfile(path):
            raise ValueError(f"SPIFFS image not found: {path}")
        ota = _make_ota(host, admin_token)
        try:
            r = ota.upload_spiffs(path)
        except OTAError as e:
            raise RuntimeError(f"SPIFFS upload failed: {e}") from e
        return r

    @mcp.tool()
    def ota_rollback(
        host: Optional[str] = None,
        admin_token: Optional[str] = None,
    ) -> dict:
        """
        Roll back to the previous OTA slot. The device reboots ~500 ms after
        the response. Fails (HTTP 409) if no rollback target exists — call
        ``ota_get_info`` first and check ``can_rollback``.

        Parameters: host, admin_token (as in ota_get_info).

        Returns: success, message.

        WARNING: This is destructive — the running image will be marked
        INVALID and the device will boot the previously-valid slot.
        """
        ota = _make_ota(host, admin_token)
        try:
            return ota.rollback()
        except OTAError as e:
            raise RuntimeError(f"Rollback failed: {e}") from e
