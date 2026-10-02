"""Coverage for bugbuster_mcp.tools.daq: DAQ HAT settings, SMU source,
live measurement, accumulators and the trigger/flag engine."""
from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import create_autospec, patch

import pytest

from bugbuster.daq_config import (
    DaqAction, DaqConfig, DaqKey, DaqRange, DaqTrigEdge, DaqTrigger,
    DaqTrigLogic, DaqTrigRole, DaqTrigSource,
)
from bugbuster_mcp.tools import daq
from tests.unit._mock_client import make_client_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


@pytest.fixture
def env():
    mcp = _DummyMCP()
    daq.register(mcp)
    bb = make_client_mock()
    bb.daq = create_autospec(DaqConfig, instance=True)
    bb.daq_trigger = create_autospec(DaqTrigger, instance=True)
    bb.hat_status_cached.return_value = {"detected": True}
    with patch("bugbuster_mcp.session.get_client", return_value=bb) as gc:
        yield SimpleNamespace(t=mcp.tools, bb=bb, gc=gc)


def _io_cfg(io=3, role=DaqTrigRole.FLAG, edge=DaqTrigEdge.ANY,
            source=DaqTrigSource.DIGITAL, thr=1.5):
    return {"io": io, "role": role, "edge": edge, "source": source,
            "threshold_v": thr}


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------
def test_registers_every_documented_tool(env):
    assert set(env.t) == {
        "daq_get_settings", "daq_get_setting", "daq_set_setting",
        "daq_set_source", "daq_measure", "observe_daq", "daq_energy_reset",
        "daq_charge_reset", "daq_set_io_role", "daq_set_trigger_logic",
        "daq_arm", "daq_get_trigger_state",
    }


# ---------------------------------------------------------------------------
# HAT guard: every tool refuses without a detected HAT and sends nothing
# ---------------------------------------------------------------------------
_GUARDED = [
    ("daq_get_settings", {}),
    ("daq_get_setting", {"name": "dut_voltage_mv"}),
    ("daq_set_setting", {"name": "autoranging", "value": True}),
    ("daq_set_source", {"voltage_mv": 3300}),
    ("daq_measure", {}),
    ("observe_daq", {"seconds": 0.1, "rate_hz": 10}),
    ("daq_energy_reset", {}),
    ("daq_charge_reset", {}),
    ("daq_set_io_role", {"io": 3, "role": "flag"}),
    ("daq_set_trigger_logic", {"logic": "and"}),
    ("daq_arm", {}),
    ("daq_get_trigger_state", {}),
]


@pytest.mark.parametrize("name, kwargs", _GUARDED, ids=[g[0] for g in _GUARDED])
def test_tools_refuse_without_hat(env, name, kwargs):
    env.bb.hat_status_cached.return_value = {"detected": False}
    with pytest.raises(RuntimeError, match="No HAT"):
        env.t[name](**kwargs)
    assert env.bb.daq.mock_calls == []
    assert env.bb.daq_trigger.mock_calls == []


def test_hat_guard_reports_usb_requirement(env):
    env.bb.hat_status_cached.side_effect = NotImplementedError
    with pytest.raises(RuntimeError, match="USB transport"):
        env.t["daq_measure"]()
    assert env.bb.daq.mock_calls == []


# ---------------------------------------------------------------------------
# daq_get_settings / daq_get_setting / daq_set_setting
# ---------------------------------------------------------------------------
def test_get_settings_keys_by_lowercase_name(env):
    env.bb.daq.get_all.return_value = {
        int(DaqKey.DUT_VOLTAGE_MV): 3300,
        int(DaqKey.SOURCE_ENABLE): True,
        int(DaqKey.DEVICE_LABEL): "bench",
    }
    out = env.t["daq_get_settings"]()
    assert out == {"dut_voltage_mv": 3300, "source_enable": True,
                   "device_label": "bench"}
    env.bb.daq.get_all.assert_called_once_with()


def test_get_settings_keeps_unknown_keys_under_hex_name(env):
    env.bb.daq.get_all.return_value = {int(DaqKey.AUTORANGING): True, 0xFFFE: 7}
    out = env.t["daq_get_settings"]()
    assert out == {"autoranging": True, "key_0xfffe": 7}


def test_get_settings_empty_registry(env):
    env.bb.daq.get_all.return_value = {}
    assert env.t["daq_get_settings"]() == {}


@pytest.mark.parametrize("name", ["dut_voltage_mv", "DUT_VOLTAGE_MV", "  Dut_Voltage_Mv  "])
def test_get_setting_resolves_name_case_insensitively(env, name):
    env.bb.daq.get.return_value = 3300
    out = env.t["daq_get_setting"](name=name)
    assert out == {"name": "dut_voltage_mv", "value": 3300}
    env.bb.daq.get.assert_called_once_with(DaqKey.DUT_VOLTAGE_MV)


def test_get_setting_unknown_name_lists_valid_names(env):
    with pytest.raises(ValueError, match=r"unknown DAQ setting 'bogus'.*DUT_VOLTAGE_MV"):
        env.t["daq_get_setting"](name="bogus")
    env.bb.daq.get.assert_not_called()


def test_set_setting_writes_value_and_echoes_it(env):
    out = env.t["daq_set_setting"](name="Brightness_Pct", value=40)
    assert out == {"name": "brightness_pct", "value": 40}
    env.bb.daq.set.assert_called_once_with(DaqKey.BRIGHTNESS_PCT, 40)


@pytest.mark.parametrize("value", [True, 2.5, "label"])
def test_set_setting_passes_value_through_unchanged(env, value):
    env.t["daq_set_setting"](name="autoranging", value=value)
    env.bb.daq.set.assert_called_once_with(DaqKey.AUTORANGING, value)


def test_set_setting_unknown_name_does_not_write(env):
    with pytest.raises(ValueError, match="unknown DAQ setting"):
        env.t["daq_set_setting"](name="nope", value=1)
    env.bb.daq.set.assert_not_called()


# ---------------------------------------------------------------------------
# daq_set_source
# ---------------------------------------------------------------------------
def test_set_source_all_arguments(env):
    out = env.t["daq_set_source"](voltage_mv=3300, current_limit_ma=500, enable=True)
    assert out == {"voltage_mv": 3300, "current_limit_ma": 500, "enabled": True}
    assert env.bb.daq.set.call_args_list == [
        ((DaqKey.DUT_VOLTAGE_MV, 3300),),
        ((DaqKey.DUT_ILIMIT_MA, 500),),
        ((DaqKey.SOURCE_ENABLE, True),),
    ]


def test_set_source_only_touches_given_fields(env):
    out = env.t["daq_set_source"](enable=False)
    assert out == {"enabled": False}
    env.bb.daq.set.assert_called_once_with(DaqKey.SOURCE_ENABLE, False)


def test_set_source_coerces_numbers_to_int(env):
    out = env.t["daq_set_source"](voltage_mv=3300.0, current_limit_ma=250.0)
    assert out == {"voltage_mv": 3300, "current_limit_ma": 250}
    assert all(isinstance(c.args[1], int) for c in env.bb.daq.set.call_args_list)


@pytest.mark.parametrize("mv", [1800, 20000])
def test_set_source_voltage_bounds_are_inclusive(env, mv):
    assert env.t["daq_set_source"](voltage_mv=mv) == {"voltage_mv": mv}
    env.bb.daq.set.assert_called_once_with(DaqKey.DUT_VOLTAGE_MV, mv)


@pytest.mark.parametrize("mv", [0, 1799, 20001, -5])
def test_set_source_rejects_out_of_range_voltage(env, mv):
    with pytest.raises(ValueError, match="voltage_mv must be 1800..20000"):
        env.t["daq_set_source"](voltage_mv=mv)
    env.bb.daq.set.assert_not_called()


@pytest.mark.parametrize("ma", [100, 2500])
def test_set_source_current_bounds_are_inclusive(env, ma):
    assert env.t["daq_set_source"](current_limit_ma=ma) == {"current_limit_ma": ma}
    env.bb.daq.set.assert_called_once_with(DaqKey.DUT_ILIMIT_MA, ma)


@pytest.mark.parametrize("ma", [0, 99, 2501])
def test_set_source_rejects_out_of_range_current(env, ma):
    with pytest.raises(ValueError, match="current_limit_ma must be 100..2500"):
        env.t["daq_set_source"](current_limit_ma=ma)
    env.bb.daq.set.assert_not_called()


def test_set_source_requires_at_least_one_argument(env):
    with pytest.raises(ValueError, match="specify at least one"):
        env.t["daq_set_source"]()
    env.bb.daq.set.assert_not_called()


def test_set_source_validates_everything_before_writing(env):
    with pytest.raises(ValueError):
        env.t["daq_set_source"](voltage_mv=3300, current_limit_ma=5000)
    env.bb.daq.set.assert_not_called()


# ---------------------------------------------------------------------------
# daq_measure / observe_daq
# ---------------------------------------------------------------------------
def _measure(rng=DaqRange.MID, i=0.01, v=3.3):
    return {"range": rng, "streaming": True, "source_enabled": True,
            "current_a": i, "voltage_v": v, "power_w": i * v,
            "energy_mwh": 0.5}


def test_measure_renders_range_as_lowercase_name(env):
    env.bb.daq.measure.return_value = _measure(DaqRange.HI)
    out = env.t["daq_measure"]()
    assert out["range"] == "hi"
    assert out["current_a"] == 0.01
    assert out["voltage_v"] == 3.3
    assert out["streaming"] is True and out["source_enabled"] is True
    assert set(out) == {"range", "streaming", "source_enabled", "current_a",
                        "voltage_v", "power_w", "energy_mwh"}
    env.bb.daq.measure.assert_called_once_with()


@pytest.mark.parametrize("rng, expected", [
    (DaqRange.MID, "mid"), (DaqRange.LO, "lo"), (DaqRange.UNKNOWN, "unknown"),
    (7, "7"),
])
def test_measure_range_variants(env, rng, expected):
    env.bb.daq.measure.return_value = _measure(rng)
    assert env.t["daq_measure"]()["range"] == expected


def test_observe_daq_samples_at_requested_rate_and_summarises(env):
    env.bb.daq.measure.side_effect = [_measure(i=0.1), _measure(i=0.2),
                                      _measure(i=0.3), _measure(i=0.4)]
    with patch("bugbuster_mcp.tools.daq.time.sleep") as sleep:
        out = env.t["observe_daq"](seconds=1.0, rate_hz=4.0)
    assert env.bb.daq.measure.call_count == 4
    assert [c.args[0] for c in sleep.call_args_list] == [0.25] * 3
    assert out["seconds"] == 1.0
    cur = out["fields"]["current_a"]
    assert cur["count"] == 4
    assert cur["min"] == pytest.approx(0.1)
    assert cur["max"] == pytest.approx(0.4)
    assert cur["mean"] == pytest.approx(0.25)
    assert cur["std"] == pytest.approx(0.1118033988749895)


def test_observe_daq_skips_booleans(env):
    env.bb.daq.measure.return_value = _measure()
    with patch("bugbuster_mcp.tools.daq.time.sleep"):
        out = env.t["observe_daq"](seconds=0.2, rate_hz=10)
    assert "streaming" not in out["fields"]
    assert "source_enabled" not in out["fields"]
    assert "power_w" in out["fields"]


def test_observe_daq_takes_at_least_one_sample(env):
    env.bb.daq.measure.return_value = _measure()
    with patch("bugbuster_mcp.tools.daq.time.sleep") as sleep:
        out = env.t["observe_daq"](seconds=0.01, rate_hz=1)
    assert env.bb.daq.measure.call_count == 1
    sleep.assert_not_called()
    assert out["fields"]["voltage_v"]["count"] == 1


@pytest.mark.parametrize("seconds, rate", [(0, 10), (-1, 10), (60.1, 10),
                                           (1, 0), (1, -2), (1, 50.1)])
def test_observe_daq_rejects_bad_window_before_touching_client(env, seconds, rate):
    with pytest.raises(ValueError):
        env.t["observe_daq"](seconds=seconds, rate_hz=rate)
    env.gc.assert_not_called()
    env.bb.daq.measure.assert_not_called()


@pytest.mark.parametrize("seconds, rate", [(60, 0.5), (0.1, 50)])
def test_observe_daq_accepts_window_limits(env, seconds, rate):
    env.bb.daq.measure.return_value = _measure()
    with patch("bugbuster_mcp.tools.daq.time.sleep"):
        out = env.t["observe_daq"](seconds=seconds, rate_hz=rate)
    assert out["seconds"] == seconds
    assert env.bb.daq.measure.call_count == max(1, round(seconds * rate))


# ---------------------------------------------------------------------------
# Accumulator resets
# ---------------------------------------------------------------------------
def test_energy_reset_sends_energy_action_only(env):
    out = env.t["daq_energy_reset"]()
    assert out["success"] is True and "Energy" in out["message"]
    env.bb.daq.action.assert_called_once_with(DaqAction.ENERGY_RESET)


def test_charge_reset_sends_charge_action_only(env):
    out = env.t["daq_charge_reset"]()
    assert out["success"] is True and "Charge" in out["message"]
    env.bb.daq.action.assert_called_once_with(DaqAction.CHARGE_RESET)


# ---------------------------------------------------------------------------
# Trigger / flag engine
# ---------------------------------------------------------------------------
def test_set_io_role_defaults_and_result_shape(env):
    env.bb.daq_trigger.get_io.return_value = _io_cfg(
        io=5, role=DaqTrigRole.OFF, edge=DaqTrigEdge.RISING)
    out = env.t["daq_set_io_role"](io=5)
    env.bb.daq_trigger.set_io.assert_called_once_with(
        5, DaqTrigRole.OFF, DaqTrigEdge.RISING, DaqTrigSource.DIGITAL, 1.5)
    env.bb.daq_trigger.get_io.assert_called_once_with(5)
    assert out == {"io": 5, "role": "off", "edge": "rising",
                   "source": "digital", "threshold_v": 1.5}


def test_set_io_role_analog_trigger(env):
    env.bb.daq_trigger.get_io.return_value = _io_cfg(
        io=3, role=DaqTrigRole.TRIGGER, edge=DaqTrigEdge.FALLING,
        source=DaqTrigSource.ANALOG, thr=2.5)
    out = env.t["daq_set_io_role"](io=3, role="trigger", edge="falling",
                                   source="analog", threshold_v=2.5)
    env.bb.daq_trigger.set_io.assert_called_once_with(
        3, DaqTrigRole.TRIGGER, DaqTrigEdge.FALLING, DaqTrigSource.ANALOG, 2.5)
    assert out["role"] == "trigger" and out["edge"] == "falling"
    assert out["source"] == "analog" and out["threshold_v"] == 2.5


def test_set_io_role_ignores_case_and_whitespace(env):
    env.bb.daq_trigger.get_io.return_value = _io_cfg()
    env.t["daq_set_io_role"](io=3, role="  FLAG ", edge="Any", source=" Digital")
    env.bb.daq_trigger.set_io.assert_called_once_with(
        3, DaqTrigRole.FLAG, DaqTrigEdge.ANY, DaqTrigSource.DIGITAL, 1.5)


def test_set_io_role_propagates_client_validation(env):
    env.bb.daq_trigger.set_io.side_effect = ValueError("io must be 1..12, got 13")
    with pytest.raises(ValueError, match="io must be 1..12"):
        env.t["daq_set_io_role"](io=13, role="flag")
    env.bb.daq_trigger.get_io.assert_not_called()


@pytest.mark.parametrize("kwargs", [
    pytest.param({"role": "bogus"}, id="role"),
    pytest.param({"edge": "sideways"}, id="edge"),
    pytest.param({"source": "optical"}, id="source"),
])
def test_set_io_role_bad_enum_is_a_value_error(env, kwargs):
    with pytest.raises(ValueError):
        env.t["daq_set_io_role"](io=3, **kwargs)
    env.bb.daq_trigger.set_io.assert_not_called()


def test_set_trigger_logic_and(env):
    out = env.t["daq_set_trigger_logic"](logic="AND")
    assert out == {"logic": "and"}
    env.bb.daq_trigger.set_logic.assert_called_once_with(DaqTrigLogic.AND)


def test_set_trigger_logic_defaults_to_or(env):
    out = env.t["daq_set_trigger_logic"]()
    assert out == {"logic": "or"}
    env.bb.daq_trigger.set_logic.assert_called_once_with(DaqTrigLogic.OR)


def test_set_trigger_logic_bad_value_is_a_value_error(env):
    with pytest.raises(ValueError):
        env.t["daq_set_trigger_logic"](logic="xor")
    env.bb.daq_trigger.set_logic.assert_not_called()


def test_arm_defaults_and_lowercases_logic(env):
    env.bb.daq_trigger.status.return_value = {
        "logic": DaqTrigLogic.OR, "armed": True, "fired": False}
    out = env.t["daq_arm"]()
    env.bb.daq_trigger.arm.assert_called_once_with(True, 0)
    assert out == {"logic": "or", "armed": True, "fired": False}


def test_arm_with_pre_samples_and_disarm(env):
    env.bb.daq_trigger.status.return_value = {
        "logic": DaqTrigLogic.AND, "armed": False, "fired": True}
    out = env.t["daq_arm"](armed=False, pre_samples=12500)
    env.bb.daq_trigger.arm.assert_called_once_with(False, 12500)
    assert out == {"logic": "and", "armed": False, "fired": True}


def test_get_trigger_state_renders_all_ios(env):
    ios = [_io_cfg(io=n, role=DaqTrigRole.TRIGGER if n == 2 else DaqTrigRole.OFF,
                   edge=DaqTrigEdge.FALLING if n == 2 else DaqTrigEdge.RISING,
                   source=DaqTrigSource.ANALOG if n == 3 else DaqTrigSource.DIGITAL)
           for n in range(1, 13)]
    env.bb.daq_trigger.get_all.return_value = {
        "logic": DaqTrigLogic.AND, "armed": True, "fired": False, "ios": ios}
    out = env.t["daq_get_trigger_state"]()
    env.bb.daq_trigger.get_all.assert_called_once_with()
    assert out["logic"] == "and"
    assert out["armed"] is True and out["fired"] is False
    assert len(out["ios"]) == 12
    assert out["ios"][1]["role"] == "trigger" and out["ios"][1]["edge"] == "falling"
    assert out["ios"][2]["source"] == "analog"
    assert out["ios"][0]["role"] == "off" and out["ios"][0]["source"] == "digital"
    assert [io["io"] for io in out["ios"]] == list(range(1, 13))
