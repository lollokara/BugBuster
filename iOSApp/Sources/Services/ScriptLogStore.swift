import Foundation
import Combine

/// Ring line level (`<L>` in `<ts_ms> <L> <src> <text>`).
enum ScriptLogLevel: String, CaseIterable, Identifiable {
    case error = "E", warning = "W", info = "I", debug = "D"

    var id: String { rawValue }

    var label: String {
        switch self {
        case .error: return "Error"
        case .warning: return "Warning"
        case .info: return "Info"
        case .debug: return "Debug"
        }
    }
}

struct ScriptLogLine: Identifiable, Equatable {
    let id: Int
    /// Device uptime in ms; nil for markers and unstructured (legacy) lines.
    let tsMs: UInt32?
    let level: ScriptLogLevel
    /// "mpy", "sys", or "" for markers / unstructured lines.
    let source: String
    let text: String
    /// App-side note ("Device log restarted", "Started 'x.py'"), never filtered out.
    let isMarker: Bool

    var plainText: String {
        if isMarker { return "--- \(text) ---" }
        guard let tsMs else { return text }
        return "\(tsMs) \(level.rawValue) \(source)" + (text.isEmpty ? "" : " \(text)")
    }

    /// Parses one ring line without its newline. Anything that is not
    /// `<ts> <L> <src> <text>` is kept verbatim as an info line.
    static func parse(_ raw: String, id: Int) -> ScriptLogLine {
        let parts = raw.split(separator: " ", maxSplits: 3, omittingEmptySubsequences: false)
        if parts.count >= 3,
           let ts = UInt32(parts[0]),
           parts[1].count == 1,
           let level = ScriptLogLevel(rawValue: String(parts[1])),
           !parts[2].isEmpty,
           parts[2].allSatisfy({ $0.isASCII && $0.isLetter }) {
            let text = parts.count == 4 ? String(parts[3]) : ""
            return ScriptLogLine(id: id, tsMs: ts, level: level, source: String(parts[2]), text: text, isMarker: false)
        }
        return ScriptLogLine(id: id, tsMs: nil, level: .info, source: "", text: raw, isMarker: false)
    }

    static func marker(_ text: String, id: Int) -> ScriptLogLine {
        ScriptLogLine(id: id, tsMs: nil, level: .info, source: "", text: text, isMarker: true)
    }
}

/// One `GET /api/scripts/logs?since=` page, transport-independent (built by ScriptsClient).
struct ScriptLogPage: Equatable {
    let data: Data
    let since: UInt64
    let next: UInt64
    let dropped: UInt64

    /// The device cursor went backwards (reboot / ring reset): re-read from 0.
    var restarted: Bool { next < since }
}

/// Splits pages into lines at byte level. A page can end mid-line or mid
/// UTF-8 character; the tail waits here until its newline arrives.
struct ScriptLogAssembler {
    static let maxPartial = 4096

    private var pending = Data()

    var hasPartial: Bool { !pending.isEmpty }

    mutating func feed(_ data: Data) -> [String] {
        pending.append(data)
        var lines: [String] = []
        var start = pending.startIndex
        while let newline = pending[start...].firstIndex(of: 0x0A) {
            var line = pending[start..<newline]
            if line.last == 0x0D { line = line.dropLast() }
            lines.append(String(decoding: line, as: UTF8.self))
            start = pending.index(after: newline)
        }
        pending = Data(pending[start...])
        if pending.count > Self.maxPartial {        // runaway output with no newline
            lines.append(String(decoding: pending, as: UTF8.self))
            pending.removeAll()
        }
        return lines
    }

    mutating func reset() { pending.removeAll() }
}

struct ScriptLogFilter: Equatable {
    var levels: Set<ScriptLogLevel> = Set(ScriptLogLevel.allCases)
    var query = ""

    func matches(_ line: ScriptLogLine) -> Bool {
        if line.isMarker { return true }
        guard levels.contains(line.level) else { return false }
        let q = query.trimmingCharacters(in: .whitespacesAndNewlines)
        return q.isEmpty || line.text.localizedCaseInsensitiveContains(q) || line.source.localizedCaseInsensitiveContains(q)
    }

    func apply(_ lines: [ScriptLogLine]) -> [ScriptLogLine] { lines.filter(matches) }
}

/// The app's copy of the device log ring: the resumable cursor plus parsed lines.
@MainActor
final class ScriptLogStore: ObservableObject {
    static let defaultCapacity = 5000

    @Published private(set) var lines: [ScriptLogLine] = []
    /// Pause auto-scroll (the console keeps receiving lines).
    @Published var isPaused = false
    /// Byte offset to pass as `since` on the next poll.
    private(set) var cursor: UInt64 = 0

    private var assembler = ScriptLogAssembler()
    private var nextId = 0
    private let capacity: Int

    init(capacity: Int = ScriptLogStore.defaultCapacity) {
        self.capacity = capacity
    }

    var lastLine: ScriptLogLine? { lines.last { !$0.isMarker } }

    var plainText: String { lines.map(\.plainText).joined(separator: "\n") }

    func ingest(_ page: ScriptLogPage) {
        if page.restarted {
            assembler.reset()
            cursor = 0
            append([.marker("Device log restarted", id: takeId())])
            return
        }
        var batch: [ScriptLogLine] = []
        // since == 0 is the first attach: older bytes were never ours to lose.
        if page.dropped > 0 && page.since > 0 {
            assembler.reset()   // the pending partial line's continuation was overwritten
            batch.append(.marker("\(page.dropped) bytes of log dropped (ring overflow)", id: takeId()))
        }
        for raw in assembler.feed(page.data) {
            batch.append(ScriptLogLine.parse(raw, id: takeId()))
        }
        cursor = page.next
        append(batch)
    }

    func appendMarker(_ text: String) {
        append([.marker(text, id: takeId())])
    }

    /// Hide what is on screen; the cursor stays, so cleared lines never come back.
    func clear() { lines = [] }

    /// New device: forget everything, read its ring from the start.
    func reset() {
        lines = []
        cursor = 0
        assembler.reset()
        isPaused = false
    }

    private func takeId() -> Int {
        nextId += 1
        return nextId
    }

    private func append(_ batch: [ScriptLogLine]) {
        guard !batch.isEmpty else { return }
        var updated = lines
        updated.append(contentsOf: batch)
        if updated.count > capacity { updated.removeFirst(updated.count - capacity) }
        lines = updated
    }
}
