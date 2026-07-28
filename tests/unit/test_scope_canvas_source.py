"""The Live pill must be reachable and must not sit on the lane labels."""
from pathlib import Path

SRC = Path("iOSApp/Sources/Views/DaqScopeCanvasView.swift").read_text()


def test_live_button_is_not_pinned_to_the_lane_label_corner():
    """topLeading is where laneTag draws the max-value label."""
    assert "liveButton" in SRC
    idx = SRC.index("liveButton")
    window = SRC[max(0, idx - 400):idx + 400]
    assert "bottomTrailing" in window, (
        "Live pill must be explicitly placed away from the top-left lane tag")


def test_live_button_is_above_the_touch_overlay():
    """A UIKit overlay stacked later in the ZStack swallows the tap, so the
    pill must come AFTER the overlay's call site. Anchoring on the bare type
    name would match the struct DECLARATION at the top of the file and pass
    unconditionally."""
    body_start = SRC.index("ZStack(alignment: .topLeading)")
    body = SRC[body_start:]
    overlay = body.index("TwoFingerPanOverlay(")
    pill = body.index("liveButton")
    assert overlay < pill, (
        "TwoFingerPanOverlay must be added BEFORE liveButton in the ZStack so "
        "the pill is topmost and actually receives taps")


def test_pan_overlay_is_not_interaction_disabled():
    """The Live pill is made tappable by ZStack ORDER (topmost wins), never by
    disabling the pan overlay. Killing its hit-testing would take two-finger
    pan-to-scroll with it — the overlay exists precisely to receive touches."""
    assert "TwoFingerPanOverlay" in SRC
    idx = SRC.index("TwoFingerPanOverlay(")
    window = SRC[idx:idx + 500]
    assert "allowsHitTesting(false)" not in window, (
        "disabling the pan overlay's hit-testing breaks two-finger pan; the "
        "Live pill is made tappable by ordering it AFTER the overlay instead")
