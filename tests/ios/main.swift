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
        expect(m.state, .recovering(rung: .redialSocket, attempt: attempt, wasPaused: false),
               "socket loss redials (attempt \(attempt))")
        check(fx.contains(where: { if case .scheduleRetry = $0 { return true }; return false }),
              "redial is scheduled with backoff, not immediate")
        _ = m.handle(.retryRequested)   // the scheduled retry fires
    }
    // Rung 2: escalate to rejoining the hotspot.
    _ = m.handle(.socketClosed("reset"))
    if case .recovering(let rung, _, _) = m.state {
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
        _ = m.handle(.socketClosed("reset"))
        // .recycleDevice is fired by performCurrentRung (via .retryRequested)
        // once the ladder lands on .recycleDevice, not eagerly by escalate()
        // — escalate() firing it too used to recycle the device twice, ~1s
        // apart, for every entry onto that rung (finding: Important 8).
        let fx = m.handle(.retryRequested)
        if fx.contains(.recycleDevice) { sawRecycle = true }
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
        .streaming, .paused, .recovering(rung: .redialSocket, attempt: 1, wasPaused: false),
        .failed(reason: "x"),
    ]
    for s in states {
        var m = DaqLinkStateMachine(state: s)
        _ = m.handle(.stopRequested)
        expect(m.state, .idle, "stopRequested from \(s) must reach idle")
    }
}

// --- Critical 1 regression: recovering must not swallow join/provision failures
do {
    // hotspotJoinFailed while .recovering (e.g. rung .rejoinHotspot fires
    // .joinHotspot and the phone can't see the AP) used to hit `default:
    // return []` — no state change, no retry, no escalation: permanently
    // wedged with no path back except an app restart / power-cycle.
    var m = DaqLinkStateMachine(state: .recovering(rung: .rejoinHotspot, attempt: 1, wasPaused: false))
    let fx = m.handle(.hotspotJoinFailed("no AP in range"))
    check(!fx.isEmpty, "hotspotJoinFailed while recovering must not be silently swallowed")
    if case .recovering = m.state {} else {
        check(false, "hotspotJoinFailed while recovering must still be recovering (or failed), got \(m.state)")
    }
}

do {
    // provisioningFailed while .recovering (rung .recycleDevice fires
    // .requestProvisioning, which can time out) — same wedge.
    var m = DaqLinkStateMachine(state: .recovering(rung: .recycleDevice, attempt: 1, wasPaused: false))
    let fx = m.handle(.provisioningFailed("provisioning timed out"))
    check(!fx.isEmpty, "provisioningFailed while recovering must not be silently swallowed")
    if case .recovering = m.state {} else {
        check(false, "provisioningFailed while recovering must still be recovering (or failed), got \(m.state)")
    }
}

// --- Important 5 regression: a paused link that recovers must stay paused --
do {
    var m = DaqLinkStateMachine()
    _ = m.handle(.startRequested); _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined);  _ = m.handle(.socketReady)
    _ = m.handle(.pauseRequested)
    expect(m.state, .paused, "paused before the outage")

    // Socket dies while paused -> quietly recovers via the ladder.
    _ = m.handle(.socketClosed("reset"))
    if case .recovering(_, _, let wasPaused) = m.state {
        check(wasPaused, "recovering from a paused link must remember it was paused")
    } else {
        check(false, "expected .recovering after a paused link lost its socket, got \(m.state)")
    }

    // Drive it through the rejoin-hotspot rung's full round trip (credentials
    // -> hotspot join -> socket ready) to make sure wasPaused survives every
    // intermediate hop, not just the first one.
    _ = m.handle(.retryRequested)          // fires .openSocket on the redial rung
    _ = m.handle(.socketClosed("reset again"))   // escalate to rejoinHotspot eventually
    for _ in 0..<(DaqLinkStateMachine.maxAttemptsPerRung) {
        _ = m.handle(.retryRequested)
        _ = m.handle(.socketClosed("still down"))
    }
    if case .recovering(let rung, _, let wasPaused) = m.state {
        expect(rung, .rejoinHotspot, "escalated to the rejoin-hotspot rung")
        check(wasPaused, "wasPaused must survive escalation across rungs")
    } else {
        check(false, "expected still .recovering, got \(m.state)")
    }
    _ = m.handle(.credentialsReady)
    _ = m.handle(.hotspotJoined)
    let fx = m.handle(.socketReady)
    expect(m.state, .paused, "a paused link that recovers must land back in .paused, not .streaming")
    check(!fx.contains(.sendStart), "must NOT send CMD_START behind the user's back on a silent recovery")
}

// --- Property test: no event may be silently swallowed while recovering ----
// This is the test class that would have caught Critical 1: for EVERY event,
// from EVERY (rung, attempt) combination `.recovering` can be in, the machine
// must either change state or emit at least one effect. Nothing may vanish
// into `default: return []`.
do {
    // DaqLinkEvent has associated values, so it can't just be `CaseIterable`
    // without adding that conformance to production code for a test's sake.
    // Enumerate representative instances of every case here instead.
    //
    // Excluded on purpose (documented no-ops while recovering, not bugs):
    // `.stageReported` (provisioning-stage progress; recovery re-provisioning
    // reports through the same BLE channel but the ladder doesn't render
    // per-stage progress, only the rung/attempt), `.dataStalled` (the
    // watchdog only runs while `.streaming`, so this can't fire while
    // recovering in practice), `.pauseRequested`/`.resumeRequested` (no
    // transport exists to pause/resume yet while the ladder itself is still
    // trying to reconnect one), and `.startRequested` (there's already a live
    // session; a second start request while recovering has nothing new to
    // do). All of these hit `default: return []` deliberately, not because
    // they were forgotten — unlike `.provisioningFailed`/`.hotspotJoinFailed`
    // (Critical 1), which are genuine failure/completion signals that must
    // always move the ladder forward.
    let allEvents: [DaqLinkEvent] = [
        .credentialsReady,
        .provisioningFailed("x"),
        .hotspotJoined,
        .hotspotJoinFailed("x"),
        .socketReady,
        .socketClosed("x"),
        .stopRequested,
        .retryRequested,
    ]

    for rung in DaqRecoveryRung.allCases {
        for attempt in 1...DaqLinkStateMachine.maxAttemptsPerRung {
            for wasPaused in [false, true] {
                for event in allEvents {
                    let startState: DaqLinkState = .recovering(rung: rung, attempt: attempt, wasPaused: wasPaused)
                    var m = DaqLinkStateMachine(state: startState)
                    let fx = m.handle(event)
                    let changed = m.state != startState
                    check(changed || !fx.isEmpty,
                          "event \(event) from .recovering(rung: \(rung), attempt: \(attempt), wasPaused: \(wasPaused)) was silently swallowed (no state change, no effects)")
                }
            }
        }
    }
}

runScopeAxisTests()

if failures == 0 {
    print("all DaqLinkStateMachine + ScopeAxis tests passed")
    exit(0)
} else {
    print("\(failures) failure(s)")
    exit(1)
}
