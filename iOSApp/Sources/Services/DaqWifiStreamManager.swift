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
    /// Stream decimation applied on the device: effective sample spacing is
    /// decimation / sampleRate seconds (usb_wave_hdr_t byte 22).
    let decimation: UInt8
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

    /// adaq_ok_bits (extension v2 @28): bit0 = FINE ok, bit1 = COARSE ok,
    /// bit2 = VOLT ok. Sent by the firmware since extension v2 but never
    /// surfaced on the phone — a dead voltage ADC looked identical to a
    /// dropped voltage stream.
    let adaqOkBits: UInt8?
    // Extension v5 (@72-87): per-record-type TX accounting from the device.
    let waveIFrames: UInt32?
    let waveVFrames: UInt32?
    let waveIDrops: UInt32?
    let waveVDrops: UInt32?

    /// nil when the firmware predates extension v2.
    var voltAdcOK: Bool? { adaqOkBits.map { $0 & 0x04 != 0 } }
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

    /// Fold the oldest `n` samples into `hist` as a (min, max) envelope with
    /// `fold` raw samples per pair, then drop them from self. This is the
    /// recent→history eviction: raw resolution is kept for the newest data,
    /// while evicted data survives as peak-preserving envelope points.
    mutating func foldOldest(_ n: Int, fold: Int, into hist: inout DaqChannelBuffer) {
        let n = Swift.min(n, t.count)
        guard n > 0, fold >= 1 else { return }
        let hasSrc = !src.isEmpty
        var i = 0
        while i < n {
            let end = Swift.min(i + fold, n)
            var minIdx = i, maxIdx = i
            for idx in i..<end {
                if v[idx] < v[minIdx] { minIdx = idx }
                if v[idx] > v[maxIdx] { maxIdx = idx }
            }
            let first = Swift.min(minIdx, maxIdx)
            let second = Swift.max(minIdx, maxIdx)
            hist.t.append(t[first]); hist.v.append(v[first])
            if hasSrc { hist.src.append(first < src.count ? src[first] : 0) }
            if second != first {
                hist.t.append(t[second]); hist.v.append(v[second])
                if hasSrc { hist.src.append(second < src.count ? src[second] : 0) }
            }
            i = end
        }
        t.removeFirst(n)
        v.removeFirst(n)
        if hasSrc { src.removeFirst(Swift.min(n, src.count)) }
    }
}

/// Incremental min/max envelope reducer: folds every `fold` pushed samples
/// into exactly two entries (min, max — in temporal order) appended to an
/// output buffer. Lets the render pipeline read an always-current ~2 kHz
/// envelope of the raw recent ring in O(columns) instead of re-scanning
/// hundreds of thousands of raw samples every tick.
struct EnvelopeAccumulator {
    let fold: Int
    private var n = 0
    private var minT = 0.0, maxT = 0.0
    private var minV: Float = 0, maxV: Float = 0
    private var minS: UInt8 = 0, maxS: UInt8 = 0

    init(fold: Int) { self.fold = fold }

    mutating func reset() { n = 0 }

    mutating func push(t: Double, v: Float, src: UInt8, hasSrc: Bool,
                       into out: inout DaqChannelBuffer) {
        if n == 0 {
            minT = t; maxT = t; minV = v; maxV = v; minS = src; maxS = src
        } else {
            if v < minV { minV = v; minT = t; minS = src }
            if v > maxV { maxV = v; maxT = t; maxS = src }
        }
        n += 1
        if n >= fold {
            n = 0
            if minT <= maxT {
                out.t.append(minT); out.v.append(minV)
                out.t.append(maxT); out.v.append(maxV)
                if hasSrc { out.src.append(minS); out.src.append(maxS) }
            } else {
                out.t.append(maxT); out.v.append(maxV)
                out.t.append(minT); out.v.append(minV)
                if hasSrc { out.src.append(maxS); out.src.append(minS) }
            }
        }
    }
}

/// Per-stream monotonic clock. The wire's per-block wall-clock timestamps are
/// taken when a batch is *flushed*, not when its first sample was acquired —
/// bursty ring draining on the P4 makes consecutive blocks' nominal spans
/// overlap (observed on the bench: 12.5 ms-span blocks arriving ~6 ms apart),
/// which put backwards jumps in the t axis and drew as walls of vertical
/// spikes. Sample *indices* are strictly monotonic, so time is derived from
/// start_index × dt instead; wall clock is only used once, after ~2 s, to
/// calibrate the true rate (the header's nominal rate can be off), plus a
/// hard monotonic clamp as a last line of defense.
private struct DaqStreamClock {
    private var refIndex: UInt64?
    private var refWallUs: UInt64 = 0
    private var anchorT: Double = 0
    private var rateScale: Double = 1.0
    private var scaleLocked = false
    private var lastEndT = -Double.infinity

    mutating func reset() { self = DaqStreamClock() }

    /// Returns (t of first sample, effective dt) for a block.
    /// `sessionT0Us` is the shared wall-clock zero so the voltage and current
    /// streams stay mutually aligned.
    mutating func blockTimes(startIndex: UInt64, wallUs: UInt64, sessionT0Us: UInt64,
                             dtNominal: Double, count: Int) -> (t0: Double, dt: Double) {
        if refIndex == nil {
            refIndex = startIndex
            refWallUs = wallUs
            // SIGNED delta: this stream's first block can be stamped slightly
            // EARLIER than the other stream's block that set the session zero
            // (batches flush independently). The unsigned form wrapped to
            // ~1.8e19 µs and anchored the whole current stream outside every
            // window — no current/power trace, ever.
            anchorT = Double(Int64(bitPattern: wallUs &- sessionT0Us)) / 1_000_000.0
        }
        let idxDelta = Double(startIndex &- (refIndex ?? startIndex))
        if !scaleLocked, dtNominal > 0 {
            let wallSpan = Double(wallUs &- refWallUs) / 1_000_000.0
            if wallSpan > 2.0, idxDelta > 0 {
                let s = wallSpan / (idxDelta * dtNominal)
                if s.isFinite, s > 0.25, s < 4.0 { rateScale = s }
                scaleLocked = true
            }
        }
        let dt = dtNominal > 0 ? dtNominal * rateScale : 0
        var t0 = anchorT + idxDelta * dt
        if t0 < lastEndT { t0 = lastEndT + dt }
        lastEndT = t0 + Double(max(count - 1, 0)) * dt
        return (t0, dt)
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
    // Two-tier storage per channel: `voltage`/`current` are raw recent-sample
    // windows (256k ≈ 4 s at 64 kSps, full resolution for zoom-in); overflow
    // is folded 32:1 as a (min,max) envelope into `voltageHist`/`currentHist`
    // (≈2 kHz envelope → 240k entries ≈ a minute of history before *it*
    // halves). Long recordings keep their whole span; only very old data
    // gets progressively coarser.
    private(set) var voltage = DaqChannelBuffer()
    private(set) var current = DaqChannelBuffer()
    private(set) var voltageHist = DaqChannelBuffer()
    private(set) var currentHist = DaqChannelBuffer()
    /// Incrementally-maintained envelope tiers of the recent rings — the
    /// render tick picks the finest tier that fits the display budget for
    /// the current zoom (raw → 8:1 → 64:1 → history), so resolution steps
    /// down progressively as the visible span grows.
    private(set) var voltageMid = DaqChannelBuffer()
    private(set) var currentMid = DaqChannelBuffer()
    private(set) var voltageReduced = DaqChannelBuffer()
    private(set) var currentReduced = DaqChannelBuffer()
    private var voltMidAcc = EnvelopeAccumulator(fold: DaqStreamEngine.midFold)
    private var currMidAcc = EnvelopeAccumulator(fold: DaqStreamEngine.midFold)
    private var voltAcc = EnvelopeAccumulator(fold: DaqStreamEngine.reducedFold)
    private var currAcc = EnvelopeAccumulator(fold: DaqStreamEngine.reducedFold)
    private(set) var recordCount = 0
    /// Queue-confined RX frame counts by record type. Compared against the
    /// device's extension-v5 TX counters to localise voltage loss: TX>>RX
    /// means the wire dropped it, TX==RX==0 means it was never produced.
    private(set) var rxWaveI = 0
    private(set) var rxWaveV = 0
    /// Wall-clock of the last decoded data frame. The watchdog compares
    /// against this: a socket can sit .ready with zero frames arriving, which
    /// previously showed as "streaming" indefinitely.
    private(set) var lastFrameAt: Date?
    private var rxBuffer = Data()
    private var firstTimestampUs: UInt64?
    private var voltClock = DaqStreamClock()
    private var currClock = DaqStreamClock()
    private let recentCap = 262_144
    private let historyCap = 245_760
    private let historyFold = 32
    static let midFold = 8
    static let reducedFold = 64

    // `connection` and `autoStartOnReady` are touched from BOTH the main
    // thread (connect/pause/resume/send) and the pipeline queue (state
    // handler, receive loop) — guard them with a lock; an unsynchronized
    // class-reference swap here is a real crash under pause/resume churn.
    private let connLock = NSLock()
    private var _connection: NWConnection?
    private var _autoStartOnReady = true

    private var connection: NWConnection? {
        get { connLock.lock(); defer { connLock.unlock() }; return _connection }
        set { connLock.lock(); _connection = newValue; connLock.unlock() }
    }

    /// Whether a newly-ready socket should immediately send CMD_START.
    /// False while the user has the stream paused, so a silent socket
    /// reconnect doesn't un-pause behind their back.
    var autoStartOnReady: Bool {
        get { connLock.lock(); defer { connLock.unlock() }; return _autoStartOnReady }
        set { connLock.lock(); _autoStartOnReady = newValue; connLock.unlock() }
    }

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
            // Runs on Self.queue (conn.start below). Ignore events from a
            // superseded connection: after a redial, the old connection's
            // .cancelled/.failed must not clobber the live one's state (this
            // let a stale reconnect kill a just-established stream).
            guard let self, self.connection === conn else { return }
            print("[daq-net] state=\(state)")
            switch state {
            case .ready:
                if self.autoStartOnReady {
                    self.sendControlFrame(type: .start)
                }
                self.receiveLoop(conn)
            case .waiting(let err):
                // NWConnection parks in .waiting when the path drops (e.g.
                // "No network route" flaps right after a hotspot join) and
                // can sit there forever on a no-internet AP. Give it 3 s to
                // become viable, then fail it over to the reconnect path
                // instead of silently hanging.
                print("[daq-net] waiting: \(err)")
                Self.queue.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                    guard let self, self.connection === conn else { return }
                    if case .waiting = conn.state {
                        print("[daq-net] still waiting after 3s — failing over")
                        conn.cancel()
                        let cb = self.onConnectionState
                        DispatchQueue.main.async {
                            cb?(.failed(NWError.posix(.ETIMEDOUT)))
                        }
                    }
                }
            default:
                break
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
            voltageHist.removeAll()
            currentHist.removeAll()
            voltageMid.removeAll()
            currentMid.removeAll()
            voltageReduced.removeAll()
            currentReduced.removeAll()
            voltMidAcc.reset()
            currMidAcc.reset()
            voltAcc.reset()
            currAcc.reset()
            recordCount = 0
            rxWaveI = 0
            rxWaveV = 0
            lastFrameAt = nil
            firstTimestampUs = nil
            voltClock.reset()
            currClock.reset()
        }
    }

    /// Recent-ring overflow: evict the oldest quarter into the envelope
    /// history (and the matching span from the reduced mirror); when history
    /// itself overflows, halve its resolution.
    private func trimIfNeeded(_ recent: inout DaqChannelBuffer,
                              _ mid: inout DaqChannelBuffer,
                              _ reduced: inout DaqChannelBuffer,
                              _ hist: inout DaqChannelBuffer) {
        if recent.count > recentCap {
            let evict = recentCap / 4   // multiple of both fold factors
            recent.foldOldest(evict, fold: historyFold, into: &hist)
            dropFront(&mid, (evict / Self.midFold) * 2)
            dropFront(&reduced, (evict / Self.reducedFold) * 2)
            if hist.count > historyCap { hist.envelopeHalve() }
        }
    }

    private func dropFront(_ buf: inout DaqChannelBuffer, _ n: Int) {
        let n = min(buf.count, n)
        guard n > 0 else { return }
        buf.t.removeFirst(n)
        buf.v.removeFirst(n)
        if !buf.src.isEmpty { buf.src.removeFirst(min(n, buf.src.count)) }
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
            voltMidAcc.push(t: t, v: voltageV, src: 0, hasSrc: false, into: &voltageMid)
            currMidAcc.push(t: t, v: currentA, src: 0, hasSrc: true, into: &currentMid)
            voltAcc.push(t: t, v: voltageV, src: 0, hasSrc: false, into: &voltageReduced)
            currAcc.push(t: t, v: currentA, src: 0, hasSrc: true, into: &currentReduced)
            recordCount += 1
            trimIfNeeded(&voltage, &voltageMid, &voltageReduced, &voltageHist)
            trimIfNeeded(&current, &currentMid, &currentReduced, &currentHist)
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
                // A server-side close arrives as a clean EOF with NO state
                // transition — swallowing it left the UI showing "ready"
                // with a dead socket. Fail the connection over explicitly so
                // the manager's reconnect path reacts.
                print("[daq-net] receive ended error=\(String(describing: error)) eof=\(isComplete)")
                conn.cancel()
                self.connection = nil
                let cb = self.onConnectionState
                DispatchQueue.main.async { cb?(.failed(error ?? NWError.posix(.ECONNRESET))) }
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

            // Bulk-parse the f32/meta arrays with raw pointer loads — the
            // per-byte Data-subscript path cost real CPU at tens of
            // kilosamples/sec (host is little-endian, matching the wire).
            let values: [Float] = payload.withUnsafeBytes { raw in
                var out = [Float]()
                out.reserveCapacity(count)
                for i in 0..<count {
                    out.append(Float(bitPattern: raw.loadUnaligned(fromByteOffset: 24 + i * 4,
                                                                   as: UInt32.self)))
                }
                return out
            }

            var meta: [UInt8]? = nil
            if !isVoltage {
                let metaBase = 24 + count * 4
                meta = Array(payload[(payload.startIndex + metaBase)..<(payload.startIndex + metaBase + count)])
            }
            return .wave(DaqWaveBlock(startIndex: startIndex, timestampUs: timestampUs,
                                      sampleRate: sampleRate,
                                      decimation: max(payload.u8(22), 1),
                                      values: values, meta: meta,
                                      isVoltage: isVoltage))

        case .status:
            guard payload.count >= 20 else { return nil }
            let inVoltage = payload.count >= 28 ? payload.f32(20) : 0
            let inCurrent = payload.count >= 28 ? payload.f32(24) : 0
            let framesTx = payload.count >= 40 ? payload.u32(36) : nil
            let bytesPerSec = payload.count >= 44 ? payload.u32(40) : nil
            let fifoDrop = payload.count >= 48 ? payload.u32(44) : nil
            let adaqOk = payload.count >= 29 ? payload.u8(28) : nil
            let wiFrames = payload.count >= 76 ? payload.u32(72) : nil
            let wvFrames = payload.count >= 80 ? payload.u32(76) : nil
            let wiDrops  = payload.count >= 84 ? payload.u32(80) : nil
            let wvDrops  = payload.count >= 88 ? payload.u32(84) : nil
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
                fifoDropFrames: fifoDrop,
                adaqOkBits: adaqOk,
                waveIFrames: wiFrames,
                waveVFrames: wvFrames,
                waveIDrops: wiDrops,
                waveVDrops: wvDrops
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

    #if DEBUG
    // ---- Temporary spike-forensics instrumentation ------------------------
    // Answers "are the on-screen spikes real wire data or a client bug":
    // logs (a) blocks whose timeline overlaps/regresses vs. the previous
    // block and (b) raw sample neighborhoods around large voltage jumps.
    private var dbgLastVoltEndT: Double = -.infinity
    private var dbgLogsRemaining = 40

    private func logBlockAnomalies(_ block: DaqWaveBlock, tBase: Double, dt: Double) {
        guard dbgLogsRemaining > 0, block.isVoltage, !block.values.isEmpty else { return }
        if tBase < dbgLastVoltEndT {
            dbgLogsRemaining -= 1
            print(String(format: "[daq-dbg] WAVE_V time REGRESSION: block t0=%.6f < prev end %.6f (rate=%u decim=%u count=%d)",
                         tBase, dbgLastVoltEndT, block.sampleRate, block.decimation, block.values.count))
        }
        var v = block.values[0]
        for i in 1..<block.values.count {
            let d = abs(block.values[i] - v)
            if d > 0.5 {
                dbgLogsRemaining -= 1
                let lo = max(0, i - 3), hi = min(block.values.count, i + 3)
                let ctx = block.values[lo..<hi].map { String(format: "%.4f", $0) }.joined(separator: " ")
                print(String(format: "[daq-dbg] WAVE_V jump %.3fV at sample %d/%d (t0=%.6f rate=%u): [%@]",
                             d, i, block.values.count, tBase, block.sampleRate, ctx))
                if dbgLogsRemaining <= 0 { break }
            }
            v = block.values[i]
        }
        dbgLastVoltEndT = tBase + Double(block.values.count - 1) * dt
    }
    #endif

    private func handle(_ record: DaqRecord) {
        lastFrameAt = Date()
        recordCount += 1
        switch record {
        case .wave(let block):
            if block.isVoltage { rxWaveV += 1 } else { rxWaveI += 1 }
            if firstTimestampUs == nil { firstTimestampUs = block.timestampUs }
            let sessionT0 = firstTimestampUs ?? block.timestampUs
            // Effective inter-sample spacing includes device-side stream
            // decimation (header rate is the raw ODR for WAVE_I).
            let dtNominal = block.sampleRate > 0
                ? Double(block.decimation) / Double(block.sampleRate) : 0
            // Index-based monotonic time (see DaqStreamClock).
            let (tBase, dt): (Double, Double)
            if block.isVoltage {
                (tBase, dt) = voltClock.blockTimes(startIndex: block.startIndex,
                                                   wallUs: block.timestampUs,
                                                   sessionT0Us: sessionT0,
                                                   dtNominal: dtNominal,
                                                   count: block.values.count)
            } else {
                (tBase, dt) = currClock.blockTimes(startIndex: block.startIndex,
                                                   wallUs: block.timestampUs,
                                                   sessionT0Us: sessionT0,
                                                   dtNominal: dtNominal,
                                                   count: block.values.count)
            }
            #if DEBUG
            logBlockAnomalies(block, tBase: tBase, dt: dt)
            #endif

            if block.isVoltage {
                voltage.t.reserveCapacity(voltage.count + block.values.count)
                for (i, v) in block.values.enumerated() {
                    let t = tBase + Double(i) * dt
                    voltage.t.append(t)
                    voltage.v.append(v)
                    voltMidAcc.push(t: t, v: v, src: 0, hasSrc: false, into: &voltageMid)
                    voltAcc.push(t: t, v: v, src: 0, hasSrc: false, into: &voltageReduced)
                }
                trimIfNeeded(&voltage, &voltageMid, &voltageReduced, &voltageHist)
            } else {
                current.t.reserveCapacity(current.count + block.values.count)
                for (i, v) in block.values.enumerated() {
                    let t = tBase + Double(i) * dt
                    current.t.append(t)
                    current.v.append(v)
                    // Source bits live at meta[1:0]=range, [3:2]=source.
                    let m: UInt8 = block.meta.map { i < $0.count ? $0[i] : 0 } ?? 0
                    let s = (m >> 2) & 0x03
                    current.src.append(s)
                    currMidAcc.push(t: t, v: v, src: s, hasSrc: true, into: &currentMid)
                    currAcc.push(t: t, v: v, src: s, hasSrc: true, into: &currentReduced)
                }
                trimIfNeeded(&current, &currentMid, &currentReduced, &currentHist)
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

// `ProvisioningStage` now lives in DaqLinkState.swift, alongside the state
// machine (`DaqLinkStateMachine`) that consumes it and replaces the old
// `ProvisioningState` enum below.

// MARK: - Manager (main-actor UI surface)

@MainActor
final class DaqWifiStreamManager: NSObject, ObservableObject {
    static let shared = DaqWifiStreamManager()

    /// The single source of truth for the link. Everything the UI shows is a
    /// projection of this; nothing else stores link state.
    @Published private(set) var linkState: DaqLinkState = .idle
    @Published var lastError: String? = nil
    @Published var lastStatus: DaqStatus? = nil
    /// Main-actor mirror of the engine's RX counts, refreshed on each STATUS
    /// frame (10 Hz) — cheap, and STATUS is exactly when the device's own TX
    /// counters update, so both sides of the comparison move together.
    @Published var rxCounts: (i: Int, v: Int) = (0, 0)

    /// Compatibility projections so existing call sites keep compiling.
    var isConnected: Bool {
        switch linkState {
        case .streaming, .paused: return true
        default: return false
        }
    }
    var isStreaming: Bool { linkState == .streaming }

    /// Background stream pipeline; ScopeRenderModel reads its buffers.
    let engine = DaqStreamEngine()

    /// Last endpoint we connected to, for silent socket-level reconnects.
    private var lastEndpoint: (host: String, port: UInt16)?

    private var machine = DaqLinkStateMachine()
    private var retryTask: Task<Void, Never>?
    private var watchdogTask: Task<Void, Never>?
    /// Set once the BLE transport is known, so recovery can reprovision
    /// without the caller passing it in again.
    private var ble: BLETransport?

    override init() {
        super.init()
        engine.onConnectionState = { [weak self] state in
            Task { @MainActor in
                guard let self else { return }
                switch state {
                case .ready:
                    self.lastError = nil
                    self.send(.socketReady)
                case .failed(let err):
                    self.lastError = err.localizedDescription
                    self.engine.cancelConnection()
                    self.send(.socketClosed(err.localizedDescription))
                case .cancelled:
                    break   // cancellation is always deliberate; the state
                            // machine already moved when we asked for it
                default:
                    break
                }
            }
        }
        engine.onStatus = { [weak self] status in
            Task { @MainActor in
                guard let self else { return }
                self.lastStatus = status
                self.rxCounts = (self.engine.rxWaveI, self.engine.rxWaveV)
            }
        }
    }

    /// The ONLY way link state changes. Runs the reducer, publishes the new
    /// state, then performs the returned effects — so state and side effects
    /// can never disagree.
    func send(_ event: DaqLinkEvent) {
        let effects = machine.handle(event)
        linkState = machine.state
        for effect in effects { perform(effect) }
    }

    private func perform(_ effect: DaqLinkEffect) {
        switch effect {
        case .requestProvisioning:
            guard let ble else {
                send(.provisioningFailed("No BLE connection to the mainboard"))
                return
            }
            Task { await self.runProvisioning(ble: ble) }

        case .joinHotspot:
            guard let creds = pendingCredentials else {
                send(.hotspotJoinFailed("No credentials"))
                return
            }
            Task {
                let ok = await self.joinDaqHotspot(ssid: creds.ssid, password: creds.password)
                self.send(ok ? .hotspotJoined
                             : .hotspotJoinFailed("Could not join DAQ HAT WiFi hotspot"))
            }

        case .openSocket:
            guard let ep = lastEndpoint, !ep.host.isEmpty,
                  NWEndpoint.Port(rawValue: ep.port) != nil else {
                send(.socketClosed("Invalid endpoint"))
                return
            }
            engine.autoStartOnReady = false   // sendStart effect owns CMD_START
            engine.connect(host: ep.host, port: ep.port)

        case .sendStart:
            engine.sendControlFrame(type: .start)
            startWatchdog()

        case .sendStop:
            engine.sendControlFrame(type: .stop)
            stopWatchdog()

        case .closeSocket:
            stopWatchdog()
            engine.cancelConnection()

        case .removeHotspotConfig:
            // THE single call site. Multiple owners caused the documented
            // bug where an async removal dropped the association a second
            // into a freshly-established stream.
            if let ssid = joinedHotspotSSID {
                NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid)
                joinedHotspotSSID = nil
            }

        case .recycleDevice:
            guard let ble else { return }
            Task { _ = await ble.apiRequest(path: "/api/daq/wifi_stream/recycle", body: nil) }

        case .resetBuffers:
            engine.resetBuffers()

        case .scheduleRetry(let afterMs):
            retryTask?.cancel()
            retryTask = Task { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(afterMs) * 1_000_000)
                guard !Task.isCancelled else { return }
                self?.send(.retryRequested)
            }
        }
    }

    /// Frames must keep arriving while streaming. A connected-but-silent link
    /// is indistinguishable from a dead one from the user's side, and used to
    /// show "streaming" forever.
    private static let stallTimeout: TimeInterval = 4.0

    private func startWatchdog() {
        watchdogTask?.cancel()
        watchdogTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self, self.linkState == .streaming else { continue }
                let last = self.engine.lastFrameAt
                if last == nil || Date().timeIntervalSince(last!) > Self.stallTimeout {
                    self.send(.dataStalled)
                }
            }
        }
    }

    private func stopWatchdog() {
        watchdogTask?.cancel()
        watchdogTask = nil
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
        // Mock mode bypasses the ladder entirely — jam the state directly to
        // .streaming rather than driving it through provisioning/join/socket.
        machine = DaqLinkStateMachine(state: .streaming)
        linkState = machine.state
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
        machine = DaqLinkStateMachine(state: .idle)
        linkState = machine.state
    }

    private func appendMockSample(at t: Double) {
        let voltage = 5.0 + 0.4 * sin(t * 2 * .pi * 0.5) + Double.random(in: -0.03...0.03)
        let current = 0.12 + 0.05 * sin(t * 2 * .pi * 1.3 + 0.6) + Double.random(in: -0.006...0.006)
        engine.appendMock(t: t, voltageV: Float(voltage), currentA: Float(max(0, current)))
    }

    // MARK: - BLE-driven bring-up

    private var joinedHotspotSSID: String?
    private var pendingCredentials: (ssid: String, password: String,
                                     host: String, port: UInt16)?

    /// Entry point from the UI's play button.
    func start(ble: BLETransport) {
        self.ble = ble
        send(.startRequested)
    }

    /// Drives the BLE start + status poll, reporting progress as events. No
    /// longer owns any state of its own — that was the bug (a stuck
    /// "joining" screen even after real data was flowing).
    private func runProvisioning(ble: BLETransport) async {
        guard let data = await ble.apiRequest(path: "/api/daq/wifi_stream/start", body: nil),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (obj["ok"] as? Bool) == true else {
            send(.provisioningFailed("Device rejected the start request"))
            return
        }

        // 20s: the full chain is a DDP round-trip to the C6 + up to ~3s of
        // P4-side wifi_ap_start() retries + softAP/DNS/TCP bring-up + the S3
        // reassembling 4 chunked info frames at its 250ms poll cadence, all
        // before this BLE poll can see "ready". See
        // .mex/patterns/daq-hat-ios-wifi-streaming.md.
        let deadline = Date().addingTimeInterval(20.0)
        while !Task.isCancelled {
            if Date() >= deadline {
                send(.provisioningFailed("Timed out waiting for DAQ WiFi credentials"))
                return
            }
            if let data = await ble.apiRequest(path: "/api/daq/wifi_stream/status", body: nil),
               let status = try? JSONDecoder().decode(DaqWifiStreamStatus.self, from: data) {
                switch status.state {
                case "ready":
                    guard let ssid = status.ssid, let password = status.password,
                          let host = status.host, let port = status.port else {
                        send(.provisioningFailed("Malformed ready status"))
                        return
                    }
                    pendingCredentials = (ssid, password, host, UInt16(port))
                    lastEndpoint = (host, UInt16(port))
                    send(.credentialsReady)
                    return
                case "failed":
                    send(.provisioningFailed("DAQ HAT reported hotspot failure"))
                    return
                case "starting":
                    if let stage = status.stage.flatMap(ProvisioningStage.init(rawValue:)) {
                        send(.stageReported(stage))
                    }
                default:
                    break   // "idle" — keep polling
                }
            }
            try? await Task.sleep(nanoseconds: 300_000_000)
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

    // MARK: - Stream control
    //
    // Every one of these is now just an event into the state machine — the
    // reducer decides what effects follow, so state and side effects can
    // never disagree. See DaqLinkState.swift for the full ladder.

    func pauseStream() { send(.pauseRequested) }

    /// Resume after `pauseStream()`. If the socket died while paused, the
    /// reducer is already in `.recovering` (via the `.paused, .socketClosed`
    /// transition) and this is simply ignored — the ladder owns getting back
    /// to `.streaming` from there.
    func resumeStream(ble: BLETransport) async {
        self.ble = ble
        send(.resumeRequested)
    }

    func disconnect() { send(.stopRequested) }

    /// User-facing Retry from the failure card.
    func retry() { send(.retryRequested) }

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
