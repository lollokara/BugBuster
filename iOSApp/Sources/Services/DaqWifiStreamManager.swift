import Foundation
import Network
import Combine
import NetworkExtension
import UIKit

// =============================================================================
// DaqWifiStreamManager.swift — iOS client for the DAQ HAT power-analyzer
// binary stream over the P4 softAP (ESP-Hosted/C6 as WiFi radio), TCP port
// 5566. This is a raw byte-for-byte port of the wire protocol defined in
// Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h (v2) and framed by
// tcp_backend.c -- same frames the desktop app decodes over USB
// (DesktopApp/BugBuster/src-tauri/src/daq_proto.rs is the reference decoder).
//
// Unlike ConnectionManager/BLETransport, there is no HTTP/BLE control plane
// here -- the P4 softAP is a separate radio path the operator brings up via
// its serial CLI (`wifiap on <ssid> <password>`). The phone must first join
// that WiFi network in iOS Settings (no NEHotspotConfiguration entitlement
// in this app), then this manager opens a raw TCP socket to the AP gateway
// (192.168.4.1 by default -- ESP-IDF's standard softAP DHCP netif) and speaks
// the frame protocol directly over Network.framework's NWConnection, the
// first use of that API in this codebase (existing code uses NetService for
// discovery and URLSession for HTTP; there is no existing raw-socket path to
// mirror).
// =============================================================================

// MARK: - Wire protocol constants (usb_proto.h)

private enum DaqWire {
    static let magic0: UInt8 = 0xBB
    static let magic1: UInt8 = 0x50
    static let version: UInt8 = 2
    static let headerLen = 12
    static let crcLen = 2

    enum RecType: UInt8 {
        case waveI = 0x01
        case stats = 0x02
        case energy = 0x03
        case fft = 0x04
        case marker = 0x05
        case status = 0x06
        case waveV = 0x07
    }

    enum CmdType: UInt8 {
        case start = 0x80
        case stop = 0x81
        case setRate = 0x82
    }
}

// MARK: - Little-endian byte readers

private extension Data {
    func u8(_ off: Int) -> UInt8 { self[startIndex + off] }
    func u16(_ off: Int) -> UInt16 {
        UInt16(self[startIndex + off]) | (UInt16(self[startIndex + off + 1]) << 8)
    }
    func u32(_ off: Int) -> UInt32 {
        var v: UInt32 = 0
        for i in 0..<4 { v |= UInt32(self[startIndex + off + i]) << (8 * i) }
        return v
    }
    func u64(_ off: Int) -> UInt64 {
        var v: UInt64 = 0
        for i in 0..<8 { v |= UInt64(self[startIndex + off + i]) << (8 * i) }
        return v
    }
    func f32(_ off: Int) -> Float { Float(bitPattern: u32(off)) }
}

// MARK: - Decoded records

struct DaqWaveBlock {
    let startIndex: UInt64
    let timestampUs: UInt64
    let sampleRate: UInt32
    let values: [Float]      // current (WAVE_I) or voltage (WAVE_V)
    let meta: [UInt8]?       // WAVE_I only
    let isVoltage: Bool
}

struct DaqStatus {
    let sampleRate: UInt32
    let overflowCount: UInt32
    let range: UInt8
    let streaming: Bool
    let rangeLocked: Bool
    let sourceEnabled: Bool
    let vdutSet: Float
    let ilimitSet: Float
    let inVoltage: Float
    let inCurrent: Float
    let framesTx: UInt32?
    let bytesPerSec: UInt32?
    let fifoDropFrames: UInt32?
}

struct DaqMarker {
    let sampleIndex: UInt64
    let timestampUs: UInt64
    let channel: UInt8
    let edge: UInt8
    let kind: UInt8
}

enum DaqRecord {
    case wave(DaqWaveBlock)
    case status(DaqStatus)
    case marker(DaqMarker)
    case unknown(UInt8)
}

// MARK: - BLE-driven provisioning state

enum ProvisioningState: Equatable {
    case idle
    case requestingStart
    case waitingForCredentials
    case credentialsReady(ssid: String, password: String, host: String, port: UInt16)
    case failed(String)
}

// MARK: - Manager

@MainActor
final class DaqWifiStreamManager: NSObject, ObservableObject {
    static let shared = DaqWifiStreamManager()

    @Published var isConnected = false
    @Published var isStreaming = false
    @Published var lastError: String? = nil
    @Published var lastStatus: DaqStatus? = nil
    @Published var totalRecordsReceived = 0
    @Published var provisioningState: ProvisioningState = .idle

    /// Rolling (time_s, current_A, voltage_V) samples for a simple live chart,
    /// same "cap at N, append tail" contract as ScopeStreamManager.sampleBuffer.
    @Published var currentSamples: [(t: Double, value: Float)] = []
    @Published var voltageSamples: [(t: Double, value: Float)] = []
    /// Parallel to currentSamples: decoded source bits from each WAVE_I sample's meta byte
    /// (0 = FINE, 1 = COARSE, 2 = BLEND), used to per-segment tint the current trace.
    @Published var currentSampleSources: [UInt8] = []

    private var connection: NWConnection?
    private var rxBuffer = Data()
    private var firstTimestampUs: UInt64? = nil
    private let maxSamples = 2000

    // MARK: - Mock / Demo Mode
    //
    // Synthetic waveform for UI screenshotting/debugging without real DAQ HAT
    // hardware. Backfills ~60s of history at launch (so "full span" / "last 30s"
    // timebase presets have something to show) then keeps appending like a live
    // stream. Never touches the network.

    private var mockTimer: Timer?
    private var mockElapsed: Double = 0

    func connectMock() {
        isConnected = true
        isStreaming = true
        lastError = nil
        totalRecordsReceived = 0
        currentSamples = []
        voltageSamples = []
        currentSampleSources = []
        mockElapsed = 0

        // dt chosen so maxSamples (2000) exactly spans backfillSeconds — otherwise
        // the rolling "cap at N, drop tail" buffer would truncate "full span" below
        // what was just backfilled.
        let backfillSeconds = 60.0
        let dt = backfillSeconds / Double(maxSamples)
        var t = -backfillSeconds
        while t < 0 {
            appendMockSample(at: t)
            t += dt
        }
        mockTimer = Timer.scheduledTimer(withTimeInterval: dt, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.appendMockSample(at: self.mockElapsed)
            self.mockElapsed += dt
        }
    }

    func disconnectMock() {
        mockTimer?.invalidate()
        mockTimer = nil
        isConnected = false
        isStreaming = false
    }

    private func appendMockSample(at t: Double) {
        let voltage = 5.0 + 0.4 * sin(t * 2 * .pi * 0.5) + Double.random(in: -0.03...0.03)
        let current = 0.12 + 0.05 * sin(t * 2 * .pi * 1.3 + 0.6) + Double.random(in: -0.006...0.006)
        voltageSamples.append((t: t, value: Float(voltage)))
        currentSamples.append((t: t, value: Float(max(0, current))))
        currentSampleSources.append(0)
        if voltageSamples.count > maxSamples {
            voltageSamples.removeFirst(voltageSamples.count - maxSamples)
            currentSamples.removeFirst(currentSamples.count - maxSamples)
            currentSampleSources.removeFirst(currentSampleSources.count - maxSamples)
        }
        totalRecordsReceived += 1
    }

    // MARK: - BLE-driven bring-up

    private var pollTask: Task<Void, Never>?
    private var joinedHotspotSSID: String?

    /// Ask the mainboard (over BLE) to bring up the DAQ HAT's WiFi hotspot,
    /// then poll status until credentials are ready. Returns true if the
    /// start request itself was accepted (does not wait for readiness).
    func requestStreamStart(ble: BLETransport) async -> Bool {
        provisioningState = .requestingStart
        guard let data = await ble.apiRequest(path: "/api/daq/wifi_stream/start", body: nil) else {
            provisioningState = .failed("No response from device")
            return false
        }
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (obj["ok"] as? Bool) == true else {
            provisioningState = .failed("Start request rejected")
            return false
        }
        provisioningState = .waitingForCredentials
        beginPollingStatus(ble: ble)
        return true
    }

    /// Tell the mainboard to tear down the hotspot and stop any in-flight polling.
    func requestStreamStop(ble: BLETransport) async {
        pollTask?.cancel()
        pollTask = nil
        _ = await ble.apiRequest(path: "/api/daq/wifi_stream/stop", body: nil)
        provisioningState = .idle
    }

    private func beginPollingStatus(ble: BLETransport) {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            guard let self else { return }
            // 8s was too tight: the full chain is DDP round-trip to the C6 +
            // up to ~3s of P4-side wifi_ap_start() retries (waiting for the
            // C6's ESP-Hosted stack to come up) + ~1-1.5s softAP/DNS/TCP
            // bring-up + ~1s for the S3 to reassemble the 4 chunked info
            // frames at its own 250ms HAT-poll cadence, all before this BLE
            // poll even sees "ready" -- plus BLE's own per-request overhead
            // on top of that. Observed real-world timeouts at 8s with the
            // P4/S3 side otherwise succeeding cleanly. See .mex/patterns/
            // daq-hat-ios-wifi-streaming.md.
            let deadline = Date().addingTimeInterval(20.0)
            while !Task.isCancelled {
                if Date() >= deadline {
                    self.provisioningState = .failed("Timed out waiting for DAQ WiFi credentials")
                    return
                }
                if let data = await ble.apiRequest(path: "/api/daq/wifi_stream/status", body: nil),
                   let status = try? JSONDecoder().decode(DaqWifiStreamStatus.self, from: data) {
                    switch status.state {
                    case "ready":
                        guard let ssid = status.ssid, let password = status.password,
                              let host = status.host, let port = status.port else {
                            self.provisioningState = .failed("Malformed ready status")
                            return
                        }
                        self.provisioningState = .credentialsReady(
                            ssid: ssid, password: password, host: host, port: UInt16(port)
                        )
                        return
                    case "failed":
                        self.provisioningState = .failed("DAQ HAT reported hotspot failure")
                        return
                    default:
                        break // "idle" / "starting" -- keep polling
                    }
                }
                try? await Task.sleep(nanoseconds: 300_000_000)
            }
        }
    }

    /// End-to-end orchestration: BLE start -> poll status -> auto-join WiFi -> connect socket.
    func startFullStreamFlow(ble: BLETransport) async {
        guard await requestStreamStart(ble: ble) else { return }

        // Wait for provisioningState to settle into credentialsReady/failed
        // (beginPollingStatus runs concurrently and mutates it directly).
        while true {
            switch provisioningState {
            case .credentialsReady(let ssid, let password, let host, let port):
                let joined = await joinDaqHotspot(ssid: ssid, password: password)
                if joined {
                    connect(host: host, port: port)
                } else {
                    provisioningState = .failed("Could not join DAQ HAT WiFi hotspot")
                }
                return
            case .failed:
                return
            default:
                try? await Task.sleep(nanoseconds: 150_000_000)
            }
        }
    }

    // MARK: - WiFi auto-join

    private func joinDaqHotspot(ssid: String, password: String) async -> Bool {

        let configuration = NEHotspotConfiguration(ssid: ssid, passphrase: password, isWEP: false)
        configuration.joinOnce = true

        let result: Error? = await withCheckedContinuation { continuation in
            NEHotspotConfigurationManager.shared.apply(configuration) { error in
                continuation.resume(returning: error)
            }
        }

        if let error = result as NSError? {
            if error.domain == NEHotspotConfigurationErrorDomain,
               error.code == NEHotspotConfigurationError.alreadyAssociated.rawValue {
                joinedHotspotSSID = ssid
                return true
            }
            print("[DaqWifiStreamManager] NEHotspotConfiguration apply() failed: "
                  + "domain=\(error.domain) code=\(error.code) \(error.localizedDescription)")
            lastError = error.localizedDescription
            return false
        }

        joinedHotspotSSID = ssid
        return true
    }

    func connect(host: String, port: UInt16 = 5566) {
        disconnect()
        lastError = nil

        guard !host.isEmpty else {
            lastError = "Invalid host"
            return
        }
        let nwHost = NWEndpoint.Host(host)
        guard let nwPort = NWEndpoint.Port(rawValue: port) else {
            lastError = "Invalid port"
            return
        }

        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.requiredInterfaceType = .wifi
        let conn = NWConnection(host: nwHost, port: nwPort, using: params)
        connection = conn

        conn.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            Task { @MainActor in
                switch state {
                case .ready:
                    self.isConnected = true
                    self.lastError = nil
                    self.sendStart()
                    self.receiveLoop()
                case .failed(let err):
                    self.isConnected = false
                    self.isStreaming = false
                    self.lastError = err.localizedDescription
                case .cancelled:
                    self.isConnected = false
                    self.isStreaming = false
                default:
                    break
                }
            }
        }
        conn.start(queue: .main)
    }

    func disconnect() {
        if connection != nil {
            sendStop()
        }
        connection?.cancel()
        connection = nil
        rxBuffer.removeAll()
        firstTimestampUs = nil
        isConnected = false
        isStreaming = false

        if let ssid = joinedHotspotSSID {
            NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid)
            joinedHotspotSSID = nil
        }
    }

    // MARK: - Send control frames (real CRC required, type >= 0x80)

    private func sendControlFrame(type: DaqWire.CmdType, payload: Data = Data()) {
        guard let conn = connection else { return }
        var frame = Data()
        frame.append(DaqWire.magic0)
        frame.append(DaqWire.magic1)
        frame.append(DaqWire.version)
        frame.append(type.rawValue)
        frame.append(0) // flags
        frame.append(0) // reserved
        appendU32LE(&frame, 0) // seq (host doesn't need to track its own seq)
        appendU16LE(&frame, UInt16(payload.count))
        frame.append(payload)

        // CRC-16/CCITT-FALSE over [2, 12+payload_len) i.e. everything after magic.
        let crcRange = frame[frame.startIndex + 2 ..< frame.endIndex]
        let crc = Self.crc16(Array(crcRange), init: 0xFFFF)
        appendU16LE(&frame, crc)

        conn.send(content: frame, completion: .contentProcessed { _ in })
    }

    private func sendStart() {
        sendControlFrame(type: .start)
        isStreaming = true
    }

    private func sendStop() {
        sendControlFrame(type: .stop)
        isStreaming = false
    }

    /// Send USB_CMD_SET_RATE (0x82): usb_cmd_rate_t { u32 current_sps; u32 voltage_sps; u8 decimation; u8 pad[3]; }
    func sendSetRate(currentSps: UInt32, voltageSps: UInt32, decimation: UInt8 = 1) {
        var payload = Data()
        appendU32LE(&payload, currentSps)
        appendU32LE(&payload, voltageSps)
        payload.append(decimation)
        payload.append(0) // pad
        payload.append(0) // pad
        payload.append(0) // pad
        sendControlFrame(type: .setRate, payload: payload)
    }

    private static func crc16(_ data: [UInt8], init initVal: UInt16) -> UInt16 {
        var crc = initVal
        for b in data {
            crc ^= UInt16(b) << 8
            for _ in 0..<8 {
                if crc & 0x8000 != 0 {
                    crc = (crc << 1) ^ 0x1021
                } else {
                    crc <<= 1
                }
            }
        }
        return crc
    }

    private func appendU16LE(_ d: inout Data, _ v: UInt16) {
        d.append(UInt8(v & 0xFF))
        d.append(UInt8((v >> 8) & 0xFF))
    }

    private func appendU32LE(_ d: inout Data, _ v: UInt32) {
        for i in 0..<4 { d.append(UInt8((v >> (8 * i)) & 0xFF)) }
    }

    // MARK: - Receive loop + frame parsing

    private func receiveLoop() {
        guard let conn = connection else { return }
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            Task { @MainActor in
                if let data, !data.isEmpty {
                    self.rxBuffer.append(data)
                    self.drainFrames()
                }
                if let error {
                    self.lastError = error.localizedDescription
                    self.isConnected = false
                    self.isStreaming = false
                    return
                }
                if isComplete {
                    self.isConnected = false
                    self.isStreaming = false
                    return
                }
                self.receiveLoop()
            }
        }
    }

    private func drainFrames() {
        while true {
            guard rxBuffer.count >= DaqWire.headerLen else { return }

            // Resync on the 2-byte magic if the stream ever desyncs.
            if rxBuffer.u8(0) != DaqWire.magic0 || rxBuffer.u8(1) != DaqWire.magic1 {
                if let idx = rxBuffer.dropFirst().firstIndex(of: DaqWire.magic0) {
                    rxBuffer.removeSubrange(rxBuffer.startIndex..<idx)
                } else {
                    rxBuffer.removeAll()
                }
                continue
            }

            let payloadLen = Int(rxBuffer.u16(10))
            let totalLen = DaqWire.headerLen + payloadLen + DaqWire.crcLen
            guard rxBuffer.count >= totalLen else { return } // wait for more bytes

            let type = rxBuffer.u8(3)
            let payloadStart = rxBuffer.startIndex + DaqWire.headerLen
            let payload = rxBuffer.subdata(in: payloadStart..<(payloadStart + payloadLen))

            if let record = Self.decodePayload(type: type, payload: payload) {
                handle(record)
            }

            rxBuffer.removeSubrange(rxBuffer.startIndex..<(rxBuffer.startIndex + totalLen))
        }
    }

    private static func decodePayload(type: UInt8, payload: Data) -> DaqRecord? {
        guard let rec = DaqWire.RecType(rawValue: type) else { return .unknown(type) }
        switch rec {
        case .waveI, .waveV:
            guard payload.count >= 24 else { return nil }
            let startIndex = payload.u64(0)
            let timestampUs = payload.u64(8)
            let sampleRate = payload.u32(16)
            var count = Int(payload.u16(20))
            let isVoltage = rec == .waveV
            let bytesPerSample = isVoltage ? 4 : 5
            while count > 0 && 24 + count * bytesPerSample > payload.count { count -= 1 }

            var values = [Float]()
            values.reserveCapacity(count)
            for i in 0..<count { values.append(payload.f32(24 + i * 4)) }

            var meta: [UInt8]? = nil
            if !isVoltage {
                var m = [UInt8]()
                m.reserveCapacity(count)
                let metaBase = 24 + count * 4
                for i in 0..<count { m.append(payload.u8(metaBase + i)) }
                meta = m
            }
            return .wave(DaqWaveBlock(startIndex: startIndex, timestampUs: timestampUs,
                                      sampleRate: sampleRate, values: values, meta: meta,
                                      isVoltage: isVoltage))

        case .status:
            guard payload.count >= 20 else { return nil }
            let inVoltage = payload.count >= 28 ? payload.f32(20) : 0
            let inCurrent = payload.count >= 28 ? payload.f32(24) : 0
            let framesTx = payload.count >= 40 ? payload.u32(36) : nil
            let bytesPerSec = payload.count >= 44 ? payload.u32(40) : nil
            let fifoDrop = payload.count >= 48 ? payload.u32(44) : nil
            return .status(DaqStatus(
                sampleRate: payload.u32(0),
                overflowCount: payload.u32(4),
                range: payload.u8(8),
                streaming: payload.u8(9) != 0,
                rangeLocked: payload.u8(10) != 0,
                sourceEnabled: payload.u8(11) != 0,
                vdutSet: payload.f32(12),
                ilimitSet: payload.f32(16),
                inVoltage: inVoltage,
                inCurrent: inCurrent,
                framesTx: framesTx,
                bytesPerSec: bytesPerSec,
                fifoDropFrames: fifoDrop
            ))

        case .marker:
            guard payload.count >= 20 else { return nil }
            return .marker(DaqMarker(
                sampleIndex: payload.u64(0),
                timestampUs: payload.u64(8),
                channel: payload.u8(16),
                edge: payload.u8(17),
                kind: payload.u8(18)
            ))

        case .stats, .energy, .fft:
            // Not surfaced in the phone UI yet; parsed elsewhere if needed.
            return nil
        }
    }

    private func handle(_ record: DaqRecord) {
        totalRecordsReceived += 1
        switch record {
        case .wave(let block):
            if firstTimestampUs == nil { firstTimestampUs = block.timestampUs }
            let t0 = firstTimestampUs ?? block.timestampUs
            let dt = block.sampleRate > 0 ? 1.0 / Double(block.sampleRate) : 0
            let tBase = Double(block.timestampUs &- t0) / 1_000_000.0

            var appended: [(t: Double, value: Float)] = []
            appended.reserveCapacity(block.values.count)
            for (i, v) in block.values.enumerated() {
                appended.append((t: tBase + Double(i) * dt, value: v))
            }

            if block.isVoltage {
                voltageSamples.append(contentsOf: appended)
                if voltageSamples.count > maxSamples {
                    voltageSamples.removeFirst(voltageSamples.count - maxSamples)
                }
            } else {
                currentSamples.append(contentsOf: appended)
                if currentSamples.count > maxSamples {
                    currentSamples.removeFirst(currentSamples.count - maxSamples)
                }

                if let meta = block.meta {
                    let sources = meta.map { ($0 >> 2) & 0x03 }
                    currentSampleSources.append(contentsOf: sources)
                } else {
                    currentSampleSources.append(contentsOf: Array(repeating: UInt8(0), count: appended.count))
                }
                if currentSampleSources.count > maxSamples {
                    currentSampleSources.removeFirst(currentSampleSources.count - maxSamples)
                }
            }

        case .status(let status):
            lastStatus = status
            isStreaming = status.streaming

        case .marker:
            break // no marker overlay on the phone chart yet

        case .unknown:
            break
        }
    }
}
