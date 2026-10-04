import Foundation

/// One row of the docs browser: a module, class, namespace, function, method,
/// constant or example from the bundled catalogue.
struct FirmwareDocEntry: Identifiable, Hashable {
    enum Kind: String, Hashable {
        case module, type, namespace, function, method, constant, example
    }

    /// Catalogue path (`daq.run.new`, `bugbuster.Channel.set_voltage`) or `example:<file>`.
    let id: String
    let kind: Kind
    /// Owning module ("" for examples).
    let module: String
    let title: String
    let signature: String
    let doc: String
    /// What "Insert" puts at the caret: a call with the required arguments, or an example's source.
    let snippet: String
    /// Module the snippet references, added as `import <module>` when the script lacks it.
    let requiredImport: String?
}

struct FirmwareDocSection: Identifiable, Equatable {
    let title: String
    let entries: [FirmwareDocEntry]

    var id: String { title }
}

enum FirmwareDocs {
    static func entries(_ catalogue: FirmwareAPICatalogue) -> [FirmwareDocEntry] {
        var out: [FirmwareDocEntry] = []
        for m in catalogue.modules {
            out.append(FirmwareDocEntry(id: m.name, kind: .module, module: m.name, title: m.name,
                                        signature: "import \(m.name)", doc: m.doc,
                                        snippet: "import \(m.name)\n", requiredImport: nil))
            for fn in m.functions {
                let path = "\(m.name).\(fn.name)"
                out.append(FirmwareDocEntry(id: path, kind: .function, module: m.name, title: path,
                                            signature: "\(m.name).\(fn.signature)", doc: fn.doc,
                                            snippet: callSnippet(path, fn), requiredImport: m.name))
            }
            for cls in m.classes {
                let path = "\(m.name).\(cls.name)"
                let isNamespace = cls.kind == .namespace
                if isNamespace {
                    out.append(FirmwareDocEntry(id: path, kind: .namespace, module: m.name, title: path,
                                                signature: path, doc: cls.doc, snippet: path, requiredImport: m.name))
                } else {
                    out.append(FirmwareDocEntry(id: path, kind: .type, module: m.name, title: path,
                                                signature: "\(m.name).\(cls.constructorSignature)", doc: cls.doc,
                                                snippet: callSnippet(path, cls.constructor), requiredImport: m.name))
                }
                // Instance methods read naturally on a lower-cased receiver: `channel.set_voltage(voltage)`.
                let receiver = isNamespace ? path : cls.name.lowercased()
                for fn in cls.methods where !fn.name.hasPrefix("_") {
                    out.append(FirmwareDocEntry(id: "\(path).\(fn.name)", kind: .method, module: m.name,
                                                title: isNamespace ? "\(path).\(fn.name)" : "\(cls.name).\(fn.name)",
                                                signature: "\(receiver).\(fn.signature)", doc: fn.doc,
                                                snippet: callSnippet("\(receiver).\(fn.name)", fn),
                                                requiredImport: isNamespace ? m.name : nil))
                }
                for constant in cls.constants {
                    out.append(constantEntry(constant, owner: path, module: m.name))
                }
            }
            for constant in m.constants {
                out.append(constantEntry(constant, owner: m.name, module: m.name))
            }
        }
        for ex in catalogue.examples {
            out.append(FirmwareDocEntry(id: "example:\(ex.name)", kind: .example, module: "", title: ex.title,
                                        signature: ex.name, doc: ex.doc, snippet: ex.source, requiredImport: nil))
        }
        return out
    }

    /// Lower score is better: 0 name prefix, 1 name contains, 2 signature contains, 3 doc contains.
    static func search(_ entries: [FirmwareDocEntry], query: String) -> [FirmwareDocEntry] {
        let q = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !q.isEmpty else { return entries }
        func score(_ e: FirmwareDocEntry) -> Int? {
            let title = e.title.lowercased()
            let last = title.split(separator: ".").last.map(String.init) ?? title
            if last.hasPrefix(q) || title.hasPrefix(q) { return 0 }
            if title.contains(q) { return 1 }
            if e.signature.lowercased().contains(q) { return 2 }
            if e.doc.lowercased().contains(q) { return 3 }
            return nil
        }
        return entries
            .compactMap { e in score(e).map { (entry: e, score: $0) } }
            .sorted { $0.score != $1.score ? $0.score < $1.score : $0.entry.title.lowercased() < $1.entry.title.lowercased() }
            .map { $0.entry }
    }

    /// Modules in catalogue order with Examples last; a query gives one "Results" section (≤ 100 rows).
    static func sections(_ entries: [FirmwareDocEntry], query: String) -> [FirmwareDocSection] {
        if !query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            let results = search(entries, query: query)
            return results.isEmpty ? [] : [FirmwareDocSection(title: "Results", entries: Array(results.prefix(100)))]
        }
        var order: [String] = []
        var groups: [String: [FirmwareDocEntry]] = [:]
        for e in entries {
            let key = e.kind == .example ? "Examples" : e.module
            if groups[key] == nil { order.append(key) }
            groups[key, default: []].append(e)
        }
        return order.map { FirmwareDocSection(title: $0, entries: groups[$0] ?? []) }
    }

    /// `callee(req1, req2, kwreq=kwreq)`: only the arguments without defaults.
    static func callSnippet(_ callee: String, _ function: FirmwareFunction?) -> String {
        let args = (function?.params ?? []).compactMap { p -> String? in
            guard p.isRequired else { return nil }
            return p.kind == .keywordOnly ? "\(p.name)=\(p.name)" : p.name
        }
        return "\(callee)(\(args.joined(separator: ", ")))"
    }

    /// True when `text` has no `import <module>` (also inside a comma list or with `as`)
    /// and no `from <module> import`.
    static func needsImport(_ module: String, in text: String) -> Bool {
        let m = NSRegularExpression.escapedPattern(for: module)
        let pattern = #"(?m)^\s*(?:import\s+(?:[\w.]+(?:\s+as\s+\w+)?\s*,\s*)*\#(m)(?:\s+as\s+\w+)?\s*(?:,|$)|from\s+\#(m)\s+import\b)"#
        guard let re = try? NSRegularExpression(pattern: pattern) else { return true }
        return re.firstMatch(in: text, range: NSRange(location: 0, length: (text as NSString).length)) == nil
    }

    private static func constantEntry(_ constant: FirmwareConstant, owner: String, module: String) -> FirmwareDocEntry {
        let path = "\(owner).\(constant.name)"
        return FirmwareDocEntry(id: path, kind: .constant, module: module, title: path,
                                signature: constant.annotation.map { "\(path): \($0)" } ?? path,
                                doc: "", snippet: path, requiredImport: module)
    }
}
