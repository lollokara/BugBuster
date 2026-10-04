import XCTest
@testable import BugBuster

final class ScriptRuntimeModelsTests: XCTestCase {
    // Shape of api_scripts_status() (api_scripts.cpp) while a file script runs.
    private let v2Running = """
    {"running":true,"currentScriptId":7,"totalRuns":12,"totalErrors":1,"lastError":"",
     "mode":"EPHEMERAL","globalsBytes":0,"globalsCount":0,"autoResetCount":0,"lastEvalAtMs":120500,
     "idleForMs":0,"watermarkSoftHit":false,"name":"logger.py","source":"manual","state":"running",
     "lastExit":"none","startedAt":1760000000,"startedAtEpoch":true,"fileSlotId":7,
     "fileSlotName":"logger.py","lastScriptId":7}
    """

    func testDecodesV2Status() throws {
        let s = try JSONDecoder().decode(ScriptRunStatus.self, from: Data(v2Running.utf8))
        XCTAssertTrue(s.running)
        XCTAssertEqual(s.name, "logger.py")
        XCTAssertEqual(s.source, .manual)
        XCTAssertEqual(s.state, .running)
        XCTAssertEqual(s.lastExit, ScriptExitKind.none)
        XCTAssertEqual(s.startedAt, 1_760_000_000)
        XCTAssertTrue(s.startedAtEpoch)
        XCTAssertEqual(s.fileSlotId, 7)
        XCTAssertTrue(s.holdsFileSlot)
        XCTAssertTrue(s.isActive)
        XCTAssertEqual(s.displayName, "logger.py")
    }

    func testLegacyStatusDecodesWithDerivedState() throws {
        let legacy = #"{"running":true,"currentScriptId":3,"totalRuns":1,"totalErrors":0,"lastError":""}"#
        let s = try JSONDecoder().decode(ScriptRunStatus.self, from: Data(legacy.utf8))
        XCTAssertEqual(s.state, .running)
        XCTAssertEqual(s.source, .manual)
        XCTAssertFalse(s.holdsFileSlot)
        XCTAssertTrue(s.isActive)
        XCTAssertEqual(s.displayName, "script")

        let idle = try JSONDecoder().decode(ScriptRunStatus.self, from: Data(#"{"running":false}"#.utf8))
        XCTAssertEqual(idle.state, .idle)
        XCTAssertFalse(idle.isActive)
    }

    func testUnknownEnumStringsDoNotFailDecoding() throws {
        let json = #"{"running":false,"state":"paused","source":"hub","lastExit":"crashed"}"#
        let s = try JSONDecoder().decode(ScriptRunStatus.self, from: Data(json.utf8))
        XCTAssertEqual(s.state, .unknown)
        XCTAssertEqual(s.source, .unknown)
        XCTAssertEqual(s.lastExit, .unknown)
    }

    func testStoppingCountsAsActiveAndReplNameFallsBack() {
        XCTAssertTrue(ScriptRunStatus(state: .stopping).isActive)
        XCTAssertFalse(ScriptRunStatus(state: .done).isActive)
        XCTAssertEqual(ScriptRunStatus(running: true, source: .repl, state: .running).displayName, "REPL")
        XCTAssertEqual(ScriptRunStatus(name: "a.py", state: .done).displayName, "a.py")
    }

    func testDecodesAutorunStatusBothKeyStyles() throws {
        let json = """
        {"enabled":true,"has_script":true,"io12_high":true,"last_run_ok":false,"last_run_id":4,
         "scriptName":"boot.py","ranThisBoot":true,"running":false}
        """
        let a = try JSONDecoder().decode(ScriptAutorunStatus.self, from: Data(json.utf8))
        XCTAssertEqual(a, ScriptAutorunStatus(enabled: true, hasScript: true, io12High: true, lastRunOk: false,
                                              lastRunId: 4, scriptName: "boot.py", ranThisBoot: true, running: false))
        let old = try JSONDecoder().decode(ScriptAutorunStatus.self, from: Data(#"{"enabled":false}"#.utf8))
        XCTAssertEqual(old.scriptName, "")
        XCTAssertFalse(old.ranThisBoot)
    }

    func testDecodesStorage() throws {
        let json = #"{"totalBytes":1048576,"usedBytes":4096,"freeBytes":1044480,"scriptCount":2,"maxScriptBytes":32768,"maxScripts":64}"#
        let s = try JSONDecoder().decode(ScriptStorageInfo.self, from: Data(json.utf8))
        XCTAssertEqual(s.scriptCount, 2)
        XCTAssertEqual(s.maxScriptBytes, 32768)
    }

    func testScriptNameMirrorsFirmwareValidation() {
        for ok in ["a.py", "my_script-2.py", "x.y.py", String(repeating: "a", count: 29) + ".py"] {
            XCTAssertTrue(ScriptName.isValid(ok), ok)
        }
        for bad in ["", ".py", ".hidden.py", "a.txt", "a py.py", "a/b.py", "é.py",
                    String(repeating: "a", count: 30) + ".py"] {
            XCTAssertFalse(ScriptName.isValid(bad), bad)
        }
        XCTAssertEqual(ScriptName.normalized("  blink "), "blink.py")
        XCTAssertEqual(ScriptName.normalized("blink.py"), "blink.py")
        XCTAssertNil(ScriptName.normalized("bad name"))
        XCTAssertNil(ScriptName.normalized("   "))
    }

    func testElapsedFromEpochStart() {
        let s = ScriptRunStatus(running: true, state: .running, startedAt: 1_000, startedAtEpoch: true)
        XCTAssertEqual(ScriptElapsed.seconds(s, now: Date(timeIntervalSince1970: 1_065), anchor: nil), 65)
    }

    func testElapsedFromUptimeAnchor() {
        let s = ScriptRunStatus(running: true, state: .running, startedAt: 10_000, startedAtEpoch: false)
        let t0 = Date(timeIntervalSince1970: 500)
        let anchor = UptimeAnchor(uptimeMs: 40_000, at: t0)
        XCTAssertEqual(ScriptElapsed.seconds(s, now: t0.addingTimeInterval(2), anchor: anchor), 32)
        XCTAssertNil(ScriptElapsed.seconds(s, now: t0, anchor: nil))
        XCTAssertNil(ScriptElapsed.seconds(ScriptRunStatus(), now: t0, anchor: anchor))
    }

    func testElapsedNeverNegative() {
        let s = ScriptRunStatus(running: true, state: .running, startedAt: 2_000, startedAtEpoch: true)
        XCTAssertEqual(ScriptElapsed.seconds(s, now: Date(timeIntervalSince1970: 1_990), anchor: nil), 0)
    }

    func testElapsedFormat() {
        XCTAssertEqual(ScriptElapsed.format(0), "0s")
        XCTAssertEqual(ScriptElapsed.format(59.9), "59s")
        XCTAssertEqual(ScriptElapsed.format(65), "1m 05s")
        XCTAssertEqual(ScriptElapsed.format(3_725), "1h 02m")
    }

    func testPollPolicy() {
        XCTAssertEqual(ScriptPollPolicy.make(consoleVisible: true, scriptsTabVisible: false, active: false),
                       ScriptPollPolicy(status: 1, logs: 1))
        XCTAssertEqual(ScriptPollPolicy.make(consoleVisible: false, scriptsTabVisible: true, active: true),
                       ScriptPollPolicy(status: 1, logs: 5))
        XCTAssertEqual(ScriptPollPolicy.make(consoleVisible: false, scriptsTabVisible: true, active: false),
                       ScriptPollPolicy(status: 1, logs: nil))
        XCTAssertEqual(ScriptPollPolicy.make(consoleVisible: false, scriptsTabVisible: false, active: true),
                       ScriptPollPolicy(status: 5, logs: 5))
        XCTAssertEqual(ScriptPollPolicy.make(consoleVisible: false, scriptsTabVisible: false, active: false),
                       ScriptPollPolicy(status: 5, logs: nil))
        XCTAssertEqual(ScriptPollPolicy(status: 1, logs: 5).tick, 1)
        XCTAssertEqual(ScriptPollPolicy(status: 5, logs: nil).tick, 5)
    }
}
