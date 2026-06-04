"""Regression tests for Scripts/MeasureCurrent.py hardware mapping."""

import importlib.util
import pathlib

def _load_measure_current():
    path = pathlib.Path(__file__).resolve().parents[2] / "Scripts" / "MeasureCurrent.py"
    spec = importlib.util.spec_from_file_location("measure_current_script", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_ioblock_4_is_logical_d_on_public_adc_channel_3():
    module = _load_measure_current()
    info = module.IO_BLOCK_MAP[4]

    assert info["label"] == "D"
    assert info["analog_io"] == 12
    assert info["adc_ch"] == 3
    assert info["adc_label"] == "D"
    assert info["efuse"] is module.PowerControl.EFUSE4


def test_ioblock_3_is_logical_c_on_public_adc_channel_2():
    module = _load_measure_current()
    info = module.IO_BLOCK_MAP[3]

    assert info["label"] == "C"
    assert info["analog_io"] == 9
    assert info["adc_ch"] == 2
    assert info["adc_label"] == "C"
    assert info["efuse"] is module.PowerControl.EFUSE3
