"""WEB-23: GET /api/scope/stream ran its `for (;;)` SSE loop inside the httpd
handler. ESP-IDF httpd serves every socket from one task, so while a scope tab
was open no other request was answered (board: /api/status 0/10, all 3 s
timeouts).

B: the handler detaches the request (httpd_req_async_handler_begin) and runs
the loop on its own task; the httpd task is free immediately.
"""

import re

import pytest

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import REPO_ROOT

WEB = REPO_ROOT / "Firmware/ESP32/src/web/webserver.cpp"


@pytest.mark.xfail(strict=True, reason="WEB-23")
def test_scope_sse_handler_detaches_from_the_httpd_task():
    body = re.sub(r"//[^\n]*", "", extract_function(WEB, r"static esp_err_t handle_get_scope_stream\("))
    assert "httpd_req_async_handler_begin" in body
    assert "for (;;)" not in body
