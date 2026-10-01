"""PLT-06: upload loops retried HTTPD_SOCK_ERR_TIMEOUT forever, so a client
that stalls mid-body held the single httpd task (every other request,
including /api/status, waited) until it closed the socket. Every retry site
must go through the bounded recv_timeout_retry() helper."""

import re

import pytest

from tests.lib.srcread import read_source

WEBSERVER = "Firmware/ESP32/src/web/webserver.cpp"


def _timeout_sites(src: str) -> list[str]:
    return [m.group(0) for m in re.finditer(r"[^\n]*HTTPD_SOCK_ERR_TIMEOUT[^\n]*(?:\n[^\n]*){0,2}", src)]


def test_upload_loops_were_found():
    assert len(_timeout_sites(read_source(WEBSERVER))) >= 7


@pytest.mark.xfail(strict=True, reason="PLT-06")
def test_every_recv_timeout_retry_is_bounded():
    src = read_source(WEBSERVER)
    unbounded = [s.strip().splitlines()[0] for s in _timeout_sites(src)
                 if "continue" in s and "recv_timeout_retry(" not in s]
    assert unbounded == [], f"unbounded retries: {unbounded}"
