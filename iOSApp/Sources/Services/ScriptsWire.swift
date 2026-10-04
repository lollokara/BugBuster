import Foundation

/// Request body for one scripts-API call.
enum ScriptsWireBody {
    /// JSON object: the HTTP `application/json` body, or the BLE tunnel `body`.
    case json([String: Any])
    /// Raw `text/plain` (script upload, lint over HTTP). The tunnel cannot carry it.
    case text(String)
}

/// What came back, with the HTTP status the webserver uses for this route.
/// Over BLE the status is reconstructed by `ScriptsTunnelStatus`.
struct ScriptsWireReply {
    let status: Int
    /// Lower-cased header names; empty over BLE.
    let headers: [String: String]
    let body: Data

    init(status: Int, headers: [String: String] = [:], body: Data) {
        self.status = status
        self.headers = Dictionary(headers.map { ($0.key.lowercased(), $0.value) }, uniquingKeysWith: { first, _ in first })
        self.body = body
    }

    var isSuccess: Bool { (200...299).contains(status) }

    var json: [String: Any]? { try? JSONSerialization.jsonObject(with: body) as? [String: Any] }

    func header(_ name: String) -> String? { headers[name.lowercased()] }

    static func json(_ status: Int, _ object: [String: Any], headers: [String: String] = [:]) -> ScriptsWireReply {
        let data = (try? JSONSerialization.data(withJSONObject: object, options: [.withoutEscapingSlashes])) ?? Data()
        return ScriptsWireReply(status: status, headers: headers, body: data)
    }

    static func text(_ status: Int, _ text: String, headers: [String: String] = [:]) -> ScriptsWireReply {
        ScriptsWireReply(status: status, headers: headers, body: Data(text.utf8))
    }
}

/// The one seam between the Scripts feature and a transport. `ScriptsClient` reads
/// `kind` to pick the framing (raw text vs base64 JSON pages).
protocol ScriptsWire: AnyObject {
    var kind: TransportKind { get }
    func send(_ method: String, _ path: String, query: [String: String], body: ScriptsWireBody?) async throws -> ScriptsWireReply
}

/// HTTP status the webserver would send for a tunnelled `api_core` reply
/// (`send_scripts_result`, webserver.cpp): a string `running` means the single
/// file-script slot is taken (409), any `error` is 400, everything else 200.
enum ScriptsTunnelStatus {
    static func status(for body: Data) -> Int {
        guard let object = try? JSONSerialization.jsonObject(with: body) as? [String: Any] else { return 200 }
        if object["running"] is String { return 409 }
        if object["error"] != nil { return 400 }
        return 200
    }
}

/// `ScriptsWire` over the live connection: HTTP on Wi-Fi, the API tunnel on BLE.
/// Reads `transport` on every call, so a Wi-Fi → BLE switch needs no rebuild.
/// Holds the manager strongly: it never owns a wire, so there is no cycle, and a
/// wire whose manager vanished would otherwise report the wrong `kind`.
final class ConnectionScriptsWire: ScriptsWire {
    private let cm: ConnectionManager

    init(_ cm: ConnectionManager) {
        self.cm = cm
    }

    var kind: TransportKind { cm.transport }

    func send(_ method: String, _ path: String, query: [String: String], body: ScriptsWireBody?) async throws -> ScriptsWireReply {
        switch cm.transport {
        case .wifi:
            var data: Data?
            var contentType: String?
            switch body {
            case .json(let object)?:
                data = try JSONSerialization.data(withJSONObject: object)
                contentType = "application/json"
            case .text(let text)?:
                data = Data(text.utf8)
                contentType = "text/plain"
            case nil:
                break
            }
            let r = try await cm.httpExchange(method: method, path: path, query: query,
                                              body: data, contentType: contentType, timeout: 10)
            return ScriptsWireReply(status: r.status, headers: r.headers, body: r.body)
        case .ble:
            var tunnelBody: [String: Any]?
            switch body {
            case .json(let object)?: tunnelBody = object
            case .text?: throw ConnectionAPIError.needsWiFi("Sending raw script text")
            case nil: break
            }
            let tunnelPath = ConnectionManager.bleTunnelPath(path, query: query)
            guard let data = await cm.bleAPI.apiRequest(path: tunnelPath, body: tunnelBody, timeout: 8.0) else {
                throw ConnectionAPIError.bleNoResponse(path)
            }
            return ScriptsWireReply(status: ScriptsTunnelStatus.status(for: data), body: data)
        }
    }
}
