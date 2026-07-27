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
