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
    """A UIKit overlay stacked later in the ZStack swallows the tap."""
    assert SRC.index("TwoFingerPanOverlay") < SRC.index("liveButton"), (
        "TwoFingerPanOverlay must come BEFORE liveButton in the ZStack so the "
        "button is on top and actually receives taps")


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
