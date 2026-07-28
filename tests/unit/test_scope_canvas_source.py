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


import re

ROOT = Path("iOSApp/Sources/Views/iPadRootView.swift")


def test_sidebar_auto_collapses_after_idle():
    src = ROOT.read_text()
    assert "columnVisibility" in src, "NavigationSplitView has no visibility binding to drive"
    # Anchor on the actual declaration, not incidental digits elsewhere in the
    # file: this must fail if the timeout value is changed to anything else.
    match = re.search(
        r"sidebarIdleTimeout\s*:\s*TimeInterval\s*=\s*(\d+)", src)
    assert match is not None, "sidebarIdleTimeout must be declared as a TimeInterval constant"
    assert match.group(1) == "15", "idle timeout must be 15s"
    # The visibility state must actually be bound into the NavigationSplitView
    # constructor, not just declared and left unused.
    assert re.search(r"NavigationSplitView\(columnVisibility:\s*\$columnVisibility\)", src), (
        "columnVisibility must be bound into the NavigationSplitView constructor")
    # The collapse action must actually flip the binding to .detailOnly.
    assert re.search(r"columnVisibility\s*=\s*\.detailOnly", src), (
        "collapse must set columnVisibility to .detailOnly")


CONN = Path("iOSApp/Sources/Services/ConnectionManager.swift")
VDUT_CARD = Path("iOSApp/Sources/Views/VDUTControlsCard.swift")


def test_vdut_state_is_prefetched_not_fetched_on_open():
    src = CONN.read_text()
    assert "startVdutPrefetch" in src, "no background VDUT prefetch"
    assert "vdutPrefetchTask" in src
    # The prefetch loop must actually be wired to run once the connection is
    # live, not just declared and left dangling — otherwise it never fires
    # and the card is back to relying on an open-time fetch in practice.
    assert re.search(r"func startPolling\(\)\s*\{\s*startVdutPrefetch\(\)", src), (
        "startVdutPrefetch() must be started when polling begins (i.e. once "
        "connected), or the background prefetch never actually runs")


def test_vdut_prefetch_is_also_started_over_ble():
    """BLE is the primary transport while the DAQ stream is running (the phone
    can't reach the S3 over HTTP once joined to the DAQ hotspot), so wiring
    the prefetch into startPolling() alone leaves BLE-connected users with
    exactly the stale-setpoint bug this task fixes, with the suite still
    green. Must be covered independently of the WiFi-path assertion above."""
    src = CONN.read_text()
    assert re.search(r"func startBLEPolling\(\)\s*\{\s*startVdutPrefetch\(\)", src), (
        "startVdutPrefetch() must also be started when BLE polling begins, "
        "or BLE-connected users still see stale VDUT setpoints on open")


def _extract_braced_block(src: str, anchor_pattern: str) -> str:
    """Find `anchor_pattern`, then brace-match from the next '{' to return the
    exact extent of that block, however long it is. Robust to reformatting
    and to the anchor appearing more than once elsewhere, since we only ever
    match this specific function signature."""
    match = re.search(anchor_pattern, src)
    assert match is not None, f"could not find {anchor_pattern!r} in source"
    start = src.index("{", match.end() - 1)
    depth = 0
    for i, ch in enumerate(src[start:], start=start):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[start:i]
    raise AssertionError("unbalanced braces while scanning block")


def test_vdut_prefetch_backs_off_on_transport_degraded_and_daq_stream_busy():
    """startPolling() deliberately backs off to a probe-only cadence and skips
    its slow multi-request cycle once transportDegraded is set ("so we don't
    pile more requests onto a wedged httpd"), and separately skips its body
    entirely while a DAQ stream is busy bringing up / recovering. The VDUT
    prefetch loop must respect both, or it keeps hammering a wedged transport
    / competes for the single serialized request slot exactly when it's
    scarcest (DAQ wifi-stream provisioning and recovery)."""
    src = CONN.read_text()
    body = _extract_braced_block(src, r"func startVdutPrefetch\(\)\s*\{")

    assert "transportDegraded" in body, (
        "the prefetch loop must gate on transportDegraded, mirroring "
        "startPolling()'s circuit breaker, or it keeps firing at a wedged "
        "transport")
    assert "DaqWifiStreamManager" in body and "linkState" in body, (
        "the prefetch loop must check DaqWifiStreamManager.shared.linkState "
        "so it backs off during DAQ wifi-stream bring-up/recovery, not just "
        "ScopeStreamManager.isStreaming (too narrow: that only covers the "
        "SSE scope stream, not the DAQ hotspot link)")
    # The gate must actually guard the refreshVdutStatus() call, not just be
    # present somewhere unrelated in the loop body.
    refresh_idx = body.index("refreshVdutStatus")
    guard_region = body[:refresh_idx]
    assert "transportDegraded" in guard_region and "linkState" in guard_region, (
        "the transportDegraded/linkState checks must guard the "
        "refreshVdutStatus() call itself, not merely appear elsewhere in the loop")


def test_vdut_prefetch_treats_only_transitional_daq_states_as_busy():
    """The distinction that matters is TRANSITIONAL vs STEADY-STATE, not
    busy-vs-not-busy. Bring-up (.provisioning/.joiningWiFi/.connecting) and
    .recovering genuinely contend for the single serialized command-channel
    slot and resolve in seconds. A fully-established .streaming/.paused link
    does NOT contend — the high-rate data rides its own TCP socket by then —
    and treating it as busy would suspend VDUT prefetch for the entire
    (indefinite) duration of a live capture, exactly when VDUTControlsCard is
    most likely to be opened (it's reached from ScopeTab's settings sheet,
    opened while watching a stream). This asserts the actual state
    partition, not just that `linkState` is referenced somewhere — a
    too-broad gate (e.g. "busy unless .idle/.failed") passes the weaker
    check in test_vdut_prefetch_backs_off_on_transport_degraded_and_daq_stream_busy
    but must fail here."""
    src = CONN.read_text()
    fn_body = _extract_braced_block(src, r"func startVdutPrefetch\(\)\s*\{")
    switch_body = _extract_braced_block(
        fn_body, r"switch\s+DaqWifiStreamManager\.shared\.linkState\s*\{")

    # Parse each `case A, B, C:` clause and the boolean it assigns, so the
    # test reflects the actual partition rather than assuming a variable name.
    partition: dict[str, bool] = {}
    for case_list, value in re.findall(
            r"case\s+([^:{]+):\s*\n?\s*\w+\s*=\s*(true|false)", switch_body):
        is_busy = value == "true"
        for state in case_list.split(","):
            partition[state.strip()] = is_busy

    transitional = {".provisioning", ".joiningWiFi", ".connecting", ".recovering"}
    steady_or_idle = {".idle", ".streaming", ".paused", ".failed"}

    missing = transitional - partition.keys()
    assert not missing, f"transitional DAQ states not handled in the gate: {missing}"
    assert all(partition[s] for s in transitional), (
        "all transitional DAQ link states (.provisioning/.joiningWiFi/"
        ".connecting/.recovering) must be treated as busy — they contend for "
        "the single serialized command-channel request slot")

    missing2 = steady_or_idle - partition.keys()
    assert not missing2, f"steady/idle DAQ states not handled in the gate: {missing2}"
    assert not partition[".streaming"], (
        ".streaming must NOT be treated as busy — the high-rate data rides "
        "its own TCP socket by then, not the command channel, so gating on "
        "it would suspend VDUT prefetch for the entire duration of a live "
        "capture (the exact scenario VDUTControlsCard is opened in)")
    assert not partition[".paused"], ".paused is steady-state, not transitional"


def test_vdut_card_does_not_depend_on_an_open_time_fetch():
    """The card must render already-prefetched ConnectionManager state, not
    kick off its own fetch when it opens — that is what makes the menu show
    stale setpoints that snap a moment later. Anchor on the onAppear BLOCK
    itself (brace-matched), not a fixed-width slice after a fixed string:
    a slice would silently examine the wrong region if the file gains
    another `.onAppear`, and would raise IndexError if it gains none."""
    src = VDUT_CARD.read_text()
    match = re.search(r"\.onAppear\s*\{", src)
    assert match is not None, "VDUTControlsCard must seed its drafts in onAppear"

    # Brace-match from the opening '{' to find the exact extent of the
    # onAppear closure, however long it is.
    start = match.end() - 1
    depth = 0
    end = None
    for i, ch in enumerate(src[start:], start=start):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    assert end is not None, "unbalanced braces while scanning onAppear block"
    onAppear_block = src[start:end]

    assert "refreshVdutStatus" not in onAppear_block, (
        "the card must render already-prefetched values, not kick off the fetch "
        "when it opens — that is what makes the menu show stale setpoints")
