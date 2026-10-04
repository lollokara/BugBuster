import XCTest
@testable import BugBuster

/// Device double behind FakeScriptsWire: scripted status, log pages and run-file replies.
final class FakeScriptsDevice {
    var status: [String: Any] = ["running": false, "state": "idle"]
    /// One chunk per logs call; the cursor advances by the chunk's byte count.
    var logChunks: [String] = []
    /// Next logs call answers as a rebooted device (cursor back to 0).
    var resetNext = false
    var runReplies: [ScriptsWireReply] = []
    var statusHTTP = 200
    lazy var wire = FakeScriptsWire { [unowned self] in self.answer($0) }

    func answer(_ call: FakeScriptsWire.Call) -> ScriptsWireReply {
        switch call.path {
        case "/api/scripts/status":
            return statusHTTP == 200 ? .json(200, status) : .json(statusHTTP, ["error": "Admin token required"])
        case "/api/scripts/logs":
            if resetNext {
                resetNext = false
                return .text(200, "", headers: ["X-BugBuster-Log-Next": "0"])
            }
            let since = UInt64(call.query["since"] ?? "0") ?? 0
            let text = logChunks.isEmpty ? "" : logChunks.removeFirst()
            return .text(200, text, headers: ["X-BugBuster-Log-Next": String(since + UInt64(text.utf8.count))])
        case "/api/scripts/run-file":
            if !runReplies.isEmpty { return runReplies.removeFirst() }
            return .json(200, ["ok": true, "id": 1, "name": call.query["name"] ?? "", "background": call.query["background"] == "1"])
        case "/api/scripts/autorun/status":
            return .json(200, ["enabled": true, "scriptName": "boot.py", "ranThisBoot": true])
        default:
            return .json(200, ["ok": true])
        }
    }

    func count(_ path: String) -> Int { wire.calls.filter { $0.path == path }.count }
}

final class ScriptRunManagerTests: XCTestCase {
    private static let running: [String: Any] = [
        "running": true, "state": "running", "name": "logger.py", "fileSlotName": "logger.py",
        "source": "manual", "startedAt": 990, "startedAtEpoch": true]
    private static let busy = ScriptsWireReply.json(409, ["ok": false, "error": "a script is running; pass replace=1 to stop it",
                                                         "running": "autorun_main.py", "id": 7])

    @MainActor
    private func make(_ device: FakeScriptsDevice, clock: @escaping () -> Date = { Date(timeIntervalSince1970: 1_000) }) -> ScriptRunManager {
        let m = ScriptRunManager(sleeper: { _ in }, clock: clock, autoStart: false)
        m.connect(client: ScriptsClient(wire: device.wire), deviceKey: "dev-A", transport: .wifi)
        return m
    }

    // MARK: run / replace

    @MainActor
    func testForegroundRunOpensConsoleAndMarksLog() async {
        let d = FakeScriptsDevice()
        let m = make(d)
        await m.run("a.py", background: false)
        XCTAssertTrue(m.consoleVisible)
        XCTAssertEqual(m.log.lines.last?.text, "Started 'a.py'")
        XCTAssertEqual(d.wire.calls.first { $0.path == "/api/scripts/run-file" }?.query, ["name": "a.py"])
    }

    @MainActor
    func testBackgroundRunKeepsConsoleClosed() async {
        let d = FakeScriptsDevice()
        let m = make(d)
        await m.run("a.py", background: true)
        XCTAssertFalse(m.consoleVisible)
        XCTAssertEqual(m.log.lines.last?.text, "Started 'a.py' in background")
        XCTAssertEqual(d.wire.calls.first { $0.path == "/api/scripts/run-file" }?.query, ["name": "a.py", "background": "1"])
    }

    @MainActor
    func testBusyPromptUsesTheHolderFromThe409Body() async {
        let d = FakeScriptsDevice()               // status still says idle: the holder started between polls
        d.runReplies = [Self.busy]
        let m = make(d)
        await m.refreshStatus()
        await m.run("a.py", background: false)
        XCTAssertEqual(m.replacePrompt?.running, "autorun_main.py")
        XCTAssertEqual(m.replacePrompt?.message, "Stop 'autorun_main.py' and run 'a.py'?")
        XCTAssertFalse(m.consoleVisible)
    }

    @MainActor
    func testConfirmReplaceKeepsBackgroundFlag() async {
        let d = FakeScriptsDevice()
        d.runReplies = [Self.busy]
        let m = make(d)
        await m.run("a.py", background: true)
        await m.confirmReplace()
        let runs = d.wire.calls.filter { $0.path == "/api/scripts/run-file" }
        XCTAssertEqual(runs.count, 2)
        XCTAssertEqual(runs[1].query, ["name": "a.py", "background": "1", "replace": "1"])
        XCTAssertNil(m.replacePrompt)
        XCTAssertFalse(m.consoleVisible)
    }

    @MainActor
    func testCancelReplaceSendsNothing() async {
        let d = FakeScriptsDevice()
        d.runReplies = [Self.busy]
        let m = make(d)
        await m.run("a.py", background: false)
        m.cancelReplace()
        XCTAssertNil(m.replacePrompt)
        XCTAssertEqual(d.count("/api/scripts/run-file"), 1)
    }

    @MainActor
    func testStopRefreshesStatus() async {
        let d = FakeScriptsDevice()
        let m = make(d)
        await m.stop()
        XCTAssertEqual(d.wire.paths(), ["/api/scripts/stop", "/api/scripts/status"])
    }

    // MARK: polling cadence

    @MainActor
    func testConsoleVisiblePollsStatusAndLogsEverySecond() async {
        var now = Date(timeIntervalSince1970: 1_000)
        let d = FakeScriptsDevice()
        let m = make(d, clock: { now })
        m.consoleVisible = true
        let wait = await m.tick()
        XCTAssertEqual(wait, 1)
        XCTAssertEqual(d.count("/api/scripts/status"), 1)
        XCTAssertEqual(d.count("/api/scripts/logs"), 1)
        now += 0.5
        _ = await m.tick()
        XCTAssertEqual(d.count("/api/scripts/status"), 1)
        now += 0.5
        _ = await m.tick()
        XCTAssertEqual(d.count("/api/scripts/status"), 2)
        XCTAssertEqual(d.count("/api/scripts/logs"), 2)
    }

    @MainActor
    func testPillCadenceIsFiveSeconds() async {
        var now = Date(timeIntervalSince1970: 1_000)
        let d = FakeScriptsDevice()
        d.status = Self.running
        let m = make(d, clock: { now })
        let wait = await m.tick()
        XCTAssertEqual(wait, 5)
        XCTAssertEqual(d.count("/api/scripts/status"), 1)
        XCTAssertEqual(d.count("/api/scripts/logs"), 1)      // active after the status poll → pill logs
        now += 1
        _ = await m.tick()
        XCTAssertEqual(d.count("/api/scripts/status"), 1)
        now += 4
        _ = await m.tick()
        XCTAssertEqual(d.count("/api/scripts/status"), 2)
        XCTAssertEqual(d.count("/api/scripts/logs"), 2)
    }

    @MainActor
    func testIdleAndHiddenPollsOnlyStatus() async {
        let d = FakeScriptsDevice()
        let m = make(d)
        let wait = await m.tick()
        XCTAssertEqual(wait, 5)
        XCTAssertEqual(d.count("/api/scripts/status"), 1)
        XCTAssertEqual(d.count("/api/scripts/logs"), 0)
    }

    @MainActor
    func testREPLVisiblePollsLogsEverySecond() async {
        var now = Date(timeIntervalSince1970: 1_000)
        let d = FakeScriptsDevice()
        let m = make(d, clock: { now })
        m.replVisible = true                                  // BLE mini-REPL: output arrives through the log
        XCTAssertEqual(m.policy, ScriptPollPolicy(status: 1, logs: 1))
        let wait = await m.tick()
        XCTAssertEqual(wait, 1)
        XCTAssertEqual(d.count("/api/scripts/logs"), 1)
        now += 1
        _ = await m.tick()
        XCTAssertEqual(d.count("/api/scripts/status"), 2)
        XCTAssertEqual(d.count("/api/scripts/logs"), 2)
        m.replVisible = false
        XCTAssertEqual(m.policy, ScriptPollPolicy(status: 5, logs: nil))
    }

    @MainActor
    func testScriptsTabPollsStatusEverySecond() async {
        var now = Date(timeIntervalSince1970: 1_000)
        let d = FakeScriptsDevice()
        let m = make(d, clock: { now })
        m.scriptsTabVisible = true
        let wait = await m.tick()
        XCTAssertEqual(wait, 1)
        now += 1
        _ = await m.tick()
        XCTAssertEqual(d.count("/api/scripts/status"), 2)
        XCTAssertEqual(d.count("/api/scripts/logs"), 0)
    }

    // MARK: logs

    @MainActor
    func testFinishedRunDrainsFinalLogs() async {
        let d = FakeScriptsDevice()
        d.status = Self.running
        let m = make(d)
        await m.refreshStatus()
        d.status = ["running": false, "state": "error", "lastExit": "error"]
        d.logChunks = ["5 E mpy ValueError: boom\n"]
        await m.refreshStatus()
        XCTAssertEqual(d.count("/api/scripts/logs"), 1)
        XCTAssertEqual(m.log.lastLine?.level, .error)
        XCTAssertEqual(m.log.lastLine?.text, "ValueError: boom")
    }

    @MainActor
    func testDrainFollowsFullPages() async {
        let d = FakeScriptsDevice()
        d.logChunks = [String(repeating: "1 I mpy x\n", count: 410), "2 I mpy tail\n"]     // 4100 bytes, then a short page
        let m = make(d)
        await m.drainLogs()
        XCTAssertEqual(d.wire.calls.filter { $0.path == "/api/scripts/logs" }.map { $0.query["since"] }, ["0", "4100"])
        XCTAssertEqual(m.log.lastLine?.text, "tail")
    }

    @MainActor
    func testRestartRereadsFromZero() async {
        let d = FakeScriptsDevice()
        d.logChunks = ["1 I mpy a\n"]
        let m = make(d)
        await m.drainLogs()
        d.resetNext = true
        d.logChunks = ["1 I mpy fresh\n"]
        await m.drainLogs()
        XCTAssertEqual(d.wire.calls.filter { $0.path == "/api/scripts/logs" }.map { $0.query["since"] }, ["0", "10", "0"])
        XCTAssertTrue(m.log.lines.contains { $0.isMarker && $0.text == "Device log restarted" })
        XCTAssertEqual(m.log.lastLine?.text, "fresh")
    }

    // MARK: connection lifecycle

    @MainActor
    func testDisconnectStopsPollingAndClearsStatus() async {
        let d = FakeScriptsDevice()
        d.status = Self.running
        let m = make(d)
        await m.refreshStatus()
        XCTAssertNotNil(m.status)
        m.disconnect()
        XCTAssertNil(m.status)
        XCTAssertNil(m.client)
        XCTAssertFalse(m.isConnected)
        let before = d.wire.calls.count
        _ = await m.tick()
        await m.run("a.py", background: false)
        XCTAssertEqual(d.wire.calls.count, before)
    }

    @MainActor
    func testDeviceChangeResetsLog() async {
        let d = FakeScriptsDevice()
        d.logChunks = ["1 I mpy a\n"]
        let m = make(d)
        await m.drainLogs()
        XCTAssertEqual(m.log.cursor, 10)
        m.disconnect()
        m.connect(client: ScriptsClient(wire: d.wire), deviceKey: "dev-A", transport: .ble)
        XCTAssertEqual(m.log.cursor, 10)                  // same device: keep the cursor and lines
        m.connect(client: ScriptsClient(wire: d.wire), deviceKey: "dev-B", transport: .wifi)
        XCTAssertEqual(m.log.cursor, 0)
        XCTAssertTrue(m.log.lines.isEmpty)
    }

    @MainActor
    func testAttachFollowsConnectionState() async throws {
        let cm = ConnectionManager()
        let m = ScriptRunManager(sleeper: { _ in }, autoStart: false)
        m.attach(cm)
        cm.activeDevice = DiscoveredDevice(hostname: "bb", ip: "", port: 0, isBle: true, bleId: UUID())
        cm.transport = .ble
        cm.connectionState = .connected
        try await Task.sleep(nanoseconds: 100_000_000)        // the sink hops through the main queue
        XCTAssertTrue(m.isConnected)
        XCTAssertEqual(m.transport, .ble)
        cm.connectionState = .disconnected
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertFalse(m.isConnected)
    }

    // MARK: REPL gate, autorun, errors, loop

    @MainActor
    func testREPLReadOnlyWhileFileSlotHeld() async {
        let d = FakeScriptsDevice()
        d.status = Self.running
        let m = make(d)
        await m.refreshStatus()
        XCTAssertTrue(m.isREPLReadOnly)
        d.status = ["running": true, "state": "running", "source": "repl"]
        await m.refreshStatus()
        XCTAssertFalse(m.isREPLReadOnly)
    }

    @MainActor
    func testAutorunRefreshAndDisable() async {
        let d = FakeScriptsDevice()
        let m = make(d)
        await m.refreshAutorun()
        XCTAssertEqual(m.autorun?.scriptName, "boot.py")
        await m.setAutorun(enabled: false, name: nil)
        XCTAssertEqual(d.count("/api/scripts/autorun/disable"), 1)
        XCTAssertEqual(d.count("/api/scripts/autorun/status"), 2)
    }

    @MainActor
    func testErrorsSurfaceInLastError() async {
        let d = FakeScriptsDevice()
        d.statusHTTP = 401
        let m = make(d)
        await m.refreshStatus()
        XCTAssertEqual(m.lastError, "The device rejected the admin token")
    }

    @MainActor
    func testLoopSleepsForThePolicyTick() async {
        var sleeps: [TimeInterval] = []
        var now = Date(timeIntervalSince1970: 1_000)
        let d = FakeScriptsDevice()
        let m = ScriptRunManager(sleeper: { seconds in
            sleeps.append(seconds)
            now += seconds                                    // virtual time: each sleep makes the next poll due
            if sleeps.count >= 2 { throw CancellationError() }
        }, clock: { now })
        m.consoleVisible = true                               // no client yet: no loop
        m.connect(client: ScriptsClient(wire: d.wire), deviceKey: "dev-A", transport: .wifi)
        await m.waitForPollingLoop()
        XCTAssertEqual(sleeps, [1, 1])
        XCTAssertEqual(d.count("/api/scripts/logs"), 2)
    }
}
