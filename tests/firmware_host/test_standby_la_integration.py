"""Source-structure guards for the LA HAT standby integration.

These pin the properties that must not regress silently and that a host build cannot show:
the USB-descriptor patch and thread-affinity non-negotiables, the "no copy of the wire
contract" rule, the gates in front of every hardware entry point, and that standby never
reaches for the factory-reset / calibration / flash paths.
"""

import re

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source

RP = "Firmware/RP2040"
SRC = f"{RP}/src"


def _src(name: str) -> str:
    return read_source(f"{SRC}/{name}")


def _code(text: str) -> str:
    """Strip comments so a guard cannot be satisfied (or tripped) by prose."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


STANDBY_FILES = ["bb_standby.c", "bb_standby.h", "bb_standby_core.c", "bb_standby_core.h"]


def test_usb_descriptor_patch_and_task_affinity_untouched():
    desc = _src("bb_usb_descriptors.c")
    assert "desc_configuration[bb_la_start + 6] = 0xFF;" in desc
    assert "desc_configuration[bb_la_start + 7] = 0xFF;" in desc
    integ = _src("bb_main_integrated.c")
    assert "vTaskCoreAffinitySet(tud_taskhandle, (1 << 0))" in integ     # USB on Core 0
    assert "vTaskCoreAffinitySet(bb_taskhandle, (1 << 1))" in integ      # commands on Core 1


def test_standby_never_touches_tinyusb_endpoints_or_logs_from_usb_paths():
    banned = ("tud_vendor_n_write_clear", "tud_vendor_n_fifo_clear", "tud_vendor_n_read_flush",
              "tud_vendor_n_rx_reprime", "abort_xfer", "usbd_edpt_", "bb_la_usb_abort_bulk",
              "bb_la_usb_soft_reset", "bb_la_log", "bb_la_stop(", "bb_la_start_stream")
    for name in STANDBY_FILES:
        code = _code(_src(name))
        for token in banned:
            assert token not in code, f"{name} must not use {token}"
    # the USB thread's send path must stay free of the relay log
    send_pending = extract_function(f"{SRC}/bb_la_usb.c", r"^void bb_la_usb_send_pending\(void\)")
    assert "bb_la_log" not in send_pending
    assert "bb_standby" not in send_pending


def test_wire_contract_is_included_never_copied():
    for name in STANDBY_FILES:
        text = _src(name)
        code = _code(text)
        assert not re.search(r"\}\s*bb_standby_(request|reply)_t\s*;", code), f"{name} redefines a wire struct"
        assert "0x7C" not in code and "0x9C" not in code, f"{name} hard-codes a wire opcode"
    for name in ("bb_standby_core.h", "bb_standby.h"):
        assert '#include "standby_wire.h"' in _src(name)
    cmake = read_source(f"{RP}/CMakeLists.txt")
    assert "../DAQ_HAT/common" in cmake
    assert "src/bb_standby_core.c" in cmake and "src/bb_standby.c" in cmake
    # DAP frames are observed without editing the debugprobe submodule
    assert "-Wl,--wrap=DAP_ExecuteCommand" in cmake
    assert "-Wl,--wrap=DAP_ProcessCommand" in cmake
    assert "__wrap_DAP_ExecuteCommand" in _src("bb_standby.c")
    assert "__wrap_DAP_ProcessCommand" in _src("bb_standby.c")


def test_every_hardware_entry_point_is_gated_in_front_of_the_work():
    main = _code(_src("bb_main.c"))
    disp = extract_function(f"{SRC}/bb_main.c", r"^static void dispatch_command\(const HatFrame \*frame\)")
    assert "BB_HAT_CMD_STANDBY" in disp and "bb_standby_handle_frame" in disp
    assert disp.index("bb_standby_cmd_enter") < disp.index("dispatch_command_inner(frame)")
    assert "bb_standby_cmd_leave" in disp and "HAT_ERR_BUSY" in disp
    # the read loop must reach the gate, never the raw dispatcher
    assert re.search(r"\bdispatch_command\(&frame\)", main)
    assert "dispatch_command_inner(&frame)" not in main

    usb = _code(extract_function(f"{SRC}/bb_la_usb.c", r"^static void handle_stream_command\("))
    start = usb[usb.index("case LA_USB_CMD_START_STREAM"):usb.index("case LA_USB_CMD_STOP")]
    assert start.index("bb_standby_work_enter") < start.index("bb_la_start_stream")
    assert start.count("bb_standby_work_leave") == 2          # failed start AND successful start
    assert "START_REJECTED" in start[:start.index("bb_la_start_stream")]   # refused up front like a rejected start
    # STOP stays ungated cleanup
    stop = usb[usb.index("case LA_USB_CMD_STOP"):]
    assert "bb_standby" not in stop

    integ = extract_function(f"{SRC}/bb_main_integrated.c", r"^void tud_unmount_cb\(void\)")
    assert "bb_standby_usb_unmounted" in integ


def test_periodic_power_monitor_stops_in_standby():
    loop = _code(_src("bb_main.c"))
    for m in re.finditer(r"bb_power_update\(\);", loop):
        before = loop[max(0, m.start() - 120):m.start()]
        assert "bb_standby_monitor_allowed()" in before, "bb_power_update() reachable while asleep"
    assert "bb_standby_poll()" in loop and "bb_irq_pulse();" in loop


def test_standby_does_not_use_factory_reset_flash_or_calibration_writes():
    for name in ("bb_standby.c", "bb_standby_core.c"):
        code = _code(_src(name))
        for token in ("bb_hat_v2_handle_reset", "flash_", "ds4424", "bb_hat_v2_set_rail_voltage",
                      "bb_hat_v2_set_io_voltage", "handle_calibrate", "handle_set_rail_enable"):
            assert token not in code, f"{name} must not use {token}"
    restore = extract_function(f"{SRC}/bb_hat_v2.c", r"^bool bb_hat_v2_standby_restore\(void\)")
    for token in ("flash_save", "bb_power_set", "gpio_put", "s_flash_cal.cal", "memcpy", "persist"):
        assert token not in restore, f"standby restore must not use {token}"
    # the only rail enables in the standby binding are OFF (a literal false)
    code = _code(_src("bb_standby.c"))
    for call in re.findall(r"bb_power_set(?:_3v3_adj)?\([^)]*\)", code):
        assert "false" in call, f"standby must only switch rails off: {call}"


def test_leds_stay_dark_through_any_refresh_while_asleep():
    hat = _src("bb_hat_v2.c")
    upd = extract_function(f"{SRC}/bb_hat_v2.c", r"^static void ws2812_update\(void\)")
    assert "s_leds_dark ? 0u : s_ws2812_buffer[i]" in upd
    # every LED write goes through ws2812_update(), the single place that honours the dark flag
    assert hat.count("ws2812_put_pixel(") == 2          # definition + the one use in ws2812_update
    handler = extract_function(f"{SRC}/bb_hat_v2.c", r"^void handle_set_led_state\(")
    assert "ws2812_update()" in handler


def test_rail_ids_and_stage_numbers_track_their_owners():
    glue = _src("bb_standby.c")
    for rail in ("3V3_ADJ", "VADJ3", "VADJ4"):
        assert f"BB_SB_RAIL_{rail} == HAT_RAIL_{rail}" in glue
    core = _src("bb_standby_core.h")
    s3 = read_source("Firmware/ESP32/src/power/standby_policy.h")
    steps = re.search(r"typedef enum \{\s*STANDBY_NO_STEP,(.*?)\} StandbyStep;", s3, re.S).group(1)
    names = [n.strip() for n in steps.split(",") if n.strip()]
    wanted = ["QUIESCE", "HAT_SLEEP", "MUX_OFF", "OUTPUTS_OFF", "ANALOG_OFF", "INDICATORS_OFF",
              "WAKE_SAFE", "ANALOG_ON", "REINITIALIZE", "HAT_WAKE", "INDICATORS_ON"]
    assert [n.replace("STANDBY_", "") for n in names] == wanted
    for i, n in enumerate(wanted, start=1):
        assert re.search(rf"#define BB_SB_STAGE_{n}\s+{i}u\b", core), f"stage {n} must be {i}"
