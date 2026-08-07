"""Parity guard: python/bugbuster/daq_config.py must mirror the firmware registry.

`DaqKey` is a hand-maintained copy of `daq_key_t` in
Firmware/DAQ_HAT/common/daq_config_registry.h. That copy silently drifted once -
FILTER, DECIMATION, REJECT_5060 and SR_MODE existed in firmware for months while
`DaqConfig.get_all()` returned them as unnamed raw keys 0x106-0x109. Parse the
header and fail here instead of discovering it on the bench.
"""
import pathlib
import re

import pytest

from bugbuster.daq_config import DaqKey, KEY_TYPE

_REPO = pathlib.Path(__file__).resolve().parents[2]
_HEADER = _REPO / "Firmware" / "DAQ_HAT" / "common" / "daq_config_registry.h"
_TABLE = _REPO / "Firmware" / "DAQ_HAT" / "common" / "daq_config_registry.c"

# DAQ_K_FOO = DAQ_KEY(DAQ_GRP_BAR, 0x0A),
_KEY_RE = re.compile(
    r"DAQ_K_(\w+)\s*=\s*DAQ_KEY\(\s*DAQ_GRP_(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\)")
_GROUP_RE = re.compile(r"#define\s+DAQ_GRP_(\w+)\s+(0x[0-9A-Fa-f]+)")
# { DAQ_K_FOO, DAQ_T_U32, ...
_ROW_RE = re.compile(r"\{\s*DAQ_K_(\w+)\s*,\s*DAQ_T_(\w+)\s*,")


def _firmware_keys() -> dict[str, int]:
    src = _HEADER.read_text(encoding="utf-8")
    groups = {name: int(val, 16) for name, val in _GROUP_RE.findall(src)}
    keys = {}
    for name, group, idx in _KEY_RE.findall(src):
        keys[name] = (groups[group] << 8) | int(idx, 16)
    return keys


def _firmware_types() -> dict[str, str]:
    return {name: typ for name, typ in _ROW_RE.findall(
        _TABLE.read_text(encoding="utf-8"))}


@pytest.mark.skipif(not _HEADER.exists(), reason="firmware tree not present")
def test_every_firmware_key_exists_in_python():
    fw = _firmware_keys()
    missing = sorted(set(fw) - {k.name for k in DaqKey})
    assert not missing, (
        f"DaqKey is missing firmware keys {missing}. Add them to "
        f"python/bugbuster/daq_config.py or get_all() returns them unnamed.")


@pytest.mark.skipif(not _HEADER.exists(), reason="firmware tree not present")
def test_key_values_match_firmware():
    fw = _firmware_keys()
    for key in DaqKey:
        if key.name in fw:
            assert int(key) == fw[key.name], (
                f"{key.name}: python 0x{int(key):04X} != "
                f"firmware 0x{fw[key.name]:04X}")


@pytest.mark.skipif(not _HEADER.exists(), reason="firmware tree not present")
def test_no_python_key_absent_from_firmware():
    fw = _firmware_keys()
    extra = sorted({k.name for k in DaqKey} - set(fw))
    assert not extra, f"DaqKey has keys the firmware does not define: {extra}"


@pytest.mark.skipif(not _TABLE.exists(), reason="firmware tree not present")
def test_key_types_match_the_schema_table():
    fw_types = _firmware_types()
    for key in DaqKey:
        if key.name not in fw_types:
            continue
        assert int(key) in KEY_TYPE, f"{key.name} missing from KEY_TYPE"
        assert KEY_TYPE[int(key)].name == fw_types[key.name], (
            f"{key.name}: python {KEY_TYPE[int(key)].name} != "
            f"firmware DAQ_T_{fw_types[key.name]}")


@pytest.mark.skipif(not _TABLE.exists(), reason="firmware tree not present")
def test_every_key_has_a_schema_row():
    fw_keys = set(_firmware_keys())
    rows = set(_firmware_types())
    missing = sorted(fw_keys - rows)
    assert not missing, (
        f"keys declared in daq_config_registry.h with no schema row in "
        f"daq_config_registry.c: {missing}")
