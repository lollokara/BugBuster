"""Spec 2026-10-03 section 6: hub settings and status are device routes, so they must be
reachable over the BLE tunnel (api_core_handle) as well as HTTP, and config must be admin-only."""
import re

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source

CORE = "Firmware/ESP32/src/net/api_core.cpp"
WEB = "Firmware/ESP32/src/web/webserver.cpp"
ROUTES = ("/api/hub/status", "/api/hub/config", "/api/hub/resync")
HANDLERS = {
    "handle_get_hub_status": "/api/hub/status",
    "handle_get_hub_config": "/api/hub/config",
    "handle_post_hub_config": "/api/hub/config",
    "handle_post_hub_resync": "/api/hub/resync",
}


def _handler(name):
    return extract_function(WEB, rf"static esp_err_t {name}\(httpd_req_t \*req\)")


def test_routes_are_dispatched_by_api_core_handle():
    core = read_source(CORE)
    assert '#include "api_hub.h"' in core
    for route in ROUTES:
        assert f'strcmp(path, "{route}") == 0' in core, route


def test_http_handlers_delegate_to_api_core_handle():
    for name, route in HANDLERS.items():
        body = _handler(name)
        assert "api_core_handle(" in body and f'"{route}"' in body, name


def test_config_and_resync_need_the_admin_token_but_status_does_not():
    for name in ("handle_get_hub_config", "handle_post_hub_config", "handle_post_hub_resync"):
        assert "check_admin_auth(req) != ESP_OK" in _handler(name), name
    assert "check_admin_auth" not in _handler("handle_get_hub_status")


def test_every_handler_is_registered_with_the_right_method():
    web = read_source(WEB)
    for name, route in HANDLERS.items():
        method = "HTTP_GET" if "_get_" in name else "HTTP_POST"
        assert re.search(rf'\.uri = "{re.escape(route)}", \.method = {method}, \.handler = {name}\b', web), name


def test_uri_budget_keeps_headroom():
    web = read_source(WEB)
    registered = len(re.findall(r"httpd_register_uri_handler\(", web))
    limit = int(re.search(r"max_uri_handlers\s*=\s*(\d+)", web).group(1))
    assert registered <= limit - 5, f"{registered} registrations against max_uri_handlers={limit}: raise it"
