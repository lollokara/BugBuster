import XCTest
@testable import BugBuster

final class ScriptsClientTests: XCTestCase {
    /// Acks a files/chunk request the way api_scripts_file_chunk does.
    private func chunkAck(_ call: FakeScriptsWire.Call) -> ScriptsWireReply {
        let body = call.jsonBody ?? [:]
        let off = body["off"] as? Int ?? -1
        let n = Data(base64Encoded: body["b64"] as? String ?? "")?.count ?? 0
        return .json(200, ["ok": true, "received": off + n, "final": body["final"] as? Bool ?? false])
    }

    // MARK: status / list

    func testStatusDecodes() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["running": true, "name": "a.py", "state": "running", "fileSlotName": "a.py"]) }
        let s = try await ScriptsClient(wire: wire).status()
        XCTAssertEqual(s.state, .running)
        XCTAssertTrue(s.holdsFileSlot)
        XCTAssertEqual(wire.calls.first?.method, "GET")
        XCTAssertEqual(wire.paths(), ["/api/scripts/status"])
    }

    func testFilesList() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["ok": true, "files": ["b.py", "a.py"]]) }
        let files = try await ScriptsClient(wire: wire).files()
        XCTAssertEqual(files, ["b.py", "a.py"])
    }

    func testStorageDecodes() async throws {
        let wire = FakeScriptsWire { _ in
            .json(200, [
                "totalBytes": 1048576.0,
                "usedBytes": 2048.0,
                "freeBytes": 1046528.0,
                "scriptCount": 2,
                "maxScriptBytes": 32768,
                "maxScripts": 32
            ])
        }
        let storage = try await ScriptsClient(wire: wire).storage()
        XCTAssertEqual(storage.totalBytes, 1048576)
        XCTAssertEqual(storage.usedBytes, 2048)
        XCTAssertEqual(storage.freeBytes, 1046528)
        XCTAssertEqual(storage.scriptCount, 2)
        XCTAssertEqual(storage.maxScriptBytes, 32768)
        XCTAssertEqual(storage.maxScripts, 32)
        XCTAssertEqual(wire.calls.first?.method, "GET")
        XCTAssertEqual(wire.paths(), ["/api/scripts/storage"])
    }

    func testStorageMissingFieldThrowsMalformed() async {
        let wire = FakeScriptsWire { _ in
            .json(200, [
                "totalBytes": 1048576.0,
                "usedBytes": 2048.0
            ])
        }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).storage(), .malformed("storage"))
    }

    func testStorageInvalidTypeThrowsMalformed() async {
        let wire = FakeScriptsWire { _ in
            .json(200, [
                "totalBytes": "not_a_number",
                "usedBytes": 2048.0,
                "freeBytes": 1046528.0,
                "scriptCount": 2,
                "maxScriptBytes": 32768,
                "maxScripts": 32
            ])
        }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).storage(), .malformed("storage"))
    }

    func testStorageFirmwareErrorThrows() async {
        let wire = FakeScriptsWire { _ in .json(500, ["error": "SPIFFS info unavailable"]) }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).storage(), .firmware("SPIFFS info unavailable"))
    }

    // MARK: run-file

    func testRunFileStartedSendsFlags() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["ok": true, "id": 12, "name": "a.py", "background": true]) }
        let outcome = try await ScriptsClient(wire: wire).runFile("a.py", background: true, replace: false)
        XCTAssertEqual(outcome, .started(id: 12, name: "a.py", background: true))
        XCTAssertEqual(wire.calls.first?.method, "POST")
        XCTAssertEqual(wire.calls.first?.query, ["name": "a.py", "background": "1"])
    }

    func testRunFileBusyReturnsHolder() async throws {
        let wire = FakeScriptsWire { _ in
            .json(409, ["ok": false, "error": "a script is running; pass replace=1 to stop it", "running": "logger.py", "id": 3])
        }
        let outcome = try await ScriptsClient(wire: wire).runFile("a.py", background: false, replace: false)
        XCTAssertEqual(outcome, .busy(running: "logger.py", id: 3))
    }

    func testRunFileReplaceSendsFlag() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["ok": true, "id": 13, "name": "a.py", "background": false]) }
        _ = try await ScriptsClient(wire: wire).runFile("a.py", background: false, replace: true)
        XCTAssertEqual(wire.calls.first?.query, ["name": "a.py", "replace": "1"])
    }

    func testRunFileMissingScriptIsFirmwareError() async {
        let wire = FakeScriptsWire { _ in .json(400, ["ok": false, "error": "script not found"]) }
        do {
            _ = try await ScriptsClient(wire: wire).runFile("gone.py", background: false, replace: false)
            XCTFail("expected an error")
        } catch {
            XCTAssertEqual(error as? ScriptsClientError, .firmware("script not found"))
        }
    }

    // MARK: logs

    func testLogsHTTPFraming() async throws {
        let wire = FakeScriptsWire { _ in .text(200, "1 I mpy a\n", headers: ["X-BugBuster-Log-Next": "110"]) }
        let page = try await ScriptsClient(wire: wire).logs(since: 100)
        XCTAssertEqual(wire.calls.first?.query, ["since": "100"])
        XCTAssertEqual(page, ScriptLogPage(data: Data("1 I mpy a\n".utf8), since: 100, next: 110, dropped: 0))
    }

    func testLogsHTTPDroppedIsComputed() async throws {
        let wire = FakeScriptsWire { _ in .text(200, "1 I mpy a\n", headers: ["X-BugBuster-Log-Next": "300"]) }
        let page = try await ScriptsClient(wire: wire).logs(since: 100)
        XCTAssertEqual(page.dropped, 190)       // page starts at 290, we asked for 100
    }

    func testLogsHTTPRestartIsDetected() async throws {
        let wire = FakeScriptsWire { _ in .text(200, "", headers: ["X-BugBuster-Log-Next": "40"]) }
        let page = try await ScriptsClient(wire: wire).logs(since: 500)
        XCTAssertTrue(page.restarted)
        XCTAssertEqual(page.dropped, 0)
    }

    func testLogsHTTPWithoutCursorHeaderIsMalformed() async {
        let wire = FakeScriptsWire { _ in .text(200, "legacy drain") }
        do {
            _ = try await ScriptsClient(wire: wire).logs(since: 0)
            XCTFail("expected malformed")
        } catch let ScriptsClientError.malformed(what) {
            XCTAssertTrue(what.contains("X-BugBuster-Log-Next"))
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    func testLogTextThatLooksLikeJSONIsNotAnError() async throws {
        let wire = FakeScriptsWire { _ in .text(200, #"{"ok":false}"#, headers: ["X-BugBuster-Log-Next": "12"]) }
        let page = try await ScriptsClient(wire: wire).logs(since: 0)
        XCTAssertEqual(String(decoding: page.data, as: UTF8.self), #"{"ok":false}"#)
    }

    func testLogsBLEFraming() async throws {
        let b64 = Data("2 W mpy hot\n".utf8).base64EncodedString()
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(200, ["ok": true, "next": 812, "n": 12, "dropped": 7, "data": b64]) }
        let page = try await ScriptsClient(wire: wire).logs(since: 793)
        XCTAssertEqual(page, ScriptLogPage(data: Data("2 W mpy hot\n".utf8), since: 793, next: 812, dropped: 7))
    }

    // MARK: files/get

    func testReadFileHTTPReturnsRawText() async throws {
        let wire = FakeScriptsWire { _ in .text(200, #"{"ok": false}  # a script can look like JSON"#) }
        let text = try await ScriptsClient(wire: wire).readFile("a.py")
        XCTAssertEqual(text, #"{"ok": false}  # a script can look like JSON"#)
        XCTAssertEqual(wire.calls.first?.query, ["name": "a.py"])
    }

    func testReadFileBLEFollowsPages() async throws {
        let source = Data((0..<5000).map { _ in UInt8(ascii: "x") })
        let wire = FakeScriptsWire(kind: .ble) { call in
            let off = Int(call.query["off"] ?? "0") ?? 0
            let len = Int(call.query["len"] ?? "0") ?? 0
            let end = min(off + len, source.count)
            return .json(200, ["ok": true, "name": "a.py", "size": source.count, "off": off, "n": end - off,
                               "data": source.subdata(in: off..<end).base64EncodedString()])
        }
        let text = try await ScriptsClient(wire: wire).readFile("a.py")
        XCTAssertEqual(text.utf8.count, 5000)
        XCTAssertEqual(wire.calls.map { $0.query["off"] }, ["0", "3072"])
        XCTAssertEqual(wire.calls.first?.query["len"], "3072")
    }

    func testReadFileBLETruncatedThrows() async {
        var calls = 0
        let wire = FakeScriptsWire(kind: .ble) { _ in
            calls += 1
            let data = calls == 1 ? Data(repeating: 0x61, count: 3072).base64EncodedString() : ""
            return .json(200, ["ok": true, "name": "a.py", "size": 5000, "off": 0, "n": 0, "data": data])
        }
        do {
            _ = try await ScriptsClient(wire: wire).readFile("a.py")
            XCTFail("a truncated read must not reach the editor")
        } catch {
            XCTAssertEqual(error as? ScriptsClientError, .malformed("files/get stopped at 3072 of 5000 bytes"))
        }
    }

    // MARK: upload

    func testWriteFileHTTPPostsText() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["ok": true]) }
        try await ScriptsClient(wire: wire).writeFile("a.py", text: "print(1)\n")
        XCTAssertEqual(wire.calls.first?.method, "POST")
        XCTAssertEqual(wire.calls.first?.path, "/api/scripts/files")
        XCTAssertEqual(wire.calls.first?.query, ["name": "a.py"])
        XCTAssertEqual(wire.calls.first?.textBody, "print(1)\n")
    }

    func testWriteFileHTTPOkFalseIsAnError() async {
        let wire = FakeScriptsWire { _ in .json(200, ["ok": false, "err": "SPIFFS full"]) }
        do {
            try await ScriptsClient(wire: wire).writeFile("a.py", text: "x")
            XCTFail("expected an error")
        } catch {
            XCTAssertEqual(error as? ScriptsClientError, .firmware("SPIFFS full"))
        }
    }

    func testWriteFileBLEChunksAndReportsProgress() async throws {
        let wire = FakeScriptsWire(kind: .ble)
        wire.handler = { [unowned self] in self.chunkAck($0) }
        let text = String(repeating: "print('hello')\n", count: 100)       // 1500 bytes
        var progress: [Double] = []
        try await ScriptsClient(wire: wire).writeFile("a.py", text: text) { progress.append($0) }
        let plan = ScriptChunkPlanner.plan(name: "a.py", data: Data(text.utf8))
        XCTAssertEqual(wire.calls.count, plan.count)
        XCTAssertTrue(wire.calls.allSatisfy { $0.path == "/api/scripts/files/chunk" && $0.method == "POST" })
        XCTAssertEqual(wire.calls.map { $0.jsonBody?["off"] as? Int }, plan.map { Optional($0.off) })
        XCTAssertEqual(wire.calls.last?.jsonBody?["final"] as? Bool, true)
        XCTAssertEqual(progress.last, 1)
    }

    func testBLEUploadRestartsFromZeroAfterALostReply() async throws {
        let wire = FakeScriptsWire(kind: .ble)
        var n = 0
        wire.handler = { [unowned self] call in
            n += 1
            if n == 3 { throw ConnectionAPIError.bleNoResponse(call.path) }
            return self.chunkAck(call)
        }
        let text = String(repeating: "x", count: 2000)
        try await ScriptsClient(wire: wire).writeFile("a.py", text: text)
        let plan = ScriptChunkPlanner.plan(name: "a.py", data: Data(text.utf8))
        XCTAssertGreaterThanOrEqual(plan.count, 3)
        XCTAssertEqual(wire.calls.count, 3 + plan.count)
        XCTAssertEqual(wire.calls[3].jsonBody?["off"] as? Int, 0)
    }

    func testBLEUploadWrongAckIsMalformed() async {
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(200, ["ok": true, "received": 1, "final": false]) }
        do {
            try await ScriptsClient(wire: wire).writeFile("a.py", text: String(repeating: "y", count: 900))
            XCTFail("expected malformed")
        } catch let ScriptsClientError.malformed(what) {
            XCTAssertTrue(what.hasPrefix("chunk ack"))
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    func testWriteRejectsBadInputBeforeAnyRequest() async {
        let wire = FakeScriptsWire()
        let client = ScriptsClient(wire: wire)
        await XCTAssertThrowsAsync(try await client.writeFile("bad name.py", text: "x"), .invalidName("bad name.py"))
        await XCTAssertThrowsAsync(try await client.writeFile("a.py", text: ""), .empty)
        await XCTAssertThrowsAsync(try await client.writeFile("a.py", text: String(repeating: "z", count: 32769)),
                                   .tooLarge(bytes: 32769))
        XCTAssertTrue(wire.calls.isEmpty)
    }

    // MARK: lint / delete / stop / autorun / errors

    func testLintHTTPReturnsResult() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["ok": false, "err": "line 2: invalid syntax"]) }
        let r = try await ScriptsClient(wire: wire).lint("x = (")
        XCTAssertEqual(r, ScriptLintResult(ok: false, message: "line 2: invalid syntax"))
        XCTAssertEqual(wire.calls.first?.textBody, "x = (")
    }

    func testLintHTTPStatus400SyntaxErrorReturnsLintResult() async throws {
        let wire = FakeScriptsWire { _ in .json(400, ["ok": false, "err": "line 1: invalid syntax"]) }
        let r = try await ScriptsClient(wire: wire).lint("def bad(")
        XCTAssertEqual(r, ScriptLintResult(ok: false, message: "line 1: invalid syntax"))
    }

    func testLintHTTPStatus400BusyReturnsLintResult() async throws {
        let wire = FakeScriptsWire { _ in .json(400, ["ok": false, "err": "Interpreter is busy running a script"]) }
        let r = try await ScriptsClient(wire: wire).lint("x = 1")
        XCTAssertEqual(r, ScriptLintResult(ok: false, message: "Interpreter is busy running a script"))
    }

    func testLintHTTPIgnoresTheName() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["ok": true]) }
        let r = try await ScriptsClient(wire: wire).lint("x = 1", name: "a.py")
        XCTAssertEqual(r, ScriptLintResult(ok: true, message: nil))
        XCTAssertEqual(wire.calls.first?.textBody, "x = 1")
        XCTAssertEqual(wire.calls.first?.query, [:])
    }

    func testLintHTTPUnauthorizedThrows() async {
        let wire = FakeScriptsWire { _ in .json(401, ["error": "Admin token required"]) }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).lint("x = 1"), .unauthorized)
    }

    func testLintHTTPServerErrorWithoutJSONThrows() async {
        let wire = FakeScriptsWire { _ in .text(500, "Internal Server Error") }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).lint("x = 1"), .http(500))
    }

    func testLintHTTPMalformedThrows() async {
        let wire = FakeScriptsWire { _ in .json(200, ["unexpected": "format"]) }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).lint("x = 1"), .malformed("lint"))
    }

    func testLintOverBLEByName() async throws {
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(200, ["ok": false, "err": "line 3: invalid syntax"]) }
        let r = try await ScriptsClient(wire: wire).lint(String(repeating: "x", count: 20_000), name: "a.py")
        XCTAssertEqual(r, ScriptLintResult(ok: false, message: "line 3: invalid syntax"))
        XCTAssertEqual(wire.calls.first?.method, "POST")
        XCTAssertEqual(wire.paths(), ["/api/scripts/lint"])
        XCTAssertEqual(wire.calls.first?.jsonBody?.count, 1)
        XCTAssertEqual(wire.calls.first?.jsonBody?["name"] as? String, "a.py")
    }

    func testLintOverBLEOldFirmwareNeedsWiFi() async {
        // Firmware before BLE lint answers the tunnel with api_error("unknown path");
        // ScriptsTunnelStatus turns its `error` into 400.
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(400, ["ok": false, "error": "unknown path"]) }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).lint("x", name: "a.py"),
                                   .needsWiFi("Checking syntax"))
        XCTAssertEqual(wire.calls.count, 1)
    }

    func testLintOverBLEMissingFileIsFirmwareError() async {
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(400, ["ok": false, "error": "script not found"]) }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).lint("x", name: "gone.py"),
                                   .firmware("script not found"))
    }

    func testLintOverBLEBadNameIsRejectedLocally() async {
        let wire = FakeScriptsWire(kind: .ble)
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).lint("x", name: "bad name.py"),
                                   .invalidName("bad name.py"))
        XCTAssertTrue(wire.calls.isEmpty)
    }

    func testLintOverBLEInlineWhenItFits() async throws {
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(200, ["ok": true]) }
        let r = try await ScriptsClient(wire: wire).lint("x = (1, 2)\n")
        XCTAssertEqual(r, ScriptLintResult(ok: true, message: nil))
        XCTAssertEqual(wire.calls.first?.jsonBody?["src"] as? String, "x = (1, 2)\n")
        XCTAssertNil(wire.calls.first?.jsonBody?["name"])
    }

    func testLintOverBLEWithoutNameTooLargeNeedsWiFi() async {
        let wire = FakeScriptsWire(kind: .ble)
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).lint(String(repeating: "x", count: 600)),
                                   .needsWiFi("Checking syntax"))
        XCTAssertTrue(wire.calls.isEmpty)
    }

    // MARK: eval

    func testEvalHTTPPostsRawSourceWithPersistFlag() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["ok": true, "id": 41]) }
        let client = ScriptsClient(wire: wire)
        let id = try await client.eval("print(1)\n")
        _ = try await client.eval("x = 2", persist: false)
        XCTAssertEqual(id, 41)
        XCTAssertEqual(wire.paths(), ["/api/scripts/eval", "/api/scripts/eval"])
        XCTAssertEqual(wire.calls.first?.method, "POST")
        XCTAssertEqual(wire.calls.first?.textBody, "print(1)\n")       // handle_post_scripts_eval wraps raw text
        XCTAssertEqual(wire.calls.first?.query, ["persist": "1"])
        XCTAssertEqual(wire.calls.last?.query, [:])
    }

    func testEvalBLESendsJSON() async throws {
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(200, ["ok": true, "id": 7]) }
        let id = try await ScriptsClient(wire: wire).eval("a = 1\nb = 2", persist: true)
        XCTAssertEqual(id, 7)
        XCTAssertEqual(wire.calls.first?.query, [:])
        XCTAssertEqual(wire.calls.first?.jsonBody?["src"] as? String, "a = 1\nb = 2")
        XCTAssertEqual(wire.calls.first?.jsonBody?["persist"] as? Bool, true)
    }

    func testEvalBusyIsBusy() async {
        let wire = FakeScriptsWire(kind: .ble) { _ in
            .json(409, ["ok": false, "error": "a script is running; eval is refused until it ends", "running": "logger.py", "id": 3])
        }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).eval("1"), .busy(running: "logger.py", id: 3))
    }

    func testEvalBLETooLargeThrowsBeforeSending() async {
        let wire = FakeScriptsWire(kind: .ble)
        let src = String(repeating: "/", count: 480)
        let bytes = BLETransport.tunnelPayload(path: "/api/scripts/eval", body: ["src": src, "persist": true],
                                               id: ScriptChunkPlanner.worstCaseRequestId)?.count ?? 0
        XCTAssertGreaterThan(bytes, BLETransport.maxTunnelRequestBytes)
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).eval(src), .tooLarge(bytes: bytes))
        XCTAssertTrue(wire.calls.isEmpty)
    }

    func testEvalBLEFitsJustUnderTheLimit() async throws {
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(200, ["ok": true, "id": 1]) }
        let src = String(repeating: "/", count: 440)
        _ = try await ScriptsClient(wire: wire).eval(src)
        XCTAssertEqual(wire.calls.count, 1)
    }

    func testEvalBLEPayloadAt511BytesFitsAnd512BytesIsRejected() async throws {
        let wire = FakeScriptsWire(kind: .ble) { _ in .json(200, ["ok": true, "id": 1]) }
        // 441 slashes yields a payload of exactly 511 bytes; 442 yields 512 bytes.
        let atLimit = String(repeating: "/", count: 441)
        let atLimitBytes = try XCTUnwrap(BLETransport.tunnelPayload(
            path: "/api/scripts/eval", body: ["src": atLimit, "persist": true],
            id: ScriptChunkPlanner.worstCaseRequestId)?.count)
        XCTAssertEqual(atLimitBytes, 511)
        _ = try await ScriptsClient(wire: wire).eval(atLimit)
        XCTAssertEqual(wire.calls.count, 1)

        let overLimit = String(repeating: "/", count: 442)
        let overLimitBytes = try XCTUnwrap(BLETransport.tunnelPayload(
            path: "/api/scripts/eval", body: ["src": overLimit, "persist": true],
            id: ScriptChunkPlanner.worstCaseRequestId)?.count)
        XCTAssertEqual(overLimitBytes, 512)
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).eval(overLimit), .tooLarge(bytes: 512))
    }

    func testEvalEmptyIsRejected() async {
        let wire = FakeScriptsWire()
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).eval(""), .empty)
        XCTAssertTrue(wire.calls.isEmpty)
    }

    func testDeleteUsesTheSharedPostSpelling() async throws {
        let wire = FakeScriptsWire()
        try await ScriptsClient(wire: wire).deleteFile("a.py")
        XCTAssertEqual(wire.calls.first?.method, "POST")
        XCTAssertEqual(wire.calls.first?.path, "/api/scripts/files/delete")
        XCTAssertEqual(wire.calls.first?.query, ["name": "a.py"])
    }

    func testStop() async throws {
        let wire = FakeScriptsWire()
        try await ScriptsClient(wire: wire).stop()
        XCTAssertEqual(wire.paths(), ["/api/scripts/stop"])
    }

    func testAutorunEnableAndDisable() async throws {
        let wire = FakeScriptsWire()
        let client = ScriptsClient(wire: wire)
        try await client.setAutorun(enabled: true, name: "boot.py")
        try await client.setAutorun(enabled: false, name: nil)
        XCTAssertEqual(wire.paths(), ["/api/scripts/autorun/enable", "/api/scripts/autorun/disable"])
        XCTAssertEqual(wire.calls.first?.query, ["name": "boot.py"])
        await XCTAssertThrowsAsync(try await client.setAutorun(enabled: true, name: nil), .invalidName(""))
    }

    func testAutorunStatusDecodes() async throws {
        let wire = FakeScriptsWire { _ in .json(200, ["enabled": true, "has_script": true, "scriptName": "boot.py", "ranThisBoot": true]) }
        let a = try await ScriptsClient(wire: wire).autorunStatus()
        XCTAssertEqual(a.scriptName, "boot.py")
        XCTAssertTrue(a.ranThisBoot)
    }

    func testUnauthorized() async {
        let wire = FakeScriptsWire { _ in .json(401, ["error": "Admin token required"]) }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).status(), .unauthorized)
    }

    func testOtherStatusWithoutTextIsHTTP() async {
        let wire = FakeScriptsWire { _ in .text(500, "") }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).status(), .http(500))
    }

    func testNeedsWiFiFromTheWireIsMapped() async {
        let wire = FakeScriptsWire { _ in throw ConnectionAPIError.needsWiFi("Sending raw script text") }
        await XCTAssertThrowsAsync(try await ScriptsClient(wire: wire).stop(), .needsWiFi("Sending raw script text"))
    }
}

/// `XCTAssertThrowsError` for async expressions, comparing a `ScriptsClientError`.
func XCTAssertThrowsAsync<T>(_ expression: @autoclosure () async throws -> T, _ expected: ScriptsClientError,
                             file: StaticString = #filePath, line: UInt = #line) async {
    do {
        _ = try await expression()
        XCTFail("expected \(expected)", file: file, line: line)
    } catch {
        XCTAssertEqual(error as? ScriptsClientError, expected, file: file, line: line)
    }
}
