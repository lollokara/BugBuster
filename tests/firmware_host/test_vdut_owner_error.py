"""S3 maps 'a battery-sim run owns VDUT' to a specific 409 instead of the generic 400.
Reads firmware text (api_core.cpp / hat.cpp / webserver.cpp are not host-compiled)."""

import re

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source

API = "Firmware/ESP32/src/net/api_core.cpp"
HAT = "Firmware/ESP32/src/hat/hat.cpp"
WEB = "Firmware/ESP32/src/web/webserver.cpp"
P4 = "Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c"


def test_owner_probe_only_runs_after_failure():
    for fn in ("api_daq_vdut_setpoint", "api_daq_vdut_enable"):
        body = extract_function(API, rf"^static char \*{fn}\(")
        fail = body.index("if (!hat_daq_vdut_")
        assert body.index("vdut_owned_by_run_error()") > fail, fn
        assert body.index("HAT not responding") > body.index("vdut_owned_by_run_error()"), fn


def test_error_body_has_text_and_run_id():
    body = extract_function(API, r"^static char \*vdut_owned_by_run_error\(")
    assert "battery simulator run %d is loaded and owns VDUT; unload it first" in body
    assert '"runId"' in body and '"error"' in body


def test_owner_probe_decodes_battsim_status_per_python_layout():
    body = extract_function(HAT, r"^int hat_daq_vdut_owner_run\(")
    # STATUS op and layout (state @1, run_id u16 @4) derived from the host decoder.
    py = read_source("python/bugbuster/battsim.py")
    assert re.search(r"STATUS = 0\b", py) and 'FMT = "<BBBBH' in py
    assert "rsp[1] == 0" in body and "rsp[4] | (rsp[5] << 8)" in body


def test_p4_refusal_is_the_condition_being_probed():
    assert "battsim_owns_supply()" in read_source(P4)


def test_http_maps_it_to_409():
    src = read_source(WEB)
    start = src.index("static esp_err_t send_api_core_result(httpd_req_t *req, char *resp, const char *fail_msg)\n{")
    body = src[start:src.index("\n}\n", start)]
    assert '"runId"' in body and "409" in body
