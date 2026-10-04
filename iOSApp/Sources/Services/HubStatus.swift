import Foundation

// =============================================================================
// HubStatus.swift - what the app knows about the ESPFleet hub: decoded API models, the reachability
// state behind the Battery "Hub" tile, and the observable model the Hub screen reads. Foundation only.
// Endpoints (all GET, verified against the live hub): /api/v1/healthz, /devices, /runs?device=,
// /runs/{uid}, /runs/{uid}/coverage, /logs?device=&limit=.
// =============================================================================

// MARK: - Address parsing

enum HubAddress: Equatable {
    case empty
    case valid(URL)
    case invalid(String)

    /// Editor input -> address. A bare "host:port" gets "http://"; the hub is plain HTTP on the LAN.
    static func parse(_ raw: String) -> HubAddress {
        var s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.isEmpty { return .empty }
        if s.lowercased().hasPrefix("https://") { return .invalid("The hub speaks plain http://, not https://.") }
        if !s.contains("://") { s = "http://" + s }
        if let u = HubSettings.normalize(s) { return .valid(u) }
        if let u = URL(string: s), u.path.count > 1 || u.query != nil {
            return .invalid("Use only the address, like http://192.168.3.87:8080 (no path).")
        }
        return .invalid("Use the form http://192.168.3.87:8080.")
    }
}

// MARK: - API models

struct HubHealth: Equatable {
    var ok: Bool
    var version: String?
    var commit: String?

    static func decode(_ data: Data) throws -> HubHealth {
        guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any], let ok = o["ok"] as? Bool
        else { throw HubError.decode("healthz") }
        return HubHealth(ok: ok, version: o["version"] as? String, commit: o["deployed_commit"] as? String)
    }
}

struct HubDevice: Identifiable, Equatable {
    var id: String                  // 12 lowercase hex for ESPFleet boards (the MAC), other ids for scanned hosts
    var name: String                // display_name
    var vendor: String
    var status: String              // online / offline / stale / unknown
    var ip: String? = nil
    var fwName: String? = nil
    var fwVersion: String? = nil
    var chip: String? = nil
    var rssi: Int? = nil
    var lastSeen: Double? = nil
    var ageS: Double? = nil
    var webURL: String? = nil

    var isOnline: Bool { status == "online" }
    var isBugBuster: Bool { vendor == "espfleet" }
    var firmware: String? {
        let parts = [fwName, fwVersion].compactMap { $0 }.filter { !$0.isEmpty }
        return parts.isEmpty ? nil : parts.joined(separator: " ")
    }

    /// True when this record is the board the app is connected to (hub id = MAC without separators).
    func matches(mac: String) -> Bool {
        let hex = HubDevice.normalizedMac(mac)
        return hex.count == 12 && id == hex
    }

    static func normalizedMac(_ mac: String) -> String { mac.lowercased().filter { $0.isHexDigit } }

    static func decodeList(_ data: Data) throws -> [HubDevice] {
        guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let arr = o["devices"] as? [[String: Any]] else { throw HubError.decode("devices") }
        return arr.compactMap { d in
            guard let id = d["id"] as? String else { return nil }
            func num(_ k: String) -> Double? { (d[k] as? NSNumber)?.doubleValue }
            let name = (d["display_name"] as? String) ?? (d["name"] as? String) ?? id
            return HubDevice(id: id, name: name, vendor: (d["vendor"] as? String) ?? "unknown",
                             status: (d["status"] as? String) ?? "unknown", ip: d["ip"] as? String,
                             fwName: d["fw_name"] as? String, fwVersion: d["fw_version"] as? String,
                             chip: d["chip"] as? String, rssi: num("rssi").map { Int($0) },
                             lastSeen: num("last_seen"), ageS: num("age_s"), webURL: d["web_url"] as? String)
        }
    }
}

struct HubRunInfo: Identifiable, Hashable {
    var id: String { uid }
    var uid: String
    var deviceID: String
    var runID: Int
    var startedAt: Double
    var name: String
    var state: String               // running / paused / stopped ...
    var lastSampleAt: Double? = nil
    var points: Int? = nil          // only the single-run endpoint carries these
    var firstTS: Double? = nil
    var lastTS: Double? = nil
    var chem: String? = nil
    var cells: Int? = nil
    var capacityMah: Int? = nil

    /// Seconds of recorded activity, or nil while the hub holds no samples yet.
    var duration: Double? {
        guard let end = lastTS ?? lastSampleAt, end > startedAt else { return nil }
        return end - startedAt
    }

    static func decode(_ o: [String: Any]) -> HubRunInfo? {
        guard let uid = o["run_uid"] as? String, let dev = o["device_id"] as? String,
              let started = (o["started_at"] as? NSNumber)?.doubleValue else { return nil }
        func num(_ k: String) -> Double? { (o[k] as? NSNumber)?.doubleValue }
        let p = o["params"] as? [String: Any]
        let rid = (o["run_id"] as? NSNumber)?.intValue ?? HubRunUid.parse(uid)?.runId ?? 0
        return HubRunInfo(uid: uid, deviceID: dev, runID: rid, startedAt: started,
                          name: (o["name"] as? String) ?? "", state: (o["state"] as? String) ?? "",
                          lastSampleAt: num("last_sample_at"), points: num("points").map { Int($0) },
                          firstTS: num("first_ts"), lastTS: num("last_ts"), chem: p?["chem"] as? String,
                          cells: (p?["cells"] as? NSNumber)?.intValue, capacityMah: (p?["capacity_mah"] as? NSNumber)?.intValue)
    }

    static func decodeList(_ data: Data) throws -> [HubRunInfo] {
        guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let arr = o["runs"] as? [[String: Any]] else { throw HubError.decode("runs") }
        return arr.compactMap(decode)
    }

    static func decodeOne(_ data: Data) throws -> HubRunInfo {
        guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any], let r = decode(o)
        else { throw HubError.decode("run") }
        return r
    }
}

struct HubLogEntry: Identifiable, Equatable {
    var id: String { "\(ts)-\(tag)-\(msg.hashValue)" }
    var ts: Double
    var level: String               // E / W / I / D
    var tag: String
    var msg: String
    var source: String

    static func decodeList(_ data: Data) throws -> [HubLogEntry] {
        guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let arr = o["logs"] as? [[String: Any]] else { throw HubError.decode("logs") }
        return arr.compactMap { l in
            guard let ts = (l["ts"] as? NSNumber)?.doubleValue else { return nil }
            return HubLogEntry(ts: ts, level: (l["level"] as? String) ?? "I", tag: (l["tag"] as? String) ?? "",
                               msg: (l["msg"] as? String) ?? "", source: (l["source"] as? String) ?? "")
        }
    }
}

/// Counts per level over a log page, for the "recent logs" summary.
struct HubLogSummary: Equatable {
    var errors = 0, warnings = 0, other = 0
    init(_ logs: [HubLogEntry]) {
        for l in logs {
            switch l.level.uppercased() { case "E": errors += 1; case "W": warnings += 1; default: other += 1 }
        }
    }
    var total: Int { errors + warnings + other }
}

// MARK: - Coverage maths

/// How much of a run's span the hub actually holds, merged over overlapping ranges.
struct HubCoverageStats {
    var spanStart: Double
    var spanEnd: Double
    var coveredSeconds: Double
    var gaps: [HubMerge.Span]

    var span: Double { max(0, spanEnd - spanStart) }
    var fraction: Double { span > 0 ? min(1, coveredSeconds / span) : 0 }
    /// Gaps shorter than this are normal jitter between buckets, not missing data.
    static let minGap = 120.0

    static func compute(_ cov: HubCoverage, from start: Double? = nil, to end: Double? = nil) -> HubCoverageStats? {
        let rs = cov.ranges.filter { $0.to >= $0.from }.sorted { $0.from < $1.from }
        guard let f = rs.first else { return nil }
        let s = start ?? f.from, e = end ?? rs.map(\.to).max()!
        var merged: [HubMerge.Span] = []
        for r in rs {
            let a = max(r.from, s), b = min(r.to, e)
            guard b >= a else { continue }
            if let last = merged.last, a <= last.to + 1 { merged[merged.count - 1].to = max(last.to, b) } else { merged.append((a, b)) }
        }
        var gaps: [HubMerge.Span] = []
        var cursor = s
        for m in merged {
            if m.from - cursor >= minGap { gaps.append((cursor, m.from)) }
            cursor = m.to
        }
        if e - cursor >= minGap { gaps.append((cursor, e)) }
        return HubCoverageStats(spanStart: s, spanEnd: e, coveredSeconds: merged.reduce(0) { $0 + ($1.to - $1.from) }, gaps: gaps)
    }
}

// MARK: - API seam

protocol HubAPI: AnyObject {
    func health() async throws -> HubHealth
    func devices() async throws -> [HubDevice]
    func runs(device: String?, limit: Int) async throws -> [HubRunInfo]
    func run(uid: String) async throws -> HubRunInfo
    func coverage(uid: String) async throws -> HubCoverage
    func logs(device: String?, limit: Int) async throws -> [HubLogEntry]
    func forgetFailure()
}

extension HubClient: HubAPI {
    func health() async throws -> HubHealth { try HubHealth.decode(try await get("/api/v1/healthz", [])) }
    func devices() async throws -> [HubDevice] { try HubDevice.decodeList(try await get("/api/v1/devices", [])) }
    func runs(device: String?, limit: Int) async throws -> [HubRunInfo] {
        var q = [URLQueryItem(name: "limit", value: String(limit))]
        if let device { q.append(URLQueryItem(name: "device", value: device)) }
        return try HubRunInfo.decodeList(try await get("/api/v1/runs", q))
    }
    func run(uid: String) async throws -> HubRunInfo { try HubRunInfo.decodeOne(try await get("/api/v1/runs/\(uid)", [])) }
    func logs(device: String?, limit: Int) async throws -> [HubLogEntry] {
        var q = [URLQueryItem(name: "limit", value: String(limit))]
        if let device { q.append(URLQueryItem(name: "device", value: device)) }
        return try HubLogEntry.decodeList(try await get("/api/v1/logs", q))
    }
    func forgetFailure() { forgetUnreachable() }
}

// MARK: - Reachability

enum HubReachability: Equatable {
    case notConfigured
    case checking
    case online
    case offline

    var caption: String {
        switch self {
        case .notConfigured: return "No hub"
        case .checking: return "Hub…"
        case .online: return "Hub online"
        case .offline: return "Hub offline"
        }
    }

    var accessibilityValue: String {
        switch self {
        case .notConfigured: return "Not configured"
        case .checking: return "Checking"
        case .online: return "Online"
        case .offline: return "Offline, unreachable"
        }
    }
}

/// Plain-language reason a hub call failed, with the fix.
enum HubFailure: Equatable {
    case unreachable, http(Int), unexpected

    init(_ error: Error) {
        switch error as? HubError {
        case .http(let c)?: self = .http(c)
        case .unreachable?: self = .unreachable
        default: self = .unexpected
        }
    }

    var message: String {
        switch self {
        case .unreachable: return "The hub did not answer."
        case .http(let c): return "The hub answered with an error (HTTP \(c))."
        case .unexpected: return "The hub sent something this app does not understand."
        }
    }

    var fix: String {
        switch self {
        case .unreachable:
            return "Join the same Wi-Fi as the hub (not the DAQ hotspot), and check the address has the form http://192.168.3.87:8080."
        case .http: return "The hub is running but failing. Try again in a moment or check its logs."
        case .unexpected: return "Check the address points at an ESPFleet hub, not another web server."
        }
    }
}

// MARK: - Observable model

@MainActor
final class HubStatusModel: ObservableObject {
    @Published private(set) var reachability: HubReachability
    @Published private(set) var health: HubHealth?
    @Published private(set) var latency: TimeInterval?
    @Published private(set) var lastCheck: Date?
    @Published private(set) var lastSync: Date?
    @Published private(set) var failure: HubFailure?

    @Published private(set) var devices: [HubDevice] = []
    @Published private(set) var runs: [HubRunInfo] = []
    @Published private(set) var logs: [HubLogEntry] = []
    @Published private(set) var loading = false
    @Published private(set) var loadedOnce = false
    @Published private(set) var dataFailure: HubFailure?

    private(set) var api: HubAPI?
    private(set) var url: URL?
    private let now: () -> Date
    private let sleep: (TimeInterval) async -> Void
    let pollInterval: TimeInterval

    init(api: HubAPI?, url: URL?, pollInterval: TimeInterval = 30, now: @escaping () -> Date = Date.init,
         sleep: @escaping (TimeInterval) async -> Void = { try? await Task.sleep(nanoseconds: UInt64($0 * 1e9)) }) {
        self.api = api
        self.url = url
        self.now = now
        self.sleep = sleep
        self.pollInterval = pollInterval
        reachability = (api == nil || url == nil) ? .notConfigured : .checking
    }

    /// Live model reading the saved settings.
    static func live(defaults: UserDefaults = .standard) -> HubStatusModel {
        #if DEBUG
        if let mock = HubMockAPI.fromEnvironment() { return mock }
        #endif
        let url = HubSettings.current(defaults)
        return HubStatusModel(api: url.map { HubClient(baseURL: $0) }, url: url)
    }

    /// Settings changed: swap the address (nil = hub off) and drop everything learnt from the old one.
    func configure(api: HubAPI?, url: URL?) {
        self.api = api
        self.url = url
        health = nil; latency = nil; failure = nil; lastCheck = nil
        devices = []; runs = []; logs = []; loadedOnce = false; dataFailure = nil
        reachability = (api == nil || url == nil) ? .notConfigured : .checking
    }

    func reloadFromSettings(defaults: UserDefaults = .standard) {
        #if DEBUG
        if HubMockAPI.fromEnvironment() != nil { return }      // the mock hub ignores the saved address
        #endif
        let u = HubSettings.current(defaults)
        if u == url, api != nil || u == nil { return }
        configure(api: u.map { HubClient(baseURL: $0) }, url: u)
    }

    /// One light health probe: updates the state, version and latency.
    func check() async {
        guard let api, url != nil else { reachability = .notConfigured; return }
        let t0 = now()
        do {
            let h = try await api.health()
            latency = max(0, now().timeIntervalSince(t0))
            health = h
            failure = nil
            reachability = h.ok ? .online : .offline
            if !h.ok { failure = .unexpected }
        } catch {
            latency = nil
            failure = HubFailure(error)
            reachability = .offline
        }
        lastCheck = now()
    }

    /// Polls while the caller's task lives (a view's `.task`, cancelled when it leaves or the app backgrounds).
    func poll() async {
        while !Task.isCancelled {
            await check()
            await sleep(pollInterval)
        }
    }

    /// Explicit refresh (pull-to-refresh / open): forget remembered failures, probe, then load the lists.
    func refresh(deviceID: String?) async {
        api?.forgetFailure()
        await check()
        guard let api, reachability == .online else { return }
        loading = true
        defer { loading = false; loadedOnce = true }
        do {
            async let d = api.devices()
            async let r = api.runs(device: nil, limit: 50)
            async let l = api.logs(device: deviceID, limit: 200)
            let (dd, rr, ll) = try await (d, r, l)
            devices = dd
            runs = rr.sorted { $0.startedAt > $1.startedAt }
            logs = ll
            dataFailure = nil
            lastSync = now()
        } catch {
            dataFailure = HubFailure(error)
            if case .unreachable = HubFailure(error) { reachability = .offline; failure = .unreachable }
        }
        await fillRunDetails()
    }

    /// The list endpoint omits sample counts; the per-run endpoint has them. Bounded to the newest runs.
    private func fillRunDetails(limit: Int = 12) async {
        guard let api else { return }
        for r in runs.prefix(limit) where r.points == nil {
            guard !Task.isCancelled, let full = try? await api.run(uid: r.uid),
                  let i = runs.firstIndex(where: { $0.uid == r.uid }) else { continue }
            runs[i] = full
        }
    }

    func device(forMac mac: String) -> HubDevice? { devices.first { $0.matches(mac: mac) } }
    func runs(forMac mac: String) -> [HubRunInfo] {
        let hex = HubDevice.normalizedMac(mac)
        return runs.filter { $0.deviceID == hex }
    }
    func otherRuns(forMac mac: String) -> [HubRunInfo] {
        let hex = HubDevice.normalizedMac(mac)
        return runs.filter { $0.deviceID != hex }
    }
}

// MARK: - Formatting

enum HubFormat {
    static func duration(_ s: Double) -> String {
        let t = Int(s.rounded())
        if t < 60 { return "\(t) s" }
        if t < 3600 { return "\(t / 60) min" }
        if t < 86_400 { return String(format: "%d h %02d min", t / 3600, (t % 3600) / 60) }
        return String(format: "%d d %d h", t / 86_400, (t % 86_400) / 3600)
    }

    static func latency(_ s: TimeInterval) -> String { s < 1 ? "\(Int((s * 1000).rounded())) ms" : String(format: "%.1f s", s) }

    static func points(_ n: Int) -> String {
        n >= 10_000 ? String(format: "%.1fk", Double(n) / 1000) : "\(n)"
    }

    static func resolution(_ res: Int) -> String {
        res <= 1 ? "1 s" : res < 3600 && res % 60 == 0 ? "\(res / 60) min" : "\(res) s"
    }
}

// MARK: - Identity of the connected board

enum HubIdentity {
    /// MAC of the connected BugBuster as last seen over BLE/HTTP ("" when never connected).
    static func mac(defaults: UserDefaults = .standard) -> String {
        let m = defaults.string(forKey: "bugbuster_last_mac") ?? ""
        #if DEBUG
        if m.isEmpty, ProcessInfo.processInfo.environment["BB_MOCK_MODE"] == "1" { return HubMockAPI.mac }
        #endif
        return m
    }
}
