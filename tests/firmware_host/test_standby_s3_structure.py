"""Source-structure guards for the S3 standby coordinator glue.

These do not compile the ESP-IDF glue (that needs PlatformIO); they pin the
properties that matter and are cheap to regress: the wire mirror, the safety
rules in standby_hw.cpp, and that every entry point still goes through the
operation barrier.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
S3 = ROOT / "Firmware" / "ESP32" / "src"
DAQ = ROOT / "Firmware" / "DAQ_HAT" / "common"


def _read(path: Path) -> str:
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def _code(text: str) -> str:
    """Strip comments so rules about forbidden calls apply to code only."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_wire_header_is_a_forwarder_to_the_daq_common_copy():
    fwd = _code(_read(S3 / "power" / "standby_wire.h"))
    includes = re.findall(r'#\s*include\s+"([^"]+)"', fwd)
    assert includes == ["../../../DAQ_HAT/common/standby_wire.h"]
    assert ((S3 / "power") / includes[0]).resolve() == (DAQ / "standby_wire.h").resolve()
    # no second copy of the contract
    assert "bb_standby_request_t" not in fwd and "BB_ST_" not in fwd


def test_hw_glue_never_touches_logic_or_raw_ports():
    code = _code(_read(S3 / "power" / "standby_hw.cpp"))
    assert "PCA_CTRL_MUX_EN" not in code      # that bit is LOGIC_EN
    assert "pca9535_set_port" not in code     # raw port writes can drop USB
    assert "pca9535_write" not in code


def test_hw_glue_step_order():
    code = _code(_read(S3 / "power" / "standby_hw.cpp"))
    dispatch = code[code.index("hw_local_step(void"):]
    order = ["step_quiesce", "step_mux_off", "step_outputs_off", "step_analog_off",
             "step_indicators_off", "step_wake_safe", "step_analog_on",
             "step_reinitialize", "step_indicators_on"]
    positions = [dispatch.find(name) for name in order]
    assert all(p >= 0 for p in positions), dict(zip(order, positions, strict=True))
    # sleep chain is ordered, wake chain is ordered
    assert positions[:5] == sorted(positions[:5])
    assert positions[5:] == sorted(positions[5:])


def test_bus_gate_held_only_while_analog_is_off():
    code = _code(_read(S3 / "power" / "standby_hw.cpp"))
    take = code.index("xSemaphoreTakeRecursive(g_spi_bus_mutex")
    give = code.index("xSemaphoreGiveRecursive(g_spi_bus_mutex")
    assert code.index("step_analog_off") < take < give
    assert code.index("step_reinitialize", take - 200) < give


def test_entry_points_use_the_operation_barrier():
    for rel in ("bbp/bbp_adapter.cpp", "net/api_core.cpp", "cli/cli.cpp",
                "net/ble_service.cpp", "hat/hat.cpp", "web/webserver.cpp"):
        text = _code(_read(S3 / rel))
        assert re.search(r"StandbyWork|standby_hw_work|standby_hw_", text), rel


def test_cmd_registered():
    reg = _read(S3 / "bbp" / "cmd_registry.cpp")
    assert "cmd_standby" in reg or "standby" in reg.lower()


def test_usb_epoch_hooks_sit_at_the_protocol_owner_boundary():
    bbp = _code(_read(S3 / "bbp" / "bbp.cpp"))
    handshake = bbp[bbp.index("bool bbpDetectHandshake"):bbp.index("void bbpExitBinaryMode")]
    exit_fn = bbp[bbp.index("void bbpExitBinaryMode"):bbp.index("void bbpRestoreCdcCli")]
    assert "standby_hw_usb_session(true)" in handshake and "s_active = true" in handshake
    assert "standby_hw_usb_session(false)" in exit_fn and "s_active = false" in exit_fn
    # never derived from a cable, DTR or an open port
    hw = _code(_read(S3 / "power" / "standby_hw.cpp"))
    inh = hw[hw.index("static uint32_t hw_inhibitors"):hw.index("static StandbyHatLink hw_hat_link")]
    assert "standby_system_usb_legacy_host(&s_sys)" in inh
    assert "bbpIsActive" not in inh and "DTR" not in inh and "cdc" not in inh.lower()


def test_only_the_usb_adapter_marks_presence_as_usb():
    adapter = _code(_read(S3 / "bbp" / "bbp_adapter.cpp"))
    assert "standby_hw_bbp(STANDBY_SRC_USB" in adapter
    cmd = _code(_read(S3 / "bbp" / "cmds" / "cmd_standby.cpp"))
    assert "STANDBY_SRC_OTHER" in cmd and "STANDBY_SRC_USB" not in cmd
    api = _code(_read(S3 / "net" / "api_standby.cpp"))
    assert "STANDBY_SRC_USB" not in api


def test_policy_persistence_is_real_and_serialised():
    hw = _code(_read(S3 / "power" / "standby_hw.cpp"))
    persist = hw[hw.index("static bool hw_persist"):hw.index("static uint32_t load_timeout")]
    assert "nvs_commit(h)" in persist and "return err == ESP_OK" in persist
    assert "nvs_erase_key" in persist and "prev" in persist            # failure restores the previous value
    for token in ("ops.write_lock = hw_write_lock", "ops.write_unlock = hw_write_unlock",
                  "ops.safe_outputs_off = hw_safe_outputs_off"):
        assert token in hw
    api = _code(_read(S3 / "net" / "api_standby.cpp"))
    assert "STANDBY_RC_HARDWARE" in api and '"persisted\\":false' in api.replace("\\\\", "\\")


def test_shutdown_and_wake_verify_by_readback():
    hw = _code(_read(S3 / "power" / "standby_hw.cpp"))
    # high impedance is confirmed from the converter, not assumed from a void apply call
    assert "tasks_read_channel_function" in hw
    outputs = hw[hw.index("static StandbyStepResult step_outputs_off"):hw.index("static StandbyStepResult step_analog_off")]
    assert "return STANDBY_STEP_FAILED" not in outputs                   # every target is attempted
    # the analog supply is never cut under an unverified switch matrix
    analog = hw[hw.index("static StandbyStepResult step_analog_off"):hw.index("static StandbyStepResult step_indicators_off")]
    assert analog.index("route_open_verified()") < analog.index("PCA_CTRL_15V_EN")
    # wake: every configuration step reports back and is read back before ready
    reinit = hw[hw.index("static StandbyStepResult step_reinitialize"):hw.index("static StandbyStepResult step_indicators_on")]
    for token in ("setupDiagnostics()", "startAdcConversion(", "verifyAdcConversion(", "route_open_verified()"):
        assert token in reinit
    # genuine fault latches / history are not wiped by a re-initialisation
    for token in ("alertStatus = 0", "supplyAlertStatus = 0", "channelAlertStatus = 0"):
        assert token not in reinit


def test_fault_safe_cleanup_attempts_everything():
    hw = _code(_read(S3 / "power" / "standby_hw.cpp"))
    safe = hw[hw.index("static bool hw_safe_outputs_off"):hw.index("void standby_runtime_init")]
    assert "return false" not in safe and safe.count("ok = ") >= 5       # accumulates, never early-returns
    assert "PCA_CTRL_15V_EN" not in safe                                 # the analog supply is not cut here
    for token in ("efuses_and_vadj_off()", "dios_off()", "PIN_LSHIFT_OE", "route_open_verified()"):
        assert token in safe


def test_boot_check_and_pg_suppression_are_narrow():
    main = _code(_read(S3 / "main.cpp"))
    assert "standby_hw_boot_check()" in main
    pca = _code(_read(S3 / "hal" / "pca9535.cpp"))
    assert "suppress_pg_events" not in pca                               # no blanket suppression
    assert "standby_transition && !rail_enabled" in pca
    assert "report_efuse_flt" in pca                                     # e-fuse faults still reach the callback


def test_unavailable_analog_is_null_never_nan():
    core = _code(_read(S3 / "net" / "api_core.cpp"))
    for key in ("dieTemp", "adcValue", "adcRaw", "measuredVoltageV"):
        assert 'api_add_measurement(' in core and f'"{key}"' in core
    # no raw float formatting of an analog value into JSON text on the S3 API routes
    assert 'cJSON_AddNumberToObject(root, "dieTemp"' not in core
    assert 'cJSON_AddNumberToObject(root, "adcValue"' not in core
    ble = _code(_read(S3 / "net" / "ble_service.cpp"))
    assert "isfinite" in ble and '"na\\":true' in ble.replace("\\\\", "\\")
    web = _read(S3 / "web" / "webserver.cpp")   # raw: its string literals defeat comment stripping
    assert 'api_add_measurement(obj, "value"' in web
