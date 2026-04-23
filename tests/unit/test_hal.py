"""
Unit tests for BugBuster HAL routing logic.

Runs WITHOUT hardware — all client calls are mocked.
"""

import unittest
from unittest.mock import MagicMock, call

from bugbuster.hal import (
    BugBusterHAL,
    DEFAULT_ROUTING,
    ANALOG_IOS,
    ANALOG_IO_MODES,
    DIGITAL_IO_MODES,
    PortMode,
    IORouting,
    _SW_A_ESP_HIGH,
    _SW_A_ADC,
    _SW_A_ESP_LOW,
    _SW_A_HAT,
    _SW_B_ESP_HIGH,
    _SW_B_ESP_LOW,
    _SW_C_ESP_HIGH,
    _SW_C_ESP_LOW,
)
from bugbuster.constants import PowerControl, ChannelFunction


def _make_hal():
    """Create a HAL with a mocked client, pre-powered-up."""
    mock_bb = MagicMock()
    hal = BugBusterHAL(mock_bb)
    # Bypass begin() power-up sequence — just mark as ready
    hal._powered_up = True
    hal._io_mode = {io: PortMode.DISABLED for io in hal._routing}
    return hal, mock_bb


# =========================================================================
# 1. Routing table consistency
# =========================================================================

class TestRoutingTableConsistency(unittest.TestCase):
    """Verify structural invariants of DEFAULT_ROUTING."""

    def test_all_12_ios_present(self):
        self.assertEqual(set(DEFAULT_ROUTING.keys()), set(range(1, 13)))

    def test_analog_ios_have_channel(self):
        for io_num in (1, 4, 7, 10):
            rt = DEFAULT_ROUTING[io_num]
            self.assertIsNotNone(
                rt.channel,
                f"IO {io_num} is analog-capable but channel is None",
            )

    def test_digital_only_ios_have_no_channel(self):
        digital_only = set(range(1, 13)) - ANALOG_IOS
        for io_num in digital_only:
            rt = DEFAULT_ROUTING[io_num]
            self.assertIsNone(
                rt.channel,
                f"IO {io_num} is digital-only but has channel={rt.channel}",
            )

    def test_mux_device_indices_valid(self):
        for io_num, rt in DEFAULT_ROUTING.items():
            self.assertIn(
                rt.mux_device, range(4),
                f"IO {io_num} has invalid mux_device={rt.mux_device}",
            )

    def test_analog_ios_have_analog_modes(self):
        for io_num in ANALOG_IOS:
            rt = DEFAULT_ROUTING[io_num]
            self.assertEqual(rt.valid_modes, ANALOG_IO_MODES)

    def test_digital_ios_have_digital_modes(self):
        digital_only = set(range(1, 13)) - ANALOG_IOS
        for io_num in digital_only:
            rt = DEFAULT_ROUTING[io_num]
            self.assertEqual(rt.valid_modes, DIGITAL_IO_MODES)

    def test_channels_are_0_through_3(self):
        channels = sorted(
            rt.channel for rt in DEFAULT_ROUTING.values() if rt.channel is not None
        )
        self.assertEqual(channels, [0, 1, 2, 3])

    def test_each_io_has_esp_gpio(self):
        for io_num, rt in DEFAULT_ROUTING.items():
            self.assertIsInstance(rt.esp_gpio, int, f"IO {io_num} missing esp_gpio")

    def test_block_assignment(self):
        for io_num in range(1, 7):
            self.assertEqual(DEFAULT_ROUTING[io_num].block, 1)
        for io_num in range(7, 13):
            self.assertEqual(DEFAULT_ROUTING[io_num].block, 2)


# =========================================================================
# 2. MUX bitmask computation
# =========================================================================

class TestSetMux(unittest.TestCase):
    """Verify _set_mux() computes correct switch bitmasks per IO group."""

    def test_analog_io_analog_mode_sets_adc_bit(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[1]  # analog IO, position 1, mux_device 0
        hal._set_mux(rt, PortMode.ANALOG_IN)
        # Should set bit 1 (S2 = ADC) in device 0
        expected_state = [_SW_A_ADC, 0, 0, 0]
        mock_bb.mux_set_all.assert_called_with(expected_state)

    def test_analog_io_digital_high_sets_esp_high_bit(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[1]
        hal._set_mux(rt, PortMode.DIGITAL_IN)
        expected_state = [_SW_A_ESP_HIGH, 0, 0, 0]
        mock_bb.mux_set_all.assert_called_with(expected_state)

    def test_analog_io_digital_low_sets_esp_low_bit(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[1]
        hal._set_mux(rt, PortMode.DIGITAL_OUT_LOW)
        expected_state = [_SW_A_ESP_LOW, 0, 0, 0]
        mock_bb.mux_set_all.assert_called_with(expected_state)

    def test_analog_io_hat_sets_hat_bit(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[1]
        hal._set_mux(rt, PortMode.HAT)
        expected_state = [_SW_A_HAT, 0, 0, 0]
        mock_bb.mux_set_all.assert_called_with(expected_state)

    def test_analog_io_disabled_clears_group_a(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[1]
        # First set something, then disable
        hal._mux_state[0] = 0xFF
        hal._set_mux(rt, PortMode.DISABLED)
        # Group A (bits 0-3) cleared, rest preserved
        self.assertEqual(hal._mux_state[0], 0xF0)

    def test_position2_digital_high_sets_group_b(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[2]  # position 2, mux_device 0
        hal._set_mux(rt, PortMode.DIGITAL_OUT)
        expected_state = [_SW_B_ESP_HIGH, 0, 0, 0]
        mock_bb.mux_set_all.assert_called_with(expected_state)

    def test_position2_digital_low_sets_group_b_low(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[2]
        hal._set_mux(rt, PortMode.DIGITAL_IN_LOW)
        expected_state = [_SW_B_ESP_LOW, 0, 0, 0]
        mock_bb.mux_set_all.assert_called_with(expected_state)

    def test_position3_digital_high_sets_group_c(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[3]  # position 3, mux_device 0
        hal._set_mux(rt, PortMode.DIGITAL_OUT)
        expected_state = [_SW_C_ESP_HIGH, 0, 0, 0]
        mock_bb.mux_set_all.assert_called_with(expected_state)

    def test_position3_digital_low_sets_group_c_low(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[3]
        hal._set_mux(rt, PortMode.DIGITAL_OUT_LOW)
        expected_state = [_SW_C_ESP_LOW, 0, 0, 0]
        mock_bb.mux_set_all.assert_called_with(expected_state)

    def test_different_devices_independent(self):
        """IOs on different mux devices write to separate state slots."""
        hal, mock_bb = _make_hal()
        # IO 7 is on mux_device 3, position 1
        rt7 = hal._routing[7]
        hal._set_mux(rt7, PortMode.ANALOG_OUT)
        self.assertEqual(hal._mux_state[3], _SW_A_ADC)
        self.assertEqual(hal._mux_state[0], 0)  # device 0 untouched

    def test_multiple_ios_same_device_coexist(self):
        """IOs 1 (pos 1), 2 (pos 2), 3 (pos 3) share device 0 without conflict."""
        hal, mock_bb = _make_hal()
        hal._set_mux(hal._routing[1], PortMode.ANALOG_IN)   # group A
        hal._set_mux(hal._routing[2], PortMode.DIGITAL_OUT)  # group B
        hal._set_mux(hal._routing[3], PortMode.DIGITAL_OUT)  # group C
        expected = _SW_A_ADC | _SW_B_ESP_HIGH | _SW_C_ESP_HIGH
        self.assertEqual(hal._mux_state[0], expected)


# =========================================================================
# 3. Mode validation
# =========================================================================

class TestModeValidation(unittest.TestCase):
    """configure() must reject invalid modes for each IO type."""

    def test_analog_mode_rejected_on_digital_only_io(self):
        hal, _ = _make_hal()
        with self.assertRaises(ValueError):
            hal.configure(2, PortMode.ANALOG_IN)

    def test_current_mode_rejected_on_digital_only_io(self):
        hal, _ = _make_hal()
        with self.assertRaises(ValueError):
            hal.configure(5, PortMode.CURRENT_OUT)

    def test_rtd_rejected_on_digital_only_io(self):
        hal, _ = _make_hal()
        with self.assertRaises(ValueError):
            hal.configure(8, PortMode.RTD)

    def test_hart_rejected_on_digital_only_io(self):
        hal, _ = _make_hal()
        with self.assertRaises(ValueError):
            hal.configure(11, PortMode.HART)

    def test_hat_rejected_on_digital_only_io(self):
        hal, _ = _make_hal()
        with self.assertRaises(ValueError):
            hal.configure(12, PortMode.HAT)

    def test_digital_mode_accepted_on_digital_only_io(self):
        hal, mock_bb = _make_hal()
        # Should not raise
        hal.configure(2, PortMode.DIGITAL_OUT)
        self.assertEqual(hal._io_mode[2], PortMode.DIGITAL_OUT)

    def test_analog_mode_accepted_on_analog_io(self):
        hal, mock_bb = _make_hal()
        hal.configure(1, PortMode.ANALOG_IN)
        self.assertEqual(hal._io_mode[1], PortMode.ANALOG_IN)

    def test_digital_mode_accepted_on_analog_io(self):
        hal, mock_bb = _make_hal()
        hal.configure(4, PortMode.DIGITAL_OUT)
        self.assertEqual(hal._io_mode[4], PortMode.DIGITAL_OUT)

    def test_disabled_accepted_on_any_io(self):
        hal, mock_bb = _make_hal()
        for io_num in range(1, 13):
            hal.configure(io_num, PortMode.DISABLED)
            self.assertEqual(hal._io_mode[io_num], PortMode.DISABLED)

    def test_invalid_io_number_raises(self):
        hal, _ = _make_hal()
        with self.assertRaises(ValueError):
            hal.configure(0, PortMode.DIGITAL_IN)
        with self.assertRaises(ValueError):
            hal.configure(13, PortMode.DIGITAL_IN)

    def test_configure_before_begin_raises(self):
        mock_bb = MagicMock()
        hal = BugBusterHAL(mock_bb)
        # _powered_up is False by default
        with self.assertRaises(RuntimeError):
            hal.configure(1, PortMode.ANALOG_IN)


# =========================================================================
# 4. Digital read/write routing
# =========================================================================

class TestDigitalReadRouting(unittest.TestCase):
    """Verify read_digital() dispatches to correct hardware path."""

    def test_read_analog_io_digital_in_uses_get_status(self):
        """Analog IO (channel != None) in DIGITAL_IN reads via get_status DIN."""
        hal, mock_bb = _make_hal()
        hal._io_mode[1] = PortMode.DIGITAL_IN
        mock_bb.get_status.return_value = {
            "channels": {0: {"din_state": True}},
        }
        result = hal.read_digital(1)
        mock_bb.get_status.assert_called_once()
        self.assertTrue(result)

    def test_read_analog_io_digital_in_low_uses_esp_gpio(self):
        """Analog IO in DIGITAL_IN_LOW reads via ESP GPIO (dio path)."""
        hal, mock_bb = _make_hal()
        hal._io_mode[1] = PortMode.DIGITAL_IN_LOW
        mock_bb.dio_read.return_value = {"value": True}
        result = hal.read_digital(1)
        mock_bb.dio_configure.assert_called_once_with(1, 1)  # gpio=1, mode=INPUT
        mock_bb.dio_read.assert_called_once_with(1)
        self.assertTrue(result)

    def test_read_digital_only_io_uses_esp_gpio(self):
        """Digital-only IO (channel=None) always uses ESP GPIO path."""
        hal, mock_bb = _make_hal()
        hal._io_mode[2] = PortMode.DIGITAL_IN
        mock_bb.dio_read.return_value = {"value": False}
        result = hal.read_digital(2)
        # IO 2 has esp_gpio=2
        mock_bb.dio_configure.assert_called_once_with(2, 1)
        mock_bb.dio_read.assert_called_once_with(2)
        self.assertFalse(result)

    def test_read_digital_wrong_mode_raises(self):
        hal, _ = _make_hal()
        hal._io_mode[1] = PortMode.ANALOG_IN
        with self.assertRaises(RuntimeError):
            hal.read_digital(1)

    def test_read_digital_returns_false_on_empty_response(self):
        hal, mock_bb = _make_hal()
        hal._io_mode[5] = PortMode.DIGITAL_IN
        mock_bb.dio_read.return_value = None
        result = hal.read_digital(5)
        self.assertFalse(result)


class TestDigitalWriteRouting(unittest.TestCase):
    """Verify write_digital() dispatches to correct hardware path."""

    def test_write_analog_io_digital_out_uses_set_digital_output(self):
        """Analog IO (channel != None) in DIGITAL_OUT writes via AD74416H."""
        hal, mock_bb = _make_hal()
        hal._io_mode[4] = PortMode.DIGITAL_OUT
        hal.write_digital(4, True)
        mock_bb.set_digital_output.assert_called_once_with(1, on=True)

    def test_write_analog_io_digital_out_low_uses_esp_gpio(self):
        """Analog IO in DIGITAL_OUT_LOW writes via ESP GPIO."""
        hal, mock_bb = _make_hal()
        hal._io_mode[4] = PortMode.DIGITAL_OUT_LOW
        hal.write_digital(4, True)
        # IO 4 uses logical IO 4 in the protocol
        mock_bb.dio_configure.assert_called_once_with(4, 2)  # io=4, mode=OUTPUT
        mock_bb.dio_write.assert_called_once_with(4, True)

    def test_write_digital_only_io_uses_esp_gpio(self):
        """Digital-only IO (channel=None) always uses ESP GPIO path."""
        hal, mock_bb = _make_hal()
        hal._io_mode[3] = PortMode.DIGITAL_OUT
        hal.write_digital(3, False)
        # IO 3: channel=None, so takes the else branch even for DIGITAL_OUT
        mock_bb.dio_configure.assert_called_once_with(3, 2)
        mock_bb.dio_write.assert_called_once_with(3, False)

    def test_write_digital_wrong_mode_raises(self):
        hal, _ = _make_hal()
        hal._io_mode[1] = PortMode.DIGITAL_IN
        with self.assertRaises(RuntimeError):
            hal.write_digital(1, True)

    def test_write_digital_off(self):
        """Verify False state is passed through."""
        hal, mock_bb = _make_hal()
        hal._io_mode[10] = PortMode.DIGITAL_OUT
        hal.write_digital(10, False)
        mock_bb.set_digital_output.assert_called_once_with(2, on=False)


# =========================================================================
# 5. Power block enable
# =========================================================================

class TestEnableIoBlockPower(unittest.TestCase):
    """Verify _enable_io_block_power() activates the correct PCA controls."""

    def test_block1_io1_enables_vadj1_and_efuse1(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[1]
        hal._enable_io_block_power(rt)
        mock_bb.power_set.assert_any_call(PowerControl.VADJ1, on=True)
        mock_bb.power_set.assert_any_call(PowerControl.EFUSE1, on=True)
        mock_bb.idac_set_voltage.assert_called_once_with(1, 12.0)

    def test_block1_io5_enables_vadj1_and_efuse2(self):
        hal, mock_bb = _make_hal()
        rt = hal._routing[5]
        hal._enable_io_block_power(rt)
        mock_bb.power_set.assert_any_call(PowerControl.VADJ1, on=True)
        mock_bb.power_set.assert_any_call(PowerControl.EFUSE2, on=True)

    def test_block2_io7_enables_vadj2_and_efuse4(self):
        # PCB swap: physical P3 (IO7..IO9) is wired to EFUSE4
        hal, mock_bb = _make_hal()
        rt = hal._routing[7]
        hal._enable_io_block_power(rt)
        mock_bb.power_set.assert_any_call(PowerControl.VADJ2, on=True)
        mock_bb.power_set.assert_any_call(PowerControl.EFUSE4, on=True)
        mock_bb.idac_set_voltage.assert_called_once_with(2, 12.0)

    def test_block2_io12_enables_vadj2_and_efuse3(self):
        # PCB swap: physical P4 (IO10..IO12) is wired to EFUSE3
        hal, mock_bb = _make_hal()
        rt = hal._routing[12]
        hal._enable_io_block_power(rt)
        mock_bb.power_set.assert_any_call(PowerControl.VADJ2, on=True)
        mock_bb.power_set.assert_any_call(PowerControl.EFUSE3, on=True)

    def test_supply_not_re_enabled_if_already_on(self):
        hal, mock_bb = _make_hal()
        hal._supplies_on.add(PowerControl.VADJ1)
        hal._efuses_on.add(PowerControl.EFUSE1)
        rt = hal._routing[1]
        hal._enable_io_block_power(rt)
        # power_set should NOT be called since both are already on
        mock_bb.power_set.assert_not_called()

    def test_efuse_skipped_if_already_on(self):
        # PCB swap: IO7 routes through EFUSE4 (physical P3 wiring)
        hal, mock_bb = _make_hal()
        hal._efuses_on.add(PowerControl.EFUSE4)
        rt = hal._routing[7]
        hal._enable_io_block_power(rt)
        # supply (VADJ2) should be enabled, but efuse4 should not
        power_calls = mock_bb.power_set.call_args_list
        called_controls = [c[0][0] for c in power_calls]
        self.assertIn(PowerControl.VADJ2, called_controls)
        self.assertNotIn(PowerControl.EFUSE4, called_controls)

    def test_ios_sharing_same_efuse_only_enable_once(self):
        """IO 1 and IO 2 share EFUSE1 — second call should skip it."""
        hal, mock_bb = _make_hal()
        hal._enable_io_block_power(hal._routing[1])
        mock_bb.reset_mock()
        hal._enable_io_block_power(hal._routing[2])
        # VADJ1 and EFUSE1 already on — nothing should be called
        mock_bb.power_set.assert_not_called()


if __name__ == "__main__":
    unittest.main()
