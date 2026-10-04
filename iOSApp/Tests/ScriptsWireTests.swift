import XCTest
@testable import BugBuster

/// BLE tunnel double that hands out queued replies (nil = no reply / timeout).
final class ScriptsTunnelFakeBLE: BLEAPITransport {
    var requests: [(path: String, body: [String: Any]?)] = []
    var replies: [Data?] = []

    func apiRequest(path: String, body: [String: Any]?, timeout: TimeInterval) async -> Data? {
        requests.append((path, body))
        return replies.isEmpty ? nil : replies.removeFirst()
    }
}

final class ScriptsWireTests: XCTestCase {
    @MainActor
    private func bleManager(_ fake: ScriptsTunnelFakeBLE) -> ConnectionManager {
        let cm = ConnectionManager()
        cm.bleAPI = fake
        cm.transport = .ble
        cm.activeDevice = DiscoveredDevice(hostname: "bb", ip: "", port: 0, isBle: true, bleId: UUID())
        return cm
    }

    // MARK: status synthesis (mirrors webserver.cpp send_scripts_result)

    func testTunnelStatusMirrorsSendScriptsResult() {
        XCTAssertEqual(ScriptsTunnelStatus.status(for: Data(#"{"ok":false,"error":"busy","running":"a.py","id":3}"#.utf8)), 409)
        XCTAssertEqual(ScriptsTunnelStatus.status(for: Data(#"{"ok":false,"error":"valid name required"}"#.utf8)), 400)
        XCTAssertEqual(ScriptsTunnelStatus.status(for: Data(#"{"enabled":true,"running":false}"#.utf8)), 200)
        XCTAssertEqual(ScriptsTunnelStatus.status(for: Data(#"{"running":true,"name":"a.py"}"#.utf8)), 200)
        XCTAssertEqual(ScriptsTunnelStatus.status(for: Data("not json".utf8)), 200)
    }

    // MARK: ConnectionScriptsWire over BLE

    @MainActor
    func testBLESendAppendsSortedQueryAndPassesJSONBody() async throws {
        let fake = ScriptsTunnelFakeBLE()
        fake.replies = [Data(#"{"ok":true,"id":4,"name":"a.py","background":true}"#.utf8)]
        let wire = ConnectionScriptsWire(bleManager(fake))
        let reply = try await wire.send("POST", "/api/scripts/run-file",
                                        query: ["name": "a.py", "background": "1"], body: .json(["x": 1]))
        XCTAssertEqual(fake.requests.map(\.path), ["/api/scripts/run-file?background=1&name=a.py"])
        XCTAssertEqual(fake.requests.first?.body?["x"] as? Int, 1)
        XCTAssertEqual(reply.status, 200)
        XCTAssertTrue(reply.headers.isEmpty)
        XCTAssertEqual(wire.kind, .ble)
    }

    @MainActor
    func testBLEBusyReplyBecomes409() async throws {
        let fake = ScriptsTunnelFakeBLE()
        fake.replies = [Data(#"{"ok":false,"error":"a script is running; pass replace=1 to stop it","running":"b.py","id":9}"#.utf8)]
        let reply = try await ConnectionScriptsWire(bleManager(fake))
            .send("POST", "/api/scripts/run-file", query: ["name": "a.py"], body: nil)
        XCTAssertEqual(reply.status, 409)
        XCTAssertEqual(reply.json?["running"] as? String, "b.py")
    }

    @MainActor
    func testBLERefusesRawText() async {
        let fake = ScriptsTunnelFakeBLE()
        let wire = ConnectionScriptsWire(bleManager(fake))
        do {
            _ = try await wire.send("POST", "/api/scripts/files", query: ["name": "a.py"], body: .text("print(1)"))
            XCTFail("expected needsWiFi")
        } catch ConnectionAPIError.needsWiFi {
        } catch {
            XCTFail("unexpected \(error)")
        }
        XCTAssertTrue(fake.requests.isEmpty)
    }

    @MainActor
    func testBLENoReplyThrows() async {
        let fake = ScriptsTunnelFakeBLE()
        do {
            _ = try await ConnectionScriptsWire(bleManager(fake)).send("GET", "/api/scripts/status", query: [:], body: nil)
            XCTFail("expected bleNoResponse")
        } catch ConnectionAPIError.bleNoResponse(let path) {
            XCTAssertEqual(path, "/api/scripts/status")
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    // MARK: HTTP exchange

    @MainActor
    func testHTTPWithoutAnIPThrowsBadURL() async {
        let cm = ConnectionManager()
        cm.transport = .wifi
        cm.activeDevice = DiscoveredDevice(hostname: "bb", ip: "  ", port: 80)
        do {
            _ = try await cm.httpExchange(method: "GET", path: "/api/scripts/status")
            XCTFail("expected badURL")
        } catch let error as URLError {
            XCTAssertEqual(error.code, .badURL)
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    func testReplyHeadersAreCaseInsensitive() {
        let r = ScriptsWireReply.text(200, "x", headers: ["X-BugBuster-Log-Next": "12"])
        XCTAssertEqual(r.header("x-bugbuster-log-next"), "12")
        XCTAssertEqual(r.header("X-BugBuster-Log-Next"), "12")
        XCTAssertTrue(r.isSuccess)
        XCTAssertNil(r.json)
    }

    // MARK: BLE payload

    func testTunnelPayloadKeepsSlashesAndId() throws {
        let data = try XCTUnwrap(BLETransport.tunnelPayload(path: "/api/scripts/files/chunk", body: ["b64": "//+/"], id: 255))
        let s = String(decoding: data, as: UTF8.self)
        XCTAssertFalse(s.contains("\\/"))
        XCTAssertTrue(s.contains(#""b64":"//+/""#))
        XCTAssertTrue(s.contains(#""id":255"#))
        XCTAssertTrue(s.contains(#""path":"/api/scripts/files/chunk""#))
    }

    func testEmptyBodyIsOmitted() throws {
        let s = String(decoding: try XCTUnwrap(BLETransport.tunnelPayload(path: "/p", body: [:], id: 1)), as: UTF8.self)
        XCTAssertFalse(s.contains("body"))
    }

    func testMaxTunnelRequestBytesIs511() {
        XCTAssertEqual(BLETransport.maxTunnelRequestBytes, 511)
    }

    // MARK: chunk planner

    func testEveryChunkFitsTheTunnel() throws {
        let name = String(repeating: "n", count: 29) + ".py"        // 32 chars, the longest valid name
        let data = Data(repeating: 0xFF, count: 32768)                // base64 "////…": worst case for escaping
        let chunks = ScriptChunkPlanner.plan(name: name, data: data)
        XCTAssertFalse(chunks.isEmpty)
        var expectedOff = 0
        var joined = Data()
        for (i, chunk) in chunks.enumerated() {
            XCTAssertEqual(chunk.off, expectedOff)
            XCTAssertEqual(chunk.final, i == chunks.count - 1)
            let payload = try XCTUnwrap(BLETransport.tunnelPayload(
                path: ScriptChunkPlanner.path,
                body: ScriptChunkPlanner.body(name: name, chunk: chunk),
                id: ScriptChunkPlanner.worstCaseRequestId))
            XCTAssertLessThanOrEqual(payload.count, BLETransport.maxTunnelRequestBytes, "chunk \(i)")
            expectedOff += chunk.bytes.count
            joined.append(chunk.bytes)
        }
        XCTAssertEqual(joined, data)
    }

    func testShortNamesGetBiggerChunks() {
        let short = ScriptChunkPlanner.rawBytesPerChunk(name: "a.py", totalBytes: 100)
        let long = ScriptChunkPlanner.rawBytesPerChunk(name: String(repeating: "n", count: 29) + ".py", totalBytes: 32768)
        XCTAssertGreaterThan(short, long)
        XCTAssertGreaterThan(long, 0)
        XCTAssertEqual(short % 3, 0)
    }

    func testSmallFileIsOneFinalChunk() {
        let text = Data("print(1)\n".utf8)
        XCTAssertEqual(ScriptChunkPlanner.plan(name: "a.py", data: text), [ScriptChunk(off: 0, bytes: text, final: true)])
    }

    func testEmptyDataPlansNothing() {
        XCTAssertEqual(ScriptChunkPlanner.plan(name: "a.py", data: Data()), [])
    }

    func testDataSliceIsHandled() {
        let big = Data((0..<2000).map { UInt8($0 % 251) })
        let slice = big[100..<1900]
        let joined = ScriptChunkPlanner.plan(name: "a.py", data: slice).reduce(into: Data()) { $0.append($1.bytes) }
        XCTAssertEqual(joined, Data(slice))
    }

    // MARK: test double

    func testFakeWireRecordsCalls() async throws {
        let wire = FakeScriptsWire()
        let reply = try await wire.send("GET", "/x", query: ["a": "1"], body: .json(["k": "v"]))
        XCTAssertEqual(wire.paths(), ["/x"])
        XCTAssertEqual(wire.calls.first?.jsonBody?["k"] as? String, "v")
        XCTAssertEqual(reply.json?["ok"] as? Bool, true)
    }
}
