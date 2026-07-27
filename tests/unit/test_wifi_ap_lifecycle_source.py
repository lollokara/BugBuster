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
