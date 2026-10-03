"""Spec 2026-10-03 §2: autorun reports source=autorun and takes the file-script
slot; the REPL is read-only (output only) while a file script runs; the
autorun panel can show which script it is and whether it ran this boot."""

from tests.firmware_host.fwhost import extract_function

AUTORUN = "Firmware/ESP32/src/mp/autorun.cpp"
REPL = "Firmware/ESP32/src/mp/repl_ws.cpp"


def test_autorun_submits_as_a_named_autorun_file_script():
    body = extract_function(AUTORUN, r"^static bool run_autorun_script\(")
    assert "scripting_submit(" in body and "scripting_run_string(" not in body
    assert "SCRIPT_SRC_AUTORUN" in body and "opts.is_file = true" in body


def test_autorun_result_uses_this_runs_exit_not_global_error_count():
    body = extract_function(AUTORUN, r"^static bool run_autorun_script\(")
    assert "SCRIPT_EXIT_OK" in body and "total_errors" not in body


def test_boot_check_records_that_autorun_ran():
    body = extract_function(AUTORUN, r"^void autorun_boot_check\(")
    assert body.index("s_ran_this_boot = true") < body.index("run_autorun_script(true)")


def test_status_reports_name_and_boot_flag():
    body = extract_function(AUTORUN, r"^void autorun_get_status\(")
    assert "ran_this_boot" in body and "AUTORUN_NAME_PATH" in body
    assert "AUTORUN_NAME_PATH" in extract_function(AUTORUN, r"^bool autorun_set_enabled\(")


def test_repl_lines_are_repl_sourced_and_refused_while_busy():
    body = extract_function(REPL, r"^static esp_err_t handle_repl_ws\(")
    assert "SCRIPT_SRC_REPL" in body and "SCRIPT_SUBMIT_BUSY" in body
    assert "scripting_run_string(" not in body


def test_repl_ctrl_c_cannot_stop_a_file_script():
    body = extract_function(REPL, r"^static esp_err_t handle_repl_ws\(")
    ctrl_c = body.index("if (c == 0x03)")
    stop = body.index("scripting_stop()", ctrl_c)
    assert "file_script_running()" in body[ctrl_c:stop]
