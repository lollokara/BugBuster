import Foundation

/// One `POST /api/scripts/files/chunk` of a BLE upload.
struct ScriptChunk: Equatable {
    let off: Int
    let bytes: Data
    let final: Bool
}

/// Splits a script so every tunnel request
/// `{"path":…,"body":{"name","off","b64","final"},"id":…}` fits the firmware's
/// 511-byte request limit (512-byte buffer incl. NUL). The overhead is measured with the real serializer.
enum ScriptChunkPlanner {
    static let path = "/api/scripts/files/chunk"
    /// `BLETransport.reqIdCounter` is a UInt8.
    static let worstCaseRequestId = 255

    static func body(name: String, chunk: ScriptChunk) -> [String: Any] {
        ["name": name, "off": chunk.off, "b64": chunk.bytes.base64EncodedString(), "final": chunk.final]
    }

    /// Raw bytes per chunk (a multiple of 3, so base64 has no padding until the last chunk).
    static func rawBytesPerChunk(name: String, totalBytes: Int,
                                 maxRequestBytes: Int = BLETransport.maxTunnelRequestBytes) -> Int {
        // Widest values: the largest offset, and `false` (longer than `true`).
        let probe: [String: Any] = ["name": name, "off": totalBytes, "b64": "", "final": false]
        guard let overhead = BLETransport.tunnelPayload(path: path, body: probe, id: worstCaseRequestId)?.count else { return 0 }
        return max(0, (maxRequestBytes - overhead) / 4 * 3)
    }

    static func plan(name: String, data: Data,
                     maxRequestBytes: Int = BLETransport.maxTunnelRequestBytes) -> [ScriptChunk] {
        let step = rawBytesPerChunk(name: name, totalBytes: data.count, maxRequestBytes: maxRequestBytes)
        guard step > 0, !data.isEmpty else { return [] }
        let base = data.startIndex
        var chunks: [ScriptChunk] = []
        var off = 0
        while off < data.count {
            let end = min(off + step, data.count)
            chunks.append(ScriptChunk(off: off, bytes: data.subdata(in: (base + off)..<(base + end)), final: end == data.count))
            off = end
        }
        return chunks
    }
}
