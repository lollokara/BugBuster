import re

from tests.lib.srcread import read_source

SRC = "Firmware/ESP32/src/hub/hub_logs.cpp"


def test_ring_and_batch_limits_match_the_spec():
    src = read_source(SRC)
    assert "#define RING_BYTES      (64u * 1024u)" in src
    assert "#define SHIP_BYTES      4096u" in src and "#define SHIP_PERIOD_MS  5000u" in src
    assert '"/api/v1/ingest/logs"' in src


def test_producers_never_block():
    src = read_source(SRC)
    assert "xSemaphoreTake(s_lock, 0)" in src
    hook = src[src.index("static int hub_vprintf"):src.index("static void mpy_tee")]
    assert "portMAX_DELAY" not in hook


def test_only_s3_logs_obey_the_level_threshold():
    assert "src == HUB_LOGSRC_S3 && !hub_level_enabled(level, s_level)" in read_source(SRC)


def test_a_4xx_batch_is_dropped_but_a_5xx_batch_is_kept():
    src = read_source(SRC)
    assert re.search(r"if \(res == HUB_RES_RETRY\) \{.*?return HUB_STEP_RETRY;", src, re.S)
    assert "hub_ring_drop(&s_ring, n > lost ? n - lost : 0)" in src    # overwritten entries are not double counted


def test_script_tee_hook_is_declared_and_called():
    assert "void scripting_set_log_tee(" in read_source("Firmware/ESP32/src/mp/scripting.h")
    cpp = read_source("Firmware/ESP32/src/mp/scripting.cpp")
    assert re.search(r"static void ring_emit\(.*?\)\s*\{.*?s_log_tee\(line, len\);", cpp, re.S)
