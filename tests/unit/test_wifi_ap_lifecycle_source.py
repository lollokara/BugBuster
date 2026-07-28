"""P4 softAP lifecycle must never leave state only a power-cycle can clear."""
from pathlib import Path

SRC = Path("Firmware/DAQ_HAT/ESP32P4/src/link/wifi_ap.c").read_text()


def _body(name: str) -> str:
    marker = f"esp_err_t {name}("
    start = SRC.index(marker)
    nxt = SRC.find("\nesp_err_t ", start + 1)
    if nxt == -1:
        nxt = SRC.find("\nbool ", start + 1)
    if nxt == -1:
        nxt = len(SRC)
    return SRC[start:nxt]


def test_stop_always_clears_s_up_even_when_esp_wifi_stop_fails():
    """The wedge: an errored stop used to leave s_up=true, so the next start
    short-circuited and reported success for an AP that was never up."""
    body = _body("wifi_ap_stop")
    assert "s_up = false" in body
    # s_up must be cleared unconditionally, not guarded behind a success check.
    assert "if (err != ESP_OK) return err;" not in body, \
        "stop still bails out before clearing s_up"


def test_start_does_not_treat_cached_s_up_as_proof_the_ap_is_running():
    body = _body("wifi_ap_start")
    assert "esp_wifi_get_mode" in body, \
        "start must verify against the driver, not trust the cached flag"


def test_start_recovers_from_not_stopped_instead_of_failing_hard():
    body = _body("wifi_ap_start")
    assert "ESP_ERR_WIFI_NOT_STOPPED" in body


def test_each_non_idempotent_init_step_has_its_own_guard():
    """A mid-sequence failure must not replay already-completed steps.
    esp_netif_create_default_wifi_ap() replayed once hard-aborted the board."""
    body = _body("wifi_ap_init")
    for guard in ("s_hosted_ok", "s_netif_ok", "s_wifi_ok", "s_handler_ok"):
        assert f"if (!{guard})" in body, f"missing per-step guard {guard}"
    for guard, call in (("s_hosted_ok", "esp_hosted_init"),
                        ("s_wifi_ok", "esp_wifi_init"),
                        ("s_handler_ok", "esp_event_handler_instance_register")):
        assert body.index(guard) < body.index(call), \
            f"{call} is not guarded by {guard}"


def test_event_handler_registered_at_most_once():
    """Re-registering the same handler stacks duplicate callbacks."""
    body = _body("wifi_ap_init")
    assert "if (!s_handler_ok)" in body


# --- Idle-teardown liveness: association, not just TCP ----------------------

BOARD = Path("Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c")


def test_ap_tracks_associated_station_count():
    """The AP must maintain a station count from the association events.

    Counting only tcp_backend_connected() made a phone that had associated but
    not yet opened the socket look identical to an empty room, so the idle
    timer tore the AP down mid-join.
    """
    src = SRC
    assert "s_sta_count" in src, "no associated-station counter"
    assert "s_sta_count++" in src, "AP_STACONNECTED does not increment the count"
    assert "if (s_sta_count > 0) s_sta_count--;" in src, (
        "AP_STADISCONNECTED must saturate at 0 — an unmatched disconnect would "
        "underflow to ~4e9 and pin the AP 'alive' forever, defeating teardown")
    assert "uint32_t wifi_ap_sta_count(void)" in src, "count is not exposed"


def test_ap_stop_clears_the_station_count():
    """Stale non-zero count would keep the next session's idle timer reset."""
    body = _body("wifi_ap_stop")
    assert "s_sta_count = 0" in body, "wifi_ap_stop must clear the station count"


def test_idle_teardown_treats_association_as_liveness():
    src = BOARD.read_text()
    assert "wifi_ap_sta_count() > 0" in src, (
        "the idle-teardown timer still counts only TCP connections")
