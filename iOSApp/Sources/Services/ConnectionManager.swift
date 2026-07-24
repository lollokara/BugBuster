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

/// Which physical transport the active connection uses.
public enum TransportKind: String, Codable {
    case wifi
    case ble
}

public struct DiscoveredDevice: Identifiable, Hashable, Codable {
    public var id: String { isBle ? "ble:\(bleId?.uuidString ?? hostname)" : (mac.isEmpty ? hostname : mac) }
    public let hostname: String
    public let ip: String
    public let port: Int
    public let firmware: String
    public let mac: String
    public let model: String
    public let isBle: Bool
    public let bleId: UUID?

    public init(hostname: String, ip: String, port: Int, firmware: String = "", mac: String = "", model: String = "", isBle: Bool = false, bleId: UUID? = nil) {
        self.hostname = hostname
        self.ip = ip
        self.port = port
        self.firmware = firmware
        self.mac = mac
        self.model = model
        self.isBle = isBle
        self.bleId = bleId
    }
}

public class ConnectionManager: NSObject, ObservableObject, NetServiceBrowserDelegate, NetServiceDelegate {
    @Published public var connectionState: ConnectionState = .disconnected
    @Published public var discoveredDevices: [DiscoveredDevice] = []
    @Published public var activeDevice: DiscoveredDevice? = nil
    @Published public var adminToken: String = ""
    @Published public var lshiftOe: Bool = false

    /// Transport backing the current/last connection. Selected when the user
    /// taps a discovered device (Bonjour = .wifi, BLE peripheral = .ble).
    @Published public var transport: TransportKind = .wifi

    // MARK: - BLE
    /// CoreBluetooth control plane mirroring the WiFi API surface.
    public let ble = BLETransport()
    /// BugBuster peripherals seen over BLE (separate list from Bonjour results).
    @Published public var bleDevices: [DiscoveredDevice] = []
    /// True once CoreBluetooth is powered on and authorised.
    @Published public var bleAvailable: Bool = false
    /// True while a BLE scan is active (mirrors BLETransport.isScanning).
    @Published public var bleScanning: Bool = false
    private var cancellables: Set<AnyCancellable> = []
    private var blePollTask: Task<Void, Never>? = nil
    
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
    @Published public var lastGpios: [GPIOPin] = []
    @Published public var lastWifiStatus: WifiStatus? = nil
    @Published public var lastDeviceVersion: DeviceVersion? = nil
    @Published public var toastMessage: ToastMessage? = nil

    // MARK: - VDUT (DAQ HAT DUT power supply) — iOS UI scaffold only
    //
    // NOT YET IMPLEMENTED IN FIRMWARE. This is the client-side half of a
    // protocol that doesn't exist on the DAQ HAT yet — the endpoints below
    // are what iOS calls; a firmware agent needs to add the matching
    // handlers. Documenting the expected shape now so both sides agree:
    //
    // Distinct from the mainboard/HAT power RAILS (VLOGIC/VADJ1-4, already
    // wired via /api/hat/v2/rail/*) — VDUT is the DAQ HAT's programmable
    // supply that biases/powers the device under test during acquisition
    // (the same rail exercised by the P4 calibration flow in
    // Firmware/DAQ_HAT/ESP32P4/src/cal/range_cal.c, "enable the DUT supply
    // at 2V before prompting").
    //
    // Needed firmware surface: three JSON paths, reachable over BOTH transports
    // (not HTTP-only). Implement them as `api_core_handle(method,path,body)`
    // cases in Firmware/ESP32/src/net/api_core.cpp (see the
    // /api/daq/wifi_stream/{start,stop,status} trio there for the exact
    // pattern) so the existing HTTP webserver.cpp routes AND the NimBLE GATT
    // tunnel (net/ble_service.cpp) both get them for free — that shared
    // dispatcher is exactly why iOS's `postAction`/`bleDecoded` calls below
    // don't need any transport-specific branching once firmware exists.
    // The S3 itself doesn't own the DUT supply hardware — it has to relay
    // to the P4 over the HAT UART link with a new request/response command
    // pair modeled on HATP_CMD_STAGE_READ (0x75, S3-initiated request + P4
    // reply), NOT the one-way HAT_CMD_DAQ_TELEMETRY push pattern.
    //
    //   GET  /api/daq/vdut/status
    //     -> { "present": bool, "enabled": bool,
    //          "voltageSetpointV": number, "currentLimitMa": number,
    //          "measuredVoltageV": number, "measuredCurrentMa": number,
    //          "fault": bool }
    //
    //   POST /api/daq/vdut/enable   body: { "enabled": bool }
    //     -> 200 on success (mirrors postAction convention below)
    //
    //   POST /api/daq/vdut/setpoint body: { "voltageV": number, "currentLimitMa": number }
    //     -> 200 on success; firmware should clamp to hardware-safe range
    //        and reject (non-2xx) out-of-range requests rather than silently
    //        clamping, so the UI can surface an error instead of drifting
    //        from what it displayed.
    //
    // Until firmware exists, `refreshVdutStatus()` will simply fail (caught,
    // no-op) and the UI shows the last-known/default state.
    @Published public var vdutPresent: Bool = false
    @Published public var vdutEnabled: Bool = false
    @Published public var vdutVoltageSetpointV: Double = 3.3
    @Published public var vdutCurrentLimitMa: Double = 100
    @Published public var vdutMeasuredVoltageV: Double? = nil
    @Published public var vdutMeasuredCurrentMa: Double? = nil
    @Published public var vdutFault: Bool = false
    
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
        setupBLE()
    }

    // MARK: - BLE wiring

    private func setupBLE() {
        // Mirror the transport's discovered peripherals into our own list,
        // adapting BLEDevice -> DiscoveredDevice (marked isBle).
        ble.$devices
            .receive(on: DispatchQueue.main)
            .sink { [weak self] devs in
                self?.bleDevices = devs.map { d in
                    DiscoveredDevice(hostname: d.name, ip: "", port: 0, firmware: "",
                                     mac: "", model: "bugbuster-s3", isBle: true, bleId: d.id)
                }
            }
            .store(in: &cancellables)

        ble.$poweredOn
            .receive(on: DispatchQueue.main)
            .sink { [weak self] on in
                guard let self = self else { return }
                self.bleAvailable = on
                // Begin discovery as soon as the radio is ready, while idle.
                if on && self.connectionState != .connected {
                    self.ble.startScan()
                }
            }
            .store(in: &cancellables)

        ble.$isScanning
            .receive(on: DispatchQueue.main)
            .sink { [weak self] s in self?.bleScanning = s }
            .store(in: &cancellables)

        ble.onDisconnect = { [weak self] in
            guard let self = self else { return }
            if self.transport == .ble {
                self.blePollTask?.cancel()
                self.blePollTask = nil
                self.updateOnMain {
                    if self.connectionState == .connected {
                        self.connectionState = .disconnected
                    }
                }
            }
        }
    }

    /// Begin scanning for BugBuster peripherals over BLE.
    public func startBLEScan() {
        ble.startScan()
    }

    public func stopBLEScan() {
        ble.stopScan()
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
            if self.transport == .wifi,
               self.connectionState == .disconnected || self.connectionState == .error || self.connectionState == .unauthorized {
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
        // Guard against the BLE path leaking through with an empty IP (which
        // URLSession resolves to localhost, spamming connection-refused errors).
        guard !ip.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return false }
        stopDiscovery()
        updateOnMain {
            self.transport = .wifi
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

    // MARK: - BLE connection

    /// Connect to a BugBuster peripheral over BLE: open the GATT link, read the
    /// device identity, present the admin token, and (on success) start the BLE
    /// poll loop. Mirrors `connect(ip:token:)` for the WiFi path.
    public func connectBLE(_ device: DiscoveredDevice, token: String) async -> Bool {
        guard let bleId = device.bleId else { return false }
        let cleanToken = token.trimmingCharacters(in: .whitespacesAndNewlines)
        stopDiscovery()
        ble.stopScan()
        updateOnMain {
            self.transport = .ble
            self.connectionState = .connecting
        }

        let linked = await ble.connect(id: bleId)
        guard linked else {
            NSLog("[BLE] connectBLE: GATT link/discovery failed")
            updateOnMain { self.connectionState = .error }
            return false
        }

        // Read identity (pre-auth) to learn the MAC for token bookkeeping.
        var deviceMac = ""
        if let infoData = await ble.readInfo(),
           let info = try? JSONSerialization.jsonObject(with: infoData) as? [String: Any] {
            deviceMac = (info["mac"] as? String) ?? ""
        }
        NSLog("[BLE] connectBLE: linked OK, mac=%@", deviceMac.isEmpty ? "(none)" : deviceMac)
        let normMac = deviceMac.uppercased().trimmingCharacters(in: .whitespacesAndNewlines)

        // Resolve a token: explicit > saved-by-MAC.
        var useToken = cleanToken
        if useToken.isEmpty, !normMac.isEmpty, let saved = savedTokens[normMac] {
            useToken = saved
        }
        if useToken.isEmpty {
            updateOnMain {
                self.connectionState = .unauthorized
                self.adminToken = ""
                self.activeDevice = device
            }
            return false
        }

        let authed = await ble.authenticate(token: useToken)
        NSLog("[BLE] connectBLE: authenticate -> %@", authed ? "OK" : "REJECTED")
        if !authed {
            updateOnMain {
                self.connectionState = .unauthorized
                self.adminToken = useToken
                self.activeDevice = device
            }
            return false
        }

        updateOnMain {
            self.adminToken = useToken
            self.activeDevice = DiscoveredDevice(hostname: device.hostname, ip: "", port: 0,
                                                 firmware: device.firmware, mac: deviceMac,
                                                 model: device.model, isBle: true, bleId: bleId)
            self.connectionState = .connected

            if !normMac.isEmpty {
                var tokens = self.savedTokens
                tokens[normMac] = useToken
                self.savedTokens = tokens
                UserDefaults.standard.set(deviceMac, forKey: "bugbuster_last_mac")
            }
        }

        startBLEPolling()
        return true
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
        blePollTask?.cancel()
        blePollTask = nil
        stopPolling()
        if transport == .ble {
            ble.disconnect()
        }
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
            self.lastGpios = []
            self.lastWifiStatus = nil
            self.lastDeviceVersion = nil
            self.lshiftOe = false
            self.transport = .wifi
        }
        UserDefaults.standard.removeObject(forKey: "bugbuster_ip")
        UserDefaults.standard.removeObject(forKey: "bugbuster_token")
        UserDefaults.standard.removeObject(forKey: "bugbuster_last_mac")
        stopMockUpdates()
        startDiscovery()
    }

    // MARK: - Mock / Demo Mode
    //
    // UI-only synthetic data path for screenshotting/debugging layout without
    // real hardware. Never touches the network — bypasses `tryConnect`/
    // `startPolling` entirely and drives its own jitter timer instead.

    private var mockTimer: Timer?
    public private(set) var isMockActive = false

    public func connectMock() {
        stopDiscovery()
        isMockActive = true
        transport = .wifi
        activeDevice = DiscoveredDevice(
            hostname: "bugbuster-mock",
            ip: "127.0.0.1",
            port: 80,
            firmware: "3.6.1-mock",
            mac: "AA:BB:CC:DD:EE:FF",
            model: "BugBuster DAQ HAT (Mock)"
        )
        adminToken = "mock"
        connectionState = .connected
        populateMockData()
        mockTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.jitterMockData()
        }
        Task { @MainActor in DaqWifiStreamManager.shared.connectMock() }
    }

    private func stopMockUpdates() {
        mockTimer?.invalidate()
        mockTimer = nil
        isMockActive = false
        Task { @MainActor in DaqWifiStreamManager.shared.disconnectMock() }
    }

    private func mockChannel(_ id: Int, function: String, functionCode: Int, value: Double) -> ChannelState {
        ChannelState(
            id: id, function: function, functionCode: functionCode,
            adcRaw: Int(value * 100000), adcValue: value, adcRange: 0, adcRate: 1, adcMux: 0,
            dacCode: 2048, dacValue: value, dinState: id % 2 == 0, dinCounter: id * 12,
            doState: false, alert: 0, alertMask: 0, rtdExcitationUa: nil
        )
    }

    private func populateMockData() {
        let channels = [
            mockChannel(0, function: "CH_FUNC_VIN", functionCode: 3, value: 3.301),
            mockChannel(1, function: "CH_FUNC_VOUT", functionCode: 1, value: 2.500),
            mockChannel(2, function: "CH_FUNC_IIN_EXT", functionCode: 4, value: 0.0142),
            mockChannel(3, function: "CH_FUNC_HIGH_Z", functionCode: 0, value: 0.0)
        ]
        lastStatus = DeviceStatus(
            spiOk: true, i2cOk: true, muxOk: true, dieTemp: 42.5,
            alertStatus: 0, alertMask: 0, supplyAlertStatus: 0, supplyAlertMask: 0,
            liveStatus: 1, channels: channels, diagnostics: [], muxStates: [0, 0, 0, 0],
            freeHeap: 182_000, minFreeHeap: 150_000, uptimeMs: 3_723_000
        )
        for ch in channels {
            channelHistory[ch.id] = (0..<60).map { _ in ch.adcValue + Double.random(in: -0.02...0.02) }
        }
        lastOverview = OverviewSnapshot(
            idac: IDACState(present: true, channels: []),
            ioexp: IOExpState(
                present: true,
                enables: IOExpEnables(vadj1: true, vadj2: true, analog15v: true, mux: true, usbHub: true),
                powerGood: IOExpPowerGood(logic: true, vadj1: true, vadj2: true),
                efuses: [
                    IOExpEFuse(id: 0, enabled: true, fault: false),
                    IOExpEFuse(id: 1, enabled: true, fault: false)
                ]
            ),
            rails: [
                OverviewRail(rail: 0, name: "VLOGIC", voltage: 3.301, ok: true),
                OverviewRail(rail: 1, name: "VADJ1", voltage: 5.012, ok: true),
                OverviewRail(rail: 2, name: "VADJ2", voltage: 12.045, ok: true)
            ]
        )
        lastSelftest = SelftestStatus(
            boot: SelftestBoot(ran: true, passed: true, vadj1V: 5.01, vadj2V: 12.02, vlogicV: 3.30),
            calibration: SelftestCalibration(status: 2, channel: 0, points: 8, lastVoltageV: 3.30, errorMv: 1.2),
            workerEnabled: false, supplyMonitorActive: true
        )
        lastHatStatus = HatStatus(detected: true, present: true, kind: "daq")
        lastHatRails = [
            HatRail(railId: 0, enabled: true, voltageMv: 3300, targetVoltageMv: 3300, currentMa: 120, status: 0),
            HatRail(railId: 1, enabled: true, voltageMv: 5000, targetVoltageMv: 5000, currentMa: 340, status: 0),
            HatRail(railId: 2, enabled: false, voltageMv: 0, targetVoltageMv: 0, currentMa: 0, status: 0)
        ]
        lastUsbPd = USBPDStatus(
            present: true, attached: true, cc: "CC1", voltageV: 20.0, currentA: 3.0, powerW: 60.0,
            pdResponse: 1,
            sourcePdos: [
                USBPDSourcePDO(voltage: "5V", detected: true, maxCurrentA: 3.0, maxPowerW: 15.0),
                USBPDSourcePDO(voltage: "20V", detected: true, maxCurrentA: 3.0, maxPowerW: 60.0)
            ],
            selectedPdo: 1
        )
        lastGpios = (0..<8).map { GPIOPin(pin: $0, mode: $0 % 3, input: $0 % 2 == 0, output: $0 % 2 != 0) }
        lastWifiStatus = WifiStatus(
            connected: true, staSSID: "Bench-WiFi", staIP: "192.168.1.42", rssi: -52,
            apSSID: "BugBuster-AA22", apIP: "192.168.4.1", apMAC: "AA:BB:CC:DD:EE:FF"
        )
        lastDeviceVersion = DeviceVersion(esp32: "3.6.1", hat: "bb-hat-3.5")
    }

    private func jitterMockData() {
        guard connectionState == .connected, isMockActive, var status = lastStatus else { return }
        let jittered = status.channels.map { ch -> ChannelState in
            let delta = Double.random(in: -0.01...0.01)
            return mockChannel(ch.id, function: ch.function, functionCode: ch.functionCode, value: max(0, ch.adcValue + delta))
        }
        status = DeviceStatus(
            spiOk: status.spiOk, i2cOk: status.i2cOk, muxOk: status.muxOk, dieTemp: status.dieTemp + Double.random(in: -0.2...0.2),
            alertStatus: status.alertStatus, alertMask: status.alertMask,
            supplyAlertStatus: status.supplyAlertStatus, supplyAlertMask: status.supplyAlertMask,
            liveStatus: status.liveStatus, channels: jittered, diagnostics: status.diagnostics,
            muxStates: status.muxStates, freeHeap: status.freeHeap, minFreeHeap: status.minFreeHeap,
            uptimeMs: status.uptimeMs + 500
        )
        lastStatus = status
        for ch in jittered {
            var hist = channelHistory[ch.id] ?? []
            hist.append(ch.adcValue)
            if hist.count > 60 { hist.removeFirst(hist.count - 60) }
            channelHistory[ch.id] = hist
        }
    }

    private struct VdutStatus: Decodable {
        let present: Bool
        let enabled: Bool
        let voltageSetpointV: Double
        let currentLimitMa: Double
        let measuredVoltageV: Double?
        let measuredCurrentMa: Double?
        let fault: Bool
    }

    /// Best-effort refresh; firmware doesn't implement this endpoint yet, so
    /// failures are expected and silently ignored (UI keeps last-known state).
    /// Works over both transports: BLE tunnels the same `/api/daq/vdut/status`
    /// path through `api_core_handle` on the device (see `bleDecoded`), same
    /// as `/api/status`/`/api/hat`/etc — no separate BLE-specific endpoint needed.
    public func refreshVdutStatus() async {
        let status: VdutStatus?
        if transport == .ble {
            status = await bleDecoded(VdutStatus.self, path: "/api/daq/vdut/status")
        } else {
            status = try? await getRequest(path: "/api/daq/vdut/status")
        }
        guard let status else { return }
        updateOnMain {
            self.vdutPresent = status.present
            self.vdutEnabled = status.enabled
            self.vdutVoltageSetpointV = status.voltageSetpointV
            self.vdutCurrentLimitMa = status.currentLimitMa
            self.vdutMeasuredVoltageV = status.measuredVoltageV
            self.vdutMeasuredCurrentMa = status.measuredCurrentMa
            self.vdutFault = status.fault
        }
    }

    public func setVdutEnable(_ enabled: Bool) async -> Bool {
        do {
            let ok = try await postAction(path: "/api/daq/vdut/enable", json: ["enabled": enabled])
            if ok { updateOnMain { self.vdutEnabled = enabled } }
            return ok
        } catch {
            return false
        }
    }

    public func setVdutSetpoint(voltageV: Double, currentLimitMa: Double) async -> Bool {
        do {
            let ok = try await postAction(path: "/api/daq/vdut/setpoint", json: [
                "voltageV": voltageV,
                "currentLimitMa": currentLimitMa
            ])
            if ok {
                updateOnMain {
                    self.vdutVoltageSetpointV = voltageV
                    self.vdutCurrentLimitMa = currentLimitMa
                }
            }
            return ok
        } catch {
            return false
        }
    }

    public func startPolling() {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            var cycle = 0
            while !Task.isCancelled {
                guard let self = self,
                      self.connectionState == .connected,
                      self.transport == .wifi,
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
                    if let ws: WifiStatus = await self.fetchDecoded(WifiStatus.self, ip: ip, path: "/api/wifi", token: token) {
                        updateOnMain { self.lastWifiStatus = ws }
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

    // MARK: - BLE polling + response adapters

    /// Poll the device over the BLE control plane. Mirrors `startPolling()` but
    /// uses the API tunnel; cadence is gentler since BLE round-trips are slower.
    public func startBLEPolling() {
        blePollTask?.cancel()
        blePollTask = Task { [weak self] in
            var cycle = 0
            while !Task.isCancelled {
                guard let self = self, self.connectionState == .connected, self.transport == .ble else {
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                    continue
                }
                cycle += 1

                if let status = await self.bleFetchStatus() {
                    updateOnMain { self.updateStatus(status) }
                }
                // Slower multi-endpoint cycle every ~4 status polls.
                if cycle % 4 == 0 {
                    await self.bleFetchHat()
                    await self.bleFetchUsbPd()
                    await self.bleFetchWifi()
                    if let ov: OverviewSnapshot = await self.bleDecoded(OverviewSnapshot.self, path: "/api/overview") {
                        updateOnMain { self.updateLastOverview(ov) }
                    }
                    if let gpios: [GPIOPin] = await self.bleDecoded([GPIOPin].self, path: "/api/gpio") {
                        updateOnMain { self.lastGpios = gpios }
                    }
                }
                try? await Task.sleep(nanoseconds: 600_000_000)
            }
        }
    }

    /// Issue a JSON request over the BLE API tunnel and decode the `{...}` body.
    private func bleJSON(_ path: String, body: [String: Any]? = nil) async -> [String: Any]? {
        guard let data = await ble.apiRequest(path: path, body: body) else { return nil }
        return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    }

    /// Issue a BLE API tunnel request and decode the JSON directly into `T`.
    /// Used for endpoints whose BLE payload mirrors the HTTP shape 1:1
    /// (e.g. /api/overview, /api/gpio).
    private func bleDecoded<T: Decodable>(_ type: T.Type, path: String, body: [String: Any]? = nil) async -> T? {
        guard let data = await ble.apiRequest(path: path, body: body) else { return nil }
        return try? JSONDecoder().decode(type, from: data)
    }

    /// Decode `/api/status` over BLE. Since api_core now returns the rich,
    /// HTTP-identical shape on every transport, the BLE path decodes the very
    /// same `DeviceStatus` model the HTTP path uses — no compact mapper.
    private func bleFetchStatus() async -> DeviceStatus? {
        return await bleDecoded(DeviceStatus.self, path: "/api/status")
    }

    /// Decode `/api/hat` (HatStatus) + `/api/hat/v2/rails` (HatRailsResponse) over BLE.
    private func bleFetchHat() async {
        guard let ht: HatStatus = await bleDecoded(HatStatus.self, path: "/api/hat") else { return }
        updateOnMain { self.lastHatStatus = ht }
        if ht.isPresent {
            if let rr: HatRailsResponse = await bleDecoded(HatRailsResponse.self, path: "/api/hat/v2/rails") {
                updateOnMain { self.lastHatRails = rr.rails }
            }
        }
    }

    /// Decode `/api/usbpd` (rich USBPDStatus) over BLE.
    private func bleFetchUsbPd() async {
        if let pd: USBPDStatus = await bleDecoded(USBPDStatus.self, path: "/api/usbpd") {
            updateOnMain { self.lastUsbPd = pd }
        }
    }

    /// Decode `/api/wifi` (rich WifiStatus) over BLE.
    private func bleFetchWifi() async {
        if let ws: WifiStatus = await bleDecoded(WifiStatus.self, path: "/api/wifi") {
            updateOnMain { self.lastWifiStatus = ws }
        }
    }

    /// Fetch slow endpoints once (on connect or pull-to-refresh). Sequential,
    /// reuses self.session — no throwaway URLSession objects.
    public func fetchOverviewOnce() {
        guard connectionState == .connected, let device = activeDevice else { return }
        if transport == .ble {
            Task {
                if let ov: OverviewSnapshot = await self.bleDecoded(OverviewSnapshot.self, path: "/api/overview") {
                    updateOnMain { self.updateLastOverview(ov) }
                }
                if let gpios: [GPIOPin] = await self.bleDecoded([GPIOPin].self, path: "/api/gpio") {
                    updateOnMain { self.lastGpios = gpios }
                }
                await self.bleFetchHat()
                await self.bleFetchUsbPd()
                await self.bleFetchWifi()
            }
            return
        }
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
            if let ws: WifiStatus = await fetchDecoded(WifiStatus.self, ip: ip, path: "/api/wifi", token: token) {
                updateOnMain { self.lastWifiStatus = ws }
            }
            if let v: DeviceVersion = await fetchDecoded(DeviceVersion.self, ip: ip, path: "/api/device/version", token: token) {
                updateOnMain { self.lastDeviceVersion = v }
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
        // BLE sessions carry an empty IP; never fall back to HTTP (empty host
        // resolves to localhost and floods connection-refused errors).
        guard !ip.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
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
        guard !ip.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { throw URLError(.badURL) }
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

        // BLE control plane: tunnel the request and read back the {"ok":...} flag.
        // Paths the firmware tunnel doesn't implement return ok:false gracefully.
        if transport == .ble {
            guard let obj = await bleJSON(path, body: json) else { return false }
            return (obj["ok"] as? Bool) ?? false
        }

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
                r.railId == railId ? HatRail(railId: r.railId, enabled: enabled, voltageMv: r.voltageMv, targetVoltageMv: r.targetVoltageMv, currentMa: r.currentMa, status: r.status) : r
            }
        }
    }

    public func fetchHatRailsQuick() {
        guard connectionState == .connected, let device = activeDevice else { return }
        if transport == .ble {
            Task { await self.bleFetchHat() }
            return
        }
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
        if transport == .ble {
            Task {
                if let ov: OverviewSnapshot = await self.bleDecoded(OverviewSnapshot.self, path: "/api/overview") {
                    updateOnMain { self.updateLastOverview(ov) }
                }
            }
            return
        }
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
        if transport == .ble {
            Task { await self.bleFetchUsbPd() }
            return
        }
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
        if transport == .ble {
            // Firmware also attaches HAT calibration data under "hat" (ignored by
            // CalibrationPointsResponse; available for future UI).
            if let res: CalibrationPointsResponse = await bleDecoded(CalibrationPointsResponse.self, path: "/api/idac/cal/points?ch=\(ch)") {
                return res.points
            }
            return []
        }
        let ip = device.ip; let token = adminToken
        if let res: CalibrationPointsResponse = await fetchDecoded(CalibrationPointsResponse.self, ip: ip, path: "/api/idac/cal/points?ch=\(ch)", token: token) {
            return res.points
        }
        return []
    }

    /// RP2040 HAT calibration data (state + validation metrics), read back by
    /// the ESP32 over the HAT UART. Works over both BLE and WiFi/HTTP.
    /// Pass `rail` to select which rail's stored points are exported (default:
    /// whatever rail the live cal-engine status refers to).
    public func fetchHatCalibration(rail: Int? = nil) async -> HatCalibration? {
        guard connectionState == .connected, let device = activeDevice else { return nil }
        let path = rail.map { "/api/hat/calibration?rail=\($0)" } ?? "/api/hat/calibration"
        if transport == .ble {
            return await bleDecoded(HatCalibration.self, path: path)
        }
        let ip = device.ip; let token = adminToken
        return await fetchDecoded(HatCalibration.self, ip: ip, path: path, token: token)
    }

    /// Start the RP2040 HAT auto-calibration sweep for a given rail (1=VADJ3,
    /// 2=VADJ4). Routed through api_core so it works over BLE and WiFi.
    public func startHatCalibration(rail: Int) async -> Bool {
        do {
            return try await postAction(path: "/api/hat/v2/calibrate/start", json: ["railId": rail])
        } catch {
            return false
        }
    }

    // MARK: - GPIO

    public func fetchGpios() {
        guard connectionState == .connected, let device = activeDevice else { return }
        if transport == .ble {
            Task {
                if let gpios: [GPIOPin] = await self.bleDecoded([GPIOPin].self, path: "/api/gpio") {
                    updateOnMain { self.lastGpios = gpios }
                }
            }
            return
        }
        let ip = device.ip; let token = adminToken
        Task {
            // /api/gpio returns a bare JSON array of GPIO objects.
            if let gpios: [GPIOPin] = await fetchDecoded([GPIOPin].self, ip: ip, path: "/api/gpio", token: token) {
                updateOnMain { self.lastGpios = gpios }
            }
        }
    }

    public func configureGpio(pin: Int, mode: Int) async -> Bool {
        do {
            let ok = try await postAction(path: "/api/gpio/\(pin)/config", json: ["mode": mode])
            if ok { fetchGpios() }
            return ok
        } catch { return false }
    }

    public func setGpioOutput(pin: Int, value: Bool) async -> Bool {
        do {
            let ok = try await postAction(path: "/api/gpio/\(pin)/set", json: ["value": value])
            if ok { fetchGpios() }
            return ok
        } catch { return false }
    }

    // MARK: - Device Reset

    public func resetDevice() async -> Bool {
        do {
            return try await postAction(path: "/api/device/reset", json: [:])
        } catch { return false }
    }

    // MARK: - Device Version

    public func fetchDeviceVersion() {
        guard connectionState == .connected, let device = activeDevice else { return }
        let ip = device.ip; let token = adminToken
        Task {
            if let v: DeviceVersion = await fetchDecoded(DeviceVersion.self, ip: ip, path: "/api/device/version", token: token) {
                updateOnMain { self.lastDeviceVersion = v }
            }
        }
    }

    // MARK: - WiFi Status

    public func fetchWifiStatus() {
        guard connectionState == .connected, let device = activeDevice else { return }
        if transport == .ble {
            Task { await self.bleFetchWifi() }
            return
        }
        let ip = device.ip; let token = adminToken
        Task {
            if let ws: WifiStatus = await fetchDecoded(WifiStatus.self, ip: ip, path: "/api/wifi", token: token) {
                updateOnMain { self.lastWifiStatus = ws }
            }
        }
    }

    // MARK: - OTA Rollback

    public func otaRollback() async -> Bool {
        do {
            return try await postAction(path: "/api/ota/rollback", json: [:])
        } catch { return false }
    }

    // MARK: - Toast

    public func showToast(_ text: String, type: ToastType = .info) {
        updateOnMain {
            self.toastMessage = ToastMessage(text: text, type: type)
        }
        // Auto-dismiss after 3 seconds
        updateOnMain(after: 3_000_000_000) {
            self.toastMessage = nil
        }
    }
}

public struct CalibrationPointsResponse: Codable {
    public let ch: Int
    public let count: Int
    public let valid: Bool
    public let points: [CalibrationPoint]
}

/// RP2040 HAT calibration snapshot from GET /api/hat/calibration
/// (live cal-engine state + validation metrics for the selected rail).
public struct HatCalibration: Codable {
    public let hatPresent: Bool
    public let ok: Bool
    public let state: Int?
    public let progress: Int?
    public let railId: Int?
    public let lastError: Int?
    public let persistState: Int?
    public let stage: Int?
    public let point: Int?
    public let code: Int?
    public let measuredMv: Int?
    public let minMv: Int?
    public let maxMv: Int?
    public let maxGapMv: Int?
    public let maxErrorMv: Int?
    public let validationFlags: Int?
    // Stored cal points exported from the RP2040 (paginated server-side).
    public let pointsRail: Int?
    public let pointsCount: Int?
    public let pointsValid: Bool?
    public let points: [CalibrationPoint]?
}

