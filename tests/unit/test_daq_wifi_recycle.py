"""DAQ WiFi stream state machine: no terminal states, no double bring-up."""
from pathlib import Path

BOARD = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c").read_text()
BOARD_H = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.h").read_text()


def test_failed_state_decays_back_to_idle():
    """A stuck FAILED reports a permanent failure to the phone forever."""
    assert "WIFI_STREAM_FAILED_DECAY_MS" in BOARD
    assert "failed_at_ms" in BOARD_H
    assert "DAQ_WIFI_STREAM_IDLE" in BOARD[BOARD.index("WIFI_STREAM_FAILED_DECAY_MS"):]


def test_bringup_task_has_an_explicit_liveness_guard():
    """xTaskCreate can succeed before the task runs; don't rely on the
    caller having set STARTING first to prevent a second spawn."""
    assert "s_bringup_alive" in BOARD
    start = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_START")
    end = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_STOP")
    assert "s_bringup_alive" in BOARD[start:end], "START does not check liveness"


def test_bringup_task_clears_liveness_on_every_exit_path():
    body = BOARD[BOARD.index("static void wifi_stream_bringup_task"):
                 BOARD.index("static void wifi_stream_teardown")]
    # Both the failure return and the success fallthrough must clear it.
    assert body.count("s_bringup_alive = false") >= 2


S3LINK = Path("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h").read_text()
HAT_H = Path("Firmware/ESP32/src/hat/hat.h").read_text()
HAT_CPP = Path("Firmware/ESP32/src/hat/hat.cpp").read_text()
API = Path("Firmware/ESP32/src/net/api_core.cpp").read_text()
WEB = Path("Firmware/ESP32/src/web/webserver.cpp").read_text()


def test_recycle_command_byte_matches_on_both_sides_of_the_hat_link():
    import re
    assert re.search(r"HATP_CMD_DAQ_WIFI_STREAM_RECYCLE\s+0x79u", S3LINK)
    assert "HAT_CMD_DAQ_WIFI_STREAM_RECYCLE  0x79" in HAT_CPP + HAT_H or \
           "HAT_CMD_DAQ_WIFI_STREAM_RECYCLE 0x79" in HAT_H
    # The two constants must encode the same byte or the link silently breaks.
    p4 = re.search(r"HATP_CMD_DAQ_WIFI_STREAM_RECYCLE\s+0x([0-9A-Fa-f]{2})", S3LINK).group(1)
    s3 = re.search(r"HAT_CMD_DAQ_WIFI_STREAM_RECYCLE\s+0x([0-9A-Fa-f]{2})", HAT_H).group(1)
    assert p4.lower() == s3.lower(), f"P4 0x{p4} != S3 0x{s3}"


def test_recycle_is_unconditional_unlike_cooperative_stop():
    """Recycle must not be gated on current state -- that is the whole point."""
    start = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_RECYCLE")
    end = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_INFO")
    body = BOARD[start:end]
    assert "wifi_stream_teardown" in body
    assert "s_bringup_alive = false" in body, "must clear a stuck bring-up flag"


def test_recycle_reachable_over_ble_and_http():
    assert "/api/daq/wifi_stream/recycle" in API, "not in api_core dispatch (BLE+HTTP)"
    assert "/api/daq/wifi_stream/recycle" in WEB, "no HTTP route registered"
    assert "hat_daq_wifi_stream_recycle" in HAT_H and "hat_daq_wifi_stream_recycle" in HAT_CPP


def test_recycle_cancels_inflight_bringup_with_a_bounded_wait():
    """A bring-up task can be mid-flight (AP retry loop, or about to publish
    READY) when RECYCLE lands. RECYCLE must ask it to cancel and wait --
    bounded, never unbounded -- rather than blindly clearing s_bringup_alive,
    or the task can resurrect the softAP / stamp READY right after RECYCLE's
    teardown, silently undoing the recycle."""
    assert "static volatile bool s_bringup_cancel;" in BOARD
    # RECYCLE must set the cancel flag and wait on it with a bounded loop
    # (never an unbounded block on s_bringup_alive).
    start = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_RECYCLE")
    end = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_INFO")
    body = BOARD[start:end]
    assert "s_bringup_cancel = true" in body
    assert "BRINGUP_CANCEL_WAIT_MS" in body, "wait must be bounded, not unbounded"
    assert "while" in body and "s_bringup_alive" in body

    # The bring-up task must check the cancel flag before it ever publishes
    # READY, and must clear s_bringup_alive/s_bringup_cancel on that path too.
    task_start = BOARD.index("static void wifi_stream_bringup_task")
    ready_idx = BOARD.index("DAQ_WIFI_STREAM_READY", task_start)
    task_body_before_ready = BOARD[task_start:ready_idx]
    assert "s_bringup_cancel" in task_body_before_ready, \
        "bring-up task never checks cancel before publishing READY"

    # A fresh START must not let a stale cancel from a previous recycle leak
    # into the new attempt.
    start_start = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_START")
    start_end = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_STOP")
    assert "s_bringup_cancel = false" in BOARD[start_start:start_end]
