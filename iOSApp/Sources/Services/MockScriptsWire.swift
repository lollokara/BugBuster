#if DEBUG
import Foundation

/// Simulated scripts API for `BB_MOCK_MODE=1` (screenshots, layout checks). Answers
/// like the HTTP server (`kind == .wifi`): text/plain logs with
/// `X-BugBuster-Log-Next`, the raw file text, `409` + the busy body. Never
/// touches the network.
final class MockScriptsWire: ScriptsWire {
    let kind: TransportKind = .wifi

    private let now: () -> Date
    private let lock = NSLock()
    private let bootTime: Date
    private var files: [String: String]
    private var running: String?
    private var runningSource = "autorun"
    private var runId = 41
    private var totalRuns = 41
    private var startedEpoch: Double
    private var ring = Data()
    private var lastEmit: Date
    private var tick = 0
    private var autorunEnabled = true
    private var autorunName = "background_logger.py"

    init(now: @escaping () -> Date = Date.init) {
        self.now = now
        let t = now()
        bootTime = t.addingTimeInterval(-75)
        lastEmit = t
        startedEpoch = t.timeIntervalSince1970 - 75
        var files = ["hello.py": "print('hello from BugBuster')\n"]
        for example in FirmwareAPICatalogue.bundled?.examples ?? [] { files[example.name] = example.source }
        if files["background_logger.py"] == nil {
            files["background_logger.py"] = "import bugbuster\nwhile True:\n    print('tick')\n    bugbuster.sleep(1000)\n"
        }
        self.files = files
        running = "background_logger.py"
        seedLog()
    }

    func send(_ method: String, _ path: String, query: [String: String], body: ScriptsWireBody?) async throws -> ScriptsWireReply {
        lock.withLock {
            emitDueLines()
            return handle(method, path, query, body)
        }
    }

    private func handle(_ method: String, _ path: String, _ query: [String: String], _ body: ScriptsWireBody?) -> ScriptsWireReply {
        switch path {
        case "/api/scripts/status":
            return .json(200, statusJSON())
        case "/api/scripts/files" where method == "POST":
            guard let name = query["name"], case .text(let text)? = body else {
                return .json(400, ["ok": false, "error": "valid name required"])
            }
            files[name] = text
            return .json(200, ["ok": true])
        case "/api/scripts/files":
            return .json(200, ["ok": true, "files": files.keys.sorted()])
        case "/api/scripts/storage":
            let used = files.values.reduce(0) { $0 + $1.utf8.count }
            return .json(200, ["totalBytes": 1_048_576, "usedBytes": used, "freeBytes": 1_048_576 - used,
                               "scriptCount": files.count, "maxScriptBytes": 32_768, "maxScripts": 64])
        case "/api/scripts/files/get":
            guard let name = query["name"], let text = files[name] else { return .json(404, ["error": "script not found"]) }
            return .text(200, text, headers: ["Content-Type": "text/x-python"])
        case "/api/scripts/files/delete":
            guard let name = query["name"], files.removeValue(forKey: name) != nil else {
                return .json(400, ["ok": false, "error": "script not found"])
            }
            return .json(200, ["ok": true])
        case "/api/scripts/run-file":
            guard let name = query["name"], files[name] != nil else { return .json(400, ["ok": false, "error": "script not found"]) }
            if let holder = running {
                guard query["replace"] == "1" else {
                    return .json(409, ["ok": false, "error": "a script is running; pass replace=1 to stop it",
                                       "running": holder, "id": runId])
                }
                append("W", "sys", "replace: stopping '\(holder)'")
            }
            runId += 1
            totalRuns += 1
            running = name
            runningSource = "manual"
            startedEpoch = now().timeIntervalSince1970
            append("I", "sys", "run '\(name)' (id \(runId))")
            return .json(200, ["ok": true, "id": runId, "name": name, "background": query["background"] == "1"])
        case "/api/scripts/eval":
            if let holder = running {
                return .json(409, ["ok": false, "error": "a script is running; eval is refused until it ends",
                                   "running": holder, "id": runId])
            }
            runId += 1
            totalRuns += 1
            return .json(200, ["ok": true, "id": runId])
        case "/api/scripts/stop":
            if let holder = running {
                append("I", "sys", "stopped '\(holder)'")
                running = nil
            }
            return .json(200, ["ok": true])
        case "/api/scripts/logs":
            let since = Int(query["since"] ?? "0") ?? 0
            let from = min(max(0, since), ring.count)
            let to = min(ring.count, from + 4096)
            return ScriptsWireReply(status: 200, headers: ["Content-Type": "text/plain", "X-BugBuster-Log-Next": String(to)],
                                    body: ring.subdata(in: from..<to))
        case "/api/scripts/lint":
            if running != nil {
                return .json(200, ["ok": false, "err": "Interpreter is busy running a script"])
            }
            var srcToCheck: String?
            if case .text(let source)? = body {
                srcToCheck = source
            } else if case .json(let dict)? = body {
                if let name = dict["name"] as? String {
                    guard let fileContent = files[name] else {
                        return .json(200, ["ok": false, "err": "script not found"])
                    }
                    srcToCheck = fileContent
                } else if let inline = dict["src"] as? String {
                    srcToCheck = inline
                }
            }
            if let src = srcToCheck {
                if src.contains("syntax_error") {
                    return .json(200, ["ok": false, "err": "line 1: invalid syntax"])
                }
                return .json(200, ["ok": true])
            }
            return .json(200, ["ok": true])
        case "/api/scripts/autorun/status":
            return .json(200, ["enabled": autorunEnabled, "has_script": autorunEnabled, "io12_high": true,
                               "last_run_ok": true, "last_run_id": 41,
                               "scriptName": autorunEnabled ? autorunName : "", "ranThisBoot": true,
                               "running": running == autorunName && runningSource == "autorun"])
        case "/api/scripts/autorun/enable":
            guard let name = query["name"], files[name] != nil else { return .json(400, ["ok": false, "error": "script not found"]) }
            autorunEnabled = true
            autorunName = name
            return .json(200, ["ok": true])
        case "/api/scripts/autorun/disable":
            autorunEnabled = false
            return .json(200, ["ok": true])
        default:
            return .json(400, ["ok": false, "error": "unknown path"])
        }
    }

    private func statusJSON() -> [String: Any] {
        let active = running != nil
        return ["running": active, "currentScriptId": runId, "totalRuns": totalRuns, "totalErrors": 1, "lastError": "",
                "name": running ?? "", "source": active ? runningSource : "manual",
                "state": active ? "running" : "done", "lastExit": active ? "none" : "stopped",
                "startedAt": startedEpoch, "startedAtEpoch": true,
                "fileSlotId": active ? runId : 0, "fileSlotName": running ?? "", "lastScriptId": runId]
    }

    private func seedLog() {
        append("E", "mpy", "Traceback (most recent call last):", atMs: 2_000)
        append("E", "mpy", "  File \"sweep.py\", line 12, in <module>", atMs: 2_001)
        append("E", "mpy", "OSError: [Errno 19] ENODEV", atMs: 2_002)
        append("I", "sys", "autorun: running background_logger.py (id 41)", atMs: 4_000)
        for k in 0..<8 {
            if k == 5 {
                append("W", "mpy", "ch0 above 2.6 V", atMs: UInt32(5_000 + k * 1_000))
            } else {
                append("I", "mpy", String(format: "ch0 = %.4f V", 2.5 + Double(k) * 0.0013), atMs: UInt32(5_000 + k * 1_000))
            }
        }
        append("D", "sys", "log shipper: hub unreachable, 0 B buffered", atMs: 70_000)
    }

    /// One line per whole second since the last emission while a script runs (≤ 10 per call).
    private func emitDueLines() {
        let due = Int(now().timeIntervalSince(lastEmit))
        guard due > 0 else { return }
        lastEmit = lastEmit.addingTimeInterval(TimeInterval(due))
        guard running != nil else { return }
        for _ in 0..<min(due, 10) {
            tick += 1
            switch tick % 9 {
            case 4: append("W", "mpy", "ch0 above 2.6 V")
            case 7: append("D", "mpy", "adc settle 3 ms")
            default: append("I", "mpy", String(format: "ch0 = %.4f V", 2.5 + Double(tick % 13) * 0.0011))
            }
        }
    }

    private func append(_ level: String, _ source: String, _ text: String, atMs ms: UInt32? = nil) {
        let ts = ms ?? UInt32(max(0, now().timeIntervalSince(bootTime)) * 1000)
        ring.append(Data("\(ts) \(level) \(source) \(text)\n".utf8))
    }
}
#endif
