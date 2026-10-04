"""Spec 2026-10-03 §2 guards over scripting.cpp (MicroPython/FreeRTOS: not
host-compilable, so these read the firmware text; the pure logic they call is
executed in test_script_runtime.py).

- The log ring only ever receives structured `<ts> <L> <src> <text>` lines.
- Admission and the file-slot claim are one critical section (two racing
  run-file calls cannot both start).
- replace=1 never deletes taskMicroPython: the holder can own s_hat_mutex or a
  bus lock, and a killed holder deadlocks the board. It waits 3 s, then queues
  a VM reset and enqueues the new script behind the holder."""

import re

from tests.firmware_host.fwhost import extract_defines, extract_function
from tests.lib.srcread import read_source

SCRIPTING = "Firmware/ESP32/src/mp/scripting.cpp"
HEADER = "Firmware/ESP32/src/mp/scripting.h"


def test_ring_receives_structured_lines():
    body = extract_function(SCRIPTING, r"^void scripting_log_push\(")
    assert "sr_line_feed(" in body
    assert "log_push_locked(str" not in body, "raw bytes would bypass the <ts> <L> <src> prefix"


def test_admission_and_claim_share_one_critical_section():
    body = extract_function(SCRIPTING, r"^ScriptSubmitResult scripting_submit\(")
    take = body.index("xSemaphoreTake(s_status_mutex")
    admit = body.index("sr_admit(")
    claim = body.index("claim_slot_locked(")
    give = body.index("xSemaphoreGive(s_status_mutex)")
    assert take < admit < claim < give


def test_legacy_entry_points_go_through_submit():
    assert "scripting_submit(" in extract_function(SCRIPTING, r"^bool scripting_run_string\(")
    assert "scripting_submit_file(" in extract_function(SCRIPTING, r"^bool scripting_run_file\(")


def test_replace_never_deletes_the_vm_task():
    assert "vTaskDelete" not in read_source(SCRIPTING)


def test_replace_waits_then_queues_a_reset():
    body = extract_function(SCRIPTING, r"^ScriptSubmitResult scripting_submit\(")
    wait = body.index("scripting_wait_slot_free(MP_REPLACE_STOP_TIMEOUT_MS)")
    assert body.index("scripting_stop()") < wait < body.index("scripting_reset_vm()")


def test_replace_timeout_is_three_seconds():
    defs = extract_defines("Firmware/ESP32/src/config.h", ["MP_REPLACE_STOP_TIMEOUT_MS"])
    assert re.search(r"MP_REPLACE_STOP_TIMEOUT_MS\s+3000u", defs)


def test_stop_is_classified_not_counted_as_error():
    body = extract_function(SCRIPTING, r"^static void taskMicroPython\(")
    assert "mp_type_KeyboardInterrupt" in body
    assert "SCRIPT_EXIT_STOPPED" in body


def test_partial_line_flushed_before_done():
    body = extract_function(SCRIPTING, r"^static void taskMicroPython\(")
    assert body.index("log_flush()") < body.index("status_set_done(")


def test_slot_released_only_by_its_own_completion():
    body = extract_function(SCRIPTING, r"^static void status_set_done\(")
    assert "file_slot_id == cmd->id" in body


def test_status_carries_spec_fields():
    src = read_source(HEADER)
    for field in ("char         name[SCRIPT_NAME_MAX + 1];", "ScriptSource source;",
                  "ScriptState  state;", "ScriptExit   last_exit;", "uint32_t     started_at;",
                  "uint32_t     file_slot_id;"):
        assert field in src, field
