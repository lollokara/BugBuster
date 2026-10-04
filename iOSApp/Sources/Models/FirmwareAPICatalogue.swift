import Foundation
import os

/// The on-device MicroPython API, built from the `.pyi` stubs by
/// `python/tools/stubs_to_json.py` (schema `bugbuster.firmware-api/1`) and
/// bundled as `firmware_api.json`. Single source for the editor's
/// autocomplete and the docs browser.
struct FirmwareAPICatalogue: Decodable, Equatable {
    static let supportedSchema = "bugbuster.firmware-api/1"

    struct UnsupportedSchema: Error, Equatable {
        let schema: String
    }

    let schema: String
    let modules: [FirmwareModule]
    let examples: [FirmwareExample]

    func module(named name: String) -> FirmwareModule? {
        modules.first { $0.name == name }
    }

    static func decode(_ data: Data) throws -> FirmwareAPICatalogue {
        let catalogue = try JSONDecoder().decode(FirmwareAPICatalogue.self, from: data)
        guard catalogue.schema == supportedSchema else {
            throw UnsupportedSchema(schema: catalogue.schema)
        }
        return catalogue
    }

    /// The catalogue shipped in the app, or nil when the resource is missing or
    /// unreadable: autocomplete and docs then stay empty instead of crashing.
    static let bundled: FirmwareAPICatalogue? = load(bundle: .main)

    static func load(bundle: Bundle) -> FirmwareAPICatalogue? {
        guard let url = bundle.url(forResource: "firmware_api", withExtension: "json") else {
            log.error("firmware_api.json is not in bundle \(bundle.bundlePath, privacy: .public)")
            return nil
        }
        do {
            return try decode(Data(contentsOf: url))
        } catch {
            log.error("firmware_api.json: \(String(describing: error), privacy: .public)")
            return nil
        }
    }

    private static let log = Logger(subsystem: "com.lorenzo.bugbuster", category: "scripts")
}

struct FirmwareModule: Decodable, Equatable, Identifiable {
    var id: String { name }
    let name: String
    let doc: String
    let functions: [FirmwareFunction]
    let classes: [FirmwareClass]
    let constants: [FirmwareConstant]
}

struct FirmwareClass: Equatable, Identifiable {
    enum Kind: String, Equatable {
        case `class`
        /// A module-level object used as a function group, e.g. `daq.run`.
        case namespace
    }

    var id: String { name }
    let name: String
    let kind: Kind
    let doc: String
    let methods: [FirmwareFunction]
    let constants: [FirmwareConstant]
}

extension FirmwareClass: Decodable {
    private enum CodingKeys: String, CodingKey { case name, kind, doc, methods, constants }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        // A kind added to stubs_to_json later degrades to a plain class.
        kind = Kind(rawValue: try c.decode(String.self, forKey: .kind)) ?? .class
        doc = try c.decodeIfPresent(String.self, forKey: .doc) ?? ""
        methods = try c.decodeIfPresent([FirmwareFunction].self, forKey: .methods) ?? []
        constants = try c.decodeIfPresent([FirmwareConstant].self, forKey: .constants) ?? []
    }
}

extension FirmwareClass {
    /// `__init__` of a class (what `Name(...)` calls); nil for namespaces.
    var constructor: FirmwareFunction? {
        kind == .class ? methods.first { $0.name == "__init__" } : nil
    }

    /// `__init__(channel: int) -> None` shown as `Channel(channel: int)`.
    var constructorSignature: String {
        guard let ctor = constructor else { return "\(name)()" }
        var sig = ctor.signature
        if sig.hasPrefix("__init__") { sig = name + sig.dropFirst("__init__".count) }
        if sig.hasSuffix(" -> None") { sig = String(sig.dropLast(" -> None".count)) }
        return sig
    }
}

struct FirmwareFunction: Decodable, Equatable, Identifiable {
    var id: String { name }
    let name: String
    let signature: String
    let params: [FirmwareParam]
    let returns: String?
    let doc: String
}

struct FirmwareParam: Equatable {
    enum Kind: String, Equatable {
        case positional
        case keywordOnly = "keyword_only"
        case varPositional = "var_positional"
        case varKeyword = "var_keyword"
    }

    let name: String
    let kind: Kind
    let annotation: String?
    let defaultValue: String?

    var acceptsKeyword: Bool { kind == .positional || kind == .keywordOnly }
    var isRequired: Bool { defaultValue == nil && acceptsKeyword }
}

extension FirmwareParam: Decodable {
    private enum CodingKeys: String, CodingKey {
        case name, kind, annotation
        case defaultValue = "default"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        kind = Kind(rawValue: try c.decode(String.self, forKey: .kind)) ?? .positional
        annotation = try c.decodeIfPresent(String.self, forKey: .annotation)
        defaultValue = try c.decodeIfPresent(String.self, forKey: .defaultValue)
    }
}

struct FirmwareConstant: Decodable, Equatable, Identifiable {
    var id: String { name }
    let name: String
    let annotation: String?
    let value: String?
}

struct FirmwareExample: Decodable, Equatable, Identifiable {
    var id: String { name }
    let name: String
    let title: String
    let doc: String
    let source: String
}
