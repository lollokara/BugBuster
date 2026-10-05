import Foundation

/// `source` of the current run (`sr_source_name`, script_runtime.c).
enum ScriptSourceKind: String, Equatable {
    case manual, autorun, repl, unknown

    /// Absent (pre-v2 firmware) reads as manual; an unknown spelling as `.unknown`.
    init(wire: String?) {
        guard let wire else { self = .manual; return }
        self = ScriptSourceKind(rawValue: wire) ?? .unknown
    }
}

/// `state` of the slot (`sr_state_name`).
enum ScriptRunState: String, Equatable {
    case idle, running, stopping, error, done, unknown

    /// Pre-v2 firmware has no `state`: derive it from the legacy `running` flag.
    init(wire: String?, legacyRunning: Bool) {
        guard let wire else { self = legacyRunning ? .running : .idle; return }
        self = ScriptRunState(rawValue: wire) ?? .unknown
    }
}

/// `lastExit` of the previous run (`sr_exit_name`).
enum ScriptExitKind: String, Equatable {
    case none, ok, error, stopped, unknown

    init(wire: String?) {
        guard let wire else { self = .none; return }
        self = ScriptExitKind(rawValue: wire) ?? .unknown
    }
}

/// `GET /api/scripts/status` (firmware plan "Shared JSON contract"). Every key is
/// optional on the wire so an older firmware still decodes.
struct ScriptRunStatus: Equatable {
    var running = false
    var currentScriptId = 0
    var totalRuns = 0
    var totalErrors = 0
    var lastError = ""
    var name = ""
    var source: ScriptSourceKind = .manual
    var state: ScriptRunState = .idle
    var lastExit: ScriptExitKind = .none
    /// Epoch seconds when `startedAtEpoch`, otherwise device uptime in ms.
    var startedAt: Double = 0
    var startedAtEpoch = false
    var fileSlotId = 0
    var fileSlotName = ""
    var lastScriptId = 0

    /// A file script holds the single slot: run-file answers 409, eval/REPL input are refused.
    var holdsFileSlot: Bool { !fileSlotName.isEmpty }

    /// The pill and the status header show while this is true.
    var isActive: Bool { state == .running || state == .stopping || holdsFileSlot }

    var displayName: String {
        if holdsFileSlot { return fileSlotName }
        if !name.isEmpty { return name }
        return source == .repl ? "REPL" : "script"
    }
}

extension ScriptRunStatus: Decodable {
    private enum CodingKeys: String, CodingKey {
        case running, currentScriptId, totalRuns, totalErrors, lastError, name, source, state, lastExit
        case startedAt, startedAtEpoch, fileSlotId, fileSlotName, lastScriptId
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        running = try c.decodeIfPresent(Bool.self, forKey: .running) ?? false
        currentScriptId = try c.decodeIfPresent(Int.self, forKey: .currentScriptId) ?? 0
        totalRuns = try c.decodeIfPresent(Int.self, forKey: .totalRuns) ?? 0
        totalErrors = try c.decodeIfPresent(Int.self, forKey: .totalErrors) ?? 0
        lastError = try c.decodeIfPresent(String.self, forKey: .lastError) ?? ""
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? ""
        source = ScriptSourceKind(wire: try c.decodeIfPresent(String.self, forKey: .source))
        state = ScriptRunState(wire: try c.decodeIfPresent(String.self, forKey: .state), legacyRunning: running)
        lastExit = ScriptExitKind(wire: try c.decodeIfPresent(String.self, forKey: .lastExit))
        startedAt = try c.decodeIfPresent(Double.self, forKey: .startedAt) ?? 0
        startedAtEpoch = try c.decodeIfPresent(Bool.self, forKey: .startedAtEpoch) ?? false
        fileSlotId = try c.decodeIfPresent(Int.self, forKey: .fileSlotId) ?? 0
        fileSlotName = try c.decodeIfPresent(String.self, forKey: .fileSlotName) ?? ""
        lastScriptId = try c.decodeIfPresent(Int.self, forKey: .lastScriptId) ?? 0
    }
}

/// `GET /api/scripts/autorun/status`: legacy snake_case keys plus the v2 camelCase ones.
struct ScriptAutorunStatus: Equatable {
    var enabled = false
    var hasScript = false
    /// IO12 read now; LOW at boot suppresses autorun (autorun.cpp gate 3).
    var io12High = true
    var lastRunOk = false
    var lastRunId = 0
    var scriptName = ""
    var ranThisBoot = false
    var running = false
}

extension ScriptAutorunStatus: Decodable {
    private enum CodingKeys: String, CodingKey {
        case enabled
        case hasScript = "has_script"
        case io12High = "io12_high"
        case lastRunOk = "last_run_ok"
        case lastRunId = "last_run_id"
        case scriptName, ranThisBoot, running
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? false
        hasScript = try c.decodeIfPresent(Bool.self, forKey: .hasScript) ?? false
        io12High = try c.decodeIfPresent(Bool.self, forKey: .io12High) ?? true
        lastRunOk = try c.decodeIfPresent(Bool.self, forKey: .lastRunOk) ?? false
        lastRunId = try c.decodeIfPresent(Int.self, forKey: .lastRunId) ?? 0
        scriptName = try c.decodeIfPresent(String.self, forKey: .scriptName) ?? ""
        ranThisBoot = try c.decodeIfPresent(Bool.self, forKey: .ranThisBoot) ?? false
        running = try c.decodeIfPresent(Bool.self, forKey: .running) ?? false
    }
}

/// Result of `POST /api/scripts/run-file`: started, or the slot holder from the 409 body.
enum ScriptRunOutcome: Equatable {
    case started(id: Int, name: String, background: Bool)
    case busy(running: String, id: Int)
}

/// `GET /api/scripts/storage`.
struct ScriptStorageInfo: Decodable, Equatable {
    let totalBytes: Double
    let usedBytes: Double
    let freeBytes: Double
    let scriptCount: Int
    let maxScriptBytes: Int
    let maxScripts: Int
}

/// Mirrors `script_storage_validate_name()` (script_storage.cpp) so a bad name is
/// rejected before a round trip.
enum ScriptName {
    static let maxLength = 32
    static let maxBodyBytes = 32 * 1024
    private static let allowed = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")

    static func isValid(_ name: String) -> Bool {
        guard (1...maxLength).contains(name.count), !name.hasPrefix("."), name.hasSuffix(".py") else { return false }
        return name.allSatisfy { allowed.contains($0) }
    }

    /// Trims whitespace and adds ".py" when missing; nil when the result is invalid.
    static func normalized(_ raw: String) -> String? {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        let name = trimmed.hasSuffix(".py") ? trimmed : trimmed + ".py"
        return isValid(name) ? name : nil
    }
}

/// Device uptime sample from `/api/status` (`uptimeMs`) and when it arrived.
struct UptimeAnchor: Equatable {
    let uptimeMs: Double
    let at: Date

    func uptimeMs(at now: Date) -> Double { uptimeMs + now.timeIntervalSince(at) * 1000 }
}

enum ScriptElapsed {
    /// Seconds the active run has been going, or nil when unknown. Epoch starts use
    /// the wall clock; uptime starts need an uptime anchor.
    static func seconds(_ status: ScriptRunStatus, now: Date, anchor: UptimeAnchor?) -> TimeInterval? {
        guard status.isActive, status.startedAt > 0 else { return nil }
        if status.startedAtEpoch { return max(0, now.timeIntervalSince1970 - status.startedAt) }
        guard let anchor else { return nil }
        return max(0, (anchor.uptimeMs(at: now) - status.startedAt) / 1000)
    }

    static func format(_ seconds: TimeInterval) -> String {
        let t = Int(seconds.rounded(.down))
        if t < 60 { return "\(t)s" }
        if t < 3600 { return String(format: "%dm %02ds", t / 60, t % 60) }
        return String(format: "%dh %02dm", t / 3600, (t % 3600) / 60)
    }
}

/// How often to poll, from what is on screen (spec §4: 1 s while the log
/// console is visible, 5 s for the pill).
struct ScriptPollPolicy: Equatable {
    let status: TimeInterval
    let logs: TimeInterval?

    var tick: TimeInterval { min(status, logs ?? status) }

    static func make(consoleVisible: Bool, scriptsTabVisible: Bool, active: Bool) -> ScriptPollPolicy {
        if consoleVisible { return ScriptPollPolicy(status: 1, logs: 1) }
        if scriptsTabVisible { return ScriptPollPolicy(status: 1, logs: active ? 5 : nil) }
        if active { return ScriptPollPolicy(status: 5, logs: 5) }
        return ScriptPollPolicy(status: 5, logs: nil)
    }
}
