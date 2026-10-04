import XCTest
@testable import BugBuster

/// Scripted device behind the BLE tunnel: answers config writes, status and run-list reads.
final class BattSimFakeWire: BLEAPITransport {
    var requests: [(path: String, body: [String: Any]?)] = []
    /// Reply to the CONFIG_ACTION call. nil = no reply (transport timeout).
    var actionReply: Data?
    var statusState = 0
    var statusRun = 0
    var statusLastError = 0
    var runIds: [Int] = []
    /// Called after each CONFIG_ACTION so a test can change the device state.
    var onAction: (() -> Void)?
    /// Polls (status or run list) that answer "HAT busy" before the device answers again.
    var busyPolls = 0
    /// Reads that failed so far (counted for assertions).
    var busyHits = 0

    static let ok = Data(#"{"ok":true,"data":""}"#.utf8)
    static let rejected = Data(#"{"ok":false,"error":"rejected by the DAQ HAT"}"#.utf8)

    func statusBlob() -> String {
        var b = [UInt8](repeating: 0, count: 96)
        b[1] = UInt8(statusState); b[3] = UInt8(statusLastError)
        b[4] = UInt8(statusRun & 0xFF); b[5] = UInt8(statusRun >> 8)
        return Data(b).base64EncodedString()
    }

    func runListBlob() -> String {
        var b: [UInt8] = [UInt8(runIds.count), 0, UInt8(statusRun), 0]
        for id in runIds { b += [UInt8(id & 0xFF), UInt8(id >> 8)] }
        return Data(b).base64EncodedString()
    }

    func apiRequest(path: String, body: [String: Any]?, timeout: TimeInterval) async -> Data? {
        requests.append((path, body))
        if path == "/api/daq/config", body?["op"] as? Int == 4 {
            let r = actionReply
            onAction?()
            return r
        }
        if path == "/api/daq/bs" {
            if busyPolls > 0 { busyPolls -= 1; busyHits += 1; return Data(#"{"ok":false,"error":"HAT busy: a run action is in progress"}"#.utf8) }
            let op = body?["op"] as? Int
            let blob = op == 0 ? statusBlob() : op == 1 ? runListBlob() : ""
            return Data(#"{"ok":true,"data":"\#(blob)"}"#.utf8)
        }
        return Self.ok
    }
}

final class BattSimDeleteTests: XCTestCase {

    @MainActor
    private func makeService(_ fake: BattSimFakeWire) -> BattSimService {
        let cm = ConnectionManager()
        cm.bleAPI = fake
        cm.transport = .ble
        cm.activeDevice = DiscoveredDevice(hostname: "bb", ip: "", port: 0, isBle: true, bleId: UUID())
        return BattSimService(cm)
    }

    @MainActor
    func testDeleteRequestBytesWithRunSelection() async throws {
        let fake = BattSimFakeWire()
        fake.actionReply = BattSimFakeWire.ok
        let service = makeService(fake)
        try await service.runAction(.delete, run: 5, pollNanos: 1_000_000)
        let cfg = fake.requests.filter { $0.path == "/api/daq/config" }
        // select run 5 (key 0x080E, u16, len 2) then action 13 (0x0d)
        XCTAssertEqual(cfg[0].body?["op"] as? Int, 1)
        XCTAssertEqual(cfg[0].body?["args"] as? String, "0e0804020500")
        XCTAssertEqual(cfg[1].body?["op"] as? Int, 4)
        XCTAssertEqual(cfg[1].body?["args"] as? String, "0d")
    }

    @MainActor
    func testLostReplyPollsUntilRunIsGoneInsteadOfTimingOut() async throws {
        let fake = BattSimFakeWire()
        fake.actionReply = nil                     // the S3 gave up / BLE reply lost
        fake.runIds = [1, 2, 5]
        fake.statusRun = 2; fake.statusState = 4
        fake.busyPolls = 3                         // P4 still busy for the first polls
        fake.onAction = { [unowned fake] in
            // The P4 finishes the delete a moment later.
            Task { @MainActor in try? await Task.sleep(nanoseconds: 20_000_000); fake.runIds = [1, 2] }
        }
        let service = makeService(fake)
        try await service.runAction(.delete, run: 5, settleTimeout: 5, pollNanos: 5_000_000)
        XCTAssertEqual(fake.runIds, [1, 2])
        XCTAssertGreaterThanOrEqual(fake.busyHits, 3, "busy replies must be retried, not surfaced")
        let actions = fake.requests.filter { $0.path == "/api/daq/config" && $0.body?["op"] as? Int == 4 }
        XCTAssertEqual(actions.count, 1, "the action must be sent exactly once")
    }

    @MainActor
    func testLostReplyThatNeverSettlesReportsTimeout() async {
        let fake = BattSimFakeWire()
        fake.actionReply = nil
        fake.runIds = [1, 2, 5]
        let service = makeService(fake)
        do {
            try await service.runAction(.delete, run: 5, settleTimeout: 0.1, pollNanos: 10_000_000)
            XCTFail("must not report success while the run is still listed")
        } catch BattSimError.transport(let m) {
            XCTAssertTrue(m.contains("did not finish"), m)
        } catch { XCTFail("\(error)") }
    }

    @MainActor
    func testP4RefusalIsFinalAndNotPolled() async {
        let fake = BattSimFakeWire()
        fake.actionReply = BattSimFakeWire.rejected
        fake.statusLastError = 3
        let service = makeService(fake)
        let before = fake.requests.count
        do {
            try await service.runAction(.delete, run: 2, settleTimeout: 5, pollNanos: 1_000_000)
            XCTFail("a refused delete must throw")
        } catch BattSimError.rejected { } catch { XCTFail("\(error)") }
        // one status read for the baseline, select-run, the action; no settle polling
        let polls = fake.requests[before...].filter { $0.path == "/api/daq/bs" && $0.body?["op"] as? Int == 1 }
        XCTAssertTrue(polls.isEmpty)
    }

    func testSettlePredicates() {
        func st(_ state: Int, _ run: Int) -> BsStatus { var s = BsStatus(); s.state = state; s.runId = run; return s }
        XCTAssertFalse(BattSim.actionSettled(.delete, run: 5, status: nil, runIds: nil, prevRunId: nil))
        XCTAssertFalse(BattSim.actionSettled(.delete, run: 5, status: nil, runIds: [1, 5], prevRunId: nil))
        XCTAssertTrue(BattSim.actionSettled(.delete, run: 5, status: nil, runIds: [1], prevRunId: nil))
        XCTAssertFalse(BattSim.actionSettled(.load, run: 2, status: st(0, 0), runIds: nil, prevRunId: nil))
        XCTAssertTrue(BattSim.actionSettled(.load, run: 2, status: st(4, 2), runIds: nil, prevRunId: nil))
        XCTAssertTrue(BattSim.actionSettled(.unload, run: nil, status: st(0, 0), runIds: nil, prevRunId: 2))
        XCTAssertFalse(BattSim.actionSettled(.newRun, run: nil, status: st(1, 2), runIds: nil, prevRunId: 2))
        XCTAssertTrue(BattSim.actionSettled(.newRun, run: nil, status: st(1, 3), runIds: nil, prevRunId: 2))
        XCTAssertTrue(BattSim.actionSettled(.reopen, run: nil, status: st(1, 2), runIds: nil, prevRunId: 2))
    }

    func testRefusalTextExplainsLoadedRun() {
        XCTAssertTrue(BattSim.refusalText(.delete, lastError: 3).contains("Unload it first"))
        XCTAssertEqual(BattSim.refusalText(.delete, lastError: 9), "run not found")
        XCTAssertTrue(BattSim.isLongAction(.load) && !BattSim.isLongAction(.start))
    }
}
