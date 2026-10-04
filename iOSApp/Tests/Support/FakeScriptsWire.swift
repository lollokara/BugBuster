import Foundation
@testable import BugBuster

/// Scripted `ScriptsWire`: records every call and answers through `handler`.
final class FakeScriptsWire: ScriptsWire {
    struct Call {
        let method: String
        let path: String
        let query: [String: String]
        let body: ScriptsWireBody?

        var jsonBody: [String: Any]? {
            if case .json(let object)? = body { return object }
            return nil
        }

        var textBody: String? {
            if case .text(let text)? = body { return text }
            return nil
        }
    }

    var kind: TransportKind
    private(set) var calls: [Call] = []
    var handler: (Call) throws -> ScriptsWireReply
    var asyncHandler: ((Call) async throws -> ScriptsWireReply)?

    init(kind: TransportKind = .wifi,
         handler: @escaping (Call) throws -> ScriptsWireReply = { _ in .json(200, ["ok": true]) }) {
        self.kind = kind
        self.handler = handler
        self.asyncHandler = nil
    }

    init(kind: TransportKind = .wifi,
         asyncHandler: @escaping (Call) async throws -> ScriptsWireReply) {
        self.kind = kind
        self.handler = { _ in .json(200, ["ok": true]) }
        self.asyncHandler = asyncHandler
    }

    func send(_ method: String, _ path: String, query: [String: String], body: ScriptsWireBody?) async throws -> ScriptsWireReply {
        let call = Call(method: method, path: path, query: query, body: body)
        calls.append(call)
        if let asyncHandler {
            return try await asyncHandler(call)
        }
        return try handler(call)
    }

    func paths() -> [String] { calls.map(\.path) }
}
