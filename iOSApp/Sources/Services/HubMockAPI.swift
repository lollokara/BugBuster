#if DEBUG
import Foundation

/// Simulated hub for `BB_MOCK_MODE=1` (screenshots, layout checks). `BB_MOCK_HUB` picks the state:
/// online (default), offline, none (hub switched off), slow (online, answers after 1.5 s).
final class HubMockAPI: HubAPI {
    static let mac = "44:1b:f6:c4:78:84"
    static let id = "441bf6c47884"
    var online = true
    var delay: TimeInterval = 0

    @MainActor
    static func fromEnvironment() -> HubStatusModel? {
        let env = ProcessInfo.processInfo.environment
        guard env["BB_MOCK_MODE"] == "1" else { return nil }
        let url = URL(string: "http://192.168.3.87:8080")!
        switch env["BB_MOCK_HUB"] ?? "online" {
        case "none": return HubStatusModel(api: nil, url: nil)
        case "offline":
            let m = HubMockAPI(); m.online = false
            return HubStatusModel(api: m, url: url)
        case "slow":
            let m = HubMockAPI(); m.delay = 1.5
            return HubStatusModel(api: m, url: url)
        default: return HubStatusModel(api: HubMockAPI(), url: url)
        }
    }

    private func gate() async throws {
        if delay > 0 { try? await Task.sleep(nanoseconds: UInt64(delay * 1e9)) }
        if !online { throw HubError.unreachable }
    }

    func forgetFailure() {}

    func health() async throws -> HubHealth {
        try await gate()
        return HubHealth(ok: true, version: "1.0.0", commit: "276fc052e7d59a41220bc8f343e691258b4cd12b")
    }

    func devices() async throws -> [HubDevice] {
        try await gate()
        let t = Date().timeIntervalSince1970
        func dev(_ id: String, _ name: String, _ vendor: String, _ status: String, _ fw: String?, _ ver: String?, age: Double) -> HubDevice {
            HubDevice(id: id, name: name, vendor: vendor, status: status, ip: "192.168.3.\(abs(id.hashValue) % 200 + 20)",
                      fwName: fw, fwVersion: ver, chip: vendor == "espfleet" ? "ESP32-S3" : "ESP32", rssi: -60,
                      lastSeen: t - age, ageS: age, webURL: nil)
        }
        return [
            dev(Self.id, "bugbuster-c47885", "espfleet", "online", "bugbuster", "nightly-build86-152-g70563a47", age: 12),
            dev("1cdbd4eebd4c", "ltp8802-pmbus", "espfleet", "offline", nil, "573fd48-dirty", age: 1_150_571),
            dev("30c922502c94", "ESPDesk", "esphome", "online", "ESPHome", "2026.8.2", age: 58),
            dev("8813bfd5f5a8", "ESPPrinter", "esphome", "online", "ESPHome", "2026.8.2", age: 58),
            dev("a020a60a8fca", "WLED", "wled", "online", "WLED", "16.0.0", age: 58)
        ]
    }

    private func runList() -> [HubRunInfo] {
        func r(_ mac: String, _ id: Int, _ epoch: Double, _ name: String, _ state: String, hours: Double, points: Int) -> HubRunInfo {
            HubRunInfo(uid: "\(mac)-\(id)-\(Int(epoch))", deviceID: mac, runID: id, startedAt: epoch, name: name, state: state,
                       lastSampleAt: epoch + hours * 3600, points: points, firstTS: epoch + 60, lastTS: epoch + hours * 3600,
                       chem: "Li-ion", cells: 2, capacityMah: 2500)
        }
        return [
            r(Self.id, 7, 1_760_000_000, "Bench discharge", "running", hours: 4, points: 240),
            r(Self.id, 6, 1_759_000_000, "Older run", "stopped", hours: 1.5, points: 90),
            r("aabbcc112233", 3, 1_758_000_000, "Lead-acid 6S 10000mAh", "stopped", hours: 32, points: 1_920)
        ]
    }

    func runs(device: String?, limit: Int) async throws -> [HubRunInfo] {
        try await gate()
        return runList().filter { device == nil || $0.deviceID == device }.map { var x = $0; x.points = nil; x.firstTS = nil; x.lastTS = nil; return x }
    }

    func run(uid: String) async throws -> HubRunInfo {
        try await gate()
        guard let r = runList().first(where: { $0.uid == uid }) else { throw HubError.http(404) }
        return r
    }

    func coverage(uid: String) async throws -> HubCoverage {
        try await gate()
        guard let r = runList().first(where: { $0.uid == uid }), let last = r.lastTS else { throw HubError.http(404) }
        let s = r.startedAt + 60
        let mid = s + (last - s) * 0.45
        return HubCoverage(ranges: [
            .init(from: s, to: mid, res: 60, clkSrc: "HUB", clkUncMs: 13),
            .init(from: mid + 900, to: last - 600, res: 60, clkSrc: "P4_EPOCH", clkUncMs: 365),
            .init(from: last - 600, to: last, res: 1, clkSrc: "HUB", clkUncMs: 17)
        ])
    }

    func logs(device: String?, limit: Int) async throws -> [HubLogEntry] {
        try await gate()
        let t = Date().timeIntervalSince1970
        return [
            HubLogEntry(ts: t - 20, level: "W", tag: "hub", msg: "DAQ HAT rebooted", source: "s3"),
            HubLogEntry(ts: t - 95, level: "E", tag: "hat", msg: "HAT command 0x5D: failed to take mutex", source: "s3"),
            HubLogEntry(ts: t - 310, level: "I", tag: "batt", msg: "Run 7 sample batch pushed (60 points)", source: "s3"),
            HubLogEntry(ts: t - 900, level: "I", tag: "wifi", msg: "Connected to Casa!, rssi -60", source: "s3")
        ]
    }
}
#endif
