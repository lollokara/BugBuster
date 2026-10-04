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

/// One documented key of a returned dict (`Keys:` section of a docstring).
struct FirmwareKeyDoc: Decodable, Hashable {
    let name: String
    let doc: String
}

/// One documented exception (`Raises:` section).
struct FirmwareRaise: Decodable, Hashable {
    let type: String
    let doc: String
}

/// Structured docstring fields built by `stubs_to_json.py`. Every field is optional in
/// the JSON, so a catalogue written before they existed still decodes (all empty).
struct FirmwareDocDetail: Hashable {
    var summary = ""
    var description = ""
    var returnsDoc = ""
    var returnKeys: [FirmwareKeyDoc] = []
    var raises: [FirmwareRaise] = []
    /// Safety / side-effect notes.
    var notes = ""
    var examples: [String] = []

    static let empty = FirmwareDocDetail()
}

extension FirmwareDocDetail: Decodable {
    private enum CodingKeys: String, CodingKey {
        case summary, description, raises, notes, examples
        case returnsDoc = "returns_doc"
        case returnKeys = "return_keys"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        summary = try c.decodeIfPresent(String.self, forKey: .summary) ?? ""
        description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        returnsDoc = try c.decodeIfPresent(String.self, forKey: .returnsDoc) ?? ""
        returnKeys = try c.decodeIfPresent([FirmwareKeyDoc].self, forKey: .returnKeys) ?? []
        raises = try c.decodeIfPresent([FirmwareRaise].self, forKey: .raises) ?? []
        notes = try c.decodeIfPresent(String.self, forKey: .notes) ?? ""
        examples = try c.decodeIfPresent([String].self, forKey: .examples) ?? []
    }
}

/// First line of a raw docstring: the fallback summary for an older catalogue.
private func firstLine(_ doc: String) -> String {
    doc.split(separator: "\n", omittingEmptySubsequences: true).first.map(String.init) ?? ""
}

struct FirmwareModule: Equatable, Identifiable {
    var id: String { name }
    let name: String
    let doc: String
    let functions: [FirmwareFunction]
    let classes: [FirmwareClass]
    let constants: [FirmwareConstant]
    var detail: FirmwareDocDetail = .empty

    var summary: String { detail.summary.isEmpty ? firstLine(doc) : detail.summary }
}

extension FirmwareModule: Decodable {
    private enum CodingKeys: String, CodingKey { case name, doc, functions, classes, constants }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        doc = try c.decodeIfPresent(String.self, forKey: .doc) ?? ""
        functions = try c.decodeIfPresent([FirmwareFunction].self, forKey: .functions) ?? []
        classes = try c.decodeIfPresent([FirmwareClass].self, forKey: .classes) ?? []
        constants = try c.decodeIfPresent([FirmwareConstant].self, forKey: .constants) ?? []
        detail = try FirmwareDocDetail(from: decoder)
    }
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
    var detail: FirmwareDocDetail = .empty

    var summary: String { detail.summary.isEmpty ? firstLine(doc) : detail.summary }
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
        detail = try FirmwareDocDetail(from: decoder)
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

struct FirmwareFunction: Equatable, Identifiable {
    var id: String { name }
    let name: String
    let signature: String
    let params: [FirmwareParam]
    let returns: String?
    let doc: String
    var detail: FirmwareDocDetail = .empty

    var summary: String { detail.summary.isEmpty ? firstLine(doc) : detail.summary }
}

extension FirmwareFunction: Decodable {
    private enum CodingKeys: String, CodingKey { case name, signature, params, returns, doc }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        signature = try c.decode(String.self, forKey: .signature)
        params = try c.decodeIfPresent([FirmwareParam].self, forKey: .params) ?? []
        returns = try c.decodeIfPresent(String.self, forKey: .returns)
        doc = try c.decodeIfPresent(String.self, forKey: .doc) ?? ""
        detail = try FirmwareDocDetail(from: decoder)
    }
}

struct FirmwareParam: Hashable {
    enum Kind: String, Hashable {
        case positional
        case keywordOnly = "keyword_only"
        case varPositional = "var_positional"
        case varKeyword = "var_keyword"
    }

    let name: String
    let kind: Kind
    let annotation: String?
    let defaultValue: String?
    /// The `Args:` entry for this parameter ("" when undocumented).
    var doc: String = ""

    var acceptsKeyword: Bool { kind == .positional || kind == .keywordOnly }
    var isRequired: Bool { defaultValue == nil && acceptsKeyword }
}

extension FirmwareParam: Decodable {
    private enum CodingKeys: String, CodingKey {
        case name, kind, annotation, doc
        case defaultValue = "default"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        kind = Kind(rawValue: try c.decode(String.self, forKey: .kind)) ?? .positional
        annotation = try c.decodeIfPresent(String.self, forKey: .annotation)
        defaultValue = try c.decodeIfPresent(String.self, forKey: .defaultValue)
        doc = try c.decodeIfPresent(String.self, forKey: .doc) ?? ""
    }
}

struct FirmwareConstant: Equatable, Identifiable {
    var id: String { name }
    let name: String
    let annotation: String?
    let value: String?
    var doc: String = ""
}

extension FirmwareConstant: Decodable {
    private enum CodingKeys: String, CodingKey { case name, annotation, value, doc }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        annotation = try c.decodeIfPresent(String.self, forKey: .annotation)
        value = try c.decodeIfPresent(String.self, forKey: .value)
        doc = try c.decodeIfPresent(String.self, forKey: .doc) ?? ""
    }
}

struct FirmwareExample: Decodable, Equatable, Identifiable {
    var id: String { name }
    let name: String
    let title: String
    let doc: String
    let source: String
}
