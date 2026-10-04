"""Guards for the BLE script-tunnel review findings (PR #2, Codex):

* every script route the iOS Scripts tab calls must exist in the shared
  api_core dispatcher (``/api/scripts/storage`` used to be HTTP-only, so over BLE
  the whole file list failed to load);
* ``ConnectionManager.rawRequest`` must keep the DELETE verb (BLE has its own
  ``files/delete`` spelling) and decode the base64 ``files/get`` page, following
  ``off`` until ``size`` so a long script is not truncated.
"""
import re

from tests.lib.srcread import read_source

CORE = "Firmware/ESP32/src/net/api_core.cpp"
IOS_TAB = "iOSApp/Sources/Views/ScriptsTab.swift"
IOS_CM = "iOSApp/Sources/Services/ConnectionManager.swift"


def _dispatched() -> set[str]:
    return set(re.findall(r'strcmp\(sfx, "([^"]+)"\) == 0', read_source(CORE)))


def test_every_ios_script_route_is_in_the_shared_dispatcher():
    routes = set(re.findall(r'"/api/scripts/([a-z/\-]+)"', read_source(IOS_TAB)))
    routes.discard("")
    assert "storage" in routes, "iOS Scripts tab no longer reads storage - update this guard"
    # the shipped iOS client posts lint as raw text over HTTP; the BLE JSON form is
    # pinned by test_lint_is_dispatched_over_ble_with_name_or_src below.
    routes -= {"lint"}
    missing = {r for r in routes if r not in _dispatched()}
    assert not missing, f"BLE dispatcher has no case for: {sorted(missing)}"


def test_storage_is_served_by_the_shared_implementation():
    fw = read_source("Firmware/ESP32/src/net/api_scripts.cpp")
    assert "char *api_scripts_storage(" in fw
    assert 'strcmp(sfx, "storage") == 0' in read_source(CORE)
    web = read_source("Firmware/ESP32/src/web/webserver.cpp")
    body = web.split("static esp_err_t handle_get_scripts_storage", 1)[1].split("\n}\n", 1)[0]
    assert 'api_core_handle("GET", "/api/scripts/storage"' in body, "HTTP must delegate, not duplicate"


def test_ios_ble_raw_request_keeps_delete_and_decodes_file_pages():
    cm = read_source(IOS_CM).replace("\r\n", "\n")
    ble = cm.split("public func rawRequest(method: String", 1)[1].split("let trimmed", 1)[0]
    assert 'method == "DELETE" && path == "/api/scripts/files"' in ble
    assert '"/api/scripts/files/delete"' in ble
    assert 'path == "/api/scripts/files/get"' in ble and "bleScriptFileData" in ble
    pager = cm.split("func bleScriptFileData", 1)[1].split("/// Raw request", 1)[0]
    assert 'q["off"]' in pager and 'obj["size"]' in pager and "Data(base64Encoded:" in pager


def test_lint_is_dispatched_over_ble_with_name_or_src():
    assert "lint" in _dispatched()
    assert "return api_scripts_lint(path, body)" in read_source(CORE)
    fw = read_source("Firmware/ESP32/src/net/api_scripts.cpp")
    body = fw.split("char *api_scripts_lint(", 1)[1].split("\n}\n", 1)[0]
    assert '"name"' in body and '"src"' in body
    assert "script_storage_validate_name" in fw
    assert "scripting_lint_string(" in body
    assert body.count("heap_caps_malloc") == 1 and body.count("heap_caps_free") == 2  # free on error + success
    assert "api_scripts_lint" in read_source("Firmware/ESP32/src/net/api_scripts.h")
