"""Coverage for bugbuster_mcp.tools.discovery: reset_link, link_status,
device_status, device_memory, device_info, check_faults, selftest, list_boards,
discover_devices. USB/mDNS scans are patched; nothing touches a real port."""
from __future__ import annotations

import os
import sys
import types
from unittest.mock import patch

import pytest

from bugbuster.client import DeviceInfo
from bugbuster.discovery import DiscoveredDevice, UsbPort
from bugbuster.memory import HeapPool, MemoryStatus, TaskStack
from bugbuster_mcp import session
from bugbuster_mcp.tools import discovery
from tests.unit._mock_client import make_client_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


@pytest.fixture(autouse=True)
def _session_state():
    saved = (session._transport, session._port, session._host, session._vlogic,
             session._admin_token, session._active_board)
    session.configure(transport="usb", port="COM6", vlogic=3.3)
    yield
    (session._transport, session._port, session._host, session._vlogic,
     session._admin_token, session._active_board) = saved


@pytest.fixture
def tools():
    mcp = _DummyMCP()
    discovery.register(mcp)
    return mcp.tools


@pytest.fixture
def bb():
    c = make_client_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=c):
        yield c


# ---------------------------------------------------------------------------
# link tools
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("after,needle", [(True, "Link healthy."), (False, "still unhealthy"),
                                          (None, "still unhealthy")])
def test_reset_link_message(tools, after, needle):
    with patch("bugbuster_mcp.session.reconnect",
               return_value={"method": "transport_reconnect", "healthy_before": False,
                             "healthy_after": after}) as rc:
        res = tools["reset_link"]()
    rc.assert_called_once_with()
    assert res["method"] == "transport_reconnect" and needle in res["message"]


def test_reset_link_propagates_reconnect_failure(tools):
    with patch("bugbuster_mcp.session.reconnect", side_effect=OSError("port gone")):
        with pytest.raises(OSError):
            tools["reset_link"]()


def test_link_status_shape(tools):
    with patch("bugbuster_mcp.session.link_healthy", return_value=False):
        assert tools["link_status"]() == {"transport": "usb", "port": "COM6", "healthy": False}


# ---------------------------------------------------------------------------
# device_status / device_info
# ---------------------------------------------------------------------------

def test_device_status_collects_everything(tools, bb):
    bb.get_status.return_value = {"spi_ok": True}
    bb.power_get_status.return_value = {"vadj1_en": False}
    bb.hat_get_status.return_value = {"detected": True}
    assert tools["device_status"]() == {"device": {"spi_ok": True}, "power": {"vadj1_en": False},
                                        "hat": {"detected": True}, "transport": "usb"}


def test_device_status_isolates_each_failure(tools, bb):
    bb.get_status.side_effect = TimeoutError("t1")
    bb.power_get_status.side_effect = RuntimeError("pca")
    bb.hat_get_status.side_effect = NotImplementedError("usb only")
    session.configure(transport="http", host="10.0.0.2")
    res = tools["device_status"]()
    assert res == {"device": {"error": "t1"}, "power": {"error": "pca"},
                   "hat": {"error": "usb only"}, "transport": "http"}


def test_device_info(tools, bb):
    bb.get_device_info.return_value = DeviceInfo(True, 3, 0x1234, 0x5678)
    bb.get_firmware_version.return_value = (4, 2, 17)
    assert tools["device_info"]() == {"spi_ok": True, "silicon_rev": 3, "silicon_id0": 0x1234,
                                      "silicon_id1": 0x5678, "firmware_version": "4.2.17",
                                      "transport": "usb"}


def test_device_info_error_propagates(tools, bb):
    bb.get_device_info.side_effect = TimeoutError("no reply")
    with pytest.raises(TimeoutError):
        tools["device_info"]()


# ---------------------------------------------------------------------------
# device_memory
# ---------------------------------------------------------------------------

def _mem(psram_total=0, internal_free=100_000, task_free=1000):
    return MemoryStatus(
        internal=HeapPool(free_bytes=internal_free, min_ever_bytes=internal_free,
                          largest_block_bytes=internal_free // 2, total_bytes=300_000),
        psram=HeapPool(free_bytes=psram_total // 2, min_ever_bytes=0, largest_block_bytes=psram_total // 4,
                       total_bytes=psram_total),
        uptime_ms=12345,
        tasks=[TaskStack("bbp", 4096, task_free), TaskStack("idle", 1024, 10, running=False)],
    )


def test_device_memory_healthy_without_psram(tools, bb):
    bb.get_memory_status.return_value = _mem()
    res = tools["device_memory"]()
    assert res["internal"] == {"free_bytes": 100_000, "min_ever_bytes": 100_000,
                               "largest_block_bytes": 50_000, "total_bytes": 300_000,
                               "used_pct": 66.7, "fragmentation_pct": 50.0}
    assert res["psram"] is None
    assert res["tasks"][0] == {"name": "bbp", "declared_bytes": 4096, "free_bytes": 1000,
                               "peak_used_bytes": 3096, "used_pct": 75.6, "running": True}
    assert res["warnings"] == [] and res["healthy"] is True
    assert res["uptime_ms"] == 12345 and res["transport"] == "usb"
    assert res["summary"].startswith("internal ")


def test_device_memory_pressure_and_psram(tools, bb):
    bb.get_memory_status.return_value = _mem(psram_total=8_000_000, internal_free=10_000, task_free=100)
    res = tools["device_memory"]()
    assert res["psram"]["total_bytes"] == 8_000_000
    assert res["healthy"] is False
    assert any("bbp" in w for w in res["warnings"])
    assert any("internal free" in w for w in res["warnings"])


def test_device_memory_error_is_returned_not_raised(tools, bb):
    bb.get_memory_status.side_effect = NotImplementedError("old firmware")
    session.configure(transport="http", host="10.0.0.2")
    assert tools["device_memory"]() == {"error": "old firmware", "transport": "http"}


# ---------------------------------------------------------------------------
# check_faults
# ---------------------------------------------------------------------------

def _clean(bb):
    bb.get_faults.return_value = {"alert_status": 0, "supply_alert_status": 0, "channels": []}
    bb.power_get_status.return_value = {}
    bb.power_get_fault_log.return_value = []


def test_check_faults_clean(tools, bb):
    _clean(bb)
    assert tools["check_faults"]() == {"has_faults": False, "faults": ["No active faults."], "fault_log": []}


def test_check_faults_ad74416h_alerts(tools, bb):
    _clean(bb)
    bb.get_faults.return_value = {"alert_status": 0x10, "supply_alert_status": 0x2,
                                  "channels": [{"id": 0, "alert": 0}, {"alert": 0x8}]}
    res = tools["check_faults"]()
    assert res["has_faults"] is True
    assert res["faults"] == ["AD74416H global alert: 0x0010", "AD74416H supply alert: 0x0002",
                             "Channel 1 alert: 0x0008"]


def test_check_faults_only_supply_alert(tools, bb):
    _clean(bb)
    bb.get_faults.return_value = {"supply_alert_status": 0x40}
    assert tools["check_faults"]()["faults"] == ["AD74416H supply alert: 0x0040"]


def test_check_faults_efuse_and_power_good(tools, bb):
    _clean(bb)
    bb.power_get_status.return_value = {
        "efuse_faults": [False, True, False, False],
        "vadj1_en": True, "vadj1_pg": False,
        "vadj2_en": False, "vadj2_pg": False,      # off rail: no false alarm
    }
    res = tools["check_faults"]()
    assert res["has_faults"] is True
    assert len(res["faults"]) == 2
    assert res["faults"][0].startswith("E-fuse 2 tripped")
    assert "VADJ1 is enabled but power-good is lost" in res["faults"][1]


def test_check_faults_logged_trip_on_disabled_fuse(tools, bb):
    _clean(bb)
    bb.power_get_status.return_value = {"efuse_enables": [True, True, False, True],
                                        "efuse_faults": [False] * 4}
    bb.power_get_fault_log.return_value = [{"type": 0, "channel": 2}, {"type": 0, "channel": 0},
                                           {"type": 1, "channel": 3}, {"type": 0, "channel": 9}]
    res = tools["check_faults"]()
    assert res["has_faults"] is True
    assert res["faults"] == ["E-fuse 3 tripped and was auto-disabled (IO_Block 3 overcurrent). "
                             "Remove the overload, then re-enable it."]
    assert len(res["fault_log"]) == 4


def test_check_faults_read_errors_are_reported(tools, bb):
    bb.get_faults.side_effect = TimeoutError("spi")
    bb.power_get_status.side_effect = RuntimeError("pca")
    bb.power_get_fault_log.side_effect = RuntimeError("log")
    res = tools["check_faults"]()
    assert res["has_faults"] is False
    assert res["fault_log"] == []
    assert res["faults"] == ["Could not read AD74416H faults: spi", "Could not read power status: pca",
                             "Could not fetch fault log (degraded state): log", "No active faults."]


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------

def _healthy_selftest(bb):
    bb.selftest_status.return_value = {"boot": {"ran": True, "passed": True}}
    bb.selftest_internal_supplies.return_value = {"valid": True, "supplies_ok": True}
    bb.selftest_supplies_cached.return_value = {"vadj1": 5.0}


def test_selftest_all_pass(tools, bb):
    _healthy_selftest(bb)
    res = tools["selftest"]()
    assert res["all_pass"] is True and res["warnings"] == []
    assert res["summary"] == "All self-tests passed."
    assert res["supply_voltages_cached"] == res["efuse_currents"] == {"vadj1": 5.0}


def test_selftest_boot_not_run_is_warning_only(tools, bb):
    _healthy_selftest(bb)
    bb.selftest_status.return_value = {"boot": None}
    res = tools["selftest"]()
    assert res["all_pass"] is True and res["warnings"] == ["Boot self-test has not run."]


def test_selftest_invalid_supplies(tools, bb):
    _healthy_selftest(bb)
    bb.selftest_internal_supplies.return_value = {"valid": False}
    res = tools["selftest"]()
    assert res["all_pass"] is False
    assert "Internal supply measurement is not valid." in res["summary"]


def test_selftest_out_of_range_lists_both_key_styles(tools, bb):
    _healthy_selftest(bb)
    bb.selftest_internal_supplies.return_value = {"valid": True, "suppliesOk": False,
                                                  "avddHiV": 21.5, "dvcc_v": 5.01, "avssV": "n/a"}
    res = tools["selftest"]()
    assert res["all_pass"] is False
    assert res["warnings"] == ["AD74416H internal supplies out of range: dvcc_v=5.01, avddHiV=21.50"]
    assert res["summary"].startswith("Self-test issues found: AD74416H")


def test_selftest_every_read_failing(tools, bb):
    bb.selftest_status.side_effect = TimeoutError("a")
    bb.selftest_internal_supplies.side_effect = RuntimeError("b")
    bb.selftest_supplies_cached.side_effect = RuntimeError("c")
    res = tools["selftest"]()
    assert res["boot_test"] == {"error": "a"} and res["supplies"] == {"error": "b"}
    assert res["supply_voltages_cached"] == res["efuse_currents"] == {"error": "c"}
    assert res["warnings"] == ["Could not read boot test status: a", "Could not measure supplies: b"]
    # read failures alone do not flip all_pass
    assert res["all_pass"] is True


# ---------------------------------------------------------------------------
# list_boards
# ---------------------------------------------------------------------------

def test_list_boards_matches_profile_dir(tools):
    pdir = os.path.join(os.path.dirname(discovery.__file__), "..", "board_profiles")
    expected = sorted(f[:-5] for f in os.listdir(pdir) if f.endswith(".json"))
    assert sorted(tools["list_boards"]()) == expected


def test_list_boards_missing_dir(tools):
    with patch("os.path.exists", return_value=False):
        assert tools["list_boards"]() == []


def test_list_boards_filters_non_json(tools):
    with patch("os.path.exists", return_value=True), \
         patch("os.listdir", return_value=["a.json", "README.md", "b.json", "c.json.bak"]):
        assert tools["list_boards"]() == ["a", "b"]


# ---------------------------------------------------------------------------
# set_board (path validation is in test_mcp_set_board_name.py)
# ---------------------------------------------------------------------------

def test_set_board_missing_profile(tools):
    with patch("bugbuster_mcp.session.set_active_board") as sab:
        res = tools["set_board"]("no_such_board_xyz")
    assert res.startswith("Error: Board profile 'no_such_board_xyz' not found")
    sab.assert_not_called()


@pytest.mark.parametrize("profile,needle", [
    ({"description": "ESP32-C6 devkit"}, "Board profile set to 'dut' (ESP32-C6 devkit)"),
    ({"name": "dut"}, "(No description)"),
    (None, "Error: Failed to load board profile 'dut'."),
])
def test_set_board_outcomes(tools, profile, needle):
    with patch("os.path.exists", return_value=True), \
         patch("bugbuster_mcp.session.set_active_board") as sab, \
         patch("bugbuster_mcp.session.get_active_board_profile", return_value=profile):
        res = tools["set_board"]("dut")
    sab.assert_called_once_with("dut")
    assert needle in res


# ---------------------------------------------------------------------------
# discover_devices
# ---------------------------------------------------------------------------

_PORTS = [
    UsbPort("COM6", vid=0x303A, pid=0x4002, description="BugBuster", interface_index=0),
    UsbPort("COM5", vid=0x303A, pid=0x4002, description="BugBuster", interface_index=2),
    UsbPort("COM1", description="Comm port"),
]
_DEV = DiscoveredDevice("bugbuster-a1b2c3", "10.0.0.9", 8080, firmware="4.0.0", mac="AA", proto="4",
                        model="bugbuster-s3")


def test_discover_usb_and_network(tools):
    with patch("bugbuster.discovery.list_usb_ports", return_value=_PORTS) as lp, \
         patch("bugbuster.discovery.discover_mdns", return_value=[_DEV]) as dm:
        res = tools["discover_devices"](timeout_s=3)
    lp.assert_called_once_with(all_ports=True)
    dm.assert_called_once_with(timeout=3.0)
    assert res["usb_ports"][0] == {"device": "COM6", "vid_pid": "303A:4002", "description": "BugBuster",
                                   "interface": 0, "is_bugbuster": True}
    assert res["usb_ports"][2]["vid_pid"] is None and res["usb_ports"][2]["is_bugbuster"] is False
    assert res["likely_bbp_port"] == "COM6"
    assert res["active_transport"] == "usb" and res["active_port"] == "COM6"
    assert res["count"] == 1
    assert res["devices"] == [{"hostname": "bugbuster-a1b2c3", "fqdn": "bugbuster-a1b2c3.local",
                               "ip": "10.0.0.9", "port": 8080, "firmware": "4.0.0", "mac": "AA",
                               "proto": "4", "model": "bugbuster-s3",
                               "http_base": "http://bugbuster-a1b2c3.local:8080"}]


def test_discover_usb_only_no_bugbuster(tools):
    with patch("bugbuster.discovery.list_usb_ports", return_value=_PORTS[2:]), \
         patch("bugbuster.discovery.discover_mdns") as dm:
        res = tools["discover_devices"](network=False)
    dm.assert_not_called()
    assert res["likely_bbp_port"] is None and "devices" not in res


def test_discover_usb_scan_error_is_reported(tools):
    with patch("bugbuster.discovery.list_usb_ports", side_effect=OSError("access denied")):
        res = tools["discover_devices"](network=False)
    assert res["usb_error"] == "access denied"
    assert res["active_transport"] == "usb"


def test_discover_network_only(tools):
    with patch("bugbuster.discovery.list_usb_ports") as lp, \
         patch("bugbuster.discovery.discover_mdns", return_value=[]):
        res = tools["discover_devices"](usb=False)
    lp.assert_not_called()
    assert res == {"count": 0, "devices": []}


def test_discover_network_import_error_branch(tools):
    stub = types.ModuleType("bugbuster.discovery")
    with patch.dict(sys.modules, {"bugbuster.discovery": stub}):
        res = tools["discover_devices"](usb=False)
    assert res["devices"] == [] and "bugbuster[network]" in res["hint"] and res["network_error"]


def test_discover_without_zeroconf_degrades_gracefully(tools):
    with patch("bugbuster.discovery.list_usb_ports", return_value=_PORTS), \
         patch("bugbuster.discovery.discover_mdns", side_effect=ImportError("zeroconf is not installed")):
        res = tools["discover_devices"]()
    assert res["likely_bbp_port"] == "COM6"
    assert res["devices"] == [] and "network_error" in res
