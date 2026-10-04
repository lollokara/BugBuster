"""Spec 2026-10-03 §3 guards for the MicroPython `daq` module (MicroPython C
API: compiled only by the pio build, so these read the firmware text; the wire
logic it calls is executed in test_daq_codec.py)."""

import re

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source

MOD = "Firmware/ESP32/src/mp/moddaq.c"
BRIDGE = "Firmware/ESP32/src/mp/daq_bridge.cpp"
CMAKE = "Firmware/ESP32/components/micropython/CMakeLists.txt"


def _table(name: str) -> list[str]:
    src = read_source(MOD)
    block = src[src.index(f"{name}[] = {{"):]
    block = block[:block.index("};")]
    return [q for q in re.findall(r"\{\s*MP_ROM_QSTR\(MP_QSTR_(\w+)\)", block) if q != "__name__"]


def test_module_is_registered_and_in_the_qstr_pipeline():
    assert "MP_REGISTER_MODULE(MP_QSTR_daq, mp_module_daq);" in read_source(MOD)
    assert "../../src/mp/moddaq.c" in read_source(CMAKE)


def test_api_surface_matches_spec():
    globals_ = _table("daq_module_globals_table")
    assert globals_[:5] == ["present", "vdut", "read", "samples", "run"]
    assert _table("daq_run_globals_table") == [
        "status", "list", "new", "start", "pause", "stop", "reopen", "edit", "delete"]


def test_every_hardware_call_checks_for_a_daq_hat():
    src = read_source(MOD)
    for fn in re.findall(r"^static mp_obj_t (daq_\w+)\(", src, re.M):
        if fn in ("daq_present",):
            continue
        body = extract_function(MOD, rf"^static mp_obj_t {fn}\(")
        assert "require_daq()" in body, fn
    assert "mp_raise_OSError(MP_ENODEV)" in extract_function(MOD, r"^static void require_daq\(")


def test_presence_means_a_connected_daq_hat():
    body = extract_function(BRIDGE, r"^bool daq_mp_present\(")
    assert "connected" in body and "HAT_TYPE_DAQ_POWER" in body


def test_samples_maps_missing_run_to_enoent_and_honours_stop():
    body = extract_function(MOD, r"^static mp_obj_t daq_samples\(")
    assert "MP_ENOENT" in body and "scripting_stop_requested()" in body


def test_name_length_checked():
    body = extract_function(MOD, r"^static void apply_param\(")
    assert "DAQC_NAME_MAX" in body and "mp_raise_ValueError" in body


def test_vdut_failure_names_the_owning_battsim_run():
    """A loaded run makes the P4 refuse VDUT writes: raise RuntimeError with the
    same text as the S3 409 (api_core.cpp), not a bare EIO."""
    body = extract_function(MOD, r"^static MP_NORETURN void raise_vdut_failure\(")
    assert "daq_mp_vdut_owner_run()" in body
    assert "mp_type_RuntimeError" in body
    text = re.search(r'MP_ERROR_TEXT\("(battery simulator run [^"]+)"\)', body).group(1)
    api = read_source("Firmware/ESP32/src/net/api_core.cpp")
    assert text.replace("%d", "%d") in api, "moddaq text must match api_core.cpp"
    vdut = extract_function(MOD, r"^static mp_obj_t daq_vdut\(")
    assert vdut.count("raise_vdut_failure()") == 2
    assert "mp_raise_OSError(MP_EIO)" in body  # still EIO when nothing owns VDUT
