"""
BugBuster MCP — DAQ HAT settings & source-supply tools.

These reach the DAQ HAT (ESP32-P4) settings registry through the ESP32-S3 HAT
bridge (CmdId.DAQ_CONFIG). They cover the programmable DUT supply (SMU), the
acquisition front-end options, and the energy/charge accumulators.

Note: live measurement streaming (I/V/P waveforms) flows over the P4's own USB
data plane, not this control link. A single-shot snapshot of the latest fused
reading is available via daq_measure.

Tools: daq_get_settings, daq_get_setting, daq_set_setting, daq_set_source,
       daq_measure, daq_energy_reset, daq_charge_reset
"""

from __future__ import annotations
from .. import session
from ..safety import require_hat


# DUT supply limits (mirror daq_config_registry schema bounds).
_VDUT_MIN_MV, _VDUT_MAX_MV = 1800, 20000
_ILIMIT_MIN_MA, _ILIMIT_MAX_MA = 100, 2500


def _key_by_name(name: str):
    from bugbuster.daq_config import DaqKey
    try:
        return DaqKey[name.strip().upper()]
    except KeyError as exc:
        valid = ", ".join(k.name for k in DaqKey)
        raise ValueError(f"unknown DAQ setting '{name}'. Valid: {valid}") from exc


def register(mcp) -> None:

    @mcp.tool()
    def daq_get_settings() -> dict:
        """
        Read every DAQ HAT setting and return them keyed by name.

        Covers acquisition (autoranging, range, sample rate, streaming),
        the source/SMU (enable, DUT voltage mV, current limit mA), DSP/FFT,
        display, neopixel, WiFi and system labels.

        Returns: a dict of {setting_name: value}.
        """
        from bugbuster.daq_config import DaqKey
        bb = session.get_client()
        require_hat(bb)
        raw = bb.daq.get_all()
        by_name: dict[str, object] = {}
        for key, value in raw.items():
            try:
                by_name[DaqKey(key).name.lower()] = value
            except ValueError:
                by_name[f"key_0x{key:04x}"] = value
        return by_name

    @mcp.tool()
    def daq_get_setting(name: str) -> dict:
        """
        Read one DAQ HAT setting by name (e.g. "dut_voltage_mv",
        "source_enable", "autoranging").

        Returns: name, value.
        """
        bb = session.get_client()
        require_hat(bb)
        key = _key_by_name(name)
        return {"name": key.name.lower(), "value": bb.daq.get(key)}

    @mcp.tool()
    def daq_set_setting(name: str, value: int | float | bool | str) -> dict:
        """
        Write one DAQ HAT setting by name.

        Use the explicit source helpers (daq_set_source) for the supply; this
        is the general-purpose setter for any other registry key (e.g.
        "autoranging", "streaming", "fft_enable", "brightness_pct").

        Parameters:
        - name: setting key name (see daq_get_settings).
        - value: new value (bool/int/float/str as appropriate for the key).

        Returns: name, value.
        """
        bb = session.get_client()
        require_hat(bb)
        key = _key_by_name(name)
        bb.daq.set(key, value)
        return {"name": key.name.lower(), "value": value}

    @mcp.tool()
    def daq_set_source(
        voltage_mv: int | None = None,
        current_limit_ma: int | None = None,
        enable: bool | None = None,
    ) -> dict:
        """
        Configure the DAQ HAT programmable DUT supply (SMU).

        Any argument left as None is unchanged. When calibration tables are
        present on the HAT they are applied automatically for accurate output.

        Parameters:
        - voltage_mv: DUT output voltage in millivolts (1800..20000).
        - current_limit_ma: output current limit in milliamps (100..2500).
        - enable: True to turn the output on, False to turn it off.

        Returns: the resulting voltage_mv, current_limit_ma and enabled state
        that were applied this call.
        """
        from bugbuster.daq_config import DaqKey
        bb = session.get_client()
        require_hat(bb)

        applied: dict[str, object] = {}
        if voltage_mv is not None:
            if not (_VDUT_MIN_MV <= int(voltage_mv) <= _VDUT_MAX_MV):
                raise ValueError(
                    f"voltage_mv must be {_VDUT_MIN_MV}..{_VDUT_MAX_MV}")
            bb.daq.set(DaqKey.DUT_VOLTAGE_MV, int(voltage_mv))
            applied["voltage_mv"] = int(voltage_mv)
        if current_limit_ma is not None:
            if not (_ILIMIT_MIN_MA <= int(current_limit_ma) <= _ILIMIT_MAX_MA):
                raise ValueError(
                    f"current_limit_ma must be {_ILIMIT_MIN_MA}..{_ILIMIT_MAX_MA}")
            bb.daq.set(DaqKey.DUT_ILIMIT_MA, int(current_limit_ma))
            applied["current_limit_ma"] = int(current_limit_ma)
        if enable is not None:
            bb.daq.set(DaqKey.SOURCE_ENABLE, bool(enable))
            applied["enabled"] = bool(enable)
        if not applied:
            raise ValueError("specify at least one of voltage_mv, "
                             "current_limit_ma, enable")
        return applied

    @mcp.tool()
    def daq_measure() -> dict:
        """
        Read the DAQ HAT's latest live measurement.

        Returns the most recent fused readings from the supply/analyzer:
        current_a, voltage_v, power_w, accumulated energy_mwh, the active
        current range (hi/mid/lo), and the streaming/source_enabled state.

        Useful after daq_set_source to confirm the DUT is drawing the expected
        current at the programmed voltage.
        """
        bb = session.get_client()
        require_hat(bb)
        m = bb.daq.measure()
        rng = m.get("range")
        m["range"] = getattr(rng, "name", str(rng)).lower()
        return m

    @mcp.tool()
    def daq_energy_reset() -> dict:
        """Reset the DAQ HAT energy accumulator (mWh). Returns: success."""
        from bugbuster.daq_config import DaqAction
        bb = session.get_client()
        require_hat(bb)
        bb.daq.action(DaqAction.ENERGY_RESET)
        return {"success": True, "message": "Energy accumulator reset."}

    @mcp.tool()
    def daq_charge_reset() -> dict:
        """Reset the DAQ HAT charge accumulator (mAh). Returns: success."""
        from bugbuster.daq_config import DaqAction
        bb = session.get_client()
        require_hat(bb)
        bb.daq.action(DaqAction.CHARGE_RESET)
        return {"success": True, "message": "Charge accumulator reset."}
