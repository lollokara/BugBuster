"""HTTP client behaviour for the routes audited against webserver.cpp/api_core.cpp.

Each test stubs the HTTP seam (``_http_get``/``_http_post``, or the transport's
``requests`` session) and checks the path, the body and how the reply is parsed.
Reply fixtures copy the field names the firmware handler emits (see the handler
named in each section). Path/body-key coverage of every client call lives in
``test_http_route_contract.py``.
"""

from __future__ import annotations

from unittest.mock import MagicMock

import pytest

import bugbuster as bb
from bugbuster.client import AutorunStatus
from bugbuster.ota import OTAClient, OTAError
from bugbuster.transport.http import ADMIN_TOKEN_HEADER, HTTPLogicalError, HTTPTransport
from tests.unit.test_http_route_contract import FW, resolve

TOKEN = "ab" * 32


def _client(get=None, post=None) -> bb.BugBuster:
    client = bb.BugBuster.__new__(bb.BugBuster)
    client._usb = False
    client._admin_token = None
    client._http_get = MagicMock(return_value={} if get is None else get)
    client._http_post = MagicMock(return_value={"ok": True} if post is None else post)
    client._auto_claim_wrap = lambda _ids, fn: fn()
    client._hat_present_cache = True
    client._hat_status_cache = None
    client._t = MagicMock(spec=["get", "post", "delete", "start_dsp_ws_stream", "stop_dsp_ws_stream"])
    return client


def _fw_keys(method: str, path: str) -> set[str]:
    res = resolve(FW, method, path)
    assert res.error == "", res.error
    return res.keys


def _resp(status: int = 200, data=None, text: str = "") -> MagicMock:
    r = MagicMock()
    r.status_code = status
    r.ok = status < 400
    r.reason = "reason"
    r.text = text
    r.url = "http://device/api"
    if isinstance(data, Exception):
        r.json.side_effect = data
    else:
        r.json.return_value = data
    return r


def _transport() -> HTTPTransport:
    t = HTTPTransport("192.0.2.1", admin_token=TOKEN)
    t._session = MagicMock()
    return t


BASE = "http://192.0.2.1:80/api"


# ---------------------------------------------------------------------------
# /api/channel/{n}/...  (handle_channel_post_dispatch / _get_dispatch)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("call, path, body", [
    (lambda c: c.set_channel_function(2, bb.ChannelFunction.VOUT), "/channel/2/function", {"function": 1}),
    (lambda c: c.set_dac_voltage(1, 3.5, bipolar=True), "/channel/1/dac", {"voltage": 3.5, "bipolar": True}),
    (lambda c: c.set_dac_current(3, 12.0), "/channel/3/dac", {"current_mA": 12.0}),
    (lambda c: c.set_dac_code(0, 0x12345), "/channel/0/dac", {"code": 0x2345}),
    (lambda c: c.set_vout_range(1, bb.VoutRange.BIPOLAR), "/channel/1/vout/range", {"bipolar": True}),
    (lambda c: c.set_current_limit(2, bb.CurrentLimit.MA_8), "/channel/2/ilimit", {"limit8mA": True}),
    (lambda c: c.set_current_limit(2, bb.CurrentLimit.MA_25), "/channel/2/ilimit", {"limit8mA": False}),
    (lambda c: c.set_avdd_select(0, bb.AvddSelect.HI), "/channel/0/avdd", {"select": 1}),
    (lambda c: c.set_adc_config(1, rate=bb.AdcRate.SPS_9600), "/channel/1/adc/config",
     {"mux": 0, "range": 0, "rate": 13}),
    (lambda c: c.set_rtd_config(3, bb.RtdCurrent.UA_500), "/channel/3/rtd/config", {"current": 0}),
    (lambda c: c.set_digital_output(1, True), "/channel/1/do/set", {"on": True}),
    (lambda c: c.set_do_config(2, bb.DoMode.PUSH_PULL, src_sel_gpio=True, t1=3, t2=4),
     "/channel/2/do/config", {"mode": 2, "srcSelGpio": True, "t1": 3, "t2": 4}),
    (lambda c: c.set_din_config(0, 100, thresh_mode=False, debounce=2, sink=7),
     "/channel/0/din/config", {"thresh": 100, "threshMode": False, "debounce": 2, "sink": 7,
                               "sinkRange": False, "ocDet": False, "scDet": False}),
])
def test_channel_post_path_and_body(call, path, body):
    client = _client()
    call(client)
    client._http_post.assert_called_once()
    sent_path, sent_body = client._http_post.call_args.args
    assert (sent_path, sent_body) == (path, body)
    assert set(sent_body) <= _fw_keys("POST", "/api" + path)


def test_channel_writes_claim_the_analog_slot():
    client = _client()
    claimed = []
    client._auto_claim_wrap = lambda ids, fn: (claimed.append(ids), fn())
    client.set_dac_voltage(2, 1.0)
    assert claimed == [[14]]


def test_dac_readback_parses_code():
    # handle_get_dac_readback: {"channel", "activeCode", "code"}
    client = _client(get={"channel": 1, "activeCode": 4660, "code": 4660})
    assert client.get_dac_readback(1) == 4660
    client._http_get.assert_called_once_with("/channel/1/dac/readback")


def test_adc_value_parses_api_core_fields():
    # api_channel_adc: {"id", "adcRaw", "adcValue", "adcRange", "adcRate", "adcMux"}
    client = _client(get={"id": 2, "adcRaw": 0x123456, "adcValue": 4.25,
                          "adcRange": 1, "adcRate": 13, "adcMux": 0})
    r = client.get_adc_value(2)
    client._http_get.assert_called_once_with("/channel/2/adc")
    assert (r.raw, r.value, r.range, r.rate, r.mux) == (0x123456, 4.25, 1, 13, 0)


# ---------------------------------------------------------------------------
# /api/diagnostics, /api/diagnostics/config, /api/debug
# ---------------------------------------------------------------------------

def test_set_diag_config_body():
    client = _client()
    client.set_diag_config(2, 13)
    client._http_post.assert_called_once_with("/diagnostics/config", {"slot": 2, "source": 13})


def test_get_diagnostics_parses_slots_reply():
    slots = [{"slot": i, "source": i + 1, "sourceName": "X", "raw": 100 + i,
              "value": 1.5 + i, "unit": "V"} for i in range(4)]
    client = _client(get={"slots": slots})
    out = client.get_diagnostics()
    client._http_get.assert_called_once_with("/diagnostics")
    assert out == [{"slot": i, "source": i + 1, "raw": 100 + i, "value": 1.5 + i} for i in range(4)]


def test_get_debug_info_returns_raw_reply():
    reply = {"i2cBusOk": True, "ds4424": {"present": True}, "husb238": {"present": False},
             "pca9535": {"present": True}, "adcPoll": {"loops": 1, "ready": 1, "rdyIrq": 0}}
    client = _client(get=reply)
    assert client.get_debug_info() == reply
    client._http_get.assert_called_once_with("/debug")


# ---------------------------------------------------------------------------
# /api/adc/dsp/start|stop (handle_post_adc_dsp_start/_stop) - via self._t.post
# ---------------------------------------------------------------------------

def test_adc_dsp_start_body_matches_firmware_keys():
    client = _client()
    client.start_adc_dsp_stream(1, rate=bb.AdcRate.SPS_20, window_samples=256,
                                spike_threshold=0.25, n_fft_peaks=4)
    path, body = client._t.post.call_args.args
    assert path == "/adc/dsp/start"
    assert body == {"channel": 1, "rate": 1, "windowSamples": 256,
                    "spikeThreshold": 0.25, "nFftPeaks": 4}
    assert set(body) == _fw_keys("POST", "/api/adc/dsp/start")
    client._t.start_dsp_ws_stream.assert_not_called()


def test_adc_dsp_start_opens_ws_only_with_callback():
    client = _client()
    client.start_adc_dsp_stream(0, callback=lambda w: None)
    assert client._t.post.call_args.args[1]["rate"] == int(bb.AdcRate.SPS_9600)
    client._t.start_dsp_ws_stream.assert_called_once()


def test_adc_dsp_start_rejection_propagates_and_skips_ws():
    # 409 when the channel is HIGH_IMP; the transport raises HTTPLogicalError.
    client = _client()
    client._t.post.side_effect = HTTPLogicalError("HTTP 409: cannot stream DSP")
    with pytest.raises(HTTPLogicalError):
        client.start_adc_dsp_stream(0, callback=lambda w: None)
    client._t.start_dsp_ws_stream.assert_not_called()


def test_adc_dsp_stop_closes_ws_even_if_post_fails():
    client = _client()
    client._t.post.side_effect = HTTPLogicalError("HTTP 401")
    client.stop_adc_dsp_stream()
    assert client._t.post.call_args.args == ("/adc/dsp/stop",)
    client._t.stop_dsp_ws_stream.assert_called_once()


# ---------------------------------------------------------------------------
# /api/wavegen/start|stop (handle_post_wavegen_start/_stop)
# ---------------------------------------------------------------------------

def test_wavegen_start_body():
    client = _client(post={"status": "started"})
    client.start_waveform(2, bb.WaveformType.SQUARE, 10.0, 2.5, offset=2.5,
                          mode=bb.OutputMode.VOLTAGE)
    path, body = client._http_post.call_args.args
    assert path == "/wavegen/start"
    assert body == {"channel": 2, "waveform": 1, "freq_hz": 10.0, "amplitude": 2.5,
                    "offset": 2.5, "mode": 0}
    assert set(body) == _fw_keys("POST", "/api/wavegen/start")


@pytest.mark.parametrize("kwargs", [
    {"channel": 4}, {"freq_hz": 0.05}, {"freq_hz": 101.0}, {"amplitude": 12.5},
    {"offset": -13.0}, {"amplitude": 26.0, "mode": bb.OutputMode.CURRENT},
])
def test_wavegen_start_rejects_what_the_firmware_rejects(kwargs):
    args = {"channel": 0, "waveform": bb.WaveformType.SINE, "freq_hz": 1.0,
            "amplitude": 1.0, "offset": 0.0, "mode": bb.OutputMode.VOLTAGE}
    args.update(kwargs)
    client = _client()
    with pytest.raises(ValueError):
        client.start_waveform(**args)
    client._http_post.assert_not_called()


def test_wavegen_stop():
    client = _client(post={"status": "stopped"})
    client.stop_waveform()
    client._http_post.assert_called_once_with("/wavegen/stop")


# ---------------------------------------------------------------------------
# HAT: /api/hat/power, /api/hat/v2/* (webserver.cpp HAT v2 handlers)
# ---------------------------------------------------------------------------

def test_hat_get_power_parses_connectors():
    reply = {"connA": {"enabled": True, "currentMa": 12.5, "fault": False},
             "connB": {"enabled": False, "currentMa": 0.0, "fault": True},
             "ioVoltageMv": 3300}
    client = _client(get=reply)
    assert client.hat_get_power() == {
        "connectors": [{"enabled": True, "current_ma": 12.5, "fault": False},
                       {"enabled": False, "current_ma": 0.0, "fault": True}],
        "io_voltage_mv": 3300}
    client._http_get.assert_called_once_with("/hat/power")


def test_hat_set_power_body():
    client = _client(post={"ok": True, "connector": 1, "enabled": True})
    assert client.hat_set_power(1, True) is True
    client._http_post.assert_called_once_with("/hat/power", {"connector": 1, "enable": True})


def test_hat_requires_presence_before_any_request():
    client = _client()
    client._hat_present_cache = False
    with pytest.raises(bb.HatNotPresentError):
        client.hat_set_power(0, True)
    client._http_post.assert_not_called()


def test_hat_get_caps_maps_fields():
    reply = {"hwRevision": 2, "flags": 0x1F, "railCount": 3, "ledCount": 8,
             "shiftedIoCount": 8, "laRouteCount": 2, "fwMajor": 1, "fwMinor": 4}
    client = _client(get=reply)
    assert client.hat_get_caps() == {
        "hw_revision": 2, "flags": 0x1F, "rail_count": 3, "led_count": 8,
        "shifted_io_count": 8, "la_routes": 2, "fw_version": "1.4"}
    client._http_get.assert_called_once_with("/hat/v2/caps")


RAILS_REPLY = {"railCount": 2, "rails": [
    {"railId": 0, "enabled": True, "voltageMv": 3300, "targetVoltageMv": 3300,
     "currentMa": 10, "status": 0},
    {"railId": 1, "enabled": False, "voltageMv": 0, "targetVoltageMv": 5000,
     "currentMa": 0, "status": 2},
]}


def test_hat_get_rail_status_maps_fields():
    client = _client(get=RAILS_REPLY)
    assert client.hat_get_rail_status() == {"count": 2, "rails": [
        {"rail_id": 0, "enabled": True, "voltage_mv": 3300, "current_ma": 10,
         "status": 0, "target_mv": 3300},
        {"rail_id": 1, "enabled": False, "voltage_mv": 0, "current_ma": 0,
         "status": 2, "target_mv": 5000},
    ]}
    client._http_get.assert_called_once_with("/hat/v2/rails")


def test_hat_get_rail_status_surfaces_error_reply():
    client = _client(get={"ok": False, "error": "HAT not responding"})
    with pytest.raises(RuntimeError, match="HAT not responding"):
        client.hat_get_rail_status()


def test_hat_set_rail_voltage_posts_then_rereads_rails():
    client = _client(get=RAILS_REPLY, post={"ok": True, "railId": 1, "voltageMv": 5000})
    out = client.hat_set_rail_voltage(1, 5000.7)
    client._http_post.assert_called_once_with("/hat/v2/rail/voltage", {"railId": 1, "voltageMv": 5000})
    client._http_get.assert_called_once_with("/hat/v2/rails")
    assert out["count"] == 2


@pytest.mark.parametrize("call, path, body, reply, expected", [
    (lambda c: c.hat_set_led_state(3, 2), "/hat/v2/led", {"ledId": 3, "colorCode": 2},
     {"ok": True}, True),
    (lambda c: c.hat_set_io_voltage(1800), "/hat/v2/io_voltage", {"voltageMv": 1800},
     {"ok": True, "voltageMv": 1800}, True),
    (lambda c: c.hat_set_io_bank(0x0F, 0xF0, 0x00, vals=0x05), "/hat/v2/io_bank",
     {"dirs": 0x0F, "ups": 0xF0, "dns": 0x00, "vals": 0x05}, {"ok": True}, True),
    (lambda c: c.hat_set_io_bank(1, 0, 0), "/hat/v2/io_bank",
     {"dirs": 1, "ups": 0, "dns": 0, "vals": 0}, {}, False),
    (lambda c: c.hat_set_level_shift(True, False), "/hat/v2/level_shift",
     {"oe": True, "dir": False}, {"ok": True, "oe": True, "dir": False}, {"oe": True, "dir": False}),
    (lambda c: c.hat_la_log_enable(False), "/hat/v2/la/log/enable", {"enable": False},
     {"ok": True}, True),
])
def test_hat_v2_post(call, path, body, reply, expected):
    client = _client(post=reply)
    assert call(client) == expected
    client._http_post.assert_called_once_with(path, body)
    assert set(body) <= _fw_keys("POST", "/api" + path)


def test_hat_calibrate_status_maps_every_firmware_field():
    reply = {"state": 2, "progress": 50, "railId": 1, "lastError": 0, "persistState": 1,
             "stage": 3, "point": 4, "code": -8, "measuredMv": 5012, "minMv": 1200,
             "maxMv": 15000, "maxGapMv": 300, "maxErrorMv": 12, "validationFlags": 5}
    client = _client(get=reply)
    assert client.hat_calibrate_status() == {
        "state": 2, "progress": 50, "rail_id": 1, "last_error": 0, "persist_state": 1,
        "stage": 3, "point": 4, "code": -8, "measured_mv": 5012, "min_mv": 1200,
        "max_mv": 15000, "max_gap_mv": 300, "max_error_mv": 12, "validation_flags": 5}
    client._http_get.assert_called_once_with("/hat/v2/calibrate/status")


def test_hat_calibrate_import_body_uses_firmware_point_keys():
    client = _client(post={"ok": True})
    pts = [{"dac_code": -8, "measured_v": 3.4}, {"dac_code": 8, "measured_v": 5.1}]
    assert client.hat_calibrate_import(2, pts) is True
    path, body = client._http_post.call_args.args
    assert path == "/hat/v2/calibrate/import"
    assert body == {"railId": 2, "points": [{"dacCode": -8, "measuredV": 3.4},
                                            {"dacCode": 8, "measuredV": 5.1}]}
    fw = _fw_keys("POST", "/api/hat/v2/calibrate/import")
    assert set(body) | set(body["points"][0]) <= fw


def test_hat_calibrate_import_caps_points_at_six():
    client = _client()
    with pytest.raises(ValueError):
        client.hat_calibrate_import(1, [{"dac_code": 0, "measured_v": 1.0}] * 7)
    client._http_post.assert_not_called()


def test_hat_la_set_route_posts_v2_path():
    client = _client(post={"ok": True, "route": 1})
    assert client.hat_la_set_route(1) is True
    client._http_post.assert_called_once_with("/hat/v2/la/route", {"route": 1})


# ---------------------------------------------------------------------------
# /api/idac/cal/clear, /api/ioexp/rail_up, /api/lshift/oe, /api/selftest/calibrate
# ---------------------------------------------------------------------------

def test_idac_cal_clear_body():
    client = _client(post={"ok": True, "status": "cleared"})
    client.idac_cal_clear(2)
    client._http_post.assert_called_once_with("/idac/cal/clear", {"ch": 2})


def test_rail_power_up_body_and_reply():
    reply = {"rail": 2, "appliedV": 4.98, "clamped": False, "pg": True,
             "efuseFaults": [False, True]}
    client = _client(post=reply)
    out = client.rail_power_up(2, 5.0, settle_ms=250, confirm=True, power_cycle=True, efuse_mask=0x7)
    path, body = client._http_post.call_args.args
    assert path == "/ioexp/rail_up"
    assert body == {"rail": 2, "voltage": 5.0, "settleMs": 250, "confirm": True,
                    "powerCycle": True, "efuseMask": 0x3}
    assert set(body) == _fw_keys("POST", "/api/ioexp/rail_up")
    assert out == {"rail": 2, "applied_v": 4.98, "clamped": False, "pg": True,
                   "efuse_faults": [False, True]}


def test_rail_power_up_error_reply_raises():
    client = _client(post={"error": "voltage must be 3-15 V; above 12 V needs confirm"})
    with pytest.raises(ValueError, match="needs confirm"):
        client.rail_power_up(1, 14.0)


@pytest.mark.parametrize("kwargs", [{"rail": 3}, {"rail": 1, "settle_ms": 5001}])
def test_rail_power_up_validates_before_sending(kwargs):
    client = _client()
    with pytest.raises(ValueError):
        client.rail_power_up(voltage=5.0, **kwargs)
    client._http_post.assert_not_called()


@pytest.mark.parametrize("on", [True, False])
def test_level_shifter_oe_body(on):
    client = _client(post={"ok": True, "on": on})
    client.set_level_shifter_oe(on)
    client._http_post.assert_called_once_with("/lshift/oe", {"on": on})


def test_selftest_calibrate_body():
    client = _client(post={"ok": True, "status": 1, "channel": 1, "points": 0,
                           "lastVoltageV": 0.0, "errorMv": 0.0})
    client.selftest_auto_calibrate(1)
    client._http_post.assert_called_once_with("/selftest/calibrate", {"channel": 1})


def test_selftest_calibrate_reply_matches_documented_shape():
    client = _client(post={"ok": True, "status": 1, "channel": 2, "points": 3,
                           "lastVoltageV": 5.0, "errorMv": 1.5})
    out = client.selftest_auto_calibrate(2)
    assert {k: out[k] for k in ("status", "channel", "points", "error_mv")} == {
        "status": 1, "channel": 2, "points": 3, "error_mv": 1.5}


# ---------------------------------------------------------------------------
# /api/quicksetup[/{slot}[/apply|delete]] (api_quicksetup_*)
# ---------------------------------------------------------------------------

def test_quicksetup_list_returns_slots():
    slots = [{"index": i, "occupied": i == 1, "summary": None} for i in range(4)]
    client = _client(get={"slots": slots})
    assert client.quicksetup_list() == slots
    client._http_get.assert_called_once_with("/quicksetup")


def test_quicksetup_get_returns_payload():
    payload = {"name": "bench", "channels": []}
    client = _client(get=payload)
    assert client.quicksetup_get(3) == payload
    client._http_get.assert_called_once_with("/quicksetup/3")


def test_quicksetup_get_transport_error_is_none():
    client = _client()
    client._http_get.side_effect = OSError("down")
    assert client.quicksetup_get(0) is None


def test_quicksetup_get_empty_slot_is_none():
    client = _client(get={"ok": False, "error": "quick setup slot empty"})
    assert client.quicksetup_get(1) is None


def test_quicksetup_save_returns_snapshot():
    snap = {"name": "s", "ts": 1}
    client = _client(post=snap)
    assert client.quicksetup_save(2) == snap
    client._http_post.assert_called_once_with("/quicksetup/2", {})


@pytest.mark.parametrize("reply, expected", [
    ({"ok": True, "applied": True}, {"ok": True, "applied": True}),
    ({"ok": False, "applied": False, "failed": ["ch0"]}, {"ok": False, "applied": False}),
])
def test_quicksetup_apply(reply, expected):
    client = _client(post=reply)
    assert client.quicksetup_apply(1) == expected
    client._http_post.assert_called_once_with("/quicksetup/1/apply", {})


@pytest.mark.parametrize("existed", [True, False])
def test_quicksetup_delete(existed):
    client = _client(post={"ok": True, "deleted": existed})
    assert client.quicksetup_delete(0) == {"ok": True, "deleted": existed}
    client._http_post.assert_called_once_with("/quicksetup/0/delete", {})


# ---------------------------------------------------------------------------
# /api/uart/config, /api/uart/{id}/config, /api/uart/pins
# ---------------------------------------------------------------------------

def test_get_uart_config_maps_bridges():
    reply = {"bridges": [{"id": 1, "uartNum": 2, "txPin": 17, "rxPin": 18, "baudrate": 9600,
                          "dataBits": 7, "parity": 2, "stopBits": 1, "enabled": True,
                          "connected": True}]}
    client = _client(get=reply)
    assert client.get_uart_config() == [{
        "bridge_id": 1, "uart_num": 2, "tx_pin": 17, "rx_pin": 18, "baudrate": 9600,
        "data_bits": 7, "parity": 2, "stop_bits": 1, "enabled": True, "connected": True}]
    client._http_get.assert_called_once_with("/uart/config")


def test_set_uart_config_path_and_body():
    client = _client(post={"ok": True, "id": 1})
    client.set_uart_config(bridge_id=1, uart_num=2, tx_pin=5, rx_pin=6, baudrate=57600,
                           data_bits=7, parity=1, stop_bits=2, enabled=True)
    path, body = client._http_post.call_args.args
    assert path == "/uart/1/config"
    assert body == {"uartNum": 2, "txPin": 5, "rxPin": 6, "baudrate": 57600, "dataBits": 7,
                    "parity": 1, "stopBits": 2, "enabled": True}
    assert set(body) == _fw_keys("POST", "/api/uart/1/config")


def test_get_uart_pins():
    client = _client(get={"available": [1, 2, 38]})
    assert client.get_uart_pins() == [1, 2, 38]
    client._http_get.assert_called_once_with("/uart/pins")


# ---------------------------------------------------------------------------
# /api/scripts/{reset,run-file,autorun/*} (webserver.cpp scripting handlers)
# ---------------------------------------------------------------------------

def test_script_reset():
    client = _client()
    client.script_reset()
    client._http_post.assert_called_once_with("/scripts/reset")


def test_script_run_file_ok():
    client = _client(post={"ok": True, "id": 7})
    st = client.script_run_file("blink.py")
    client._http_post.assert_called_once_with("/scripts/run-file?name=blink.py")
    assert (st.is_running, st.script_id) == (True, 7)


def test_script_run_file_failure_carries_firmware_err():
    client = _client(post={"ok": False, "err": "queue_full_or_file_not_found"})
    with pytest.raises(RuntimeError, match="queue_full_or_file_not_found"):
        client.script_run_file("blink.py")


def test_autorun_status_maps_snake_case_reply():
    client = _client(get={"enabled": True, "has_script": True, "io12_high": False,
                          "last_run_ok": True, "last_run_id": 42})
    assert client.script_autorun_status() == AutorunStatus(True, True, False, True, 42)
    client._http_get.assert_called_once_with("/scripts/autorun/status")


def test_autorun_enable_ok_and_failure():
    client = _client(post={"ok": True})
    client.script_autorun_enable("main.py")
    client._http_post.assert_called_once_with("/scripts/autorun/enable?name=main.py")
    client._http_post.return_value = {"ok": False, "err": "file not found"}
    with pytest.raises(RuntimeError, match="file not found"):
        client.script_autorun_enable("main.py")


def test_autorun_disable_ok_and_failure():
    client = _client(post={"ok": True})
    client.script_autorun_disable()
    client._http_post.assert_called_once_with("/scripts/autorun/disable")
    client._http_post.return_value = {"ok": False, "err": "remove failed"}
    with pytest.raises(RuntimeError, match="remove failed"):
        client.script_autorun_disable()


# ---------------------------------------------------------------------------
# Transport-level routes: /api/pairing/{info,verify}, /api/board/select
# ---------------------------------------------------------------------------

def test_pairing_info_get_sends_token():
    t = _transport()
    info = {"macAddress": "aa:bb:cc:dd:ee:ff", "tokenFingerprint": "0123456789abcdef",
            "transport": "http"}
    t._session.get.return_value = _resp(200, info)
    assert t.get_pairing_info() == info
    args, kwargs = t._session.get.call_args
    assert args == (f"{BASE}/pairing/info",)
    assert kwargs["headers"] == {ADMIN_TOKEN_HEADER: TOKEN}


def test_verify_pairing_accepts_and_caches_token():
    t = _transport()
    t._session.post.return_value = _resp(200, {"ok": True})
    assert t.verify_pairing("cd" * 32) is True
    args, kwargs = t._session.post.call_args
    assert args == (f"{BASE}/pairing/verify",)
    assert kwargs["headers"] == {ADMIN_TOKEN_HEADER: "cd" * 32}
    assert t._admin_token == "cd" * 32


def test_verify_pairing_rejects_401_and_keeps_old_token():
    t = _transport()
    t._session.post.return_value = _resp(401, {"error": "Unauthorized"})
    assert t.verify_pairing("cd" * 32) is False
    assert t._admin_token == TOKEN


def test_verify_pairing_without_token_sends_nothing():
    t = HTTPTransport("192.0.2.1")
    t._session = MagicMock()
    assert t.verify_pairing() is False
    t._session.post.assert_not_called()


def test_set_board_body_and_reply():
    t = _transport()
    t._session.post.return_value = _resp(200, {"active": "esp32c6-devkit", "ok": True})
    assert t.set_board("esp32c6-devkit") == {"active": "esp32c6-devkit", "ok": True}
    args, kwargs = t._session.post.call_args
    assert args == (f"{BASE}/board/select",)
    assert kwargs["json"] == {"boardId": "esp32c6-devkit"}
    assert set(kwargs["json"]) <= _fw_keys("POST", "/api/board/select")


def test_set_board_unknown_id_raises_with_firmware_message():
    t = _transport()
    t._session.post.return_value = _resp(404, {"error": "Unknown boardId"})
    with pytest.raises(HTTPLogicalError, match="Unknown boardId"):
        t.set_board("nope")


# ---------------------------------------------------------------------------
# /api/ota/rollback (handle_post_ota_rollback)
# ---------------------------------------------------------------------------

def test_ota_rollback_ok():
    t = _transport()
    reply = {"success": True, "message": "Rolling back; device rebooting"}
    t._session.post.return_value = _resp(200, reply)
    assert OTAClient(t).rollback() == reply
    args, kwargs = t._session.post.call_args
    assert args == (f"{BASE}/ota/rollback",)
    assert kwargs["headers"] == {ADMIN_TOKEN_HEADER: TOKEN}


def test_ota_rollback_409_means_no_target():
    t = _transport()
    t._session.post.return_value = _resp(409, {"error": "No rollback target available"})
    with pytest.raises(OTAError, match="No rollback target"):
        OTAClient(t).rollback()


def test_ota_rollback_other_error_includes_body():
    t = _transport()
    t._session.post.return_value = _resp(401, {"error": "x"}, text='{"error":"Admin token required"}')
    with pytest.raises(OTAError, match="HTTP 401.*Admin token required"):
        OTAClient(t).rollback()


def test_ota_client_requires_token():
    with pytest.raises(OTAError):
        OTAClient(HTTPTransport("192.0.2.1"))


# ---------------------------------------------------------------------------
# Found while auditing: the HTTP transport cannot carry the scripting routes'
# raw (non-JSON) bodies.
# ---------------------------------------------------------------------------

def _http_bb() -> bb.BugBuster:
    client = bb.BugBuster(_transport())
    client._admin_token = TOKEN
    return client


def test_script_eval_sends_raw_source_body():
    client = _http_bb()
    client._t._session.post.return_value = _resp(200, {"ok": True, "id": 3})
    client.script_eval("print(1)")
    kwargs = client._t._session.post.call_args.kwargs
    assert kwargs.get("data") == b"print(1)"
    assert "json" not in kwargs


def test_script_get_returns_python_text():
    client = _http_bb()
    client._t._session.get.return_value = _resp(200, ValueError("not json"), text="print(1)\n")
    assert client.script_get("a.py") == "print(1)\n"
