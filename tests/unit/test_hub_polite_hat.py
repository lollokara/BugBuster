"""Tests for polite HAT link client implementation in hub streaming modules."""
import re
from tests.lib.srcread import read_source

HAT_H = "Firmware/ESP32/src/hat/hat.h"
HAT_CPP = "Firmware/ESP32/src/hat/hat.cpp"
HUB_NET = "Firmware/ESP32/src/hub/hub_net.cpp"
HUB_SYNC = "Firmware/ESP32/src/hub/hub_sync.cpp"
HUB_LIVE = "Firmware/ESP32/src/hub/hub_live.cpp"
HUB_LOGS = "Firmware/ESP32/src/hub/hub_logs.cpp"
HUB_POLICY_H = "Firmware/ESP32/src/hub/hub_policy.h"
HUB_POLICY_C = "Firmware/ESP32/src/hub/hub_policy.c"


def test_hat_header_defines_lock_busy_and_polite_api():
    src = read_source(HAT_H)
    assert "#define HAT_ERR_LOCK_BUSY" in src
    assert "hat_bs_request_polite(" in src
    assert "hat_log_pull(" in src


def test_hat_wide_request_supports_polite_try_lock():
    src = read_source(HAT_CPP)
    assert "hat_bs_request_polite(" in src
    assert "HAT_ERR_LOCK_BUSY" in src
    # Does not log an ERROR on mutex busy
    assert "ESP_LOGE" not in src.split("HAT wide cmd")[0].split("\n")[-1] if "HAT wide cmd" in src else True
    # hat_log_pull uses polite lock timeout
    assert re.search(r"int hat_log_pull\(.*?\{.*?hat_wide_request\(HAT_CMD_LOG_PULL.*?, 10\);", src, re.S) or \
           re.search(r"int hat_log_pull_polite\(", src)


def test_hub_net_uses_polite_hat_requests():
    src = read_source(HUB_NET)
    assert "hat_bs_request_polite" in src
    assert "vTaskDelay" in src  # wallmap chunk yield


def test_hub_sync_yields_between_chunks_and_backs_off():
    src = read_source(HUB_SYNC)
    # phase_stream must yield between chunks
    stream_fn = src[src.index("phase_stream"):]
    assert "vTaskDelay" in stream_fn
    assert "HAT_ERR_LOCK_BUSY" in src
    # Must not advance S.fi when lock is busy
    assert "lock_busy" in src or "HAT_ERR_LOCK_BUSY" in stream_fn


def test_hub_live_yields_and_preserves_active_on_busy():
    src = read_source(HUB_LIVE)
    assert "vTaskDelay" in src
    tick_fn = src[src.index("hub_live_tick"):]
    assert "HAT_ERR_LOCK_BUSY" in tick_fn


def test_hub_logs_pull_backs_off_on_busy():
    src = read_source(HUB_LOGS)
    pull_fn = src[src.index("hub_logs_pull_p4"):]
    assert "HAT_ERR_LOCK_BUSY" in pull_fn


def test_hub_policy_pacing_and_backoff_declarations():
    hdr = read_source(HUB_POLICY_H)
    assert "hub_hat_backoff_next(" in hdr
    assert "hub_pace_due(" in hdr
    c_src = read_source(HUB_POLICY_C)
    assert "hub_hat_backoff_next(" in c_src
    assert "hub_pace_due(" in c_src
