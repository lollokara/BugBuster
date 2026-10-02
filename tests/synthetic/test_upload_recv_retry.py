"""PLT-06: upload loops retried HTTPD_SOCK_ERR_TIMEOUT forever, so a client
that stalls mid-body held the single httpd task (every other request,
including /api/status, waited) until it closed the socket. Uploads now read
through upload_recv(), which gives up after UPLOAD_RECV_MAX_TIMEOUTS
consecutive timeouts."""

import re

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source

WEBSERVER = "Firmware/ESP32/src/web/webserver.cpp"
# recv_json_body() treats any recv error, including a timeout, as fatal.
DIRECT_RECV_ALLOWED = {"recv_json_body"}


def test_upload_recv_is_bounded():
    body = extract_function(WEBSERVER, r"^static int upload_recv\(")
    assert "recv_timeout_retry(&timeouts)" in body
    assert re.search(r"#define UPLOAD_RECV_MAX_TIMEOUTS\s+\d+", read_source(WEBSERVER))


def test_no_unbounded_timeout_retry():
    src = read_source(WEBSERVER)
    helper = extract_function(WEBSERVER, r"^static int upload_recv\(")
    outside = src.replace(helper, "")
    assert "HTTPD_SOCK_ERR_TIMEOUT" not in outside


def test_uploads_use_upload_recv():
    src = read_source(WEBSERVER)
    direct = []
    for m in re.finditer(r"httpd_req_recv\(", src):
        head = src.rfind("\n{", 0, m.start())
        sig = src[src.rfind("\n", 0, head) + 1:head]
        name = re.search(r"(\w+)\s*\(", sig)
        if name and name.group(1) not in DIRECT_RECV_ALLOWED | {"upload_recv"}:
            direct.append(name.group(1))
    assert direct == [], f"bypass upload_recv: {direct}"
    assert src.count("upload_recv(") >= 8
