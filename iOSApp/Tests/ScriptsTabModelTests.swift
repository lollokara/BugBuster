import XCTest
@testable import BugBuster

final class ScriptsTabModelTests: XCTestCase {
    /// Minimal device for the tab: one stored file, lint and upload answers.
    private final class Device {
        var files = ["b.py": "x = 1\n", "A.py": "print(1)\n"]
        var saveError: String?
        var lintError: String?
        var oldFirmware = false

        func answer(_ call: FakeScriptsWire.Call, kind: TransportKind) -> ScriptsWireReply {
            switch call.path {
            case "/api/scripts/files" where call.method == "POST":
                if let saveError { return .json(200, ["ok": false, "err": saveError]) }
                files[call.query["name"] ?? ""] = call.textBody
                return .json(200, ["ok": true])
            case "/api/scripts/files":
                return .json(200, ["ok": true, "files": Array(files.keys)])
            case "/api/scripts/storage":
                return .json(200, ["totalBytes": 1_048_576, "usedBytes": 2_048, "freeBytes": 1_046_528,
                                   "scriptCount": files.count, "maxScriptBytes": 32_768, "maxScripts": 64])
            case "/api/scripts/files/get":
                let text = files[call.query["name"] ?? ""] ?? ""
                if kind == .wifi { return .text(200, text) }
                let data = Data(text.utf8)
                return .json(200, ["ok": true, "size": data.count, "off": 0, "n": data.count, "data": data.base64EncodedString()])
            case "/api/scripts/files/chunk":
                let body = call.jsonBody ?? [:]
                let n = Data(base64Encoded: body["b64"] as? String ?? "")?.count ?? 0
                return .json(200, ["ok": true, "received": (body["off"] as? Int ?? 0) + n, "final": body["final"] as? Bool ?? false])
            case "/api/scripts/lint":
                if kind == .ble && oldFirmware { return .json(400, ["error": "unknown path"]) }
                return lintError.map { .json(400, ["ok": false, "err": $0]) } ?? .json(200, ["ok": true])
            case "/api/scripts/status":
                return .json(200, ["running": false, "state": "idle"])
            default:
                return .json(200, ["ok": true, "id": 1, "name": call.query["name"] ?? "", "background": false])
            }
        }
    }

    @MainActor
    private func make(_ device: Device, kind: TransportKind = .wifi) -> (ScriptsTabModel, FakeScriptsWire, ScriptRunManager) {
        let wire = FakeScriptsWire(kind: kind) { device.answer($0, kind: kind) }
        let manager = ScriptRunManager(sleeper: { _ in }, autoStart: false)
        manager.connect(client: ScriptsClient(wire: wire), deviceKey: "dev", transport: kind)
        return (ScriptsTabModel(manager: manager, editor: ScriptEditorModel(engine: nil)), wire, manager)
    }

    @MainActor
    func testLoadFilesSortsCaseInsensitivelyAndReadsStorage() async {
        let (m, _, _) = make(Device())
        await m.loadFiles()
        XCTAssertEqual(m.files, ["A.py", "b.py"])
        XCTAssertEqual(m.storage?.usedBytes, 2_048)
        XCTAssertFalse(m.isLoading)
    }

    @MainActor
    func testCreateNormalizesTheNameAndOpensIt() async {
        let d = Device()
        let (m, wire, _) = make(d)
        let ok = await m.create(rawName: " blink ")
        XCTAssertTrue(ok)
        XCTAssertEqual(wire.calls.first?.query, ["name": "blink.py"])
        XCTAssertEqual(m.openFile, "blink.py")
        XCTAssertEqual(m.editor.text, "# blink.py\n# Write your MicroPython code here\n")
        XCTAssertFalse(m.editor.isDirty)
    }

    @MainActor
    func testCreateRejectsInvalidAndDuplicateNames() async {
        let (m, wire, _) = make(Device())
        await m.loadFiles()
        let before = wire.calls.count
        let bad = await m.create(rawName: "bad name")
        XCTAssertFalse(bad)
        XCTAssertNotNil(m.errorMessage)
        let dup = await m.create(rawName: "b")
        XCTAssertFalse(dup)
        XCTAssertEqual(m.errorMessage, "'b.py' already exists")
        XCTAssertEqual(wire.calls.count, before)
    }

    @MainActor
    func testSaveThenLintOK() async {
        let (m, wire, _) = make(Device())
        await m.open("b.py")
        m.editor.textDidChange("x = 2\n", caret: 6)
        let saved = await m.save()
        XCTAssertTrue(saved)
        XCTAssertEqual(wire.paths().suffix(2), ["/api/scripts/files", "/api/scripts/lint"])
        XCTAssertEqual(wire.calls.last?.textBody, "x = 2\n")
        XCTAssertEqual(m.lint, .ok)
        XCTAssertFalse(m.editor.isDirty)
        XCTAssertNil(m.uploadProgress)
    }

    @MainActor
    func testLintFailureStillSaves() async {
        let d = Device()
        d.lintError = "line 1: invalid syntax"
        let (m, _, _) = make(d)
        await m.open("b.py")
        m.editor.textDidChange("x = (\n", caret: 6)
        let saved = await m.save()
        XCTAssertTrue(saved)
        XCTAssertEqual(d.files["b.py"], "x = (\n")
        XCTAssertEqual(m.lint, .failed("line 1: invalid syntax"))
        XCTAssertFalse(m.editor.isDirty)
    }

    @MainActor
    func testSaveOverBLEChunksThenLintsByName() async {
        let (m, wire, _) = make(Device(), kind: .ble)
        await m.open("b.py")
        XCTAssertEqual(m.editor.text, "x = 1\n")
        m.editor.textDidChange("x = 3\n", caret: 6)
        let saved = await m.save()
        XCTAssertTrue(saved)
        XCTAssertTrue(wire.paths().contains("/api/scripts/files/chunk"))
        let lintCall = wire.calls.last
        XCTAssertEqual(lintCall?.path, "/api/scripts/lint")
        XCTAssertEqual(lintCall?.jsonBody?["name"] as? String, "b.py")
        XCTAssertEqual(m.lint, .ok)
    }

    @MainActor
    func testSaveOverBLEOldFirmwareReportsLintNeedsWiFi() async {
        let d = Device()
        d.oldFirmware = true
        let (m, _, _) = make(d, kind: .ble)
        await m.open("b.py")
        m.editor.textDidChange("x = 3\n", caret: 6)
        let saved = await m.save()
        XCTAssertTrue(saved)
        XCTAssertEqual(m.lint, .unavailable("Saved. Syntax check needs Wi-Fi."))
    }

    @MainActor
    func testLintBusyWhileAScriptRunsIsUnavailableNotFailed() async {
        let d = Device()
        d.lintError = "Interpreter is busy running a script"
        let (m, _, _) = make(d)
        await m.open("b.py")
        m.editor.textDidChange("x = 5\n", caret: 6)
        let saved = await m.save()
        XCTAssertTrue(saved)
        XCTAssertEqual(m.lint, .unavailable("Saved. Syntax check skipped while a script runs."))
    }

    @MainActor
    func testSaveFailureKeepsTheEditorDirty() async {
        let d = Device()
        d.saveError = "SPIFFS full"
        let (m, wire, _) = make(d)
        await m.open("b.py")
        m.editor.textDidChange("x = 9\n", caret: 6)
        let saved = await m.save()
        XCTAssertFalse(saved)
        XCTAssertTrue(m.editor.isDirty)
        XCTAssertEqual(m.errorMessage, "Save failed: SPIFFS full")
        XCTAssertFalse(wire.paths().contains("/api/scripts/lint"))
    }

    @MainActor
    func testRunSavesTheDirtyOpenFileFirst() async {
        let (m, wire, manager) = make(Device())
        await m.open("b.py")
        m.editor.textDidChange("x = 4\n", caret: 6)
        await m.run("b.py", background: false)
        XCTAssertEqual(Array(wire.paths().suffix(4)),
                       ["/api/scripts/files", "/api/scripts/lint", "/api/scripts/run-file", "/api/scripts/status"])
        XCTAssertTrue(manager.consoleVisible)
    }

    @MainActor
    func testRunOfAnotherFileDoesNotSave() async {
        let (m, wire, _) = make(Device())
        await m.open("b.py")
        m.editor.textDidChange("x = 4\n", caret: 6)
        await m.run("A.py", background: true)
        XCTAssertFalse(wire.paths().contains { $0 == "/api/scripts/lint" })
        XCTAssertEqual(wire.calls.first { $0.path == "/api/scripts/run-file" }?.query, ["name": "A.py", "background": "1"])
    }

    @MainActor
    func testDeletingTheOpenFileClosesTheEditor() async {
        let (m, wire, _) = make(Device())
        await m.open("b.py")
        await m.delete("b.py")
        XCTAssertNil(m.openFile)
        XCTAssertTrue(wire.paths().contains("/api/scripts/files/delete"))
    }

    func testREPLGate() {
        let holder = ScriptRunStatus(running: true, state: .running, fileSlotName: "logger.py")
        XCTAssertFalse(ScriptREPLGate.inputEnabled(status: holder, transport: .wifi))
        XCTAssertFalse(ScriptREPLGate.inputEnabled(status: holder, transport: .ble))
        XCTAssertEqual(ScriptREPLGate.banner(status: holder, transport: .wifi),
                       "'logger.py' is running: the REPL is read-only until it stops.")
        XCTAssertTrue(ScriptREPLGate.inputEnabled(status: ScriptRunStatus(running: true, source: .repl, state: .running), transport: .wifi))
        XCTAssertNil(ScriptREPLGate.banner(status: ScriptRunStatus(), transport: .wifi))
        XCTAssertTrue(ScriptREPLGate.inputEnabled(status: nil, transport: .ble))
        XCTAssertTrue(ScriptREPLGate.inputEnabled(status: ScriptRunStatus(), transport: .ble))
        XCTAssertEqual(ScriptREPLGate.banner(status: nil, transport: .ble),
                       "Limited REPL over Bluetooth: no tab completion, output arrives via the log.")
    }

    @MainActor
    func testBLEReplSubmitEvalsAndBuildsTranscript() async {
        let (m, wire, manager) = make(Device(), kind: .ble)
        let repl = ScriptBLERepl(manager: manager)
        await repl.submit("print(1)")
        let call = wire.calls.last
        XCTAssertEqual(call?.path, "/api/scripts/eval")
        XCTAssertEqual(call?.jsonBody?["src"] as? String, "print(1)")
        XCTAssertEqual(call?.jsonBody?["persist"] as? Bool, true)
        XCTAssertEqual(repl.transcript(), ">>> print(1)")
        manager.log.ingest(ScriptLogPage(data: Data("5 I mpy 1\n5 I sys noise\n".utf8), since: 0, next: 24, dropped: 0))
        XCTAssertEqual(repl.transcript(), ">>> print(1)\n1")
        _ = m
    }

    @MainActor
    func testBLEReplTooLargeShowsAnError() async {
        let (_, _, manager) = make(Device(), kind: .ble)
        let repl = ScriptBLERepl(manager: manager)
        await repl.submit(String(repeating: "a", count: 600))
        XCTAssertNotNil(repl.error)
    }

    @MainActor
    func testBLEReplPreservesHistoryAcrossLogReset() async {
        let (m, _, manager) = make(Device(), kind: .ble)
        let repl = ScriptBLERepl(manager: manager)
        await repl.submit("print(1)")
        manager.log.ingest(ScriptLogPage(data: Data("5 I mpy 1\n".utf8), since: 0, next: 10, dropped: 0))
        XCTAssertEqual(repl.transcript(), ">>> print(1)\n1")

        // Reset device log mid-session
        manager.log.reset()
        XCTAssertTrue(manager.log.lines.isEmpty)
        XCTAssertEqual(repl.transcript(), ">>> print(1)\n1")

        // Next command works and captures its output
        await repl.submit("print(2)")
        manager.log.ingest(ScriptLogPage(data: Data("10 I mpy 2\n".utf8), since: 0, next: 10, dropped: 0))
        XCTAssertEqual(repl.transcript(), ">>> print(1)\n1\n>>> print(2)\n2")
        _ = m
    }

    @MainActor
    func testBLEReplPreservesHistoryAcrossLogTrimming() async {
        let (m, _, manager) = make(Device(), kind: .ble)
        let repl = ScriptBLERepl(manager: manager)
        await repl.submit("print(1)")
        manager.log.ingest(ScriptLogPage(data: Data("5 I mpy 1\n".utf8), since: 0, next: 10, dropped: 0))
        XCTAssertEqual(repl.transcript(), ">>> print(1)\n1")

        // Trim the log by exceeding capacity (5000 lines)
        let bulk = (2...5005).map { "\($0) I sys filler\n" }.joined()
        manager.log.ingest(ScriptLogPage(data: Data(bulk.utf8), since: 10, next: UInt64(10 + bulk.utf8.count), dropped: 0))
        XCTAssertFalse(manager.log.lines.contains { $0.text == "1" })

        // REPL output is preserved in the entry itself
        XCTAssertEqual(repl.transcript(), ">>> print(1)\n1")
        _ = m
    }

    @MainActor
    func testReplVisibleFollowsTheBLEReplView() {
        let (_, _, manager) = make(Device(), kind: .ble)
        XCTAssertFalse(manager.replVisible)
        manager.replVisible = true
        XCTAssertTrue(manager.replVisible)
    }
}
