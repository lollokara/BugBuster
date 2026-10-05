import XCTest
@testable import BugBuster

private final class FakeHub: HubAPI {
    var healthResult: Result<HubHealth, Error> = .success(HubHealth(ok: true, version: "1.0.0", commit: nil))
    var devicesResult: Result<[HubDevice], Error> = .success([])
    var runsResult: Result<[HubRunInfo], Error> = .success([])
    var forgot = 0
    func health() async throws -> HubHealth { try healthResult.get() }
    func devices() async throws -> [HubDevice] { try devicesResult.get() }
    func runs(device: String?, limit: Int) async throws -> [HubRunInfo] { try runsResult.get() }
    func run(uid: String) async throws -> HubRunInfo { throw HubError.http(404) }
    func coverage(uid: String) async throws -> HubCoverage { throw HubError.http(404) }
    func logs(device: String?, limit: Int) async throws -> [HubLogEntry] { [] }
    func forgetFailure() { forgot += 1 }
}

private func fixture(_ name: String) throws -> Data {
    let url = try XCTUnwrap(Bundle(for: HubStatusTests.self).url(forResource: name, withExtension: "json"), "missing fixture \(name)")
    return try Data(contentsOf: url)
}

private let hubURL = URL(string: "http://192.168.3.87:8080")!

@MainActor
final class HubStatusTests: XCTestCase {
    // MARK: state transitions

    func testNoAPIIsNotConfigured() async {
        let m = HubStatusModel(api: nil, url: nil)
        XCTAssertEqual(m.reachability, .notConfigured)
        await m.check()
        XCTAssertEqual(m.reachability, .notConfigured)
        XCTAssertEqual(m.reachability.caption, "No hub")
    }

    func testCheckingThenOnlineRecordsVersionAndLatency() async {
        var clock = Date(timeIntervalSince1970: 1000)
        let hub = FakeHub()
        let m = HubStatusModel(api: hub, url: hubURL, now: { defer { clock += 0.05 }; return clock })
        XCTAssertEqual(m.reachability, .checking)
        await m.check()
        XCTAssertEqual(m.reachability, .online)
        XCTAssertEqual(m.health?.version, "1.0.0")
        XCTAssertEqual(try XCTUnwrap(m.latency), 0.05, accuracy: 0.001)
        XCTAssertNotNil(m.lastCheck)
        XCTAssertNil(m.failure)
    }

    func testOnlineToOfflineAndBack() async {
        let hub = FakeHub()
        let m = HubStatusModel(api: hub, url: hubURL)
        await m.check()
        hub.healthResult = .failure(HubError.unreachable)
        await m.check()
        XCTAssertEqual(m.reachability, .offline)
        XCTAssertEqual(m.failure, .unreachable)
        XCTAssertNil(m.latency)
        XCTAssertEqual(m.health?.version, "1.0.0", "last known version survives an outage")
        hub.healthResult = .success(HubHealth(ok: true, version: "1.1.0", commit: nil))
        await m.check()
        XCTAssertEqual(m.reachability, .online)
        XCTAssertEqual(m.health?.version, "1.1.0")
        XCTAssertNil(m.failure)
    }

    func testHealthNotOkIsOffline() async {
        let hub = FakeHub(); hub.healthResult = .success(HubHealth(ok: false, version: nil, commit: nil))
        let m = HubStatusModel(api: hub, url: hubURL)
        await m.check()
        XCTAssertEqual(m.reachability, .offline)
    }

    func testServerErrorAndDecodeFailuresMapToFailure() async {
        let hub = FakeHub(); hub.healthResult = .failure(HubError.http(502))
        let m = HubStatusModel(api: hub, url: hubURL)
        await m.check()
        XCTAssertEqual(m.failure, .http(502))
        hub.healthResult = .failure(HubError.decode("healthz"))
        await m.check()
        XCTAssertEqual(m.failure, .unexpected)
    }

    func testConfigureNilTurnsOffAndClears() async {
        let hub = FakeHub(); hub.devicesResult = .success([HubDevice(id: "a", name: "a", vendor: "espfleet", status: "online")])
        let m = HubStatusModel(api: hub, url: hubURL)
        await m.refresh(deviceID: nil)
        XCTAssertEqual(m.devices.count, 1)
        m.configure(api: nil, url: nil)
        XCTAssertEqual(m.reachability, .notConfigured)
        XCTAssertTrue(m.devices.isEmpty)
        XCTAssertNil(m.health)
    }

    func testRefreshForgetsFailureAndLoadsLists() async {
        let hub = FakeHub()
        hub.runsResult = .success([
            HubRunInfo(uid: "441bf6c47884-1-100", deviceID: "441bf6c47884", runID: 1, startedAt: 100, name: "old", state: "stopped"),
            HubRunInfo(uid: "441bf6c47884-2-200", deviceID: "441bf6c47884", runID: 2, startedAt: 200, name: "new", state: "running")])
        let m = HubStatusModel(api: hub, url: hubURL)
        await m.refresh(deviceID: "441bf6c47884")
        XCTAssertEqual(hub.forgot, 1)
        XCTAssertEqual(m.runs.map(\.name), ["new", "old"], "newest first")
        XCTAssertNotNil(m.lastSync)
        XCTAssertTrue(m.loadedOnce)
    }

    func testRefreshOfflineSkipsLists() async {
        let hub = FakeHub(); hub.healthResult = .failure(HubError.unreachable)
        let m = HubStatusModel(api: hub, url: hubURL)
        await m.refresh(deviceID: nil)
        XCTAssertEqual(m.reachability, .offline)
        XCTAssertFalse(m.loadedOnce)
        XCTAssertNil(m.lastSync)
    }

    func testPollChecksRepeatedlyUntilCancelled() async {
        var sleeps: [TimeInterval] = []
        var task: Task<Void, Never>?
        let hub = FakeHub()
        let m = HubStatusModel(api: hub, url: hubURL, pollInterval: 30, sleep: { sleeps.append($0); if sleeps.count == 3 { task?.cancel() } })
        task = Task { await m.poll() }
        await task?.value
        XCTAssertEqual(sleeps, [30, 30, 30])
        XCTAssertEqual(m.reachability, .online)
    }

    func testTileCaptionsAndAccessibility() {
        XCTAssertEqual(HubReachability.online.caption, "Hub online")
        XCTAssertEqual(HubReachability.offline.caption, "Hub offline")
        XCTAssertEqual(HubReachability.online.accessibilityValue, "Online")
        XCTAssertEqual(HubReachability.notConfigured.accessibilityValue, "Not configured")
    }

    // MARK: URL normalisation

    func testAddressParsing() {
        XCTAssertEqual(HubAddress.parse("  "), .empty)
        XCTAssertEqual(HubAddress.parse("http://192.168.3.87:8080/"), .valid(hubURL))
        XCTAssertEqual(HubAddress.parse("192.168.3.87:8080"), .valid(hubURL), "bare host gets http://")
        XCTAssertEqual(HubAddress.parse("hub.local"), .valid(URL(string: "http://hub.local")!))
        if case .invalid(let why) = HubAddress.parse("https://hub.local") { XCTAssertTrue(why.contains("http://")) } else { XCTFail() }
        if case .invalid = HubAddress.parse("http://hub.local/api") {} else { XCTFail() }
        if case .invalid = HubAddress.parse("http://") {} else { XCTFail() }
    }

    func testHubSettingsDisabledReturnsNil() {
        let d = UserDefaults(suiteName: "HubStatusTests-\(UUID().uuidString)")!
        XCTAssertEqual(HubSettings.current(d)?.absoluteString, HubSettings.defaultURL)
        d.set(false, forKey: HubSettings.enabledKey)
        XCTAssertNil(HubSettings.current(d))
        d.set(true, forKey: HubSettings.enabledKey)
        d.set("http://10.0.0.5:9000/", forKey: HubSettings.urlKey)
        XCTAssertEqual(HubSettings.current(d)?.absoluteString, "http://10.0.0.5:9000")
    }

    // MARK: decoding live-hub fixtures

    func testDecodeHealthz() throws {
        let h = try HubHealth.decode(try fixture("hub_healthz"))
        XCTAssertTrue(h.ok)
        XCTAssertEqual(h.version, "1.0.0")
        XCTAssertEqual(h.commit?.count, 40)
        XCTAssertThrowsError(try HubHealth.decode(Data("{}".utf8)))
    }

    func testDecodeDevicesAndMatchesMac() throws {
        let devs = try HubDevice.decodeList(try fixture("hub_devices"))
        XCTAssertEqual(devs.count, 3)
        let bb = try XCTUnwrap(devs.first { $0.matches(mac: "44:1B:F6:C4:78:84") })
        XCTAssertEqual(bb.name, "bugbuster-c47885")
        XCTAssertTrue(bb.isBugBuster)
        XCTAssertTrue(bb.isOnline)
        XCTAssertEqual(bb.firmware, "BugBuster 6.0.0")
        XCTAssertEqual(bb.rssi, -61)
        XCTAssertFalse(devs.contains { $0.matches(mac: "") })
        let esphome = try XCTUnwrap(devs.first { $0.id == "30c922502c94" })
        XCTAssertFalse(esphome.isBugBuster)
        let offline = try XCTUnwrap(devs.first { $0.id == "1cdbd4eebd4c" })
        XCTAssertFalse(offline.isOnline)
    }

    func testDecodeRunsListAndDetail() throws {
        let list = try HubRunInfo.decodeList(try fixture("hub_runs"))
        XCTAssertEqual(list.count, 1)
        XCTAssertEqual(list[0].uid, "441bf6c47884-2-1790954740")
        XCTAssertEqual(list[0].name, "Lead-acid 6S 10000mAh")
        XCTAssertEqual(list[0].chem, "Lead-acid")
        XCTAssertEqual(list[0].cells, 6)
        XCTAssertEqual(list[0].capacityMah, 10000)
        XCTAssertNil(list[0].points)
        let one = try HubRunInfo.decodeOne(try fixture("hub_run"))
        XCTAssertEqual(one.points, 2096)
        XCTAssertEqual(try XCTUnwrap(one.duration), 1791074417 - 1790954740, accuracy: 0.5)
    }

    func testDecodeLogsAndSummary() throws {
        let logs = try HubLogEntry.decodeList(try fixture("hub_logs"))
        XCTAssertEqual(logs.count, 6)
        let s = HubLogSummary(logs)
        XCTAssertEqual(s.total, 6)
        XCTAssertEqual(s.errors + s.warnings + s.other, 6)
        XCTAssertGreaterThan(s.errors + s.warnings, 0)
        XCTAssertThrowsError(try HubLogEntry.decodeList(Data("[]".utf8)))
    }

    // MARK: coverage maths

    func testCoverageStatsFromLiveFixture() throws {
        let cov = try HubCoverage.decode(try fixture("hub_coverage"))
        let s = try XCTUnwrap(HubCoverageStats.compute(cov, from: 1790954740, to: 1791074417))
        XCTAssertEqual(s.spanStart, 1790954740)
        XCTAssertGreaterThan(s.fraction, 0.9)
        XCTAssertLessThanOrEqual(s.fraction, 1)
        XCTAssertTrue(s.gaps.contains { $0.from == 1790958119 })
    }

    func testCoverageMergesOverlapAndFindsGaps() {
        let cov = HubCoverage(ranges: [.init(from: 0, to: 1000, res: 60), .init(from: 500, to: 1500, res: 1), .init(from: 3000, to: 4000, res: 60)])
        let s = HubCoverageStats.compute(cov, from: 0, to: 5000)!
        XCTAssertEqual(s.coveredSeconds, 2500)
        XCTAssertEqual(s.gaps.count, 2)
        XCTAssertEqual(s.gaps[0].from, 1500); XCTAssertEqual(s.gaps[0].to, 3000)
        XCTAssertEqual(s.gaps[1].from, 4000)
        XCTAssertEqual(s.fraction, 0.5, accuracy: 0.001)
        XCTAssertNil(HubCoverageStats.compute(HubCoverage(ranges: [])))
    }

    func testFormatting() {
        XCTAssertEqual(HubFormat.duration(45), "45 s")
        XCTAssertEqual(HubFormat.duration(5400), "1 h 30 min")
        XCTAssertEqual(HubFormat.duration(119_000), "1 d 9 h")
        XCTAssertEqual(HubFormat.latency(0.036), "36 ms")
        XCTAssertEqual(HubFormat.resolution(60), "1 min")
        XCTAssertEqual(HubFormat.points(12_345), "12.3k")
    }
}
