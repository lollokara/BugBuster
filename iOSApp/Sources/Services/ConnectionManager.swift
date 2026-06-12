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
    
    // Saved tokens: [MAC: Token]
    public var savedTokens: [String: String] {
        get {
            UserDefaults.standard.dictionary(forKey: "bugbuster_tokens") as? [String: String] ?? [:]
        }
        set {
            UserDefaults.standard.set(newValue, forKey: "bugbuster_tokens")
        }
    }
    
    // Saved last known IPs: [MAC: IP]
    public var savedIps: [String: String] {
        get {
            UserDefaults.standard.dictionary(forKey: "bugbuster_last_ips") as? [String: String] ?? [:]
        }
        set {
            UserDefaults.standard.set(newValue, forKey: "bugbuster_last_ips")
        }
    }
    
    private var lastAutoConnectTime: Date? = nil
    
    // Live telemetries
    @Published public var lastStatus: DeviceStatus? = nil
    @Published public var channelHistory: [Int: [Double]] = [:]
    @Published public var lastOverview: OverviewSnapshot? = nil
    @Published public var lastIoExp: IOExpState? = nil
    @Published public var lastSelftest: SelftestStatus? = nil
    @Published public var lastHatStatus: HatStatus? = nil
    @Published public var lastHatRails: [HatRail] = []
    @Published public var lastUsbPd: USBPDStatus? = nil
    
    @Published public var isSearching = false

    /// True after 2+ consecutive connection-level failures (timeout / refused /
    /// connection lost). Polling backs off and the UI can show a busy state.
    /// Cleared by the first successful request.
    @Published public var transportDegraded = false
    private var consecutiveTransportFailures = 0
    
    private var serviceBrowser: NetServiceBrowser?
    private var services: Set<NetService> = []
    private var pollTimer: AnyCancellable?       // kept for compile compat, unused
    private let session: URLSession
    private var pollTask: Task<Void, Never>? = nil
    private var pollCycle: Int = 0

    private func updateOnMain(_ body: @MainActor @escaping () -> Void) {
        Task { @MainActor in body() }
    }

    private func updateOnMain(after nanoseconds: UInt64, _ body: @MainActor @escaping () -> Void) {
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: nanoseconds)
            body()
        }
    }
    
    public override init() {
        let configuration = URLSessionConfiguration.default
        configuration.timeoutIntervalForRequest = 5.0
        configuration.timeoutIntervalForResource = 10.0
        // One keep-alive TCP connection for the whole control plane. The
        // ESP32 httpd has only a handful of sockets (shared with the SSE
        // scope stream and the REPL WebSocket, which use their own sessions)
        // and serializes request handling anyway, so client-side parallelism
        // only exhausts its socket pool. All requests are additionally
        // serialized through `requestGate` so at most one is in flight.
        configuration.httpMaximumConnectionsPerHost = 1
        configuration.waitsForConnectivity = false
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        self.session = URLSession(configuration: configuration)
        
        super.init()
        
        // Load saved connection info if any
        let lastMac = UserDefaults.standard.string(forKey: "bugbuster_last_mac") ?? ""
        let normLastMac = lastMac.uppercased().trimmingCharacters(in: .whitespacesAndNewlines)
        
        var savedIp = UserDefaults.standard.string(forKey: "bugbuster_ip")
        var savedToken = UserDefaults.standard.string(forKey: "bugbuster_token")
        
        if !normLastMac.isEmpty {
            let tokens = self.savedTokens
            let ips = self.savedIps
            if let token = tokens[normLastMac] {
                savedToken = token
            }
            if let ip = ips[normLastMac] {
                savedIp = ip
            }
        }
        
        if let ip = savedIp, let token = savedToken {
            self.adminToken = token
            self.activeDevice = DiscoveredDevice(hostname: "Saved Device", ip: ip, port: 80, mac: lastMac)
            // Auto connect in background
            Task {
                let success = await self.tryConnect(ip: ip, token: token)
                if success {
                    self.startPolling()
                }
            }
        }
        
        startDiscovery()
    }
    
    public func startDiscovery() {
        updateOnMain {
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
        updateOnMain {
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
        updateOnMain {
            self.services.insert(service)
            service.delegate = self
            service.resolve(withTimeout: 5.0)
        }
    }

    public func netServiceBrowser(_ browser: NetServiceBrowser, didRemove service: NetService, moreComing: Bool) {
        updateOnMain {
            self.services.remove(service)
            self.updateDiscoveredDevices()
        }
    }

    public func netServiceBrowserDidStopSearch(_ browser: NetServiceBrowser) {
        updateOnMain {
            self.isSearching = false
        }
    }
    
    public func netServiceBrowser(_ browser: NetServiceBrowser, didNotSearch errorDict: [String : NSNumber]) {
        print("Bonjour search failed: \(errorDict)")
        updateOnMain {
            self.isSearching = false
        }
    }
    
    // MARK: - NetServiceDelegate

    public func netServiceDidResolveAddress(_ sender: NetService) {
        updateOnMain {
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
        
        updateOnMain {
            self.discoveredDevices = devices
            
            // Auto-reconnect to the last connected device if its IP has changed and Bonjour found it
            if self.connectionState == .disconnected || self.connectionState == .error || self.connectionState == .unauthorized {
                let lastMac = UserDefaults.standard.string(forKey: "bugbuster_last_mac") ?? ""
                let normLastMac = lastMac.uppercased().trimmingCharacters(in: .whitespacesAndNewlines)
                if !normLastMac.isEmpty {
                    if let matchingDevice = devices.first(where: { $0.mac.uppercased().trimmingCharacters(in: .whitespacesAndNewlines) == normLastMac }) {
                        let tokens = self.savedTokens
                        if let token = tokens[normLastMac] {
                            let now = Date()
                            if let lastTime = self.lastAutoConnectTime, now.timeIntervalSince(lastTime) < 5.0 {
                                return // cooldown
                            }
                            
                            // Only trigger auto-connect if we aren't currently trying to connect to the exact same IP
                            // or if we were disconnected/error.
                            if self.activeDevice?.ip != matchingDevice.ip || self.connectionState == .disconnected || self.connectionState == .error {
                                print("[ConnectionManager] mDNS discovered matching last device at new IP \(matchingDevice.ip). Reconnecting...")
                                self.lastAutoConnectTime = now
                                Task { @MainActor in
                                    let success = await self.connect(ip: matchingDevice.ip, token: token)
                                    if success {
                                        self.startPolling()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    public func connect(ip: String, token: String) async -> Bool {
        stopDiscovery()
        updateOnMain {
            self.connectionState = .connecting
        }
        
        let success = await tryConnect(ip: ip, token: token)
        
        if success {
            startPolling()
        } else {
            updateOnMain {
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
            let (data, response) = try await gatedData(for: request)
            guard let httpResponse = response as? HTTPURLResponse else { return false }
            
            if httpResponse.statusCode == 401 {
                updateOnMain {
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
                updateOnMain {
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
            
            let (_, verifyResponse) = try await gatedData(for: verifyRequest)
            guard let verifyHttp = verifyResponse as? HTTPURLResponse else { return false }
            
            if verifyHttp.statusCode == 401 {
                updateOnMain {
                    self.connectionState = .unauthorized
                    self.adminToken = cleanToken
                    self.activeDevice = DiscoveredDevice(hostname: "BugBuster Board", ip: cleanIp, port: 80)
                }
                return false
            }
            
            guard (200...299).contains(verifyHttp.statusCode) else { return false }
            
            // Fetch device info to get the station MAC address
            var deviceMac = ""
            if let infoUrl = URL(string: "\(urlStr)/api/device/info") {
                var infoRequest = URLRequest(url: infoUrl)
                infoRequest.httpMethod = "GET"
                if !cleanToken.isEmpty {
                    infoRequest.setValue(cleanToken, forHTTPHeaderField: "X-BugBuster-Admin-Token")
                }
                if let (infoData, _) = try? await gatedData(for: infoRequest),
                   let info = try? JSONDecoder().decode(DeviceInfo.self, from: infoData) {
                    deviceMac = info.macAddress
                }
            }
            
            let finalMac = deviceMac.isEmpty ? (self.discoveredDevices.first(where: { $0.ip == cleanIp })?.mac ?? "") : deviceMac
            let normMac = finalMac.uppercased().trimmingCharacters(in: .whitespacesAndNewlines)
            
            updateOnMain {
                self.updateStatus(status)
                self.adminToken = cleanToken
                self.activeDevice = DiscoveredDevice(
                    hostname: "BugBuster Board",
                    ip: cleanIp,
                    port: 80,
                    mac: finalMac
                )
                self.connectionState = .connected
                
                // Save pairing token and IP per MAC
                if !normMac.isEmpty {
                    var tokens = self.savedTokens
                    tokens[normMac] = cleanToken
                    self.savedTokens = tokens
                    
                    var ips = self.savedIps
                    ips[normMac] = cleanIp
                    self.savedIps = ips
                    
                    UserDefaults.standard.set(finalMac, forKey: "bugbuster_last_mac")
                }
                
                // Keep the old global keys for backward compatibility
                UserDefaults.standard.set(cleanIp, forKey: "bugbuster_ip")
                UserDefaults.standard.set(cleanToken, forKey: "bugbuster_token")
            }
            
            // Kick off a one-shot slow fetch after connect — overview/ioexp/selftest/hat.
            // This runs after the connection is established; failures are silently ignored.
            updateOnMain(after: 500_000_000) {
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
        updateOnMain {
            self.activeDevice = nil
            self.connectionState = .disconnected
            self.updateStatus(nil)
            self.lastOverview = nil
            self.lastIoExp = nil
            self.lastSelftest = nil
            self.lastHatStatus = nil
            self.lastHatRails = []
            self.lastUsbPd = nil
            self.lshiftOe = false
        }
        UserDefaults.standard.removeObject(forKey: "bugbuster_ip")
        UserDefaults.standard.removeObject(forKey: "bugbuster_token")
        UserDefaults.standard.removeObject(forKey: "bugbuster_last_mac")
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
                    updateOnMain { self.updateStatus(status) }
                }

                // Circuit breaker: when the device stops answering, drop to a
                // 2s probe cadence and skip the multi-request slow cycle so we
                // don't pile more requests onto a wedged httpd.
                let degraded = await MainActor.run { self.transportDegraded }
                if degraded {
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                    continue
                }

                // Re-check before the multi-request slow cycle: the stream may
                // have started while /api/status above was in flight.
                if cycle % 5 == 0 && !ScopeStreamManager.shared.isStreaming {
                    // Slow-cycle: reuse self.session (no new session per cycle) with
                    // per-request timeout override. Sequential so only one TCP
                    // connection is open at a time. /api/overview embeds ioexp.
                    if let ov: OverviewSnapshot = await self.fetchDecoded(OverviewSnapshot.self, ip: ip, path: "/api/overview", token: token) {
                        updateOnMain {
                            self.updateLastOverview(ov)
                        }
                    }
                    if let st: SelftestStatus = await self.fetchDecoded(SelftestStatus.self, ip: ip, path: "/api/selftest", token: token) {
                        updateOnMain { self.lastSelftest = st }
                    }
                    if let ht: HatStatus = await self.fetchDecoded(HatStatus.self, ip: ip, path: "/api/hat", token: token) {
                        updateOnMain { self.lastHatStatus = ht }
                        if ht.isPresent {
                            if let rr: HatRailsResponse = await self.fetchDecoded(HatRailsResponse.self, ip: ip, path: "/api/hat/v2/rails", token: token) {
                                updateOnMain { self.lastHatRails = rr.rails }
                            }
                        }
                    }
                    if let pd: USBPDStatus = await self.fetchDecoded(USBPDStatus.self, ip: ip, path: "/api/usbpd", token: token) {
                        updateOnMain { self.lastUsbPd = pd }
                    }
                }

                try? await Task.sleep(nanoseconds: 250_000_000)
            }
        }
    }

    public func updateStatus(_ status: DeviceStatus?) {
        self.lastStatus = status
        if let channels = status?.channels {
            for ch in channels {
                var history = self.channelHistory[ch.id] ?? []
                history.append(ch.adcValue)
                if history.count > 60 {
                    history.removeFirst(history.count - 60)
                }
                self.channelHistory[ch.id] = history
            }
        } else if status == nil {
            self.channelHistory = [:]
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
                updateOnMain { self.updateLastOverview(ov) }
            }
            if let st: SelftestStatus = await fetchDecoded(SelftestStatus.self, ip: ip, path: "/api/selftest", token: token) {
                updateOnMain { self.lastSelftest = st }
            }
            if let ht: HatStatus = await fetchDecoded(HatStatus.self, ip: ip, path: "/api/hat", token: token) {
                updateOnMain { self.lastHatStatus = ht }
                if ht.isPresent {
                    if let rr: HatRailsResponse = await fetchDecoded(HatRailsResponse.self, ip: ip, path: "/api/hat/v2/rails", token: token) {
                        updateOnMain { self.lastHatRails = rr.rails }
                    }
                }
            }
            if let pd: USBPDStatus = await fetchDecoded(USBPDStatus.self, ip: ip, path: "/api/usbpd", token: token) {
                updateOnMain { self.lastUsbPd = pd }
            }
        }
    }
    
    // MARK: - Serialized transport core

    /// FIFO gate: at most one control-plane request in flight at a time.
    /// (An actor method alone does not serialize across `await` suspension
    /// points, hence the explicit continuation queue.)
    private actor RequestGate {
        private var busy = false
        private var waiters: [CheckedContinuation<Void, Never>] = []

        func acquire() async {
            if busy {
                await withCheckedContinuation { waiters.append($0) }
            } else {
                busy = true
            }
        }

        func release() {
            if waiters.isEmpty {
                busy = false
            } else {
                waiters.removeFirst().resume()
            }
        }
    }

    private let requestGate = RequestGate()

    /// Single funnel for every control-plane request. Serializes through the
    /// gate and feeds the transport-health circuit breaker.
    private func gatedData(for request: URLRequest) async throws -> (Data, URLResponse) {
        await requestGate.acquire()
        do {
            let result = try await session.data(for: request)
            await requestGate.release()
            noteTransportSuccess()
            return result
        } catch {
            await requestGate.release()
            noteTransportFailure(error)
            throw error
        }
    }

    private func noteTransportSuccess() {
        updateOnMain {
            self.consecutiveTransportFailures = 0
            if self.transportDegraded { self.transportDegraded = false }
        }
    }

    private func noteTransportFailure(_ error: Error) {
        let ns = error as NSError
        guard ns.domain == NSURLErrorDomain,
              ns.code == NSURLErrorTimedOut
                || ns.code == NSURLErrorCannotConnectToHost
                || ns.code == NSURLErrorNetworkConnectionLost else { return }
        updateOnMain {
            self.consecutiveTransportFailures += 1
            if self.consecutiveTransportFailures >= 2 {
                self.transportDegraded = true
            }
        }
    }

    private func fetchDecoded<T: Decodable>(_ type: T.Type, ip: String, path: String, token: String, timeout: TimeInterval = 5.0) async -> T? {
        var urlStr = ip
        if !urlStr.lowercased().hasPrefix("http://") { urlStr = "http://\(urlStr)" }
        guard let url = URL(string: "\(urlStr)\(path)") else { return nil }
        var req = URLRequest(url: url)
        req.timeoutInterval = timeout
        if !token.isEmpty { req.setValue(token, forHTTPHeaderField: "X-BugBuster-Admin-Token") }
        guard let (data, resp) = try? await gatedData(for: req),
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
        
        let (data, response) = try await gatedData(for: request)
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
        
        let (_, response) = try await gatedData(for: request)
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
                updateOnMain {
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
        updateOnMain {
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
        updateOnMain {
            self.lastOverview = OverviewSnapshot(
                idac:  overview.idac,
                ioexp: newIoExp,
                rails: overview.rails
            )
        }
    }

    public func applyOptimisticHatRailEnable(railId: Int, enabled: Bool) {
        updateOnMain {
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
                updateOnMain { self.lastHatRails = rr.rails }
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
                updateOnMain { self.updateLastOverview(ov) }
            }
        }
    }

    public func fetchUsbPdQuick() {
        guard connectionState == .connected, let device = activeDevice else { return }
        let ip = device.ip; let token = adminToken
        Task {
            if let pd: USBPDStatus = await fetchDecoded(USBPDStatus.self, ip: ip, path: "/api/usbpd", token: token) {
                updateOnMain { self.lastUsbPd = pd }
            }
        }
    }

    public func setUsbPdVoltage(_ voltage: Int) async -> Bool {
        do {
            let ok = try await postAction(path: "/api/usbpd/select", json: ["voltage": voltage])
            if ok {
                // Optimistically fetch updated status after a short delay for negotiation
                updateOnMain(after: 1_000_000_000) {
                    self.fetchUsbPdQuick()
                }
            }
            return ok
        } catch {
            return false
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

    public func fetchCalibrationPoints(ch: Int) async -> [CalibrationPoint] {
        guard connectionState == .connected, let device = activeDevice else { return [] }
        let ip = device.ip; let token = adminToken
        if let res: CalibrationPointsResponse = await fetchDecoded(CalibrationPointsResponse.self, ip: ip, path: "/api/idac/cal/points?ch=\(ch)", token: token) {
            return res.points
        }
        return []
    }
}

public struct CalibrationPointsResponse: Codable {
    public let ch: Int
    public let count: Int
    public let valid: Bool
    public let points: [CalibrationPoint]
}

