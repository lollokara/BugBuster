import Foundation
import Network
import Combine
import Darwin

public enum ConnectionState: String, Codable {
    case disconnected
    case connecting
    case connected
    case unauthorized
    case error
}

public struct DiscoveredDevice: Identifiable, Hashable, Codable {
    public var id: String { mac.isEmpty ? hostname : mac }
    public let hostname: String
    public let ip: String
    public let port: Int
    public let firmware: String
    public let mac: String
    public let model: String
    
    public init(hostname: String, ip: String, port: Int, firmware: String = "", mac: String = "", model: String = "") {
        self.hostname = hostname
        self.ip = ip
        self.port = port
        self.firmware = firmware
        self.mac = mac
        self.model = model
    }
}

public class ConnectionManager: NSObject, ObservableObject, NetServiceBrowserDelegate, NetServiceDelegate {
    @Published public var connectionState: ConnectionState = .disconnected
    @Published public var discoveredDevices: [DiscoveredDevice] = []
    @Published public var activeDevice: DiscoveredDevice? = nil
    @Published public var adminToken: String = ""
    @Published public var lshiftOe: Bool = false
    
    // Live telemetries
    @Published public var lastStatus: DeviceStatus? = nil
    @Published public var lastOverview: OverviewSnapshot? = nil
    @Published public var lastIoExp: IOExpState? = nil
    @Published public var lastSelftest: SelftestStatus? = nil
    @Published public var lastHatStatus: HatStatus? = nil
    @Published public var lastHatRails: [HatRail] = []
    
    @Published public var isSearching = false
    
    private var serviceBrowser: NetServiceBrowser?
    private var services: Set<NetService> = []
    private var pollTimer: AnyCancellable?       // kept for compile compat, unused
    private let session: URLSession
    private var pollTask: Task<Void, Never>? = nil
    private var pollCycle: Int = 0
    
    public override init() {
        let configuration = URLSessionConfiguration.default
        configuration.timeoutIntervalForRequest = 5.0
        configuration.timeoutIntervalForResource = 10.0
        // Keep-alive: reuse one TCP connection to the ESP32 so lwIP sockets
        // don't pile up in TIME_WAIT and exhaust the device's socket pool.
        // URLSession handles stale keep-alive automatically by retrying on
        // connection-lost errors for idempotent GET requests.
        configuration.httpMaximumConnectionsPerHost = 5
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        self.session = URLSession(configuration: configuration)
        
        super.init()
        
        // Load saved connection info if any
        if let savedIp = UserDefaults.standard.string(forKey: "bugbuster_ip"),
           let savedToken = UserDefaults.standard.string(forKey: "bugbuster_token") {
            self.adminToken = savedToken
            self.activeDevice = DiscoveredDevice(hostname: "Saved Device", ip: savedIp, port: 80)
            // Auto connect in background
            Task {
                let success = await self.tryConnect(ip: savedIp, token: savedToken)
                if success {
                    self.startPolling()
                }
            }
        }
        
        startDiscovery()
    }
    
    public func startDiscovery() {
        DispatchQueue.main.async {
            guard self.serviceBrowser == nil else { return }
            self.isSearching = true
            self.discoveredDevices.removeAll()
            self.services.removeAll()
            
            let browser = NetServiceBrowser()
            browser.delegate = self
            self.serviceBrowser = browser
            browser.searchForServices(ofType: "_bugbuster._tcp", inDomain: "local.")
        }
    }
    
    public func stopDiscovery() {
        DispatchQueue.main.async {
            self.serviceBrowser?.stop()
            self.serviceBrowser = nil
            for s in self.services {
                s.stop()
            }
            self.services.removeAll()
            self.isSearching = false
        }
    }
    
    // MARK: - NetServiceBrowserDelegate
    
    public func netServiceBrowser(_ browser: NetServiceBrowser, didFind service: NetService, moreComing: Bool) {
        DispatchQueue.main.async {
            self.services.insert(service)
            service.delegate = self
            service.resolve(withTimeout: 5.0)
        }
    }
    
    public func netServiceBrowser(_ browser: NetServiceBrowser, didRemove service: NetService, moreComing: Bool) {
        DispatchQueue.main.async {
            self.services.remove(service)
            self.updateDiscoveredDevices()
        }
    }
    
    public func netServiceBrowserDidStopSearch(_ browser: NetServiceBrowser) {
        DispatchQueue.main.async {
            self.isSearching = false
        }
    }
    
    public func netServiceBrowser(_ browser: NetServiceBrowser, didNotSearch errorDict: [String : NSNumber]) {
        print("Bonjour search failed: \(errorDict)")
        DispatchQueue.main.async {
            self.isSearching = false
        }
    }
    
    // MARK: - NetServiceDelegate
    
    public func netServiceDidResolveAddress(_ sender: NetService) {
        DispatchQueue.main.async {
            self.updateDiscoveredDevices()
        }
    }
    
    public func netService(_ sender: NetService, didNotResolve errorDict: [String : NSNumber]) {
        print("Bonjour service failed to resolve: \(errorDict)")
    }
    
    private func updateDiscoveredDevices() {
        var devices: [DiscoveredDevice] = []
        
        for service in services {
            guard let addresses = service.addresses, !addresses.isEmpty else { continue }
            
            var ipAddress = ""
            for address in addresses {
                address.withUnsafeBytes { (ptr: UnsafeRawBufferPointer) in
                    let sockaddr = ptr.bindMemory(to: sockaddr.self)
                    if sockaddr[0].sa_family == AF_INET {
                        let sockaddrIn = ptr.bindMemory(to: sockaddr_in.self)
                        var ip = sockaddrIn[0].sin_addr
                        var ipBuffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                        if inet_ntop(AF_INET, &ip, &ipBuffer, socklen_t(INET_ADDRSTRLEN)) != nil {
                            ipAddress = String(cString: ipBuffer)
                        }
                    }
                }
                if !ipAddress.isEmpty { break }
            }
            
            if ipAddress.isEmpty {
                if let host = service.hostName {
                    ipAddress = host
                } else {
                    continue
                }
            }
            
            // Clean up hostName suffix (often has a dot at the end)
            if ipAddress.hasSuffix(".") {
                ipAddress.removeLast()
            }
            
            // Read TXT record dictionary
            var version = ""
            var mac = ""
            var model = "bugbuster-s3"
            if let txtData = service.txtRecordData() {
                let dict = NetService.dictionary(fromTXTRecord: txtData)
                if let vData = dict["version"], let v = String(data: vData, encoding: .utf8) {
                    version = v
                }
                if let mData = dict["mac"], let m = String(data: mData, encoding: .utf8) {
                    mac = m
                }
                if let moData = dict["model"], let mo = String(data: moData, encoding: .utf8) {
                    model = mo
                }
            }
            
            let device = DiscoveredDevice(
                hostname: service.name,
                ip: ipAddress,
                port: service.port,
                firmware: version,
                mac: mac,
                model: model
            )
            
            if !devices.contains(where: { $0.ip == device.ip }) {
                devices.append(device)
            }
        }
        
        DispatchQueue.main.async {
            self.discoveredDevices = devices
        }
    }
    
    public func connect(ip: String, token: String) async -> Bool {
        stopDiscovery()
        DispatchQueue.main.async {
            self.connectionState = .connecting
        }
        
        let success = await tryConnect(ip: ip, token: token)
        
        if success {
            UserDefaults.standard.set(ip, forKey: "bugbuster_ip")
            UserDefaults.standard.set(token, forKey: "bugbuster_token")
            startPolling()
        } else {
            DispatchQueue.main.async {
                if self.connectionState != .unauthorized {
                    self.connectionState = .error
                }
            }
        }
        
        return success
    }
    
    @discardableResult
    private func tryConnect(ip: String, token: String) async -> Bool {
        let cleanIp = ip.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanToken = token.trimmingCharacters(in: .whitespacesAndNewlines)
        
        var urlStr = cleanIp
        if !urlStr.lowercased().hasPrefix("http://") && !urlStr.lowercased().hasPrefix("https://") {
            urlStr = "http://\(urlStr)"
        }
        
        guard let url = URL(string: "\(urlStr)/api/status") else { return false }
        
        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        if !cleanToken.isEmpty {
            request.setValue(cleanToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
        }
        
        do {
            let (data, response) = try await session.data(for: request)
            guard let httpResponse = response as? HTTPURLResponse else { return false }
            
            if httpResponse.statusCode == 401 {
                DispatchQueue.main.async {
                    self.connectionState = .unauthorized
                    self.adminToken = cleanToken
                    self.activeDevice = DiscoveredDevice(hostname: "BugBuster Board", ip: cleanIp, port: 80)
                }
                return false
            }
            
            guard (200...299).contains(httpResponse.statusCode) else { return false }
            
            let decoder = JSONDecoder()
            let status = try decoder.decode(DeviceStatus.self, from: data)
            
            // Require a token to be entered to connect successfully
            if cleanToken.isEmpty {
                DispatchQueue.main.async {
                    self.connectionState = .unauthorized
                    self.adminToken = ""
                    self.activeDevice = DiscoveredDevice(hostname: "BugBuster Board", ip: cleanIp, port: 80)
                }
                return false
            }
            
            // Perform POST to /api/pairing/verify to validate the admin token
            guard let verifyUrl = URL(string: "\(urlStr)/api/pairing/verify") else { return false }
            var verifyRequest = URLRequest(url: verifyUrl)
            verifyRequest.httpMethod = "POST"
            verifyRequest.setValue(cleanToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
            
            let (_, verifyResponse) = try await session.data(for: verifyRequest)
            guard let verifyHttp = verifyResponse as? HTTPURLResponse else { return false }
            
            if verifyHttp.statusCode == 401 {
                DispatchQueue.main.async {
                    self.connectionState = .unauthorized
                    self.adminToken = cleanToken
                    self.activeDevice = DiscoveredDevice(hostname: "BugBuster Board", ip: cleanIp, port: 80)
                }
                return false
            }
            
            guard (200...299).contains(verifyHttp.statusCode) else { return false }
            
            DispatchQueue.main.async {
                self.lastStatus = status
                self.adminToken = cleanToken
                self.activeDevice = DiscoveredDevice(hostname: "BugBuster Board", ip: cleanIp, port: 80)
                self.connectionState = .connected
            }
            // Kick off a one-shot slow fetch after connect — overview/ioexp/selftest/hat.
            // This runs after the connection is established; failures are silently ignored.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                self.fetchOverviewOnce()
            }
            return true
        } catch {
            print("Connection test failed: \(error)")
            return false
        }
    }
    
    public func disconnect() {
        pollTask?.cancel()
        pollTask = nil
        stopPolling()
        DispatchQueue.main.async {
            self.activeDevice = nil
            self.connectionState = .disconnected
            self.lastStatus = nil
            self.lastOverview = nil
            self.lastIoExp = nil
            self.lastSelftest = nil
            self.lastHatStatus = nil
            self.lastHatRails = []
            self.lshiftOe = false
        }
        UserDefaults.standard.removeObject(forKey: "bugbuster_ip")
        UserDefaults.standard.removeObject(forKey: "bugbuster_token")
        startDiscovery()
    }
    
    public func startPolling() {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            var cycle = 0
            while !Task.isCancelled {
                guard let self = self,
                      self.connectionState == .connected,
                      let device = self.activeDevice else {
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                    continue
                }

                // Skip high-frequency diagnostic polling when active scope streaming is running
                // to prevent socket starvation and network load on the ESP32.
                if ScopeStreamManager.shared.isStreaming {
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                    continue
                }

                let ip = device.ip
                let token = self.adminToken
                cycle += 1

                if let status: DeviceStatus = try? await self.performRequest(ip: ip, path: "/api/status", token: token) {
                    DispatchQueue.main.async { self.lastStatus = status }
                }

                if cycle % 5 == 0 {
                    // Slow-cycle: reuse self.session (no new session per cycle) with
                    // per-request timeout override. Sequential so only one TCP
                    // connection is open at a time. /api/overview embeds ioexp.
                    if let ov: OverviewSnapshot = await self.fetchDecoded(OverviewSnapshot.self, ip: ip, path: "/api/overview", token: token) {
                        DispatchQueue.main.async {
                            self.updateLastOverview(ov)
                        }
                    }
                    if let st: SelftestStatus = await self.fetchDecoded(SelftestStatus.self, ip: ip, path: "/api/selftest", token: token) {
                        DispatchQueue.main.async { self.lastSelftest = st }
                    }
                    if let ht: HatStatus = await self.fetchDecoded(HatStatus.self, ip: ip, path: "/api/hat", token: token) {
                        DispatchQueue.main.async { self.lastHatStatus = ht }
                        if ht.isPresent {
                            if let rr: HatRailsResponse = await self.fetchDecoded(HatRailsResponse.self, ip: ip, path: "/api/hat/v2/rails", token: token) {
                                DispatchQueue.main.async { self.lastHatRails = rr.rails }
                            }
                        }
                    }
                }

                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }

    public func stopPolling() {
        pollTask?.cancel()
        pollTask = nil
    }

    /// Fetch slow endpoints once (on connect or pull-to-refresh). Sequential,
    /// reuses self.session — no throwaway URLSession objects.
    public func fetchOverviewOnce() {
        guard connectionState == .connected, let device = activeDevice else { return }
        let ip = device.ip
        let token = adminToken
        Task {
            if let ov: OverviewSnapshot = await fetchDecoded(OverviewSnapshot.self, ip: ip, path: "/api/overview", token: token) {
                DispatchQueue.main.async { self.updateLastOverview(ov) }
            }
            if let st: SelftestStatus = await fetchDecoded(SelftestStatus.self, ip: ip, path: "/api/selftest", token: token) {
                DispatchQueue.main.async { self.lastSelftest = st }
            }
            if let ht: HatStatus = await fetchDecoded(HatStatus.self, ip: ip, path: "/api/hat", token: token) {
                DispatchQueue.main.async { self.lastHatStatus = ht }
                if ht.isPresent {
                    if let rr: HatRailsResponse = await fetchDecoded(HatRailsResponse.self, ip: ip, path: "/api/hat/v2/rails", token: token) {
                        DispatchQueue.main.async { self.lastHatRails = rr.rails }
                    }
                }
            }
        }
    }
    
    private func fetchDecoded<T: Decodable>(_ type: T.Type, ip: String, path: String, token: String, timeout: TimeInterval = 10.0) async -> T? {
        var urlStr = ip
        if !urlStr.lowercased().hasPrefix("http://") { urlStr = "http://\(urlStr)" }
        guard let url = URL(string: "\(urlStr)\(path)") else { return nil }
        var req = URLRequest(url: url)
        req.timeoutInterval = timeout
        if !token.isEmpty { req.setValue(token, forHTTPHeaderField: "X-BugBuster-Admin-Token") }
        guard let (data, resp) = try? await session.data(for: req),
              (resp as? HTTPURLResponse)?.statusCode ?? 0 < 300 else { return nil }
        return try? JSONDecoder().decode(type, from: data)
    }

    private func performRequest<T: Decodable>(ip: String, path: String, token: String) async throws -> T {
        var urlStr = ip
        if !urlStr.lowercased().hasPrefix("http://") && !urlStr.lowercased().hasPrefix("https://") {
            urlStr = "http://\(urlStr)"
        }
        guard let url = URL(string: "\(urlStr)\(path)") else {
            throw URLError(.badURL)
        }
        
        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        if !token.isEmpty {
            request.setValue(token, forHTTPHeaderField: "X-BugBuster-Admin-Token")
        }
        
        let (data, response) = try await session.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse, (200...299).contains(httpResponse.statusCode) else {
            throw URLError(.badServerResponse)
        }
        
        let decoder = JSONDecoder()
        return try decoder.decode(T.self, from: data)
    }
    
    public func postAction(path: String, json: [String: Any]) async throws -> Bool {
        guard let device = activeDevice else { return false }
        var urlStr = device.ip
        if !urlStr.lowercased().hasPrefix("http://") && !urlStr.lowercased().hasPrefix("https://") {
            urlStr = "http://\(urlStr)"
        }
        guard let url = URL(string: "\(urlStr)\(path)") else { return false }
        
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        if !adminToken.isEmpty {
            request.setValue(adminToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
        }
        
        request.httpBody = try? JSONSerialization.data(withJSONObject: json)
        
        let (_, response) = try await session.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse else { return false }
        return (200...299).contains(httpResponse.statusCode)
    }
    
    public func getRequest<T: Decodable>(path: String) async throws -> T {
        guard let device = activeDevice else { throw URLError(.notConnectedToInternet) }
        return try await performRequest(ip: device.ip, path: path, token: adminToken)
    }
    
    public func setLShiftOe(on: Bool) async -> Bool {
        do {
            let ok = try await postAction(path: "/api/lshift/oe", json: ["on": on])
            if ok {
                DispatchQueue.main.async {
                    self.lshiftOe = on
                }
            }
            return ok
        } catch {
            return false
        }
    }

    // MARK: - Optimistic local state helpers

    /// Instantly mutate lastOverview enables so the UI reflects the toggle without
    /// waiting for the round-trip HTTP response.
    public func applyOptimisticEnable(key: String, value: Bool) {
        guard let overview = lastOverview else { return }
        let en = overview.ioexp.enables
        let newEnables = IOExpEnables(
            vadj1:    key == "vadj1"    ? value : en.vadj1,
            vadj2:    key == "vadj2"    ? value : en.vadj2,
            analog15v: key == "analog15v" ? value : en.analog15v,
            mux:      key == "mux"      ? value : en.mux,
            usbHub:   key == "usbHub"   ? value : en.usbHub
        )
        let newIoExp = IOExpState(
            present:   overview.ioexp.present,
            enables:   newEnables,
            powerGood: overview.ioexp.powerGood,
            efuses:    overview.ioexp.efuses
        )
        DispatchQueue.main.async {
            self.lastOverview = OverviewSnapshot(
                idac:  overview.idac,
                ioexp: newIoExp,
                rails: overview.rails
            )
        }
    }

    /// Instantly mutate lastOverview efuse list so the UI reflects toggle
    /// without waiting for the round-trip HTTP response.
    public func applyOptimisticEfuse(id: Int, enabled: Bool) {
        guard let overview = lastOverview else { return }
        var existing = overview.ioexp.efuses ?? []
        if existing.isEmpty {
            existing = (1...4).map { IOExpEFuse(id: $0, enabled: false, fault: false) }
        }
        let updated  = existing.map { ef in
            ef.id == id ? IOExpEFuse(id: ef.id, enabled: enabled, fault: ef.fault) : ef
        }
        let newIoExp = IOExpState(
            present:   overview.ioexp.present,
            enables:   overview.ioexp.enables,
            powerGood: overview.ioexp.powerGood,
            efuses:    updated
        )
        DispatchQueue.main.async {
            self.lastOverview = OverviewSnapshot(
                idac:  overview.idac,
                ioexp: newIoExp,
                rails: overview.rails
            )
        }
    }

    public func applyOptimisticHatRailEnable(railId: Int, enabled: Bool) {
        DispatchQueue.main.async {
            self.lastHatRails = self.lastHatRails.map { r in
                r.railId == railId ? HatRail(railId: r.railId, enabled: enabled, voltageMv: r.voltageMv, currentMa: r.currentMa, status: r.status) : r
            }
        }
    }

    public func fetchHatRailsQuick() {
        guard connectionState == .connected, let device = activeDevice else { return }
        let ip = device.ip; let token = adminToken
        Task {
            if let rr: HatRailsResponse = await fetchDecoded(HatRailsResponse.self, ip: ip, path: "/api/hat/v2/rails", token: token) {
                DispatchQueue.main.async { self.lastHatRails = rr.rails }
            }
        }
    }

    /// Lightweight post-action refresh: single /api/overview via self.session.
    public func fetchOverviewQuick() {
        guard connectionState == .connected, let device = activeDevice else { return }
        let ip = device.ip
        let token = adminToken
        Task {
            if let ov: OverviewSnapshot = await fetchDecoded(OverviewSnapshot.self, ip: ip, path: "/api/overview", token: token) {
                DispatchQueue.main.async { self.updateLastOverview(ov) }
            }
        }
    }

    private func updateLastOverview(_ ov: OverviewSnapshot) {
        let finalEfuses = ov.ioexp.efuses ?? self.lastOverview?.ioexp.efuses ?? (1...4).map { IOExpEFuse(id: $0, enabled: false, fault: false) }
        let newIoExp = IOExpState(
            present:   ov.ioexp.present,
            enables:   ov.ioexp.enables,
            powerGood: ov.ioexp.powerGood,
            efuses:    finalEfuses
        )
        self.lastOverview = OverviewSnapshot(
            idac:  ov.idac,
            ioexp: newIoExp,
            rails: ov.rails
        )
        self.lastIoExp = newIoExp
    }
}
