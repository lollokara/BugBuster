import XCTest
@testable import BugBuster

private let statusJSON = """
{"schema":1,"state":"active","ready":true,"stage":0,"generation":4,"timeoutSeconds":300,
"clients":1,"failedStage":0,"inhibitors":0,"completed":0,"failed":0,"skipped":0,"idleRemainingMs":272000}
""".data(using: .utf8)!

private func status(_ patch: (inout [String: Any]) -> Void = { _ in }) -> StandbyStatus {
    var obj = try! JSONSerialization.jsonObject(with: statusJSON) as! [String: Any]
    patch(&obj)
    return try! JSONDecoder().decode(StandbyStatus.self, from: JSONSerialization.data(withJSONObject: obj))
}

/// Records presence frames and replies from a script, with no network.
@MainActor
private final class FakeDevice {
    var frames: [(id: UInt32, present: Bool)] = []
    var results: [StandbySendResult] = []
    var fallback: StandbySendResult = .ok(nil)

    func sender() -> StandbyPresenceSender {
        { [unowned self] id, present in
            self.frames.append((id, present))
            return self.results.isEmpty ? self.fallback : self.results.removeFirst()
        }
    }
}

@MainActor
final class StandbyPresenceTests: XCTestCase {
    func testStatusDecodesTheJsonRecord() {
        let s = status()
        XCTAssertEqual(s.state, "active")
        XCTAssertTrue(s.hardwareReady)
        XCTAssertEqual(s.generation, 4)
        XCTAssertEqual(s.timeoutSeconds, 300)
        XCTAssertEqual(s.idleLine, "Standby in 4:32")
    }

    func testIdleLineExplainsWhatHoldsTheDeviceAwake() {
        XCTAssertEqual(status { $0["clients"] = 2 }.idleLine, "Held awake by 2 connected clients")
        XCTAssertEqual(status { $0["clients"] = 1; $0["inhibitors"] = 4 }.idleLine,
                       "Held awake by 1 connected client and running work")
        XCTAssertEqual(status { $0["timeoutSeconds"] = 0 }.idleLine, "Automatic standby is off")
        XCTAssertEqual(status { $0["state"] = "asleep"; $0["ready"] = false }.idleLine, "Standby")
        XCTAssertFalse(status { $0["state"] = "waking" }.hardwareReady)
    }

    func testTimeoutChoicesAreTheDevicePolicyValues() {
        XCTAssertEqual(Set(StandbyTimeout.allCases.map(\.rawValue)), [0, 60, 300, 900])
        XCTAssertEqual(StandbyTimeout.off.label, "Off")
    }

    func testClientIdsAreNeverZero() {
        for _ in 0..<500 { XCTAssertNotEqual(StandbyPresence.randomClientId(), 0) }
    }

    func testInterpretMapsRepliesToCapabilities() {
        XCTAssertEqual(StandbyPresence.interpret(data: Data(), httpStatus: 404), .unsupported)
        XCTAssertEqual(StandbyPresence.interpret(data: Data(), httpStatus: 405), .unsupported)
        XCTAssertEqual(StandbyPresence.interpret(data: Data(), httpStatus: 503), .failed)
        XCTAssertEqual(StandbyPresence.interpret(data: nil, httpStatus: nil), .failed)
        let tunnel = #"{"error":"unknown path"}"#.data(using: .utf8)
        XCTAssertEqual(StandbyPresence.interpret(data: tunnel, httpStatus: nil), .unsupported)
        let full = #"{"error":"table full"}"#.data(using: .utf8)
        XCTAssertEqual(StandbyPresence.interpret(data: full, httpStatus: nil), .failed)
        XCTAssertEqual(StandbyPresence.interpret(data: statusJSON, httpStatus: 200), .ok(status()))
    }

    func testOpenRegistersThenRefreshesAndCloseReleasesTheSameId() async throws {
        let dev = FakeDevice()
        let presence = StandbyPresence(refreshInterval: 0.02, makeId: { 77 })
        presence.open(sender: dev.sender())
        try await Task.sleep(nanoseconds: 120_000_000)
        XCTAssertGreaterThanOrEqual(dev.frames.count, 3)
        XCTAssertTrue(dev.frames.allSatisfy { $0.id == 77 && $0.present })
        XCTAssertEqual(presence.capability, .supported)

        presence.close()
        try await Task.sleep(nanoseconds: 60_000_000)
        XCTAssertEqual(dev.frames.last?.present, false)
        XCTAssertEqual(dev.frames.last?.id, 77)
        let n = dev.frames.count
        try await Task.sleep(nanoseconds: 80_000_000)
        XCTAssertEqual(dev.frames.count, n, "no heartbeat after close")
        XCTAssertEqual(presence.clientId, 0)
    }

    func testEveryEpochGetsAFreshId() {
        var next: UInt32 = 10
        let presence = StandbyPresence(refreshInterval: 60, makeId: { next += 1; return next })
        let dev = FakeDevice()
        presence.open(sender: dev.sender())
        XCTAssertEqual(presence.clientId, 11)
        presence.suspend()
        presence.open(sender: dev.sender())
        XCTAssertEqual(presence.clientId, 12)
        presence.suspend()
    }

    func testOldFirmwareGoesQuietAfterOneAttempt() async throws {
        let dev = FakeDevice()
        dev.fallback = .unsupported
        let presence = StandbyPresence(refreshInterval: 0.01, makeId: { 5 })
        presence.open(sender: dev.sender())
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertEqual(dev.frames.count, 1)
        XCTAssertEqual(presence.capability, .unsupported)
        presence.close()
        try await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(dev.frames.count, 1, "no release for an unsupported firmware")
    }

    func testTransientFailuresKeepRetrying() async throws {
        let dev = FakeDevice()
        dev.results = [.failed, .failed]
        let presence = StandbyPresence(refreshInterval: 0.01, makeId: { 9 })
        presence.open(sender: dev.sender())
        try await Task.sleep(nanoseconds: 120_000_000)
        XCTAssertGreaterThanOrEqual(dev.frames.count, 3)
        XCTAssertEqual(presence.capability, .supported)
        presence.suspend()
    }

    func testSuspendStopsWithoutARelease() async throws {
        let dev = FakeDevice()
        let presence = StandbyPresence(refreshInterval: 0.01, makeId: { 3 })
        presence.open(sender: dev.sender())
        try await Task.sleep(nanoseconds: 40_000_000)
        presence.suspend()
        let n = dev.frames.count
        try await Task.sleep(nanoseconds: 60_000_000)
        XCTAssertEqual(dev.frames.count, n)
        XCTAssertTrue(dev.frames.allSatisfy { $0.present })
    }

    func testDetachHoldsClosedUntilAttach() async throws {
        let dev = FakeDevice()
        let presence = StandbyPresence(refreshInterval: 0.01, makeId: { 8 })
        presence.open(sender: dev.sender())
        try await Task.sleep(nanoseconds: 30_000_000)
        presence.detach()
        try await Task.sleep(nanoseconds: 40_000_000)
        let n = dev.frames.count
        XCTAssertEqual(dev.frames.last?.present, false)
        XCTAssertTrue(presence.released)
        try await Task.sleep(nanoseconds: 60_000_000)
        XCTAssertEqual(dev.frames.count, n, "heartbeat must stay closed while released")

        presence.attach()
        try await Task.sleep(nanoseconds: 40_000_000)
        XCTAssertGreaterThan(dev.frames.count, n)
        XCTAssertEqual(dev.frames.last?.present, true)
        XCTAssertFalse(presence.released)
        presence.suspend()
    }
}
