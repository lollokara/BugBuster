"""Spec 2026-10-03 §2 behaviour guards for the scripts control plane
(net/api_scripts.cpp, dispatched by api_core_handle; per-route transport
parity lives in test_route_parity.py): a busy slot becomes HTTP 409 naming the
holder, eval is refused while a file script runs, log pages are capped and in
PSRAM, and a BLE chunk request fits the tunnel's request buffer."""

import re

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source

API_SCRIPTS = "Firmware/ESP32/src/net/api_scripts.cpp"
WEBSERVER = "Firmware/ESP32/src/web/webserver.cpp"
BLE = "Firmware/ESP32/src/net/ble_service.cpp"


def test_busy_reply_maps_to_409():
    body = extract_function(WEBSERVER, r"^static esp_err_t send_scripts_result\(")
    assert 'cJSON_IsString(cJSON_GetObjectItem(parsed, "running"))' in body and "409" in body


def test_run_file_and_eval_reply_name_the_holder():
    holder = extract_function(API_SCRIPTS, r"^static char \*busy_json\(")
    assert '"running"' in holder and "file_slot_name" in holder and "file_slot_id" in holder
    for fn in ("api_scripts_run_file", "api_scripts_eval"):
        body = extract_function(API_SCRIPTS, rf"^char \*{fn}\(")
        assert "SCRIPT_SUBMIT_BUSY" in body and "busy_json(" in body, fn


def test_eval_takes_no_file_slot():
    body = extract_function(API_SCRIPTS, r"^char \*api_scripts_eval\(")
    assert "is_file" not in body, "an eval must never hold the file-script slot"


def test_logs_page_is_capped_and_in_psram():
    body = extract_function(API_SCRIPTS, r"^char \*api_scripts_logs\(")
    assert "MP_LOG_RESP_MAX" in body and "MALLOC_CAP_SPIRAM" in body
    assert "scripting_get_logs_since(" in body and "scripting_get_logs(" in body


def test_legacy_http_reframing_keeps_the_log_cursor_header():
    body = extract_function(WEBSERVER, r"^static esp_err_t send_scripts_b64_as\(")
    assert "X-BugBuster-Log-Next" in body and "mbedtls_base64_decode" in body


def test_ble_request_buffer_fits_a_chunk_upload():
    m = re.search(r"static char s_api_req_buf\[(\d+)\];", read_source(BLE))
    assert m and int(m.group(1)) >= 512
