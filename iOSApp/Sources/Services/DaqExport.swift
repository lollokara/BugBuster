import Foundation

// =============================================================================
// DaqExport.swift — CSV export of the DAQ HAT's captured voltage/current
// buffers. Currently the ONLY way to get measurement data off the phone: the
// app has no other export or sync path for a session's samples.
//
// Pairing decision (read this before changing the merge below): voltage and
// current are two INDEPENDENTLY-timestamped streams — see
// `DaqStreamClock` in DaqWifiStreamManager.swift, whose whole existence is to
// derive per-stream time from each stream's own sample index because the two
// streams' blocks are flushed at different moments and can have overlapping
// or offset nominal spans. They are not guaranteed to have the same sample
// count or the same timestamps, so pairing row `k` of voltage with row `k` of
// current (index pairing) would silently mismatch the two the moment the
// streams drift out of phase — which they will, since they run at
// independent hardware rates (FINE/COARSE current ADCs vs. the VOLTAGE ADAQ,
// pinned at its own fixed rate per the comments on `DaqStatus.deviceFilter`).
//
// Instead this does a NEAREST-TIMESTAMP join: the current stream is the
// driving/primary series (it alone carries the per-sample source bits used
// for the `source` column), and for each current sample we binary-search the
// sorted voltage timestamp array for the closest voltage sample. Every
// exported row's `t_seconds` is the current sample's own timestamp; the
// paired `voltage_v` is whichever voltage sample is temporally nearest, not
// whichever happens to share the row's array index.
// =============================================================================

enum DaqExportError: Error, LocalizedError {
    case noSamples

    var errorDescription: String? {
        switch self {
        case .noSamples: return "No captured samples to export yet."
        }
    }
}

@MainActor
enum DaqExportManager {
    /// Builds a CSV from `engine`'s captured buffers and writes it to a temp
    /// file, returning the file URL for sharing. Reads the engine's
    /// queue-confined buffers on `DaqStreamEngine.queue` (never from the
    /// caller's thread — those arrays are NOT safe to touch off that queue),
    /// builds and writes the CSV off-main, and only hops back to main to
    /// hand back the result.
    static func export(engine: DaqStreamEngine, deviceOdrHz: Double?) async throws -> URL {
        try await withCheckedThrowingContinuation { continuation in
            DaqStreamEngine.queue.async {
                // Snapshot the queue-confined buffers. History-then-recent
                // concatenation is already in time order: `foldOldest`
                // (called by `trimIfNeeded`) only ever appends newly-evicted
                // (older-than-recent) samples to the END of hist, so hist is
                // itself chronological and always precedes `recent` in time.
                let voltT = engine.voltageHist.t + engine.voltage.t
                let voltV = engine.voltageHist.v + engine.voltage.v
                let currT = engine.currentHist.t + engine.current.t
                let currV = engine.currentHist.v + engine.current.v
                let currSrc = engine.currentHist.src + engine.current.src

                if currT.isEmpty && voltT.isEmpty {
                    continuation.resume(throwing: DaqExportError.noSamples)
                    return
                }

                do {
                    let url = try writeCSV(voltT: voltT, voltV: voltV,
                                            currT: currT, currV: currV, currSrc: currSrc,
                                            deviceOdrHz: deviceOdrHz)
                    continuation.resume(returning: url)
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    /// Binary search for the index of the entry in sorted `arr` nearest to
    /// `t`. `arr` must be non-empty and non-decreasing (both true for the
    /// engine's t arrays — samples are appended in acquisition order).
    nonisolated private static func nearestIndex(_ t: Double, in arr: [Double]) -> Int {
        var lo = 0
        var hi = arr.count - 1
        if t <= arr[lo] { return lo }
        if t >= arr[hi] { return hi }
        while lo < hi {
            let mid = (lo + hi) / 2
            if arr[mid] == t { return mid }
            if arr[mid] < t { lo = mid + 1 } else { hi = mid }
        }
        // `lo` is the first index with arr[lo] >= t; compare it against its
        // predecessor to find the true nearest neighbor.
        if lo > 0, abs(t - arr[lo - 1]) <= abs(arr[lo] - t) {
            return lo - 1
        }
        return lo
    }

    nonisolated private static func sourceLabel(_ s: UInt8) -> String {
        switch s {
        case 0: return "FINE"
        case 1: return "COARSE"
        case 2: return "BLEND"
        default: return "?"
        }
    }

    /// Writes the CSV incrementally via a FileHandle (not one giant in-memory
    /// String) so a multi-hundred-thousand-row export doesn't spike peak
    /// memory — flushed in bounded-size chunks.
    nonisolated private static func writeCSV(voltT: [Double], voltV: [Float],
                                  currT: [Double], currV: [Float], currSrc: [UInt8],
                                  deviceOdrHz: Double?) throws -> URL {
        let dir = FileManager.default.temporaryDirectory
        let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let url = dir.appendingPathComponent("daq_export_\(stamp).csv")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        let handle = try FileHandle(forWritingTo: url)
        defer { try? handle.close() }

        func write(_ s: String) throws {
            if let data = s.data(using: .utf8) { try handle.write(contentsOf: data) }
        }

        var header = "# BugBuster DAQ export\n"
        header += "# exported_at: \(ISO8601DateFormatter().string(from: Date()))\n"
        header += "# voltage_samples: \(voltT.count)\n"
        header += "# current_samples: \(currT.count)\n"
        if let odr = deviceOdrHz {
            header += "# device_odr_sps: \(odr)\n"
        }
        header += "# pairing: nearest-timestamp join, current stream is primary "
        header += "(see the comment at the top of DaqExport.swift for why this is not an index pair)\n"
        header += "t_seconds,voltage_v,current_a,power_w,source\n"
        try write(header)

        guard !currT.isEmpty else {
            // No current samples at all: nothing to drive the join off of.
            // Emit voltage alone rather than producing zero rows silently.
            var chunk = ""
            for i in 0..<voltT.count {
                chunk += "\(voltT[i]),\(voltV[i]),,,\n"
                if i % 4096 == 4095 { try write(chunk); chunk = "" }
            }
            if !chunk.isEmpty { try write(chunk) }
            return url
        }

        var chunk = ""
        chunk.reserveCapacity(1 << 16)
        for j in 0..<currT.count {
            let t = currT[j]
            let i = currV[j]
            let src = j < currSrc.count ? currSrc[j] : 0
            var vStr = ""
            var pStr = ""
            if !voltT.isEmpty {
                let vi = nearestIndex(t, in: voltT)
                let v = voltV[vi]
                vStr = String(v)
                pStr = String(Double(v) * Double(i))
            }
            chunk += "\(t),\(vStr),\(i),\(pStr),\(sourceLabel(src))\n"
            if j % 4096 == 4095 {
                try write(chunk)
                chunk = ""
            }
        }
        if !chunk.isEmpty { try write(chunk) }
        return url
    }
}
