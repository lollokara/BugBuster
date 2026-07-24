import Foundation
import Network
import Combine
import NetworkExtension
import UIKit

// =============================================================================
// DaqWifiStreamManager.swift — iOS client for the DAQ HAT power-analyzer
// binary stream over the P4 softAP (ESP-Hosted/C6 as WiFi radio), TCP port
// 5566. Wire protocol: Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h (v2),
// framed by tcp_backend.c — same frames the desktop app decodes over USB
// (DesktopApp/BugBuster/src-tauri/src/daq_proto.rs is the reference decoder).
//
// Architecture: the manager itself is a @MainActor ObservableObject exposing
// only low-rate UI state (connection flags, provisioning progress, last
// STATUS). All high-rate work — the NWConnection, frame parsing, and sample
// storage — lives in DaqStreamEngine, confined to one serial background
// queue (DaqStreamEngine.queue). Nothing sample-rate-proportional ever runs
// on the main thread; ScopeRenderModel ticks on the same queue and publishes
// small ready-to-draw frames to SwiftUI at display rate. This matters beyond
// smoothness: when the main thread stalls, the phone's TCP receive window
// closes, the P4's socket backpressures, and (before the firmware's
// send-all fix) frames were cut mid-write — desyncing the stream into
// garbage samples. Keeping ingest off-main breaks that feedback loop.
// =============================================================================

// MARK: - Wire protocol constants (usb_proto.h)

enum DaqWire {
    static let magic0: UInt8 = 0xBB
    static let magic1: UInt8 = 0x50
    static let version: UInt8 = 2
    static let headerLen = 12
    static let crcLen = 2
    /// USB_MAX_PAYLOAD in usb_proto.h — no legitimate frame exceeds this.
    static let maxPayload = 16384

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

extension Data {
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

// MARK: - Sample storage (SoA, queue-confined)

/// Structure-of-arrays sample buffer for one trace. Confined to
/// DaqStreamEngine.queue — never touch from any other thread.
struct DaqChannelBuffer {
    var t: [Double] = []
    var v: [Float] = []
    /// Per-sample source bits (FINE/COARSE/BLEND) — populated for the current
    /// trace only; empty for voltage.
    var src: [UInt8] = []

    var count: Int { t.count }

    mutating func removeAll() {
        t.removeAll(keepingCapacity: true)
        v.removeAll(keepingCapacity: true)
        src.removeAll(keepingCapacity: true)
    }

    /// Halve resolution by keeping (min, max) per 4-sample bucket, preserving
    /// temporal order. Bounded memory with ever-growing time coverage: old
    /// data gets coarser but is never dropped by time, so the "Full"
    /// timebase keeps covering the whole session.
    mutating func envelopeHalve() {
        let n = t.count
        guard n >= 4 else { return }
        let hasSrc = !src.isEmpty
        var outT = [Double](); outT.reserveCapacity(n / 2 + 2)
        var outV = [Float](); outV.reserveCapacity(n / 2 + 2)
        var outS = [UInt8](); if hasSrc { outS.reserveCapacity(n / 2 + 2) }
        var i = 0
        while i < n {
            let end = Swift.min(i + 4, n)
            if end - i < 2 {
                for idx in i..<end {
                    outT.append(t[idx]); outV.append(v[idx])
                    if hasSrc { outS.append(idx < src.count ? src[idx] : 0) }
                }
            } else {
                var minIdx = i, maxIdx = i
                for idx in i..<end {
                    if v[idx] < v[minIdx] { minIdx = idx }
                    if v[idx] > v[maxIdx] { maxIdx = idx }
                }
                let first = Swift.min(minIdx, maxIdx)
                let second = Swift.max(minIdx, maxIdx)
                outT.append(t[first]); outV.append(v[first])
                if hasSrc { outS.append(first < src.count ? src[first] : 0) }
                if second != first {
                    outT.append(t[second]); outV.append(v[second])
                    if hasSrc { outS.append(second < src.count ? src[second] : 0) }
                }
            }
            i = end
        }
        t = outT; v = outV; src = hasSrc ? outS : []
    }
}

// MARK: - Stream engine (background pipeline)

/// Owns the socket, the frame parser, and the sample buffers. All mutable
/// state is confined to `DaqStreamEngine.queue`; the NWConnection delivers
/// its callbacks there, and ScopeRenderModel reads the buffers from a timer
/// on the same queue, so no locking is needed for the hot path.
final class DaqStreamEngine: @unchecked Sendable {
    /// The single serial pipeline queue: socket callbacks, decoding, buffer
    /// mutation, and render-model ticks all run here.
    static let queue = DispatchQueue(label: "com.bugbuster.daq.pipeline", qos: .userInitiated)

    // -- Queue-confined state ------------------------------------------------
    private(set) var voltage = DaqChannelBuffer()
    private(set) var current = DaqChannelBuffer()
    private(set) var recordCount = 0
    private var rxBuffer = Data()
    private var firstTimestampUs: UInt64?
    /// Soft cap before envelope-halving kicks in. At the DAQ HAT's lowest
    /// real streaming rate (1 kSps) this is a full minute of raw-resolution
    /// history before the first halving.
    private let maxSamples = 60_000

    private var connection: NWConnection?

    // -- Callbacks (always invoked on the main queue) ------------------------
    var onConnectionState: (@Sendable (NWConnection.State) -> Void)?
    var onStatus: (@Sendable (DaqStatus) -> Void)?

    // MARK: Socket lifecycle (callable from any thread; NWConnection is
    // internally thread-safe, buffer resets hop onto the pipeline queue)

    func connect(host: String, port: UInt16) {
        cancelConnection()
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.requiredInterfaceType = .wifi
        let conn = NWConnection(host: NWEndpoint.Host(host),
                                port: NWEndpoint.Port(rawValue: port)!,
                                using: params)
        connection = conn
        conn.stateUpdateHandler = { [weak self] state in
            // Runs on Self.queue (conn.start below).
            guard let self else { return }
            if case .ready = state {
                self.sendControlFrame(type: .start)
                self.receiveLoop(conn)
            }
            let cb = self.onConnectionState
            DispatchQueue.main.async { cb?(state) }
        }
        conn.start(queue: Self.queue)
    }

    func cancelConnection() {
        connection?.cancel()
        connection = nil
        Self.queue.async { [self] in
            rxBuffer.removeAll(keepingCapacity: false)
            firstTimestampUs = nil
        }
    }

    var hasConnection: Bool { connection != nil }

    func resetBuffers() {
        Self.queue.async { [self] in
            voltage.removeAll()
            current.removeAll()
            recordCount = 0
            firstTimestampUs = nil
        }
    }

    // MARK: Control frames (real CRC required, type >= 0x80)

    func sendControlFrame(type: DaqWire.CmdType, payload: Data = Data()) {
        guard let conn = connection else { return }
        var frame = Data()
        frame.append(DaqWire.magic0)
        frame.append(DaqWire.magic1)
        frame.append(DaqWire.version)
        frame.append(type.rawValue)
        frame.append(0) // flags
        frame.append(0) // reserved
        Self.appendU32LE(&frame, 0) // seq (host doesn't need to track its own seq)
        Self.appendU16LE(&frame, UInt16(payload.count))
        frame.append(payload)

        // CRC-16/CCITT-FALSE over [2, 12+payload_len) i.e. everything after magic.
        let crcRange = frame[frame.startIndex + 2 ..< frame.endIndex]
        let crc = Self.crc16(Array(crcRange), init: 0xFFFF)
        Self.appendU16LE(&frame, crc)

        conn.send(content: frame, completion: .contentProcessed { _ in })
    }

    // MARK: Mock ingestion (same buffers, same queue)

    func appendMock(t: Double, voltageV: Float, currentA: Float) {
        Self.queue.async { [self] in
            voltage.t.append(t); voltage.v.append(voltageV)
            current.t.append(t); current.v.append(currentA)
            current.src.append(0)
            recordCount += 1
            if voltage.count > maxSamples { voltage.envelopeHalve() }
            if current.count > maxSamples { current.envelopeHalve() }
        }
    }

    // MARK: Receive + frame parsing (pipeline queue only)

    private func receiveLoop(_ conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, error in
            // Runs on Self.queue.
            guard let self, self.connection === conn else { return }
            if let data, !data.isEmpty {
                self.rxBuffer.append(data)
                self.drainFrames()
            }
            if error != nil || isComplete {
                // State handler / manager surface the error; just stop reading.
                return
            }
            self.receiveLoop(conn)
        }
    }

    private static let knownDataTypes: Set<UInt8> = [
        DaqWire.RecType.waveI.rawValue, DaqWire.RecType.stats.rawValue,
        DaqWire.RecType.energy.rawValue, DaqWire.RecType.fft.rawValue,
        DaqWire.RecType.marker.rawValue, DaqWire.RecType.status.rawValue,
        DaqWire.RecType.waveV.rawValue,
    ]

    /// A resync candidate is only accepted as a frame header if version, type
    /// AND length are all plausible. Magic alone is not enough: data frames
    /// carry an unchecked 0x0000 CRC, and the 2-byte magic occurs freely
    /// inside float payloads — trusting it decodes payload garbage as wild
    /// full-scale samples and lets a bogus length swallow up to 64 KB of
    /// good data.
    private func headerLooksValid() -> Bool {
        rxBuffer.u8(0) == DaqWire.magic0
            && rxBuffer.u8(1) == DaqWire.magic1
            && rxBuffer.u8(2) == DaqWire.version
            && Self.knownDataTypes.contains(rxBuffer.u8(3))
            && Int(rxBuffer.u16(10)) <= DaqWire.maxPayload
    }

    private func drainFrames() {
        while rxBuffer.count >= DaqWire.headerLen {
            guard headerLooksValid() else {
                // Drop one byte, then jump to the next magic candidate.
                if let idx = rxBuffer.dropFirst().firstIndex(of: DaqWire.magic0) {
                    rxBuffer.removeSubrange(rxBuffer.startIndex..<idx)
                } else {
                    rxBuffer.removeAll(keepingCapacity: true)
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

    static func decodePayload(type: UInt8, payload: Data) -> DaqRecord? {
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
        recordCount += 1
        switch record {
        case .wave(let block):
            if firstTimestampUs == nil { firstTimestampUs = block.timestampUs }
            let t0 = firstTimestampUs ?? block.timestampUs
            let dt = block.sampleRate > 0 ? 1.0 / Double(block.sampleRate) : 0
            let tBase = Double(block.timestampUs &- t0) / 1_000_000.0

            if block.isVoltage {
                voltage.t.reserveCapacity(voltage.count + block.values.count)
                for (i, v) in block.values.enumerated() {
                    voltage.t.append(tBase + Double(i) * dt)
                    voltage.v.append(v)
                }
                if voltage.count > maxSamples { voltage.envelopeHalve() }
            } else {
                current.t.reserveCapacity(current.count + block.values.count)
                for (i, v) in block.values.enumerated() {
                    current.t.append(tBase + Double(i) * dt)
                    current.v.append(v)
                    // Source bits live at meta[1:0]=range, [3:2]=source.
                    let m: UInt8 = block.meta.map { i < $0.count ? $0[i] : 0 } ?? 0
                    current.src.append((m >> 2) & 0x03)
                }
                if current.count > maxSamples { current.envelopeHalve() }
            }

        case .status(let status):
            let cb = onStatus
            DispatchQueue.main.async { cb?(status) }

        case .marker:
            break // no marker overlay on the phone chart yet

        case .unknown:
            break
        }
    }

    // MARK: CRC / LE helpers

    static func crc16(_ data: [UInt8], init initVal: UInt16) -> UInt16 {
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

    static func appendU16LE(_ d: inout Data, _ v: UInt16) {
        d.append(UInt8(v & 0xFF))
        d.append(UInt8((v >> 8) & 0xFF))
    }

    static func appendU32LE(_ d: inout Data, _ v: UInt32) {
        for i in 0..<4 { d.append(UInt8((v >> (8 * i)) & 0xFF)) }
    }
}

// MARK: - BLE-driven provisioning state

/// Mirrors the P4's `daq_wifi_stage_t` (`HAT_WIFI_STAGE_*`) -- real bring-up
/// progress reported over BLE, not a synthetic timer.
enum ProvisioningStage: String, Equatable {
    case requested
    case ap
    case dns
    case tcp

    var label: String {
        switch self {
        case .requested: return "Requesting hotspot from device…"
        case .ap:        return "Starting hotspot…"
        case .dns:       return "Configuring network…"
        case .tcp:       return "Starting data stream…"
        }
    }

    /// Rough fraction for a determinate progress indicator; the real
    /// bring-up isn't evenly timed across stages, so this is only meant to
    /// give a sense of forward motion, not a precise ETA.
    var fraction: Double {
        switch self {
        case .requested: return 0.15
        case .ap:        return 0.45
        case .dns:       return 0.75
        case .tcp:       return 0.9
        }
    }
}

enum ProvisioningState: Equatable {
    case idle
    case requestingStart
    case waitingForCredentials(stage: ProvisioningStage)
    case credentialsReady(ssid: String, password: String, host: String, port: UInt16)
    case joiningWifi
    case failed(String)
}

// MARK: - Manager (main-actor UI surface)

@MainActor
final class DaqWifiStreamManager: NSObject, ObservableObject {
    static let shared = DaqWifiStreamManager()

    @Published var isConnected = false
    @Published var isStreaming = false
    @Published var lastError: String? = nil
    @Published var lastStatus: DaqStatus? = nil
    @Published var provisioningState: ProvisioningState = .idle

    /// Background stream pipeline; ScopeRenderModel reads its buffers.
    let engine = DaqStreamEngine()

    override init() {
        super.init()
        engine.onConnectionState = { [weak self] state in
            Task { @MainActor in
            guard let self else { return }
            switch state {
            case .ready:
                self.isConnected = true
                self.isStreaming = true
                self.lastError = nil
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
        engine.onStatus = { [weak self] status in
            Task { @MainActor in
                guard let self else { return }
                self.lastStatus = status
                self.isStreaming = status.streaming
            }
        }
    }

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
        engine.resetBuffers()
        mockElapsed = 0

        let backfillSeconds = 60.0
        let mockSampleCount = 2000.0
        let dt = backfillSeconds / mockSampleCount
        var t = -backfillSeconds
        while t < 0 {
            appendMockSample(at: t)
            t += dt
        }
        mockTimer = Timer.scheduledTimer(withTimeInterval: dt, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.appendMockSample(at: self.mockElapsed)
                self.mockElapsed += dt
            }
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
        engine.appendMock(t: t, voltageV: Float(voltage), currentA: Float(max(0, current)))
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
        provisioningState = .waitingForCredentials(stage: .requested)
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
                    case "starting":
                        let stage = status.stage.flatMap(ProvisioningStage.init(rawValue:)) ?? .requested
                        self.provisioningState = .waitingForCredentials(stage: stage)
                    default:
                        break // "idle" -- keep polling
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
                provisioningState = .joiningWifi
                let joined = await joinDaqHotspot(ssid: ssid, password: password)
                if joined {
                    connect(host: host, port: port)
                    // Nothing else ever moves provisioningState off .joiningWifi
                    // once we get here -- the connection state handler only
                    // touches isConnected/isStreaming, not this enum -- so the
                    // play-button progress screen (gated on provisioningState
                    // in ScopeTab) got stuck showing "Joining..." forever even
                    // once real data was flowing. Back to .idle now that the
                    // join succeeded and the socket handoff is underway;
                    // isConnected/isStreaming take over from here.
                    provisioningState = .idle
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

        guard !host.isEmpty, NWEndpoint.Port(rawValue: port) != nil else {
            lastError = "Invalid host"
            return
        }
        engine.resetBuffers()
        engine.connect(host: host, port: port)
    }

    func disconnect() {
        if engine.hasConnection {
            sendStop()
        }
        engine.cancelConnection()
        isConnected = false
        isStreaming = false

        if let ssid = joinedHotspotSSID {
            NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid)
            joinedHotspotSSID = nil
        }
    }

    // MARK: - Stream control

    private func sendStop() {
        engine.sendControlFrame(type: .stop)
        isStreaming = false
    }

    /// Pause: stop the P4's sample stream over the still-open TCP socket, but
    /// keep the WiFi association and NWConnection alive -- unlike
    /// `requestStreamStop`, this does NOT tell the mainboard to tear down the
    /// softAP. Resume is then just `resumeStream()`, no rejoin needed.
    func pauseStream() {
        guard engine.hasConnection else { return }
        sendStop()
    }

    /// Resume after `pauseStream()`: reuses the still-live socket if we have
    /// one, otherwise (e.g. the connection dropped while paused) falls back
    /// to the full BLE provisioning flow.
    func resumeStream(ble: BLETransport) async {
        if engine.hasConnection, isConnected {
            engine.sendControlFrame(type: .start)
            isStreaming = true
        } else {
            await startFullStreamFlow(ble: ble)
        }
    }

    /// Send USB_CMD_SET_RATE (0x82): usb_cmd_rate_t { u32 current_sps; u32 voltage_sps; u8 decimation; u8 pad[3]; }
    func sendSetRate(currentSps: UInt32, voltageSps: UInt32, decimation: UInt8 = 1) {
        var payload = Data()
        DaqStreamEngine.appendU32LE(&payload, currentSps)
        DaqStreamEngine.appendU32LE(&payload, voltageSps)
        payload.append(decimation)
        payload.append(0) // pad
        payload.append(0) // pad
        payload.append(0) // pad
        engine.sendControlFrame(type: .setRate, payload: payload)
    }
}
