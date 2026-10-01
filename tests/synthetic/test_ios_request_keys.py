"""IOS-KEYS (IOS-20, AN-17): iOS sends HTTP bodies the firmware ignores.

Static T1 check (no Swift toolchain on the CI Linux runners or Windows): the
iOS call sites are read as text and compared with what the firmware handler
reads. Each expectation was verified against the handler:
  vout/range  api_core.cpp api_channel_vout_range reads "bipolar"
  adc/config  api_channel_adc_config rejects a body without "mux"
  rtd/config  api_channel_rtd_config reads "current" or "excitation_ua"
  quicksetup  GET /api/quicksetup returns {"slots": [...]}, name under summary
  autorun     handle_post_autorun_enable needs ?name=<file>
"""

import re

from tests.lib.srcread import read_source

OVERVIEW = read_source("iOSApp/Sources/Views/OverviewTab.swift")
SCRIPTS = read_source("iOSApp/Sources/Views/ScriptsTab.swift")


def _json_after(src: str, path_fragment: str) -> str:
    i = src.index(path_fragment)
    m = re.search(r"json:\s*\[([^\]]*)\]", src[i:i + 400])
    assert m, f"no json body after {path_fragment}"
    return m.group(1)


def test_vout_range_sends_bipolar():
    body = _json_after(OVERVIEW, '/vout/range"')
    assert '"bipolar"' in body, body


def test_adc_config_sends_mux():
    body = _json_after(OVERVIEW, '/adc/config"')
    assert '"mux"' in body, body


def test_rtd_config_sends_a_key_the_firmware_reads():
    body = _json_after(OVERVIEW, '/rtd/config"')
    assert '"excitation_ua"' in body or '"current"' in body, body


def test_quicksetup_list_decodes_the_slots_wrapper():
    i = OVERVIEW.index("func loadQuicksetups")
    fn = OVERVIEW[i:i + 600]
    assert "[QuickSetupSlot] = try? await" not in fn, "decodes a bare array"
    assert ".slots" in fn


def test_autorun_enable_passes_name_query():
    i = SCRIPTS.index("func toggleAutorun")
    fn = SCRIPTS[i:i + 900]
    assert "autorun/enable?name=" in fn, fn[:300]


def test_firmware_still_expects_these_keys():
    """Control: re-derive the expectations so the test fails if the firmware
    changes instead of silently passing."""
    api = read_source("Firmware/ESP32/src/net/api_core.cpp")
    web = read_source("Firmware/ESP32/src/web/webserver.cpp")
    assert 'body_get(body, "bipolar")' in api
    assert '"mux, range, rate required"' in api
    assert 'body_get(body, "excitation_ua")' in api
    assert 'httpd_query_key_value(query, "name"' in web
