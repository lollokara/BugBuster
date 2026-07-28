import Foundation

// =============================================================================
// DaqLinkState.swift — the DAQ WiFi link's single source of truth.
//
// Deliberately Foundation-only (no Network/UIKit/SwiftUI imports) for two
// reasons: it can be unit-tested on the host toolchain without a simulator,
// and it cannot accidentally grow I/O. All side effects are returned as
// DaqLinkEffect values for DaqWifiStreamManager to perform.
//
// This replaces seven independently-mutated flags whose invalid combinations
// produced the bug history recorded in DaqWifiStreamManager.swift: a UI
// stranded on "joining", a retry cancelling a connection that had just gone
// live, a dead socket reported as ready, a hotspot removal racing a fresh
// connect. Those states are unrepresentable here.
// =============================================================================

/// Mirrors the P4's `daq_wifi_stage_t` (`HAT_WIFI_STAGE_*`) — real bring-up
/// progress reported over BLE, not a synthetic timer.
public enum ProvisioningStage: String, Equatable, Sendable {
    case requested, ap, dns, tcp

    public var label: String {
        switch self {
        case .requested: return "Requesting hotspot from device…"
        case .ap:        return "Starting hotspot…"
        case .dns:       return "Configuring network…"
        case .tcp:       return "Starting data stream…"
        }
    }

    /// Rough fraction for a determinate progress indicator; the real bring-up
    /// isn't evenly timed across stages, so this conveys forward motion, not
    /// a precise ETA.
    public var fraction: Double {
        switch self {
        case .requested: return 0.15
        case .ap:        return 0.45
        case .dns:       return 0.75
        case .tcp:       return 0.9
        }
    }
}

/// Recovery rungs, tried in ascending order. Each rung is strictly more
/// disruptive than the last; we only escalate when the cheaper one is spent.
public enum DaqRecoveryRung: Int, Equatable, CaseIterable, Sendable {
    case redialSocket = 0    // the socket died but the network is probably fine
    case rejoinHotspot = 1   // the WiFi association was lost
    case recycleDevice = 2   // the device side is wedged — force-recycle + reprovision
}

public enum DaqLinkState: Equatable, Sendable {
    case idle
    case provisioning(stage: ProvisioningStage)
    case joiningWiFi
    case connecting
    case streaming
    case paused
    case recovering(rung: DaqRecoveryRung, attempt: Int, wasPaused: Bool)
    case failed(reason: String)

    /// True when a session exists that the user would consider "on" — used to
    /// decide whether a failure should recover silently or surface.
    public var isLive: Bool {
        switch self {
        case .streaming, .paused, .connecting, .recovering: return true
        case .idle, .provisioning, .joiningWiFi, .failed:   return false
        }
    }

    public var userFacingLabel: String {
        switch self {
        case .idle:                    return "Not streaming"
        case .provisioning(let stage): return stage.label
        case .joiningWiFi:             return "Joining DAQ HAT WiFi network…"
        case .connecting:              return "Connecting…"
        case .streaming:               return "Streaming"
        case .paused:                  return "Paused"
        case .recovering(let rung, let attempt, _):
            switch rung {
            case .redialSocket:  return "Reconnecting… (\(attempt))"
            case .rejoinHotspot: return "Rejoining WiFi… (\(attempt))"
            case .recycleDevice: return "Resetting device link… (\(attempt))"
            }
        case .failed(let reason):      return "Failed: \(reason)"
        }
    }
}

public enum DaqLinkEvent: Equatable, Sendable {
    case startRequested
    case stageReported(ProvisioningStage)
    case credentialsReady
    case provisioningFailed(String)
    case hotspotJoined
    case hotspotJoinFailed(String)
    case socketReady
    case socketClosed(String)
    case dataStalled
    case pauseRequested
    case resumeRequested
    case stopRequested
    case retryRequested
}

public enum DaqLinkEffect: Equatable, Sendable {
    case requestProvisioning
    case joinHotspot
    case openSocket
    case sendStart
    case sendStop
    case closeSocket
    case removeHotspotConfig
    case recycleDevice
    case resetBuffers
    case scheduleRetry(afterMs: Int)
}

public struct DaqLinkStateMachine: Sendable {
    /// Attempts allowed on each rung before escalating to the next.
    public static let maxAttemptsPerRung = 3

    public private(set) var state: DaqLinkState

    public init(state: DaqLinkState = .idle) { self.state = state }

    public mutating func handle(_ event: DaqLinkEvent) -> [DaqLinkEffect] {
        // stopRequested is handled uniformly from every state: there must
        // always be a way home. This is what makes "restart the app" obsolete.
        if case .stopRequested = event {
            let wasLive = state.isLive
            state = .idle
            return wasLive ? [.sendStop, .closeSocket, .removeHotspotConfig]
                           : [.closeSocket, .removeHotspotConfig]
        }

        switch (state, event) {
        // -- Bring-up ---------------------------------------------------------
        case (.idle, .startRequested), (.failed, .startRequested):
            state = .provisioning(stage: .requested)
            return [.requestProvisioning]

        case (.provisioning, .stageReported(let stage)):
            state = .provisioning(stage: stage)
            return []

        case (.provisioning, .credentialsReady):
            state = .joiningWiFi
            return [.joinHotspot]

        case (.provisioning, .provisioningFailed(let why)):
            return escalate(reason: why)

        case (.joiningWiFi, .hotspotJoined):
            state = .connecting
            return [.resetBuffers, .openSocket]

        case (.joiningWiFi, .hotspotJoinFailed(let why)):
            return escalate(reason: why)

        case (.connecting, .socketReady):
            state = .streaming
            return [.sendStart]

        // -- Recovery ---------------------------------------------------------
        // A recovering link that comes back up rejoins the happy path at the
        // right point, resetting the ladder. Both intermediate transitions
        // below stay in `.recovering` rather than jumping to the non-recovery
        // `.joiningWiFi`/`.connecting` states, because those states can't
        // carry `wasPaused` — losing it here would let a paused link that
        // recovers via the recycle rung silently resume streaming.
        case (.recovering(let rung, let attempt, let wasPaused), .credentialsReady):
            state = .recovering(rung: rung, attempt: attempt, wasPaused: wasPaused)
            return [.joinHotspot]

        case (.recovering(let rung, let attempt, let wasPaused), .hotspotJoined):
            // Stay in `.recovering` (rather than `.connecting`, as the
            // non-recovery happy path does) so `wasPaused` survives until
            // `.socketReady` decides whether to land back in `.paused`.
            state = .recovering(rung: rung, attempt: attempt, wasPaused: wasPaused)
            return [.resetBuffers, .openSocket]

        case (.recovering(_, _, let wasPaused), .socketReady):
            if wasPaused {
                state = .paused
                return []
            }
            state = .streaming
            return [.sendStart]

        case (.recovering, .retryRequested):
            return performCurrentRung()

        case (.recovering, .provisioningFailed(let why)),
             (.recovering, .hotspotJoinFailed(let why)):
            return escalate(reason: why)

        case (.failed, .retryRequested):
            state = .provisioning(stage: .requested)
            return [.requestProvisioning]

        // Any loss while a session is live enters or advances the ladder.
        case (.streaming, .socketClosed(let why)),
             (.connecting, .socketClosed(let why)):
            return escalate(reason: why)

        case (.recovering, .socketClosed(let why)):
            return escalate(reason: why)

        case (.streaming, .dataStalled):
            // Connected but silent — indistinguishable from dead, and the old
            // code showed "streaming" forever in exactly this case.
            return escalate(reason: "No data received")

        // -- Pause / resume ---------------------------------------------------
        // Pause keeps the socket AND the hotspot association: tearing either
        // down made resume replay the whole provisioning flow, and the hotspot
        // removal raced the next connect.
        case (.streaming, .pauseRequested):
            state = .paused
            return [.sendStop]

        case (.paused, .resumeRequested):
            state = .streaming
            return [.sendStart]

        case (.paused, .socketClosed):
            // Lost the socket while paused: recover quietly, stay paused-ish
            // by re-entering the ladder rather than surfacing an error. The
            // `wasPaused` flag threaded through `.recovering` is what makes
            // recovery land back in `.paused` instead of silently resuming
            // playback — see `autoStartOnReady`'s doc comment for why that
            // invariant matters.
            return escalate(reason: "Connection lost while paused")

        case (.paused, .dataStalled):
            return []   // expected while paused

        default:
            return []   // ignore events that don't apply to the current state
        }
    }

    /// Advance the recovery ladder: another attempt on the current rung, or
    /// the next rung when this one is spent, or terminal failure.
    private mutating func escalate(reason: String) -> [DaqLinkEffect] {
        let (rung, attempt, wasPaused): (DaqRecoveryRung, Int, Bool)
        switch state {
        case .recovering(let r, let a, let wp):
            if a >= Self.maxAttemptsPerRung {
                guard let next = DaqRecoveryRung(rawValue: r.rawValue + 1) else {
                    state = .failed(reason: reason)
                    return [.closeSocket]
                }
                (rung, attempt, wasPaused) = (next, 1, wp)
            } else {
                (rung, attempt, wasPaused) = (r, a + 1, wp)
            }
        case .paused:
            (rung, attempt, wasPaused) = (.redialSocket, 1, true)
        default:
            (rung, attempt, wasPaused) = (.redialSocket, 1, false)
        }
        state = .recovering(rung: rung, attempt: attempt, wasPaused: wasPaused)
        // Backoff grows with total effort spent, capped so recovery stays
        // responsive: 1s, 2s, 3s within a rung. The actual .recycleDevice
        // command is left to performCurrentRung (fired by the scheduled
        // retry below) rather than issued eagerly here too — issuing it from
        // both places recycled the device twice, ~1s apart, for no benefit
        // (and each RECYCLE bumps the P4's generation counter, one more
        // chance to orphan an in-flight bring-up task).
        return [.scheduleRetry(afterMs: attempt * 1000)]
    }

    /// Perform the action for the rung we're currently on (fired by the
    /// scheduled retry). The state stays `.recovering` on the same rung/attempt
    /// while the action is outstanding — otherwise a subsequent failure (e.g.
    /// another `socketClosed`) would land on a state that doesn't know how to
    /// keep climbing the ladder, wedging recovery indefinitely.
    private mutating func performCurrentRung() -> [DaqLinkEffect] {
        guard case .recovering(let rung, _, _) = state else { return [] }
        switch rung {
        case .redialSocket:
            // .resetBuffers before .openSocket: without it, engine.lastFrameAt
            // is left over from before the outage (cancelConnection() clears
            // rxBuffer/firstTimestampUs but not lastFrameAt), so the watchdog
            // compares against an already-stale timestamp and escalates the
            // redial via a bogus .dataStalled ~1s after it reconnects.
            return [.resetBuffers, .openSocket]
        case .rejoinHotspot:
            return [.joinHotspot]
        case .recycleDevice:
            // Force the device out of any wedged state, then reprovision from
            // scratch. This is what replaces power-cycling the DAQ HAT.
            return [.recycleDevice, .requestProvisioning]
        }
    }
}
