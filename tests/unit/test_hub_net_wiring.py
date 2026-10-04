"""Spec 2026-10-03 section 6, Discovery: browse _espfleet._tcp, register with identity = MAC."""
from tests.lib.srcread import read_source

SRC = "Firmware/ESP32/src/hub/hub_net.cpp"


def test_discovery_ignores_other_fleet_nodes():
    src = read_source(SRC)
    assert 'mdns_query_ptr("_espfleet", "_tcp"' in src
    assert 'strcmp(r->txt[k].key, "api") == 0' in src and 'strcmp(r->txt[k].key, "id") == 0' in src
    assert "if (!has_api || has_id) continue;" in src


def test_register_uses_the_existing_agent_heartbeat_with_the_sta_mac():
    src = read_source(SRC)
    assert '"/api/v1/agent/heartbeat"' in src
    assert "esp_read_mac(mac, ESP_MAC_WIFI_STA)" in src            # same id as /api/pairing/info
    assert '"/api/v1/agent/result"' in src                          # hub commands are refused, not left delivered


def test_http_is_plain_and_bounded():
    src = read_source(SRC)
    assert "cfg.timeout_ms = 4000;" in src and "https" not in src
    assert "heap_caps_malloc(HUB_BODY_CAP, MALLOC_CAP_SPIRAM)" in src
