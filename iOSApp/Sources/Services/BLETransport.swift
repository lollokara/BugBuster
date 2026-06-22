// =============================================================================
// BLETransport.swift — CoreBluetooth central for the BugBuster control plane.
//
// Mirrors the WiFi/HTTP control plane over Bluetooth Low Energy, talking to the
// NimBLE peripheral implemented in Firmware/ESP32/src/net/ble_service.cpp.
//
// GATT layout (service + characteristics), base UUID embeds ASCII "BugBuster":
//   Service 42756700-7573-7465-7200-757374657200
//     0x01 Info    (read)            : {model, mac, fw, proto}  — pre-auth
//     0x02 Auth    (write)           : admin token string; bad token => ATT auth error
//     0x10 WiFi    (write + notify)  : {ssid,password} -> notify {ok,ip}
//     0x11 Supply  (write)           : {target:"idac"|"rail", ...}
//     0x12 Sensor  (read)            : compact {t,ch[],pd,rail[]}  — auth-gated
//     0x20 APIReq  (write)           : {id,path,body}
//     0x21 APIResp (notify)          : [reqId][seq][flags bit0=last][json...]
//
// The API tunnel (0x20/0x21) carries the same path/body request surface as the
// HTTP API (a small-data subset). Responses are reassembled from MTU-sized
// notification frames keyed by request id.
//
// All CoreBluetooth state is owned by `bleQueue`; async wrappers stash a
// continuation and the delegate callbacks (which run on `bleQueue`) resume it.
// The class is `@unchecked Sendable`: every mutable field is touched only on
// `bleQueue` (or hopped to the main queue for @Published), so capturing `self`
// in the dispatch closures is safe.
// =============================================================================

import Foundation
@preconcurrency import CoreBluetooth
import Combine

/// A BugBuster device discovered over BLE. `id` is the CoreBluetooth peripheral
/// identifier (stable per-device on this iOS install).
public struct BLEDevice: Identifiable, Equatable {
    public let id: UUID
    public let name: String
    public var rssi: Int
}

public final class BLETransport: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate, @unchecked Sendable {
    // MARK: GATT UUIDs
    // NimBLE's BLE_UUID128_INIT stores the 16 bytes little-endian, so the
    // firmware's BB_UUID128(disc) macro actually advertises
    // 42756742-7573-7465-7200-0000000000<disc> (NOT the value its source
    // comment claims). These strings match what the device really exposes.
    static let svcUUID     = CBUUID(string: "42756742-7573-7465-7200-000000000000")
    static let chrInfo     = CBUUID(string: "42756742-7573-7465-7200-000000000001")
    static let chrAuth     = CBUUID(string: "42756742-7573-7465-7200-000000000002")
    static let chrWifi     = CBUUID(string: "42756742-7573-7465-7200-000000000010")
    static let chrSupply   = CBUUID(string: "42756742-7573-7465-7200-000000000011")
    static let chrSensor   = CBUUID(string: "42756742-7573-7465-7200-000000000012")
    static let chrApiReq   = CBUUID(string: "42756742-7573-7465-7200-000000000020")
    static let chrApiResp  = CBUUID(string: "42756742-7573-7465-7200-000000000021")

    // MARK: Published state (observed on the main actor by ConnectionManager)
    @Published public private(set) var poweredOn = false
    @Published public private(set) var devices: [BLEDevice] = []
    @Published public private(set) var isScanning = false

    /// Invoked (on the main queue) when the active peripheral drops.
    public var onDisconnect: (() -> Void)?

    // MARK: CoreBluetooth plumbing
    private let bleQueue = DispatchQueue(label: "com.lorenzo.bugbuster.ble")
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var chars: [CBUUID: CBCharacteristic] = [:]
    private var discovered: [UUID: (peripheral: CBPeripheral, device: BLEDevice)] = [:]

    // One outstanding GATT operation of each kind at a time (single connection).
    private let opGate = AsyncSemaphore(value: 1)

    // Pending continuations, all touched only on `bleQueue`.
    private var connectCont: CheckedContinuation<Bool, Never>?
    private var readConts: [CBUUID: CheckedContinuation<Data?, Never>] = [:]
    private var writeConts: [CBUUID: CheckedContinuation<Bool, Never>] = [:]

    // API tunnel reassembly (keyed by request id).
    private var reqIdCounter: UInt8 = 0
    private var tunnelBuffer = Data()
    private var tunnelExpectReq: UInt8 = 0
    private var tunnelNextSeq: UInt8 = 0
    private var tunnelCont: CheckedContinuation<Data?, Never>?

    // WiFi-provision notify is delivered asynchronously after the write ACKs.
    private var wifiResultCont: CheckedContinuation<Data?, Never>?

    public override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: bleQueue,
                                   options: [CBCentralManagerOptionShowPowerAlertKey: true])
    }

    // MARK: - Scanning ------------------------------------------------------

    public func startScan() {
        bleQueue.async {
            guard self.central.state == .poweredOn else {
                NSLog("[BLE] startScan ignored — central state=%ld (not poweredOn)", self.central.state.rawValue)
                return
            }
            self.discovered.removeAll()
            DispatchQueue.main.async {
                self.devices = []
                self.isScanning = true
            }
            // Scan without a service filter and match in didDiscover. The ESP32
            // advertises its 128-bit service UUID only in the scan response, which
            // iOS filtered scans can miss; the device name ("BugBuster-XXYYZZ") is
            // in the primary advertisement, so name-prefix matching is reliable.
            self.central.scanForPeripherals(withServices: nil,
                                            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
            NSLog("[BLE] scan started (unfiltered, matching BugBuster name/UUID)")
        }
    }

    public func stopScan() {
        bleQueue.async {
            self.central.stopScan()
            DispatchQueue.main.async { self.isScanning = false }
        }
    }

    // MARK: - Connection ----------------------------------------------------

    /// Connect to a discovered device and discover its GATT layout. Resolves
    /// `true` once the service, characteristics and notifications are ready.
    public func connect(id: UUID, timeout: TimeInterval = 20.0) async -> Bool {
        await withCheckedContinuation { (cont: CheckedContinuation<Bool, Never>) in
            bleQueue.async {
                guard let entry = self.discovered[id] else {
                    NSLog("[BLE] connect: peripheral %@ not in discovered set", id.uuidString)
                    cont.resume(returning: false); return
                }
                if self.connectCont != nil { self.connectCont?.resume(returning: false); self.connectCont = nil }
                self.connectCont = cont
                self.peripheral = entry.peripheral
                entry.peripheral.delegate = self
                self.chars.removeAll()
                self.central.stopScan()
                DispatchQueue.main.async { self.isScanning = false }
                NSLog("[BLE] connecting to %@ (%@)", entry.device.name, id.uuidString)
                self.central.connect(entry.peripheral, options: nil)
                self.bleQueue.asyncAfter(deadline: .now() + timeout) {
                    if let c = self.connectCont {
                        NSLog("[BLE] connect TIMED OUT after %.0fs (peripheral state=%ld)",
                              timeout, self.peripheral?.state.rawValue ?? -1)
                        self.connectCont = nil
                        c.resume(returning: false)
                    }
                }
            }
        }
    }

    public func disconnect() {
        bleQueue.async {
            if let p = self.peripheral { self.central.cancelPeripheralConnection(p) }
            self.peripheral = nil
            self.chars.removeAll()
        }
    }

    public var isConnected: Bool {
        peripheral?.state == .connected
    }

    // MARK: - High-level operations ----------------------------------------

    /// Read the pre-auth Info characteristic (model/mac/fw/proto JSON).
    public func readInfo() async -> Data? {
        await readValue(Self.chrInfo)
    }

    /// Write the admin token to the Auth characteristic. A write error from the
    /// peripheral (ATT insufficient-authentication) means the token was wrong.
    public func authenticate(token: String) async -> Bool {
        guard let data = token.data(using: .utf8) else { return false }
        return await writeValue(Self.chrAuth, data: data, withResponse: true)
    }

    /// Read the auth-gated compact sensor snapshot.
    public func readSensor() async -> Data? {
        await readValue(Self.chrSensor)
    }

    /// Send a request over the API tunnel and return the reassembled JSON.
    /// `body` is omitted from the frame when nil/empty.
    public func apiRequest(path: String, body: [String: Any]? = nil, timeout: TimeInterval = 6.0) async -> Data? {
        await opGate.wait()
        defer { Task { await opGate.signal() } }

        var payload: [String: Any] = ["path": path]
        if let body = body, !body.isEmpty { payload["body"] = body }

        return await withCheckedContinuation { (cont: CheckedContinuation<Data?, Never>) in
            bleQueue.async {
                guard let reqChar = self.chars[Self.chrApiReq], let p = self.peripheral else {
                    cont.resume(returning: nil); return
                }
                self.reqIdCounter = self.reqIdCounter &+ 1
                if self.reqIdCounter == 0 { self.reqIdCounter = 1 }
                let reqId = self.reqIdCounter
                payload["id"] = Int(reqId)

                guard let json = try? JSONSerialization.data(withJSONObject: payload),
                      json.count <= 240 else {  // firmware APIReq buffer is 256
                    cont.resume(returning: nil); return
                }

                self.tunnelBuffer.removeAll(keepingCapacity: true)
                self.tunnelExpectReq = reqId
                self.tunnelNextSeq = 0
                self.tunnelCont = cont

                p.writeValue(json, for: reqChar, type: .withResponse)
                self.bleQueue.asyncAfter(deadline: .now() + timeout) {
                    if let c = self.tunnelCont { self.tunnelCont = nil; c.resume(returning: nil) }
                }
            }
        }
    }

    /// Write the Supply characteristic (idac / hat-rail control). Returns true on ACK.
    public func writeSupply(_ json: [String: Any]) async -> Bool {
        guard let data = try? JSONSerialization.data(withJSONObject: json) else { return false }
        return await writeValue(Self.chrSupply, data: data, withResponse: true)
    }

    /// Provision WiFi: write {ssid,password} and await the {ok,ip} notify result.
    public func provisionWifi(ssid: String, password: String, timeout: TimeInterval = 25.0) async -> Data? {
        let body: [String: Any] = ["ssid": ssid, "password": password]
        guard let data = try? JSONSerialization.data(withJSONObject: body) else { return nil }

        // Arm the notify-result waiter, then perform the write.
        async let result: Data? = withCheckedContinuation { (cont: CheckedContinuation<Data?, Never>) in
            bleQueue.async {
                if let c = self.wifiResultCont { self.wifiResultCont = nil; c.resume(returning: nil) }
                self.wifiResultCont = cont
                self.bleQueue.asyncAfter(deadline: .now() + timeout) {
                    if let c = self.wifiResultCont { self.wifiResultCont = nil; c.resume(returning: nil) }
                }
            }
        }
        let ok = await writeValue(Self.chrWifi, data: data, withResponse: true)
        if !ok {
            bleQueue.async {
                if let c = self.wifiResultCont { self.wifiResultCont = nil; c.resume(returning: nil) }
            }
        }
        return await result
    }

    // MARK: - Generic GATT read/write --------------------------------------

    private func readValue(_ uuid: CBUUID, timeout: TimeInterval = 5.0) async -> Data? {
        await opGate.wait()
        defer { Task { await opGate.signal() } }
        return await withCheckedContinuation { (cont: CheckedContinuation<Data?, Never>) in
            bleQueue.async {
                guard let p = self.peripheral, let c = self.chars[uuid] else { cont.resume(returning: nil); return }
                if let prev = self.readConts[uuid] { self.readConts[uuid] = nil; prev.resume(returning: nil) }
                self.readConts[uuid] = cont
                p.readValue(for: c)
                self.bleQueue.asyncAfter(deadline: .now() + timeout) {
                    if let pending = self.readConts[uuid] { self.readConts[uuid] = nil; pending.resume(returning: nil) }
                }
            }
        }
    }

    private func writeValue(_ uuid: CBUUID, data: Data, withResponse: Bool, timeout: TimeInterval = 5.0) async -> Bool {
        await opGate.wait()
        defer { Task { await opGate.signal() } }
        return await withCheckedContinuation { (cont: CheckedContinuation<Bool, Never>) in
            bleQueue.async {
                guard let p = self.peripheral, let c = self.chars[uuid] else { cont.resume(returning: false); return }
                if !withResponse {
                    p.writeValue(data, for: c, type: .withoutResponse)
                    cont.resume(returning: true)
                    return
                }
                if let prev = self.writeConts[uuid] { self.writeConts[uuid] = nil; prev.resume(returning: false) }
                self.writeConts[uuid] = cont
                p.writeValue(data, for: c, type: .withResponse)
                self.bleQueue.asyncAfter(deadline: .now() + timeout) {
                    if let pending = self.writeConts[uuid] { self.writeConts[uuid] = nil; pending.resume(returning: false) }
                }
            }
        }
    }

    // MARK: - CBCentralManagerDelegate -------------------------------------

    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let on = central.state == .poweredOn
        NSLog("[BLE] central state=%ld poweredOn=%@ authorization=%ld",
              central.state.rawValue, on ? "YES" : "NO", CBCentralManager.authorization.rawValue)
        DispatchQueue.main.async { self.poweredOn = on }
    }

    public func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let advName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let uuids = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        // Only surface BugBuster peripherals (we scan unfiltered, so ignore others).
        let isBugBuster = (advName?.hasPrefix("BugBuster") ?? false)
            || (peripheral.name?.hasPrefix("BugBuster") ?? false)
            || uuids.contains(Self.svcUUID)
        guard isBugBuster else { return }

        let name = advName ?? peripheral.name ?? "BugBuster"
        NSLog("[BLE] discovered %@ rssi=%@ uuids=%@", name, RSSI, uuids.map { $0.uuidString })
        let dev = BLEDevice(id: peripheral.identifier, name: name, rssi: RSSI.intValue)
        discovered[peripheral.identifier] = (peripheral, dev)
        let snapshot = discovered.values.map { $0.device }.sorted { $0.rssi > $1.rssi }
        DispatchQueue.main.async { self.devices = snapshot }
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        NSLog("[BLE] didConnect — discovering services")
        peripheral.discoverServices([Self.svcUUID])
    }

    public func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        NSLog("[BLE] didFailToConnect: %@", error?.localizedDescription ?? "unknown")
        if let c = connectCont { connectCont = nil; c.resume(returning: false) }
    }

    public func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        NSLog("[BLE] didDisconnect: %@", error?.localizedDescription ?? "clean")
        // Fail any in-flight operations.
        if let c = connectCont { connectCont = nil; c.resume(returning: false) }
        if let c = tunnelCont { tunnelCont = nil; c.resume(returning: nil) }
        if let c = wifiResultCont { wifiResultCont = nil; c.resume(returning: nil) }
        for (_, c) in readConts { c.resume(returning: nil) }
        readConts.removeAll()
        for (_, c) in writeConts { c.resume(returning: false) }
        writeConts.removeAll()
        chars.removeAll()
        self.peripheral = nil
        DispatchQueue.main.async { self.onDisconnect?() }
    }

    // MARK: - CBPeripheralDelegate -----------------------------------------

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error = error {
            NSLog("[BLE] didDiscoverServices error: %@", error.localizedDescription)
        }
        guard error == nil, let svc = peripheral.services?.first(where: { $0.uuid == Self.svcUUID }) else {
            NSLog("[BLE] BugBuster service not found (services=%@)",
                  (peripheral.services?.map { $0.uuid.uuidString }) ?? [])
            if let c = connectCont { connectCont = nil; c.resume(returning: false) }
            return
        }
        NSLog("[BLE] service found — discovering characteristics")
        peripheral.discoverCharacteristics(nil, for: svc)
    }

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error = error {
            NSLog("[BLE] didDiscoverCharacteristics error: %@", error.localizedDescription)
        }
        guard error == nil, let list = service.characteristics else {
            if let c = connectCont { connectCont = nil; c.resume(returning: false) }
            return
        }
        for c in list { chars[c.uuid] = c }
        // Subscribe to the notify-bearing characteristics.
        if let resp = chars[Self.chrApiResp] { peripheral.setNotifyValue(true, for: resp) }
        if let wifi = chars[Self.chrWifi] { peripheral.setNotifyValue(true, for: wifi) }

        let ready = chars[Self.chrInfo] != nil && chars[Self.chrAuth] != nil && chars[Self.chrApiReq] != nil
        NSLog("[BLE] characteristics discovered count=%ld ready=%@ (info=%@ auth=%@ apiReq=%@)",
              list.count, ready ? "YES" : "NO",
              chars[Self.chrInfo] != nil ? "Y" : "N",
              chars[Self.chrAuth] != nil ? "Y" : "N",
              chars[Self.chrApiReq] != nil ? "Y" : "N")
        if let c = connectCont { connectCont = nil; c.resume(returning: ready) }
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        let uuid = characteristic.uuid
        let value = characteristic.value

        // Plain read completion.
        if let c = readConts[uuid] {
            readConts[uuid] = nil
            c.resume(returning: error == nil ? value : nil)
            return
        }

        // WiFi provision result notify.
        if uuid == Self.chrWifi, let c = wifiResultCont {
            wifiResultCont = nil
            c.resume(returning: error == nil ? value : nil)
            return
        }

        // API tunnel response frame: [reqId][seq][flags][json...]
        if uuid == Self.chrApiResp {
            handleTunnelFrame(value)
        }
    }

    public func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        let uuid = characteristic.uuid
        if let c = writeConts[uuid] {
            writeConts[uuid] = nil
            c.resume(returning: error == nil)
        }
    }

    // MARK: - Tunnel reassembly --------------------------------------------

    private func handleTunnelFrame(_ value: Data?) {
        guard let frame = value, frame.count >= 3, let cont = tunnelCont else { return }
        let reqId = frame[frame.startIndex]
        let seq   = frame[frame.startIndex + 1]
        let flags = frame[frame.startIndex + 2]
        guard reqId == tunnelExpectReq else { return }     // stale / mismatched
        guard seq == tunnelNextSeq else {                  // dropped/out-of-order
            tunnelCont = nil
            cont.resume(returning: nil)
            return
        }
        tunnelNextSeq = tunnelNextSeq &+ 1
        if frame.count > 3 {
            tunnelBuffer.append(frame.subdata(in: (frame.startIndex + 3)..<frame.endIndex))
        }
        if (flags & 0x01) != 0 {
            let result = tunnelBuffer
            tunnelCont = nil
            cont.resume(returning: result)
        }
    }
}

// =============================================================================
// AsyncSemaphore — minimal FIFO async semaphore used to serialise GATT ops.
// =============================================================================
actor AsyncSemaphore {
    private var value: Int
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init(value: Int) { self.value = value }

    func wait() async {
        if value > 0 {
            value -= 1
        } else {
            await withCheckedContinuation { waiters.append($0) }
        }
    }

    func signal() {
        if waiters.isEmpty {
            value += 1
        } else {
            waiters.removeFirst().resume()
        }
    }
}
