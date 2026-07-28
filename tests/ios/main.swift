// Behavioral tests for DaqLinkStateMachine. Pure Foundation so it runs on the
// host toolchain — no iOS SDK, no simulator. Run via
// tests/unit/test_daq_link_state_machine.py.
import Foundation

var failures = 0

func check(_ cond: Bool, _ what: String, file: StaticString = #file, line: UInt = #line) {
    if !cond {
        failures += 1
        print("FAIL (line \(line)): \(what)")
    }
}

func expect<T: Equatable>(_ got: T, _ want: T, _ what: String, line: UInt = #line) {
    if got != want {
        failures += 1
        print("FAIL (line \(line)): \(what) — got \(got), want \(want)")
    }
}

// --- Happy path -------------------------------------------------------------
do {
    var m = DaqLinkStateMachine()
    expect(m.state, .idle, "starts idle")

    var fx = m.handle(.startRequested)
    expect(m.state, .provisioning(stage: .requested), "start -> provisioning")
    check(fx.contains(.requestProvisioning), "start asks the device to provision")

    _ = m.handle(.stageReported(.ap))
    expect(m.state, .provisioning(stage: .ap), "stage reports flow through")

    fx = m.handle(.credentialsReady)
    expect(m.state, .joiningWiFi, "credentials -> joining")
    check(fx.contains(.joinHotspot), "credentials trigger the hotspot join")

    fx = m.handle(.hotspotJoined)
    expect(m.state, .connecting, "joined -> connecting")
    check(fx.contains(.openSocket), "join opens the socket")
    check(fx.contains(.resetBuffers), "a fresh session clears stale samples")

    fx = m.handle(.socketReady)
    expect(m.state, .streaming, "socket ready -> streaming")
    check(fx.contains(.sendStart), "streaming sends CMD_START")
}

// --- The bug this design exists to prevent ----------------------------------
do {
    // Regression: provisioningState used to stick on .joiningWifi forever
    // because nothing moved it once the socket came up.
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested)
    _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined)
    _ = m.handle(.socketReady)
    check(m.state == .streaming, "no state may strand the UI on 'joining'")
}

// --- Recovery ladder --------------------------------------------------------
do {
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested); _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined);  _ = m.handle(.socketReady)

    // Rung 1: redial, up to maxAttemptsPerRung times.
    for attempt in 1...DaqLinkStateMachine.maxAttemptsPerRung {
        let fx = m.handle(.socketClosed("reset"))
        expect(m.state, .recovering(rung: .redialSocket, attempt: attempt),
               "socket loss redials (attempt \(attempt))")
        check(fx.contains(where: { if case .scheduleRetry = $0 { return true }; return false }),
              "redial is scheduled with backoff, not immediate")
        _ = m.handle(.retryRequested)   // the scheduled retry fires
    }
    // Rung 2: escalate to rejoining the hotspot.
    _ = m.handle(.socketClosed("reset"))
    if case .recovering(let rung, _) = m.state {
        expect(rung, .rejoinHotspot, "exhausted redials escalate to rejoin")
    } else {
        check(false, "expected to still be recovering, got \(m.state)")
    }
}

do {
    // Rung 3 must call recycle — the whole point of Task 6.
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested); _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined);  _ = m.handle(.socketReady)
    var sawRecycle = false
    for _ in 0..<(DaqLinkStateMachine.maxAttemptsPerRung * 4) {
        let fx = m.handle(.socketClosed("reset"))
        if fx.contains(.recycleDevice) { sawRecycle = true }
        _ = m.handle(.retryRequested)
    }
    check(sawRecycle, "the ladder must reach a device recycle before giving up")
}

do {
    // Terminal failure is still recoverable without restarting the app.
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested); _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined);  _ = m.handle(.socketReady)
    // Exhaust the ladder with failures only. Firing .retryRequested here too
    // would trip the terminal state's own escape hatch (failed -> provisioning)
    // and we would never observe .failed at all.
    for _ in 0..<(DaqLinkStateMachine.maxAttemptsPerRung * 5) {
        _ = m.handle(.socketClosed("reset"))
    }
    if case .failed = m.state {
        let fx = m.handle(.retryRequested)
        check(fx.contains(.requestProvisioning), "Retry re-enters the ladder from the top")
        expect(m.state, .provisioning(stage: .requested), "Retry leaves the failed state")
    } else {
        check(false, "ladder must terminate in .failed, got \(m.state)")
    }
}

// --- Watchdog ---------------------------------------------------------------
do {
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested); _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined);  _ = m.handle(.socketReady)
    _ = m.handle(.dataStalled)
    if case .recovering = m.state {} else {
        check(false, "a silent-but-connected link must enter recovery, got \(m.state)")
    }
}

do {
    // A stall while paused is expected, not a fault.
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested); _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined);  _ = m.handle(.socketReady)
    _ = m.handle(.pauseRequested)
    expect(m.state, .paused, "pause is a state, not a disconnect")
    _ = m.handle(.dataStalled)
    expect(m.state, .paused, "no data while paused is not a failure")
}

do {
    // Pause must not tear down the hotspot (bench regression: it did).
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested); _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined);  _ = m.handle(.socketReady)
    let fx = m.handle(.pauseRequested)
    check(fx.contains(.sendStop), "pause stops the device stream")
    check(!fx.contains(.removeHotspotConfig), "pause must NOT drop the hotspot join")
    check(!fx.contains(.closeSocket), "pause must NOT close the socket")
    let rfx = m.handle(.resumeRequested)
    expect(m.state, .streaming, "resume returns to streaming")
    check(rfx.contains(.sendStart), "resume restarts the device stream")
}

do {
    // Only the explicit stop path may remove the hotspot configuration.
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested); _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined);  _ = m.handle(.socketReady)
    let fx = m.handle(.stopRequested)
    expect(m.state, .idle, "stop returns to idle")
    check(fx.contains(.removeHotspotConfig), "stop is the one place the config is removed")
    check(fx.contains(.closeSocket), "stop closes the socket")
}

// --- Every state has a path home -------------------------------------------
do {
    let states: [DaqLinkState] = [
        .idle, .provisioning(stage: .requested), .joiningWiFi, .connecting,
        .streaming, .paused, .recovering(rung: .redialSocket, attempt: 1),
        .failed(reason: "x"),
    ]
    for s in states {
        var m = DaqLinkStateMachine(state: s)
        _ = m.handle(.stopRequested)
        expect(m.state, .idle, "stopRequested from \(s) must reach idle")
    }
}

if failures == 0 {
    print("all DaqLinkStateMachine tests passed")
    exit(0)
} else {
    print("\(failures) failure(s)")
    exit(1)
}
