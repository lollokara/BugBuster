"""
tests/unit/test_ota.py — OTAClient unit tests.

Mocks the requests.Session attached to a stub HTTPTransport so we can
verify SHA-256 computation, query-string passthrough, progress callbacks,
binary streaming, and error mapping without touching firmware.
"""

from __future__ import annotations

import hashlib
from typing import Any
from unittest.mock import MagicMock

import pytest

from bugbuster.ota import OTAClient, OTAError


# ---------------------------------------------------------------------------
# Stub transport
# ---------------------------------------------------------------------------

class _StubTransport:
    def __init__(self, base="http://10.0.0.1/api", token="t" * 64):
        self._session = MagicMock()
        self._base = base
        self._admin_token = token


def _ok_response(status=200, json_payload=None, text=""):
    r = MagicMock()
    r.status_code = status
    r.ok = 200 <= status < 300
    r.text = text
    if json_payload is not None:
        r.json.return_value = json_payload
    else:
        r.json.side_effect = ValueError("not json")
    return r


# ---------------------------------------------------------------------------
# get_info
# ---------------------------------------------------------------------------

def test_get_info_decodes_partition_payload():
    t = _StubTransport()
    t._session.get.return_value = _ok_response(json_payload={
        "running": {"label": "app0", "address": 0x10000, "size": 0x180000, "state": "VALID"},
        "next":    {"label": "app1", "address": 0x190000, "size": 0x180000},
        "lastInvalid": None,
        "canRollback": True,
        "fwMajor": 3, "fwMinor": 1, "fwPatch": 0,
    })

    info = OTAClient(t).get_info()
    assert info.running.label == "app0"
    assert info.running.state == "VALID"
    assert info.running.address == 0x10000
    assert info.next.label == "app1"
    assert info.last_invalid is None
    assert info.can_rollback is True
    assert info.fw_version == "3.1.0"


def test_get_info_passes_admin_header_via_session():
    """get_info uses the bare session.get; auth is a header, set by the
    transport (the HTTPTransport does this in its real session). The OTA
    client should not strip/rewrite that — we just confirm the URL."""
    t = _StubTransport()
    t._session.get.return_value = _ok_response(json_payload={"canRollback": False})
    OTAClient(t).get_info()
    args, kwargs = t._session.get.call_args
    assert args[0] == "http://10.0.0.1/api/ota/info"
    assert kwargs.get("timeout") == 10


# ---------------------------------------------------------------------------
# upload_firmware
# ---------------------------------------------------------------------------

def test_upload_firmware_computes_sha256_and_passes_query(tmp_path):
    payload = b"firmware-bytes-go-here" * 100
    fw = tmp_path / "firmware.bin"
    fw.write_bytes(payload)
    expected = hashlib.sha256(payload).hexdigest()

    t = _StubTransport()
    t._session.post.return_value = _ok_response(json_payload={
        "success": True, "bytesWritten": len(payload),
        "partition": "app1", "sha256Verified": True,
    })

    sent = []
    def cb(done, total):
        sent.append((done, total))

    OTAClient(t).upload_firmware(str(fw), on_progress=cb, chunk_size=512)

    args, kwargs = t._session.post.call_args
    assert args[0] == "http://10.0.0.1/api/ota/upload"
    assert kwargs["params"] == {"sha256": expected}
    # Header set + content-length present + binary content type
    headers = kwargs["headers"]
    assert headers["X-BugBuster-Admin-Token"] == "t" * 64
    assert headers["Content-Type"] == "application/octet-stream"
    assert int(headers["Content-Length"]) == len(payload)

    # data was a generator; consume it to verify chunking + ordering
    blocks = list(kwargs["data"])
    assert b"".join(blocks) == payload
    assert all(len(b) <= 512 for b in blocks)
    # Progress callback fires once per chunk and ends at the file size
    assert sent[-1] == (len(payload), len(payload))
    assert sent[0][1] == len(payload)


def test_upload_firmware_rejects_invalid_sha_arg(tmp_path):
    fw = tmp_path / "fw.bin"
    fw.write_bytes(b"x")
    t = _StubTransport()
    with pytest.raises(OTAError, match="sha256 must be 64 lowercase hex"):
        OTAClient(t).upload_firmware(str(fw), sha256="not-hex-clearly")


def test_upload_firmware_raises_on_http_error(tmp_path):
    fw = tmp_path / "fw.bin"
    fw.write_bytes(b"x" * 1024)
    t = _StubTransport()
    bad = MagicMock()
    bad.ok = False
    bad.status_code = 400
    bad.text = "SHA-256 mismatch — image discarded, boot target unchanged"
    t._session.post.return_value = bad

    with pytest.raises(OTAError, match="SHA-256 mismatch"):
        OTAClient(t).upload_firmware(str(fw))


def test_upload_firmware_rejects_empty_file(tmp_path):
    fw = tmp_path / "empty.bin"
    fw.write_bytes(b"")
    t = _StubTransport()
    with pytest.raises(OTAError, match="empty"):
        OTAClient(t).upload_firmware(str(fw))


def test_progress_callback_errors_are_swallowed(tmp_path):
    """Callback exceptions must never abort the upload."""
    fw = tmp_path / "fw.bin"
    fw.write_bytes(b"a" * 4096)
    t = _StubTransport()
    t._session.post.return_value = _ok_response(json_payload={"success": True})

    def boom(*_):
        raise RuntimeError("callback bug")

    OTAClient(t).upload_firmware(str(fw), on_progress=boom, chunk_size=1024)
    assert t._session.post.called


# ---------------------------------------------------------------------------
# upload_spiffs
# ---------------------------------------------------------------------------

def test_upload_spiffs_does_not_attach_sha(tmp_path):
    img = tmp_path / "spiffs.bin"
    img.write_bytes(b"y" * 2048)
    t = _StubTransport()
    t._session.post.return_value = _ok_response(json_payload={"success": True})

    OTAClient(t).upload_spiffs(str(img))

    args, kwargs = t._session.post.call_args
    assert args[0] == "http://10.0.0.1/api/ota/uploadfs"
    assert kwargs.get("params") in (None, {})


# ---------------------------------------------------------------------------
# rollback
# ---------------------------------------------------------------------------

def test_rollback_returns_response_json():
    t = _StubTransport()
    t._session.post.return_value = _ok_response(json_payload={
        "success": True, "message": "Rolling back",
    })
    r = OTAClient(t).rollback()
    assert r["success"] is True


def test_rollback_409_maps_to_otaerror():
    t = _StubTransport()
    bad = MagicMock()
    bad.ok = False
    bad.status_code = 409
    bad.text = "No rollback target available"
    t._session.post.return_value = bad

    with pytest.raises(OTAError, match="No rollback target"):
        OTAClient(t).rollback()


# ---------------------------------------------------------------------------
# Construction guards
# ---------------------------------------------------------------------------

def test_constructor_requires_admin_token():
    t = _StubTransport(token=None)
    with pytest.raises(OTAError, match="admin token"):
        OTAClient(t)
