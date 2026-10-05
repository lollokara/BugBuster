import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking  // URLSession lives here outside Apple platforms (Linux/Windows CI)
#endif

// =============================================================================
// HubClient.swift - read side of the ESPFleet hub for the Battery view (spec
// 2026-10-03 section 6). Foundation only. The phone sits on the DAQ hotspot
// while a run streams and cannot reach the hub then: every call fails fast
// (2 s) and an unreachable hub is remembered for 30 s so the UI never waits twice.
// =============================================================================

enum HubSettings {
    static let urlKey = "espfleet_hub_url"
    static let defaultURL = "http://192.168.3.87:8080"
    /// Off = the phone never talks to the hub (tile shows "No hub"). On by default.
    static let enabledKey = "espfleet_hub_enabled"

    /// http://host[:port] only (the hub is plain HTTP on the LAN). Trailing "/" is dropped.
    static func normalize(_ raw: String) -> URL? {
        var s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        while s.hasSuffix("/") { s.removeLast() }
        guard let u = URL(string: s), u.scheme == "http", let host = u.host, !host.isEmpty,
              u.path.isEmpty, u.query == nil else { return nil }
        return u
    }

    /// The saved URL, or the default when none is saved or the saved one is invalid. nil when the hub is switched off.
    static func current(_ defaults: UserDefaults = .standard) -> URL? {
        if defaults.object(forKey: enabledKey) != nil, !defaults.bool(forKey: enabledKey) { return nil }
        return normalize(defaults.string(forKey: urlKey) ?? "") ?? normalize(defaultURL)
    }
}

final class HubClient {
    var baseURL: URL?
    private let session: URLSession
    private let now: () -> Date
    private let timeout: TimeInterval
    private let negativeCache: TimeInterval
    private var unreachableUntil: Date?

    init(baseURL: URL? = HubSettings.current(), session: URLSession = .shared, now: @escaping () -> Date = Date.init,
         timeout: TimeInterval = 2, negativeCache: TimeInterval = 30) {
        self.baseURL = baseURL
        self.session = session
        self.now = now
        self.timeout = timeout
        self.negativeCache = negativeCache
    }

    /// True while a recent failure makes the next call pointless.
    var isKnownUnreachable: Bool { unreachableUntil.map { now() < $0 } ?? false }

    func series(uid: String, from: Double? = nil, to: Double? = nil, bucket: Int? = nil) async throws -> HubSeries {
        var q: [URLQueryItem] = []
        if let from { q.append(URLQueryItem(name: "from", value: String(format: "%.3f", from))) }
        if let to { q.append(URLQueryItem(name: "to", value: String(format: "%.3f", to))) }
        if let bucket { q.append(URLQueryItem(name: "bucket", value: String(bucket))) }
        return try HubSeries.decode(try await get("/api/v1/runs/\(uid)/samples", q))
    }

    func coverage(uid: String) async throws -> HubCoverage {
        try HubCoverage.decode(try await get("/api/v1/runs/\(uid)/coverage", []))
    }

    /// First and last sample time the hub holds for the run, or nil when the hub has never seen it.
    func extent(uid: String) async throws -> (first: Double, last: Double)? {
        do {
            let data = try await get("/api/v1/runs/\(uid)", [])
            guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let f = (o["first_ts"] as? NSNumber)?.doubleValue, let l = (o["last_ts"] as? NSNumber)?.doubleValue
            else { return nil }
            return (f, l)
        } catch HubError.http(404) {
            return nil
        }
    }

    /// Forgets a remembered failure so an explicit refresh really asks the hub.
    func forgetUnreachable() { unreachableUntil = nil }

    func get(_ path: String, _ query: [URLQueryItem]) async throws -> Data {
        guard let base = baseURL else { throw HubError.unreachable }
        if isKnownUnreachable { throw HubError.unreachable }
        var c = URLComponents(url: base, resolvingAgainstBaseURL: false)
        c?.path = path
        c?.queryItems = query.isEmpty ? nil : query
        guard let url = c?.url else { throw HubError.unreachable }
        var req = URLRequest(url: url, timeoutInterval: timeout)
        req.cachePolicy = .reloadIgnoringLocalCacheData
        do {
            let (data, resp) = try await session.data(for: req)
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if code >= 500 { unreachableUntil = now().addingTimeInterval(negativeCache); throw HubError.http(code) }
            guard (200..<300).contains(code) else { throw HubError.http(code) }
            unreachableUntil = nil
            return data
        } catch let e as HubError {
            throw e
        } catch {
            unreachableUntil = now().addingTimeInterval(negativeCache)
            throw HubError.unreachable
        }
    }
}
