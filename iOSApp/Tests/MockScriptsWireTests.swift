import XCTest
@testable import BugBuster

final class MockScriptsWireTests: XCTestCase {
    // XCTAssert* autoclosures cannot `await`: every call is hoisted into a `let` first.
    private func get(_ wire: MockScriptsWire, _ path: String, _ query: [String: String] = [:]) async throws -> ScriptsWireReply {
        try await wire.send("GET", path, query: query, body: nil)
    }

    private func post(_ wire: MockScriptsWire, _ path: String, _ query: [String: String] = [:],
                      _ body: ScriptsWireBody? = nil) async throws -> ScriptsWireReply {
        try await wire.send("POST", path, query: query, body: body)
    }

    func testStartsWithAnAutorunScriptRunning() async throws {
        let wire = MockScriptsWire()
        let status = try await get(wire, "/api/scripts/status")
        let s = try XCTUnwrap(status.json)
        XCTAssertEqual(s["state"] as? String, "running")
        XCTAssertEqual(s["fileSlotName"] as? String, "background_logger.py")
        XCTAssertEqual(s["source"] as? String, "autorun")
        XCTAssertEqual(s["startedAtEpoch"] as? Bool, true)
        let autorun = try await get(wire, "/api/scripts/autorun/status")
        let a = try XCTUnwrap(autorun.json)
        XCTAssertEqual(a["ranThisBoot"] as? Bool, true)
        XCTAssertEqual(a["running"] as? Bool, true)
        XCTAssertEqual(a["scriptName"] as? String, "background_logger.py")
    }

    func testRunWhileBusyIs409ThenReplaceWins() async throws {
        let wire = MockScriptsWire()
        let busy = try await post(wire, "/api/scripts/run-file", ["name": "hello.py"])
        XCTAssertEqual(busy.status, 409)
        XCTAssertEqual(busy.json?["running"] as? String, "background_logger.py")
        let ok = try await post(wire, "/api/scripts/run-file", ["name": "hello.py", "replace": "1", "background": "1"])
        XCTAssertEqual(ok.status, 200)
        XCTAssertEqual(ok.json?["background"] as? Bool, true)
        let status = try await get(wire, "/api/scripts/status")
        XCTAssertEqual(status.json?["fileSlotName"] as? String, "hello.py")
        XCTAssertEqual(status.json?["source"] as? String, "manual")
    }

    func testLogsPageWithCursorHeader() async throws {
        let wire = MockScriptsWire()
        let first = try await get(wire, "/api/scripts/logs", ["since": "0"])
        let next = try XCTUnwrap(first.header("X-BugBuster-Log-Next").flatMap { UInt64($0) })
        XCTAssertEqual(next, UInt64(first.body.count))
        let text = String(decoding: first.body, as: UTF8.self)
        XCTAssertTrue(text.contains(" E mpy "))
        XCTAssertTrue(text.contains(" W mpy "))
        XCTAssertTrue(text.contains(" I sys "))
        let again = try await get(wire, "/api/scripts/logs", ["since": String(next)])
        XCTAssertEqual(again.header("x-bugbuster-log-next").flatMap { UInt64($0) }, next + UInt64(again.body.count))
    }

    func testOneLinePerSecondWhileRunning() async throws {
        var now = Date(timeIntervalSince1970: 1_000_000)
        let wire = MockScriptsWire(now: { now })
        let first = try await get(wire, "/api/scripts/logs", ["since": "0"])
        let cursor = try XCTUnwrap(first.header("x-bugbuster-log-next"))
        now += 3
        let more = try await get(wire, "/api/scripts/logs", ["since": cursor])
        XCTAssertEqual(String(decoding: more.body, as: UTF8.self).split(separator: "\n").count, 3)
    }

    func testStopGoesIdleAndStopsEmitting() async throws {
        var now = Date(timeIntervalSince1970: 1_000_000)
        let wire = MockScriptsWire(now: { now })
        _ = try await post(wire, "/api/scripts/stop")
        let status = try await get(wire, "/api/scripts/status")
        let s = try XCTUnwrap(status.json)
        XCTAssertEqual(s["running"] as? Bool, false)
        XCTAssertEqual(s["state"] as? String, "done")
        XCTAssertEqual(s["lastExit"] as? String, "stopped")
        XCTAssertEqual(s["fileSlotName"] as? String, "")
        let first = try await get(wire, "/api/scripts/logs", ["since": "0"])
        let cursor = try XCTUnwrap(first.header("x-bugbuster-log-next"))
        now += 5
        let later = try await get(wire, "/api/scripts/logs", ["since": cursor])
        XCTAssertTrue(later.body.isEmpty)
    }

    func testFilesRoundTripLintAndDelete() async throws {
        let wire = MockScriptsWire()
        _ = try await post(wire, "/api/scripts/files", ["name": "new.py"], .text("x = 1\n"))
        let file = try await get(wire, "/api/scripts/files/get", ["name": "new.py"])
        XCTAssertEqual(String(decoding: file.body, as: UTF8.self), "x = 1\n")
        let list = try await get(wire, "/api/scripts/files")
        XCTAssertTrue((list.json?["files"] as? [String])?.contains("new.py") == true)
        // Lint while a script runs (background_logger is running by default)
        let whileRunning = try await post(wire, "/api/scripts/lint", [:], .text("x = 1\n"))
        XCTAssertEqual(whileRunning.json?["ok"] as? Bool, false)
        XCTAssertEqual(whileRunning.json?["err"] as? String, "Interpreter is busy running a script")
        // Stop script to test normal linting
        _ = try await post(wire, "/api/scripts/stop")
        let bad = try await post(wire, "/api/scripts/lint", [:], .text("syntax_error here"))
        XCTAssertEqual(bad.json?["ok"] as? Bool, false)
        XCTAssertEqual(bad.json?["err"] as? String, "line 1: invalid syntax")
        let good = try await post(wire, "/api/scripts/lint", [:], .text("x = 1\n"))
        XCTAssertEqual(good.json?["ok"] as? Bool, true)
        _ = try await post(wire, "/api/scripts/files/delete", ["name": "new.py"])
        let gone = try await get(wire, "/api/scripts/files/get", ["name": "new.py"])
        XCTAssertEqual(gone.status, 404)
    }

    func testLintByName() async throws {
        let wire = MockScriptsWire()
        _ = try await post(wire, "/api/scripts/files", ["name": "bad.py"], .text("syntax_error here"))
        _ = try await post(wire, "/api/scripts/stop")
        let bad = try await post(wire, "/api/scripts/lint", [:], .json(["name": "bad.py"]))
        XCTAssertEqual(bad.json?["ok"] as? Bool, false)
        XCTAssertEqual(bad.json?["err"] as? String, "line 1: invalid syntax")
        let good = try await post(wire, "/api/scripts/lint", [:], .json(["name": "hello.py"]))
        XCTAssertEqual(good.json?["ok"] as? Bool, true)
        let missing = try await post(wire, "/api/scripts/lint", [:], .json(["name": "nonexistent.py"]))
        XCTAssertEqual(missing.json?["ok"] as? Bool, false)
        XCTAssertEqual(missing.json?["err"] as? String, "script not found")
    }

    func testEvalAnswersWithIdAndBusyWhileRunning() async throws {
        let wire = MockScriptsWire()
        // Running by default: eval should answer busy
        let busy = try await post(wire, "/api/scripts/eval", [:], .json(["src": "print(123)"]))
        XCTAssertEqual(busy.status, 409)
        XCTAssertEqual(busy.json?["ok"] as? Bool, false)
        XCTAssertEqual(busy.json?["running"] as? String, "background_logger.py")
        // Stop script and try eval again
        _ = try await post(wire, "/api/scripts/stop")
        let ok = try await post(wire, "/api/scripts/eval", [:], .json(["src": "print(123)"]))
        XCTAssertEqual(ok.status, 200)
        XCTAssertEqual(ok.json?["ok"] as? Bool, true)
        XCTAssertNotNil(ok.json?["id"] as? Int)
    }

    func testAutorunToggle() async throws {
        let wire = MockScriptsWire()
        _ = try await post(wire, "/api/scripts/autorun/disable")
        let off = try await get(wire, "/api/scripts/autorun/status")
        XCTAssertEqual(off.json?["enabled"] as? Bool, false)
        _ = try await post(wire, "/api/scripts/autorun/enable", ["name": "hello.py"])
        let on = try await get(wire, "/api/scripts/autorun/status")
        XCTAssertEqual(on.json?["scriptName"] as? String, "hello.py")
    }

    func testUnknownPathIs400() async throws {
        let reply = try await get(MockScriptsWire(), "/api/scripts/nope")
        XCTAssertEqual(reply.status, 400)
    }
}
