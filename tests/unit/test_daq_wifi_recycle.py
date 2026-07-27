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
    assert "static volatile uint32_t s_bringup_gen;" in BOARD
    # RECYCLE must bump the generation and wait on s_bringup_alive with a
    # bounded loop (never an unbounded block).
    start = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_RECYCLE")
    end = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_INFO")
    body = BOARD[start:end]
    assert "++s_bringup_gen" in body or "s_bringup_gen++" in body or "s_bringup_gen +" in body
    assert "BRINGUP_CANCEL_WAIT_MS" in body, "wait must be bounded, not unbounded"
    assert "while" in body and "s_bringup_alive" in body

    # The bring-up task must check its generation before it ever publishes
    # READY, and must clear s_bringup_alive on that path too.
    task_start = BOARD.index("static void wifi_stream_bringup_task")
    ready_idx = BOARD.index("DAQ_WIFI_STREAM_READY", task_start)
    task_body_before_ready = BOARD[task_start:ready_idx]
    assert "s_bringup_gen" in task_body_before_ready, \
        "bring-up task never checks its generation before publishing READY"


def test_recycle_orphan_cannot_be_un_cancelled_by_a_later_start():
    """The generation counter, not a boolean flag, is what makes cancellation
    stick. A prior fix used a shared s_bringup_cancel boolean: RECYCLE set it
    and waited up to BRINGUP_CANCEL_WAIT_MS, but if the old task was stuck
    inside wifi_ap_start() past that bound, RECYCLE gave up and force-cleared
    s_bringup_alive so the very next START (the iOS recovery ladder recycles
    then immediately re-provisions) wasn't blocked -- but that new START also
    reset the shared boolean to false, un-cancelling the orphan right before
    its next checkpoint. A per-attempt generation has no such ambiguity: once
    bumped (by either RECYCLE or a new START), an orphan's captured value can
    never match s_bringup_gen again, no matter what happens afterward."""
    # There must be no shared/global boolean cancel flag left for a new START
    # to reset out from under an orphan -- generation identity replaces it.
    assert "s_bringup_cancel" not in BOARD, \
        "a shared boolean cancel flag can be un-set by an unrelated START, " \
        "re-enabling an orphaned bring-up task -- must use per-attempt generation identity instead"

    # Each bring-up task instance must capture its OWN generation (passed in
    # via its task argument, not read from the shared counter at entry -- a
    # second START could already have bumped the shared counter before the
    # first task's first instruction runs) and never re-derive it from the
    # global afterward.
    task_start = BOARD.index("static void wifi_stream_bringup_task")
    task_end = BOARD.index("static void wifi_stream_teardown")
    task_body = BOARD[task_start:task_end]
    assert "my_gen" in task_body or "gen" in task_body.split("(")[1].split(")")[0], \
        "task must capture its own generation, not just poll the shared counter"
    assert "my_gen != s_bringup_gen" in task_body or "s_bringup_gen != my_gen" in task_body, \
        "checkpoints must compare captured generation against the CURRENT shared counter"

    # START must bump the generation on every spawn (giving each attempt an
    # identity distinct from whatever came before, including an orphan RECYCLE
    # gave up waiting on).
    start_start = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_START")
    start_end = BOARD.index("case HATP_CMD_DAQ_WIFI_STREAM_STOP")
    start_body = BOARD[start_start:start_end]
    assert "++s_bringup_gen" in start_body or "s_bringup_gen++" in start_body, \
        "START must bump the generation so a prior orphan can never match it again"

    # The task must be constructed with its own copy of that generation value
    # (not the raw daq_board_t* alone), e.g. via a small per-spawn arg struct.
    assert "xTaskCreate(wifi_stream_bringup_task" in start_body
    assert "gen" in start_body
