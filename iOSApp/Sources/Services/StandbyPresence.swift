import Foundation
import Combine

// =============================================================================
// StandbyPresence.swift - system standby: status model, timeout choices and the
// logical presence session for the selected Wi-Fi/BLE connection.
//
// The app is a control client only while a connection is actually established.
// Each connection epoch registers a random non-zero client id with the device,
// refreshes it every 10 s (the device expires it after 45 s and keeps at most 8)
// and releases it best-effort on disconnect. A refresh is not activity. Bonjour
// or BLE discovery, and a merely paired/associated radio, are not presence.
// =============================================================================

/// `/api/standby/*` reply: the 28-byte BBP status record as JSON (state is a string).
public struct StandbyStatus: Decodable, Equatable {
    public let schema: Int
    public let state: String
    public let ready: Bool
    public let stage: Int
    public let generation: Int
    public let timeoutSeconds: Int
    public let clients: Int
    public let failedStage: Int
    public let inhibitors: Int
    public let completed: Int
    public let failed: Int
    public let skipped: Int
    public let idleRemainingMs: Int

    public var isActive: Bool { state == "active" }

    /// Hardware commands answer BUSY until the device is active and settled.
    public var hardwareReady: Bool { isActive && ready }

    public var stateLabel: String {
        switch state {
        case "active": return "Active"
        case "preparing": return "Entering standby"
        case "asleep": return "Standby"
        case "waking": return "Waking"
        case "fault_safe": return "Fault (outputs safe)"
        default: return state
        }
    }

    /// One line on what the inactivity timer is doing right now.
    public var idleLine: String {
        guard isActive else { return stateLabel }
        if timeoutSeconds == 0 { return "Automatic standby is off" }
        if clients > 0 || inhibitors != 0 {
            var parts: [String] = []
            if clients > 0 { parts.append("\(clients) connected client\(clients == 1 ? "" : "s")") }
            if inhibitors != 0 { parts.append("running work") }
            return "Held awake by " + parts.joined(separator: " and ")
        }
        let total = max(0, Int((Double(idleRemainingMs) / 1000).rounded(.up)))
        return String(format: "Standby in %d:%02d", total / 60, total % 60)
    }
}

/// The persisted automatic-standby choices (seconds; 0 = off). Default 300.
public enum StandbyTimeout: Int, CaseIterable, Identifiable {
    case oneMinute = 60
    case fiveMinutes = 300
    case fifteenMinutes = 900
    case off = 0

    public var id: Int { rawValue }

    public var label: String {
        switch self {
        case .oneMinute: return "1 min"
        case .fiveMinutes: return "5 min"
        case .fifteenMinutes: return "15 min"
        case .off: return "Off"
        }
    }
}

public enum StandbyCapability: Equatable {
    case unknown
    case supported
    case unsupported
}

public enum StandbySendResult: Equatable {
    case ok(StandbyStatus?)
    /// Firmware without system standby (HTTP 404/405, or the BLE tunnel's "unknown path").
    case unsupported
    case failed
}

/// Where a presence epoch was registered. Captured when the epoch opens so a
/// release at disconnect still reaches the device it was registered with.
public struct StandbyRoute {
    public let transport: TransportKind
    public let ip: String
    public let token: String

    public init(transport: TransportKind, ip: String, token: String) {
        self.transport = transport
        self.ip = ip
        self.token = token
    }
}

public typealias StandbyPresenceSender = (UInt32, Bool) async -> StandbySendResult

@MainActor
public final class StandbyPresence: ObservableObject {
    @Published public private(set) var status: StandbyStatus?
    @Published public private(set) var capability: StandbyCapability = .unknown
    /// True after "Sleep now" until Wake: the heartbeat stays closed.
    @Published public private(set) var released = false
    public private(set) var clientId: UInt32 = 0

    private var sender: StandbyPresenceSender?
    private var loop: Task<Void, Never>?
    private let interval: TimeInterval
    private let makeId: () -> UInt32

    nonisolated public init(refreshInterval: TimeInterval = 10,
                            makeId: @escaping () -> UInt32 = StandbyPresence.randomClientId) {
        self.interval = refreshInterval
        self.makeId = makeId
    }

    nonisolated public static func randomClientId() -> UInt32 {
        UInt32.random(in: 1...UInt32.max)
    }

    /// Start a fresh epoch with a new random id (no-op heartbeat while released).
    public func open(sender: @escaping StandbyPresenceSender) {
        stopLoop()
        self.sender = sender
        capability = .unknown
        guard !released else { return }
        let id = makeId()
        clientId = id
        startLoop(id: id, sender: sender)
    }

    /// Stop and release the current epoch (best effort, fire and forget).
    public func close() {
        let id = clientId
        let sender = self.sender
        stopLoop()
        clientId = 0
        guard id != 0, capability != .unsupported, let sender else { return }
        Task { _ = await sender(id, false) }
    }

    /// The connection is gone: stop without a release. The device expires the id.
    public func suspend() {
        stopLoop()
        clientId = 0
        sender = nil
        released = false
        status = nil
        capability = .unknown
    }

    /// Release and hold closed (no heartbeat) until `attach()`.
    public func detach() {
        close()
        released = true
    }

    public func attach() {
        released = false
        if let sender, clientId == 0 { open(sender: sender) }
    }

    public func noteStatus(_ s: StandbyStatus) { status = s }

    /// Map a presence reply to a result. `httpStatus` is nil over the BLE tunnel.
    nonisolated public static func interpret(data: Data?, httpStatus: Int?) -> StandbySendResult {
        if let code = httpStatus {
            if code == 404 || code == 405 { return .unsupported }
            guard (200...299).contains(code) else { return .failed }
        }
        guard let data else { return .failed }
        if let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           let err = obj["error"] as? String {
            return err == "unknown path" ? .unsupported : .failed
        }
        return .ok(try? JSONDecoder().decode(StandbyStatus.self, from: data))
    }

    private func startLoop(id: UInt32, sender: @escaping StandbyPresenceSender) {
        let interval = self.interval
        loop = Task { [weak self] in
            while !Task.isCancelled {
                let result = await sender(id, true)
                guard !Task.isCancelled, let self else { return }
                switch result {
                case .ok(let st):
                    self.capability = .supported
                    if let st { self.status = st }
                case .unsupported:
                    self.capability = .unsupported
                    self.clientId = 0
                    return
                case .failed:
                    break
                }
                try? await Task.sleep(nanoseconds: UInt64(interval * 1_000_000_000))
            }
        }
    }

    private func stopLoop() {
        loop?.cancel()
        loop = nil
    }
}
