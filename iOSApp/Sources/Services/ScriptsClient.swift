import Foundation

enum ScriptsClientError: LocalizedError, Equatable {
    /// Firmware `{"error"|"err": "..."}` text, verbatim.
    case firmware(String)
    /// 409: the single file-script slot is held by `running`.
    case busy(running: String, id: Int)
    case unauthorized
    case http(Int)
    case malformed(String)
    case needsWiFi(String)
    case invalidName(String)
    case tooLarge(bytes: Int)
    case empty

    var errorDescription: String? {
        switch self {
        case .firmware(let text): return text
        case .busy(let running, _): return "'\(running)' is running"
        case .unauthorized: return "The device rejected the admin token"
        case .http(let code): return "Device returned HTTP \(code)"
        case .malformed(let what): return "Unexpected reply from the device (\(what))"
        case .needsWiFi(let what): return "\(what) needs a Wi-Fi connection"
        case .invalidName(let name):
            return "'\(name)' is not a valid script name (1–32 of A–Z a–z 0–9 _ . -, ending in .py)"
        case .tooLarge(let bytes): return "Script is \(bytes) bytes; the device keeps at most \(ScriptName.maxBodyBytes)"
        case .empty: return "A script can't be empty"
        }
    }
}

struct ScriptLintResult: Equatable {
    let ok: Bool
    let message: String?
}

/// Every `/api/scripts/*` call the app makes, for either transport. Framing per
/// the firmware plan's "Shared JSON contract":
/// - logs: HTTP `text/plain` + `X-BugBuster-Log-Next`; BLE `{next, n, dropped, data: base64}`
/// - files/get: HTTP the whole file as text; BLE base64 pages of 3072 bytes
/// - upload: HTTP raw body to `/api/scripts/files`; BLE `files/chunk` requests ≤ 512 bytes
/// - lint: HTTP raw text; BLE `{"name"}` (stored file) or `{"src"}` when it fits 512 bytes
/// - eval: HTTP raw text + `?persist=1`; BLE `{"src", "persist"}` ≤ 512 bytes
final class ScriptsClient {
    static let logPageBytes = 4096      // MP_LOG_RESP_MAX (config.h)
    static let filePageBytes = 3072     // SCRIPTS_FILE_PAGE (api_scripts.cpp)

    let wire: ScriptsWire

    init(wire: ScriptsWire) {
        self.wire = wire
    }

    // MARK: status and files

    func status() async throws -> ScriptRunStatus {
        try decode(ScriptRunStatus.self, from: try await call("GET", "/api/scripts/status"), what: "status")
    }

    func files() async throws -> [String] {
        let reply = try await call("GET", "/api/scripts/files")
        guard let names = reply.json?["files"] as? [String] else { throw ScriptsClientError.malformed("files") }
        return names
    }

    func storage() async throws -> ScriptStorageInfo {
        try decode(ScriptStorageInfo.self, from: try await call("GET", "/api/scripts/storage"), what: "storage")
    }

    func readFile(_ name: String) async throws -> String {
        guard ScriptName.isValid(name) else { throw ScriptsClientError.invalidName(name) }
        switch wire.kind {
        case .wifi:
            // Raw script text; it may itself look like JSON, so never inspect `ok`.
            let reply = try await call("GET", "/api/scripts/files/get", query: ["name": name], acceptsNotOK: true)
            return String(decoding: reply.body, as: UTF8.self)
        case .ble:
            var out = Data()
            var size = Int.max
            while out.count < size {
                let reply = try await call("GET", "/api/scripts/files/get", query: [
                    "name": name, "off": String(out.count), "len": String(Self.filePageBytes)])
                guard let object = reply.json, let total = object["size"] as? Int,
                      let b64 = object["data"] as? String, let chunk = Data(base64Encoded: b64) else {
                    throw ScriptsClientError.malformed("files/get page")
                }
                size = total
                if chunk.isEmpty { break }
                out.append(chunk)
            }
            // A truncated file in the editor would be saved back truncated.
            guard out.count == size else {
                throw ScriptsClientError.malformed("files/get stopped at \(out.count) of \(size) bytes")
            }
            return String(decoding: out, as: UTF8.self)
        }
    }

    func writeFile(_ name: String, text: String, progress: ((Double) -> Void)? = nil) async throws {
        guard ScriptName.isValid(name) else { throw ScriptsClientError.invalidName(name) }
        let data = Data(text.utf8)
        guard !data.isEmpty else { throw ScriptsClientError.empty }
        guard data.count <= ScriptName.maxBodyBytes else { throw ScriptsClientError.tooLarge(bytes: data.count) }
        switch wire.kind {
        case .wifi:
            _ = try await call("POST", "/api/scripts/files", query: ["name": name], body: .text(text))
            progress?(1)
        case .ble:
            let chunks = ScriptChunkPlanner.plan(name: name, data: data)
            do {
                try await upload(chunks, name: name, total: data.count, progress: progress)
            } catch ConnectionAPIError.bleNoResponse {
                // A lost reply leaves the device at an unknown offset. off=0 truncates
                // the temp file, so restarting once is always safe.
                try await upload(chunks, name: name, total: data.count, progress: progress)
            }
        }
    }

    func deleteFile(_ name: String) async throws {
        guard ScriptName.isValid(name) else { throw ScriptsClientError.invalidName(name) }
        _ = try await call("POST", "/api/scripts/files/delete", query: ["name": name])
    }

    // MARK: run control

    func runFile(_ name: String, background: Bool, replace: Bool) async throws -> ScriptRunOutcome {
        var query = ["name": name]
        if background { query["background"] = "1" }
        if replace { query["replace"] = "1" }
        do {
            let reply = try await call("POST", "/api/scripts/run-file", query: query)
            guard let object = reply.json, let id = object["id"] as? Int else { throw ScriptsClientError.malformed("run-file") }
            return .started(id: id, name: object["name"] as? String ?? name,
                            background: object["background"] as? Bool ?? background)
        } catch ScriptsClientError.busy(let running, let id) {
            return .busy(running: running, id: id)
        }
    }

    func stop() async throws {
        _ = try await call("POST", "/api/scripts/stop")
    }

    /// Syntax check. Over BLE the stored file `name` is linted on the device (its
    /// source never crosses the tunnel); without a name the source goes inline when
    /// the request fits 512 bytes. Firmware without BLE lint, or a source too large
    /// for the tunnel, throws `.needsWiFi("Checking syntax")`.
    func lint(_ source: String, name: String? = nil) async throws -> ScriptLintResult {
        let path = "/api/scripts/lint"
        let reply: ScriptsWireReply
        switch wire.kind {
        case .wifi:
            reply = try await call("POST", path, body: .text(source), acceptsNotOK: true)
        case .ble:
            let body: [String: Any]
            if let name {
                guard ScriptName.isValid(name) else { throw ScriptsClientError.invalidName(name) }
                body = ["name": name]
            } else {
                body = ["src": source]
                guard Self.tunnelRequestBytes(path: path, body: body) <= BLETransport.maxTunnelRequestBytes else {
                    throw ScriptsClientError.needsWiFi("Checking syntax")
                }
            }
            do {
                reply = try await call("POST", path, body: .json(body), acceptsNotOK: true)
            } catch ScriptsClientError.firmware(let text) where text == "unknown path" {
                // Firmware that predates BLE lint answers api_core's fallback.
                throw ScriptsClientError.needsWiFi("Checking syntax")
            }
        }
        guard let object = reply.json, let ok = object["ok"] as? Bool else { throw ScriptsClientError.malformed("lint") }
        return ScriptLintResult(ok: ok, message: Self.errorText(object))
    }

    /// Submits a snippet to the REPL context; returns the script id. Output arrives
    /// through the log ring. A held file-script slot throws `.busy`.
    func eval(_ src: String, persist: Bool = true) async throws -> Int {
        guard !src.isEmpty else { throw ScriptsClientError.empty }
        let path = "/api/scripts/eval"
        let reply: ScriptsWireReply
        switch wire.kind {
        case .wifi:
            // handle_post_scripts_eval wraps a raw text body as {"src": ...}; persist rides the query.
            guard src.utf8.count <= ScriptName.maxBodyBytes else { throw ScriptsClientError.tooLarge(bytes: src.utf8.count) }
            reply = try await call("POST", path, query: persist ? ["persist": "1"] : [:], body: .text(src))
        case .ble:
            let body: [String: Any] = ["src": src, "persist": persist]
            let bytes = Self.tunnelRequestBytes(path: path, body: body)
            guard bytes <= BLETransport.maxTunnelRequestBytes else { throw ScriptsClientError.tooLarge(bytes: bytes) }
            reply = try await call("POST", path, body: .json(body))
        }
        guard let id = reply.json?["id"] as? Int else { throw ScriptsClientError.malformed("eval") }
        return id
    }

    // MARK: logs

    /// One page of the non-draining log ring from byte offset `since`.
    func logs(since: UInt64) async throws -> ScriptLogPage {
        let reply = try await call("GET", "/api/scripts/logs", query: ["since": String(since)], acceptsNotOK: true)
        switch wire.kind {
        case .wifi:
            guard let raw = reply.header("X-BugBuster-Log-Next"), let next = UInt64(raw) else {
                throw ScriptsClientError.malformed("logs: no X-BugBuster-Log-Next header (firmware predates log cursors)")
            }
            let n = UInt64(reply.body.count)
            let start = next >= n ? next - n : 0
            return ScriptLogPage(data: reply.body, since: since, next: next, dropped: start > since ? start - since : 0)
        case .ble:
            guard let object = reply.json, let next = (object["next"] as? NSNumber)?.uint64Value,
                  let b64 = object["data"] as? String, let data = Data(base64Encoded: b64) else {
                throw ScriptsClientError.malformed("logs page")
            }
            let dropped = (object["dropped"] as? NSNumber)?.uint64Value ?? 0
            return ScriptLogPage(data: data, since: since, next: next, dropped: dropped)
        }
    }

    // MARK: autorun

    func autorunStatus() async throws -> ScriptAutorunStatus {
        try decode(ScriptAutorunStatus.self, from: try await call("GET", "/api/scripts/autorun/status"), what: "autorun/status")
    }

    func setAutorun(enabled: Bool, name: String?) async throws {
        if enabled {
            guard let name, ScriptName.isValid(name) else { throw ScriptsClientError.invalidName(name ?? "") }
            _ = try await call("POST", "/api/scripts/autorun/enable", query: ["name": name])
        } else {
            _ = try await call("POST", "/api/scripts/autorun/disable")
        }
    }

    // MARK: plumbing

    private func upload(_ chunks: [ScriptChunk], name: String, total: Int, progress: ((Double) -> Void)?) async throws {
        for chunk in chunks {
            let reply = try await call("POST", ScriptChunkPlanner.path, body: .json(ScriptChunkPlanner.body(name: name, chunk: chunk)))
            let received = reply.json?["received"] as? Int
            let expected = chunk.off + chunk.bytes.count
            guard received == expected else {
                throw ScriptsClientError.malformed("chunk ack \(received.map(String.init) ?? "missing"), expected \(expected)")
            }
            progress?(Double(expected) / Double(total))
        }
    }

    /// Sends one request and maps every failure to `ScriptsClientError`, except
    /// `ConnectionAPIError.bleNoResponse` (the upload retry needs it as is).
    private func call(_ method: String, _ path: String, query: [String: String] = [:],
                      body: ScriptsWireBody? = nil, acceptsNotOK: Bool = false) async throws -> ScriptsWireReply {
        let reply: ScriptsWireReply
        do {
            reply = try await wire.send(method, path, query: query, body: body)
        } catch ConnectionAPIError.needsWiFi(let what) {
            throw ScriptsClientError.needsWiFi(what)
        }
        let object = reply.json
        if reply.isSuccess {
            // TR-11b: some routes answer 200 {"ok": false, "err": ...} (script upload).
            if !acceptsNotOK, let object, (object["ok"] as? Bool) == false {
                throw ScriptsClientError.firmware(Self.errorText(object) ?? "request failed")
            }
            return reply
        }
        if reply.status == 409, let running = object?["running"] as? String {
            throw ScriptsClientError.busy(running: running, id: object?["id"] as? Int ?? 0)
        }
        if reply.status == 401 { throw ScriptsClientError.unauthorized }
        if let object, let text = Self.errorText(object) { throw ScriptsClientError.firmware(text) }
        throw ScriptsClientError.http(reply.status)
    }

    /// Size of the tunnel request with the worst-case request id.
    static func tunnelRequestBytes(path: String, body: [String: Any]) -> Int {
        BLETransport.tunnelPayload(path: path, body: body, id: ScriptChunkPlanner.worstCaseRequestId)?.count ?? Int.max
    }

    private func decode<T: Decodable>(_ type: T.Type, from reply: ScriptsWireReply, what: String) throws -> T {
        do {
            return try JSONDecoder().decode(T.self, from: reply.body)
        } catch {
            throw ScriptsClientError.malformed(what)
        }
    }

    static func errorText(_ object: [String: Any]) -> String? {
        if let text = object["error"] as? String, !text.isEmpty { return text }
        if let text = object["err"] as? String, !text.isEmpty { return text }
        return nil
    }
}
