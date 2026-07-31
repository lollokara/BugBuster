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

    // Extension v6 (@88-95): device-confirmed ADAQ7769 filter/decimation.
    // Applies to the FINE/COARSE current ADCs only — the VOLTAGE ADAQ stays
    // pinned at its own fixed rate (shared SPI bus + SYNC line with COARSE).
    // These are always what the DEVICE actually applied, never the client's
    // request — the driver clamps combinations the part can't hit.
    let deviceFilter: DaqFilter?
    let deviceAdcDec: DaqAdcDecimation?
    /// Read-only: reported by the device but not driven by this client. The
    /// ruling is that sample rate IS the ODR — there is no client-side
    /// stream-decimation path — so nothing in the UI sets this value.
    let deviceStreamDecim: UInt16?
    /// ODR in samples/sec, decoded from the wire's milli-SPS (`odr_mhz` =
    /// ODR * 1000).
    let deviceOdrHz: Double?
}

struct DaqMarker {
    let sampleIndex: UInt64
    let timestampUs: UInt64
    let channel: UInt8
    let edge: UInt8
    let kind: UInt8
}

/// usb_stat_block_t: float min, max, mean, rms, std; uint32_t count. 24 bytes.
struct DaqStatBlock {
    let min: Float
    let max: Float
    let mean: Float
    let rms: Float
    let std: Float
    let count: UInt32
}

/// usb_stats_payload_t: three DaqStatBlock in order i, v, p. 72 bytes total.
struct DaqStats {
    let i: DaqStatBlock
    let v: DaqStatBlock
    let p: DaqStatBlock
}

/// usb_energy_payload_t: double energy_mwh, energy_j, charge_mah, charge_c,
/// elapsed_s; float last_i, last_v, last_p. 52 bytes total, packed.
struct DaqEnergy {
    let energyMwh: Double
    let energyJ: Double
    let chargeMah: Double
    let chargeC: Double
    let elapsedS: Double
    let lastI: Float
    let lastV: Float
    let lastP: Float
}

enum DaqRecord {
    case wave(DaqWaveBlock)
    case status(DaqStatus)
    case marker(DaqMarker)
    case stats(DaqStats)
    case energy(DaqEnergy)
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
    /// S1: read cursor into `rxBuffer`, in bytes, relative to `rxBuffer.startIndex`.
    /// `drainFrames()` parses forward from here WITHOUT mutating rxBuffer, so a
    /// receive holding several frames costs one (amortised) compaction instead
    /// of a full-buffer memmove per frame. See `compactRxBufferIfNeeded()`.
    private var rxCursor: Int = 0
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
    /// STATS/ENERGY arrive at 10 Hz on the wire — the same cadence as
    /// STATUS — so these callbacks are dispatched to main exactly like
    /// `onStatus`, with no additional throttling needed.
    var onStats: (@Sendable (DaqStats) -> Void)?
    var onEnergy: (@Sendable (DaqEnergy) -> Void)?

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
            rxCursor = 0
            firstTimestampUs = nil
            // C1: the device restarts its sample sequence at 0 on session
            // reset. Leaving a stale refIndex in either clock makes the next
            // block's `startIndex &- refIndex` wrap to ~1.8e19 — the exact
            // unsigned-wrap bug class already fixed for `anchorT` above (see
            // the comment at DaqStreamClock.blockTimes). This is currently
            // masked because the state machine always pairs `.resetBuffers`
            // with `.openSocket`, but resetting here too is defence-in-depth
            // against a future reconnect path that forgets `.resetBuffers`.
            voltClock.reset()
            currClock.reset()
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
            // Note: rxBuffer/rxCursor are deliberately NOT touched here — this
            // effect resets sample state, not the socket's in-flight byte
            // stream. `cancelConnection()` owns clearing rxBuffer/rxCursor.
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
    ///
    /// `base` is a byte offset relative to `rxBuffer.startIndex` (S1: the
    /// read-cursor offset, NOT an absolute Data.Index) — the `u8`/`u16`
    /// helpers below are themselves startIndex-relative, so passing `base`
    /// straight through keeps that invariant.
    private func headerLooksValid(at base: Int) -> Bool {
        rxBuffer.u8(base + 0) == DaqWire.magic0
            && rxBuffer.u8(base + 1) == DaqWire.magic1
            && rxBuffer.u8(base + 2) == DaqWire.version
            && Self.knownDataTypes.contains(rxBuffer.u8(base + 3))
            && Int(rxBuffer.u16(base + 10)) <= DaqWire.maxPayload
    }

    /// S1: parses frames forward from `rxCursor` without mutating `rxBuffer`
    /// per frame — the old code did a full-buffer `removeSubrange` after
    /// EVERY frame, which memmoved the entire remaining tail once per frame
    /// (quadratic when a single 64 KB receive holds several frames).
    /// Consumed bytes are only compacted out of `rxBuffer` in
    /// `compactRxBufferIfNeeded()`, called once per `drainFrames()` pass.
    private func drainFrames() {
        while true {
            let available = rxBuffer.count - rxCursor
            guard available >= DaqWire.headerLen else { break } // wait for more bytes

            guard headerLooksValid(at: rxCursor) else {
                // Drop one byte, then jump to the next magic candidate,
                // scanning forward from the cursor (not mutating rxBuffer).
                let searchStart = rxBuffer.startIndex + rxCursor + 1
                if let idx = rxBuffer[searchStart...].firstIndex(of: DaqWire.magic0) {
                    rxCursor = idx - rxBuffer.startIndex
                } else {
                    rxCursor = rxBuffer.count
                }
                continue
            }

            let payloadLen = Int(rxBuffer.u16(rxCursor + 10))
            let totalLen = DaqWire.headerLen + payloadLen + DaqWire.crcLen
            guard available >= totalLen else { break } // partial trailing frame — wait for more bytes

            let type = rxBuffer.u8(rxCursor + 3)
            let payloadOffset = rxCursor + DaqWire.headerLen

            // S2: decode straight out of rxBuffer's storage instead of
            // materialising a per-frame `Data` via `subdata(in:)` (up to
            // 16 KB copied per frame, at up to ~64 kSPS).
            rxBuffer.withUnsafeBytes { raw in
                if let record = Self.decodePayload(type: type, buffer: raw,
                                                    payloadOffset: payloadOffset,
                                                    payloadLen: payloadLen) {
                    handle(record)
                }
            }

            rxCursor += totalLen
        }
        compactRxBufferIfNeeded()
    }

    /// S1: drop the consumed prefix (bytes [0, rxCursor)) only when it's
    /// worth the memmove — either everything pending has been consumed (the
    /// common case: cheap `removeAll(keepingCapacity:)`, no copy) or the
    /// consumed prefix has grown to a meaningful fraction of the buffer.
    /// Compacting on every frame is exactly the O(n^2) behaviour this
    /// replaces; never compacting would let rxBuffer grow unboundedly
    /// relative to genuinely pending (partial-frame) bytes.
    private func compactRxBufferIfNeeded() {
        guard rxCursor > 0 else { return }
        if rxCursor == rxBuffer.count {
            rxBuffer.removeAll(keepingCapacity: true)
            rxCursor = 0
        } else if rxCursor >= rxBuffer.count / 2 || rxCursor >= 65536 {
            rxBuffer.removeSubrange(rxBuffer.startIndex..<(rxBuffer.startIndex + rxCursor))
            rxCursor = 0
        }
    }

    /// S2: decodes a frame's payload directly out of `buffer` (the raw bytes
    /// backing `rxBuffer`) at `[payloadOffset, payloadOffset + payloadLen)`
    /// — no per-frame `Data` copy. NOTE: the payload is NOT guaranteed
    /// 4-byte aligned (the 12-byte header offset plus Data's own backing
    /// offset can leave it unaligned), so every multi-byte read here MUST go
    /// through `loadUnaligned`/`memcpy` rather than a typed `load(as:)` —
    /// the latter traps at runtime on an unaligned address.
    static func decodePayload(type: UInt8, buffer: UnsafeRawBufferPointer,
                              payloadOffset: Int, payloadLen: Int) -> DaqRecord? {
        guard let rec = DaqWire.RecType(rawValue: type) else { return .unknown(type) }

        func pu8(_ off: Int) -> UInt8 { buffer.loadUnaligned(fromByteOffset: payloadOffset + off, as: UInt8.self) }
        func pu16(_ off: Int) -> UInt16 { buffer.loadUnaligned(fromByteOffset: payloadOffset + off, as: UInt16.self) }
        func pu32(_ off: Int) -> UInt32 { buffer.loadUnaligned(fromByteOffset: payloadOffset + off, as: UInt32.self) }
        func pu64(_ off: Int) -> UInt64 { buffer.loadUnaligned(fromByteOffset: payloadOffset + off, as: UInt64.self) }
        func pf32(_ off: Int) -> Float { Float(bitPattern: pu32(off)) }
        // Doubles are NOT guaranteed 8-byte aligned either (same reasoning as
        // the u32/u16 unaligned loads above) — read the bit pattern via an
        // unaligned UInt64 load rather than `load(as: Double.self)`, which
        // would trap at runtime on an unaligned address.
        func pf64(_ off: Int) -> Double { Double(bitPattern: pu64(off)) }

        switch rec {
        case .waveI, .waveV:
            guard payloadLen >= 24 else { return nil }
            let startIndex = pu64(0)
            let timestampUs = pu64(8)
            let sampleRate = pu32(16)
            let requestedCount = Int(pu16(20))
            let isVoltage = rec == .waveV
            let bytesPerSample = isVoltage ? 4 : 5
            // C2: single division replacing the old O(n) decrement loop
            // (`while count > 0 && 24 + count * bytesPerSample > payload.count
            // { count -= 1 }`) — same resulting count for every input,
            // including the zero/negative-space edges: payloadLen >= 24 is
            // already guaranteed above, so (payloadLen - 24) is never
            // negative, and integer (floor) division reproduces exactly the
            // largest count the old loop would have converged to.
            let maxCount = (payloadLen - 24) / bytesPerSample
            let count = min(requestedCount, maxCount)

            // Bulk byte copy (not a per-element append loop, not a typed
            // load — the payload may be unaligned) — memcpy handles
            // arbitrary alignment. Host is little-endian (arm64), matching
            // the wire, so this is a straight byte copy for both f32 values
            // and (for WAVE_I) the meta bytes.
            let valuesByteOffset = payloadOffset + 24
            let values = [Float](unsafeUninitializedCapacity: count) { valBuf, initializedCount in
                if count > 0 {
                    memcpy(valBuf.baseAddress!, buffer.baseAddress!.advanced(by: valuesByteOffset), count * 4)
                }
                initializedCount = count
            }

            var meta: [UInt8]? = nil
            if !isVoltage {
                let metaByteOffset = payloadOffset + 24 + count * 4
                if count > 0 {
                    meta = [UInt8](unsafeUninitializedCapacity: count) { metaBuf, initializedCount in
                        memcpy(metaBuf.baseAddress!, buffer.baseAddress!.advanced(by: metaByteOffset), count)
                        initializedCount = count
                    }
                } else {
                    meta = []
                }
            }
            return .wave(DaqWaveBlock(startIndex: startIndex, timestampUs: timestampUs,
                                      sampleRate: sampleRate,
                                      decimation: max(pu8(22), 1),
                                      values: values, meta: meta,
                                      isVoltage: isVoltage))

        case .status:
            guard payloadLen >= 20 else { return nil }
            let inVoltage = payloadLen >= 28 ? pf32(20) : 0
            let inCurrent = payloadLen >= 28 ? pf32(24) : 0
            let framesTx = payloadLen >= 40 ? pu32(36) : nil
            let bytesPerSec = payloadLen >= 44 ? pu32(40) : nil
            let fifoDrop = payloadLen >= 48 ? pu32(44) : nil
            let adaqOk = payloadLen >= 29 ? pu8(28) : nil
            let wiFrames = payloadLen >= 76 ? pu32(72) : nil
            let wvFrames = payloadLen >= 80 ? pu32(76) : nil
            let wiDrops  = payloadLen >= 84 ? pu32(80) : nil
            let wvDrops  = payloadLen >= 88 ? pu32(84) : nil
            // Extension v6 (@88-95). Guards use each field's END offset
            // (>= 89, >= 90, >= 92, >= 96) — using start offsets would read
            // out of bounds on a frame truncated mid-field.
            let filterCode  = payloadLen >= 89 ? pu8(88) : nil
            let adcDecCode  = payloadLen >= 90 ? pu8(89) : nil
            let streamDecim = payloadLen >= 92 ? pu16(90) : nil
            let odrMilliSps = payloadLen >= 96 ? pu32(92) : nil
            return .status(DaqStatus(
                sampleRate: pu32(0),
                overflowCount: pu32(4),
                range: pu8(8),
                streaming: pu8(9) != 0,
                rangeLocked: pu8(10) != 0,
                sourceEnabled: pu8(11) != 0,
                vdutSet: pf32(12),
                ilimitSet: pf32(16),
                inVoltage: inVoltage,
                inCurrent: inCurrent,
                framesTx: framesTx,
                bytesPerSec: bytesPerSec,
                fifoDropFrames: fifoDrop,
                adaqOkBits: adaqOk,
                waveIFrames: wiFrames,
                waveVFrames: wvFrames,
                waveIDrops: wiDrops,
                waveVDrops: wvDrops,
                deviceFilter: filterCode.flatMap { DaqFilter(rawValue: $0) },
                deviceAdcDec: adcDecCode.flatMap { DaqAdcDecimation(rawValue: $0) },
                deviceStreamDecim: streamDecim,
                deviceOdrHz: odrMilliSps.map { Double($0) / 1000.0 }
            ))

        case .marker:
            guard payloadLen >= 20 else { return nil }
            return .marker(DaqMarker(
                sampleIndex: pu64(0),
                timestampUs: pu64(8),
                channel: pu8(16),
                edge: pu8(17),
                kind: pu8(18)
            ))

        case .stats:
            // usb_stats_payload_t = 3 x usb_stat_block_t{f32 min,max,mean,rms,std; u32 count},
            // in order i, v, p — 24 bytes each, 72 bytes total. Unlike STATUS
            // this record has no extension history, so it's all-or-nothing:
            // one guard at the full struct size, exactly like the WAVE
            // records' single top-of-function guard.
            guard payloadLen >= 72 else { return nil }
            func block(_ off: Int) -> DaqStatBlock {
                DaqStatBlock(min: pf32(off), max: pf32(off + 4), mean: pf32(off + 8),
                             rms: pf32(off + 12), std: pf32(off + 16), count: pu32(off + 20))
            }
            return .stats(DaqStats(i: block(0), v: block(24), p: block(48)))

        case .energy:
            // usb_energy_payload_t = double energy_mwh, energy_j, charge_mah,
            // charge_c, elapsed_s (8 bytes each, offsets 0/8/16/24/32) then
            // float last_i, last_v, last_p (offsets 40/44/48) — 52 bytes total.
            guard payloadLen >= 52 else { return nil }
            return .energy(DaqEnergy(
                energyMwh: pf64(0),
                energyJ: pf64(8),
                chargeMah: pf64(16),
                chargeC: pf64(24),
                elapsedS: pf64(32),
                lastI: pf32(40),
                lastV: pf32(44),
                lastP: pf32(48)
            ))

        case .fft:
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
                // S3: reserve v alongside t — only t was reserved before, so
                // v reallocated geometrically on every block at up to 64 kSPS.
                voltage.t.reserveCapacity(voltage.count + block.values.count)
                voltage.v.reserveCapacity(voltage.count + block.values.count)
                for (i, v) in block.values.enumerated() {
                    let t = tBase + Double(i) * dt
                    voltage.t.append(t)
                    voltage.v.append(v)
                    voltMidAcc.push(t: t, v: v, src: 0, hasSrc: false, into: &voltageMid)
                    voltAcc.push(t: t, v: v, src: 0, hasSrc: false, into: &voltageReduced)
                }
                trimIfNeeded(&voltage, &voltageMid, &voltageReduced, &voltageHist)
            } else {
                // S3: reserve v and src alongside t (same reasoning as above).
                current.t.reserveCapacity(current.count + block.values.count)
                current.v.reserveCapacity(current.count + block.values.count)
                current.src.reserveCapacity(current.count + block.values.count)
                // S4: hoist block.meta out of the per-sample loop — reading
                // an enum payload's optional-array property on every one of
                // ~64,000 samples/sec costs a retain/release each time; the
                // value itself never changes within a block.
                let blockMeta = block.meta
                for (i, v) in block.values.enumerated() {
                    let t = tBase + Double(i) * dt
                    current.t.append(t)
                    current.v.append(v)
                    // Source bits live at meta[1:0]=range, [3:2]=source.
                    let m: UInt8 = blockMeta.map { i < $0.count ? $0[i] : 0 } ?? 0
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

        case .stats(let stats):
            // Arrives at 10 Hz already — publish straight through, no
            // pipeline-queue-rate publishing here (unlike WAVE_I/WAVE_V).
            let cb = onStats
            DispatchQueue.main.async { cb?(stats) }

        case .energy(let energy):
            let cb = onEnergy
            DispatchQueue.main.async { cb?(energy) }

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
    /// Latest decoded STATS/ENERGY records (10 Hz), mirroring `lastStatus`.
    @Published var lastStats: DaqStats? = nil
    @Published var lastEnergy: DaqEnergy? = nil
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
        engine.onStats = { [weak self] stats in
            Task { @MainActor in self?.lastStats = stats }
        }
        engine.onEnergy = { [weak self] energy in
            Task { @MainActor in self?.lastEnergy = energy }
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
                // Deferred: `perform` is called synchronously from within
                // `send`'s effect loop. A same-tick re-entrant `send()` here
                // would mutate `machine` and run a nested effect list before
                // the outer loop (still iterating effects computed for the
                // state we're about to leave) has finished applying them.
                Task { @MainActor in self.send(.provisioningFailed("No BLE connection to the mainboard")) }
                return
            }
            Task { await self.runProvisioning(ble: ble) }

        case .joinHotspot:
            guard let creds = pendingCredentials else {
                Task { @MainActor in self.send(.hotspotJoinFailed("No credentials")) }
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
                Task { @MainActor in self.send(.socketClosed("Invalid endpoint")) }
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
            retryTask?.cancel()
            retryTask = nil
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

    /// Rolling grace start for the watchdog's stall check. A freshly
    /// (re)started stream legitimately has no frames yet, and the redial rung
    /// deliberately emits .resetBuffers, which clears lastFrameAt. Treating
    /// nil as "stalled" made the watchdog fire one second after EVERY
    /// reconnect — long before a first frame could plausibly arrive — so
    /// recovery could never converge:
    ///   stall -> recover -> resetBuffers -> lastFrameAt = nil -> stall ...
    /// which presented as a "Reconnecting..." banner that never cleared while
    /// the trace flickered back every few seconds (fixed in ae7dc47). This is
    /// the SAME mechanism `extendWatchdogGrace()` reuses for an
    /// in-place acq-config change: it re-arms this instance var rather than
    /// adding a second suspend/resume path, so an expected device-initiated
    /// gap gets exactly the grace a fresh stream start gets instead of being
    /// misread as a stall.
    private var watchdogGraceStartedAt: Date = Date()

    private func startWatchdog() {
        watchdogTask?.cancel()
        watchdogGraceStartedAt = Date()
        watchdogTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self else { return }   // manager is gone; nothing left to watch
                guard self.linkState == .streaming else { continue }
                let startedAt = self.watchdogGraceStartedAt
                // The later of "last real frame" and "most recent expected
                // gap start" — so a config-change grace extension always
                // wins even though lastFrameAt is non-nil and pre-dates it.
                let reference = max(self.engine.lastFrameAt ?? startedAt, startedAt)
                if Date().timeIntervalSince(reference) > Self.stallTimeout {
                    self.send(.dataStalled)
                }
            }
        }
    }

    private func stopWatchdog() {
        watchdogTask?.cancel()
        watchdogTask = nil
    }

    /// Re-arms the watchdog's grace window in place. Called around
    /// `setAcquisitionConfig`'s device round trip, which the firmware
    /// serves by stopping the fast-capture task, reprogramming both current
    /// ADAQs over SPI (blocking register writes + a SYNC pulse), then
    /// restarting it — a real, device-initiated gap with no frames, not a
    /// dead link. A no-op when the watchdog isn't running (not streaming),
    /// since there's nothing to protect.
    private func extendWatchdogGrace() {
        watchdogGraceStartedAt = Date()
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

        // BB_MOCK_RATE = samples/second to synthesize (default 33, i.e. the
        // original 2000 samples over a 60 s backfill).
        //
        // The default is a UI-dev rate, not a realistic one: a real DAQ link
        // delivers tens of thousands of samples/second. That matters because
        // the simulator CANNOT join the DAQ HAT's WiFi hotspot, so mock mode is
        // the only way to put the ingest + render pipeline under a load
        // resembling production and read the render-tick instrumentation back.
        // Raising this is how the two-tier buffer, the envelope accumulators
        // and the decimator get exercised at all.
        let env = ProcessInfo.processInfo.environment
        let rate = Double(env["BB_MOCK_RATE"] ?? "") ?? 33.0
        let backfillSeconds = 60.0
        let dt = 1.0 / max(rate, 1.0)
        var t = -backfillSeconds
        while t < 0 {
            appendMockSample(at: t)
            t += dt
        }
        // A Timer cannot fire faster than a few hundred Hz, so above that we
        // synthesize a BATCH per tick instead of a sample -- which also mirrors
        // the real wire, where samples arrive in blocks, not individually.
        let tickInterval = max(dt, 0.02)
        let perTick = max(Int((rate * tickInterval).rounded()), 1)
        mockTimer = Timer.scheduledTimer(withTimeInterval: tickInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                for _ in 0..<perTick {
                    self.appendMockSample(at: self.mockElapsed)
                    self.mockElapsed += dt
                }
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
        let clamped = max(0, current)
        engine.appendMock(t: t, voltageV: Float(voltage), currentA: Float(clamped))
        accumulateMockMeasurements(v: voltage, i: clamped, t: t)
    }

    // Mock STATS/ENERGY.
    //
    // The device sends USB_REC_STATS and USB_REC_ENERGY at 10 Hz, but mock mode
    // appends samples straight into the engine's buffers and so produces
    // neither -- which left the whole measurements panel stuck on its empty
    // state, i.e. the one screen mock mode exists to let us build without
    // hardware was the one screen it could not show. Synthesize both here from
    // the same samples the trace is drawn from, so the numbers on screen are
    // consistent with the visible waveform.
    //
    // This mirrors power_dsp.c: trapezoidal energy/charge, and min/max/mean/
    // RMS/std over the session. It does NOT exercise the wire decoder -- those
    // byte offsets are verified against real hardware by
    // tests/tools/daq_usb_stream_bench.py, not here.
    private struct MockStatAcc {
        var n: UInt32 = 0
        var sum = 0.0, sumSq = 0.0
        var min = Double.infinity, max = -Double.infinity
        mutating func push(_ x: Double) {
            n += 1; sum += x; sumSq += x * x
            if x < min { min = x }
            if x > max { max = x }
        }
        var block: DaqStatBlock {
            guard n > 0 else { return DaqStatBlock(min: 0, max: 0, mean: 0, rms: 0, std: 0, count: 0) }
            let d = Double(n)
            let mean = sum / d
            let ms = sumSq / d
            let varr = Swift.max(ms - mean * mean, 0)
            return DaqStatBlock(min: Float(min), max: Float(max), mean: Float(mean),
                                rms: Float(ms.squareRoot()), std: Float(varr.squareRoot()),
                                count: n)
        }
    }

    private var mockAccI = MockStatAcc()
    private var mockAccV = MockStatAcc()
    private var mockAccP = MockStatAcc()
    private var mockChargeC = 0.0
    private var mockEnergyJ = 0.0
    private var mockPrevI: Double?
    private var mockPrevP: Double?
    private var mockPrevT: Double?
    private var mockElapsedS = 0.0
    private var mockLastPublish = -Double.infinity

    private func accumulateMockMeasurements(v: Double, i: Double, t: Double) {
        let p = v * i
        mockAccI.push(i); mockAccV.push(v); mockAccP.push(p)
        if let pi = mockPrevI, let pp = mockPrevP, let pt = mockPrevT, t > pt {
            let dt = t - pt
            mockChargeC += 0.5 * (pi + i) * dt
            mockEnergyJ += 0.5 * (pp + p) * dt
            mockElapsedS += dt
        }
        mockPrevI = i; mockPrevP = p; mockPrevT = t

        // Publish at the device's 10 Hz cadence, not per sample.
        guard t - mockLastPublish >= 0.1 else { return }
        mockLastPublish = t
        lastEnergy = DaqEnergy(energyMwh: mockEnergyJ * (1000.0 / 3600.0),
                               energyJ: mockEnergyJ,
                               chargeMah: mockChargeC * (1000.0 / 3600.0),
                               chargeC: mockChargeC,
                               elapsedS: mockElapsedS,
                               lastI: Float(i), lastV: Float(v), lastP: Float(p))
        lastStats = DaqStats(i: mockAccI.block, v: mockAccV.block, p: mockAccP.block)
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

    /// POST /api/daq/acq_config — re-tunes the ADAQ7769 filter + ADC hardware
    /// decimation for the FINE/COARSE current ADCs only (the VOLTAGE ADAQ is
    /// pinned separately and is never affected by this call). Routed through
    /// api_core_handle(), which both HTTP and BLE dispatch through, because
    /// the phone cannot reach the S3 over HTTP while joined to the DAQ
    /// hotspot — same transport `.recycleDevice` uses.
    ///
    /// Applying a new config stops and restarts acquisition on the device to
    /// release the SPI bus; expect a brief gap in the stream, not a fault.
    ///
    /// `streamDecim` is accepted for symmetry with `DaqAcquisitionConfig` but
    /// is intentionally NOT sent: the endpoint body only carries
    /// {"filter", "adc_dec"}, and the ruling is that the sample rate IS the
    /// ODR — there is no client-driven stream-decimation path. STATUS still
    /// reports the device's stream_decim read-only.
    func setAcquisitionConfig(filter: DaqFilter, adcDec: DaqAdcDecimation, streamDecim: UInt16? = nil) {
        guard let ble else { return }
        let body: [String: Any] = [
            "filter": Int(filter.rawValue),
            "adc_dec": Int(adcDec.rawValue)
        ]
        // C1: this round trip covers daq_board_stop_fast() + a blocking SPI
        // reprogram of both current ADAQs + daq_board_run_fast() on the
        // device — no frames for that whole window. Left unguarded, the 1 Hz
        // watchdog (4s stallTimeout) can read that expected gap as a dead
        // link and kick off the full recovery ladder on a healthy stream
        // (the exact shape ae7dc47 already fixed once, via a different
        // path). A pinger keeps re-arming the grace window for the entire
        // request lifetime — not just once up front — so a slow reprogram
        // (up to apiRequest's own 6s timeout) can never outrun a single
        // fixed grace. `defer` cancels the pinger and takes one final grace
        // stamp unconditionally, on every exit path (success, device error
        // response, transport failure, or timeout) so the watchdog is never
        // left suspended — a permanently suspended watchdog would be worse
        // than a twitchy one, since a genuinely dead link would then never
        // be detected.
        extendWatchdogGrace()
        Task {
            let pinger = Task { [weak self] in
                while !Task.isCancelled {
                    self?.extendWatchdogGrace()
                    try? await Task.sleep(nanoseconds: 500_000_000)
                }
            }
            defer {
                pinger.cancel()
                self.extendWatchdogGrace()
            }
            _ = await ble.apiRequest(path: "/api/daq/acq_config", body: body)
        }
    }
}
