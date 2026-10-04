import Foundation

enum ScriptCompletionKind: String, Equatable {
    case module, function, method, type, namespace, constant, keyword
}

struct ScriptCompletion: Equatable, Identifiable {
    let label: String
    let insertText: String
    /// Caret position inside `insertText` after acceptance (UTF-16 units).
    let caretOffset: Int
    let detail: String
    let doc: String
    let kind: ScriptCompletionKind

    var id: String { "\(kind.rawValue):\(label)" }
}

struct ScriptCompletionResult: Equatable {
    /// Document range (UTF-16) that an accepted item replaces: the typed partial.
    let replaceRange: NSRange
    let items: [ScriptCompletion]

    static let empty = ScriptCompletionResult(replaceRange: NSRange(location: 0, length: 0), items: [])

    var isEmpty: Bool { items.isEmpty }
}

struct ScriptCompletionCandidate {
    let item: ScriptCompletion
    /// Tie-break after the match score (see ScriptCompletionRanker).
    let order: Int
}

enum ScriptCompletionContext: Equatable {
    case member(receiver: String, partial: String)
    case argument(callee: String, partial: String, positionalCount: Int, usedKeywords: Set<String>)
    case importModule(partial: String)
    case identifier(partial: String)
    case nothing
}

// MARK: - Catalogue index

struct FirmwareAPIIndex {
    enum Target: Equatable {
        /// `daq`
        case module(String)
        /// Static access to a class or namespace: `bugbuster.Channel`, `daq.run`.
        case type(String)
        /// A value of class `path` (`ch = bugbuster.Channel(0)`).
        case instance(String)
    }

    let catalogue: FirmwareAPICatalogue

    init(_ catalogue: FirmwareAPICatalogue) {
        self.catalogue = catalogue
    }

    var moduleNames: [String] { catalogue.modules.map(\.name) }

    func module(_ name: String) -> FirmwareModule? { catalogue.module(named: name) }

    /// `"m.C"` → class or namespace `C` of module `m`.
    func type(_ path: String) -> FirmwareClass? {
        let parts = path.split(separator: ".").map(String.init)
        guard parts.count == 2 else { return nil }
        return module(parts[0])?.classes.first { $0.name == parts[1] }
    }

    func target(forQualified path: String) -> Target? {
        let parts = path.split(separator: ".").map(String.init)
        if parts.count == 1, module(parts[0]) != nil { return .module(parts[0]) }
        if parts.count == 2, type(path) != nil { return .type(path) }
        return nil
    }

    /// A dotted receiver as written in the script: `daq`, `daq.run`, `bb`, `ch`.
    func resolve(_ expr: String, bindings: ScriptBindings) -> Target? {
        let parts = expr.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
        guard let head = parts.first, !parts.contains("") else { return nil }
        var current: Target
        if let cls = bindings.instances[head] {
            current = .instance(cls)
        } else if let qualified = bindings.aliases[head], let target = target(forQualified: qualified) {
            current = target
        } else if module(head) != nil {
            current = .module(head)
        } else {
            return nil
        }
        for name in parts.dropFirst() {
            guard case .module(let m) = current,
                  module(m)?.classes.contains(where: { $0.name == name }) == true else { return nil }
            current = .type("\(m).\(name)")
        }
        return current
    }

    /// The function behind a callee: `daq.vdut`, `ch.set_voltage`, `bugbuster.Channel` (its `__init__`), `vdut`.
    func callable(_ expr: String, bindings: ScriptBindings) -> FirmwareFunction? {
        if let qualified = bindings.aliases[expr] { return callable(qualified: qualified) }
        guard let dot = expr.lastIndex(of: ".") else { return nil }
        let owner = String(expr[..<dot])
        let name = String(expr[expr.index(after: dot)...])
        guard let target = resolve(owner, bindings: bindings) else { return nil }
        switch target {
        case .module(let m):
            return function(named: name, inModule: m)
        case .type(let path), .instance(let path):
            return type(path)?.methods.first { $0.name == name }
        }
    }

    /// A catalogue path to its callable: `daq.vdut`, `bugbuster.Channel`, `daq.run.new`.
    func callable(qualified: String) -> FirmwareFunction? {
        let parts = qualified.split(separator: ".").map(String.init)
        switch parts.count {
        case 2: return function(named: parts[1], inModule: parts[0])
        case 3: return type("\(parts[0]).\(parts[1])")?.methods.first { $0.name == parts[2] }
        default: return nil
        }
    }

    /// Class of the value returned by calling `qualified` (`"m.f"` or `"m.C"`): a class
    /// constructor, or a function whose return annotation names a class of its module.
    func instanceType(producedBy qualified: String) -> String? {
        let parts = qualified.split(separator: ".").map(String.init)
        guard parts.count == 2, let mod = module(parts[0]) else { return nil }
        if let cls = mod.classes.first(where: { $0.name == parts[1] }) {
            return cls.kind == .class ? qualified : nil
        }
        guard let returns = mod.functions.first(where: { $0.name == parts[1] })?.returns else { return nil }
        let bare = returns.replacingOccurrences(of: " | None", with: "").trimmingCharacters(in: .whitespaces)
        return mod.classes.contains { $0.name == bare && $0.kind == .class } ? "\(parts[0]).\(bare)" : nil
    }

    func members(of target: Target) -> [ScriptCompletionCandidate] {
        switch target {
        case .module(let m):
            guard let mod = module(m) else { return [] }
            var out: [ScriptCompletionCandidate] = []
            for cls in mod.classes {
                if cls.kind == .namespace {
                    out.append(.init(item: ScriptCompletion(label: cls.name, insertText: cls.name,
                                                            caretOffset: cls.name.utf16.count, detail: "namespace",
                                                            doc: cls.doc, kind: .namespace), order: 0))
                } else {
                    out.append(.init(item: Self.callItem(label: cls.name, function: cls.constructor,
                                                         detail: cls.constructorSignature, doc: cls.doc, kind: .type), order: 0))
                }
            }
            for fn in mod.functions {
                out.append(.init(item: Self.callItem(label: fn.name, function: fn, detail: fn.signature, doc: fn.doc, kind: .function),
                                 order: 1000))
            }
            for constant in mod.constants { out.append(.init(item: Self.constantItem(constant), order: 2000)) }
            return out
        case .type(let path), .instance(let path):
            guard let cls = type(path) else { return [] }
            var out: [ScriptCompletionCandidate] = []
            for fn in cls.methods where fn.name != "__init__" {
                out.append(.init(item: Self.callItem(label: fn.name, function: fn, detail: fn.signature, doc: fn.doc, kind: .method),
                                 order: 1000))
            }
            for constant in cls.constants { out.append(.init(item: Self.constantItem(constant), order: 2000)) }
            return out
        }
    }

    /// `name()` with the caret inside the parens when the callable takes arguments.
    static func callItem(label: String, function: FirmwareFunction?, detail: String, doc: String,
                         kind: ScriptCompletionKind) -> ScriptCompletion {
        let takesArgs = function?.params.contains { $0.name != "self" } ?? false
        return ScriptCompletion(label: label, insertText: label + "()",
                                caretOffset: label.utf16.count + (takesArgs ? 1 : 2),
                                detail: detail, doc: doc, kind: kind)
    }

    static func constantItem(_ constant: FirmwareConstant) -> ScriptCompletion {
        ScriptCompletion(label: constant.name, insertText: constant.name, caretOffset: constant.name.utf16.count,
                         detail: constant.annotation ?? "constant", doc: "", kind: .constant)
    }

    private func function(named name: String, inModule m: String) -> FirmwareFunction? {
        guard let mod = module(m) else { return nil }
        if let fn = mod.functions.first(where: { $0.name == name }) { return fn }
        return mod.classes.first { $0.name == name }?.constructor
    }
}

// MARK: - What the script binds

/// Names a script binds to catalogue objects, found with line regexes (no parser):
/// imports, `from … import …`, `x = mod.Class(…)`, `with mod.f(…) as x:`.
struct ScriptBindings: Equatable {
    /// local name → catalogue path (`bb` → `bugbuster`, `P` → `machine.Pin`, `vdut` → `daq.vdut`)
    var aliases: [String: String] = [:]
    /// local variable → class path of the value it holds (`ch` → `bugbuster.Channel`)
    var instances: [String: String] = [:]

    private static let assignment = try! NSRegularExpression(pattern: #"^([A-Za-z_]\w*)\s*=\s*([A-Za-z_][\w.]*)\s*\("#)
    private static let withAs = try! NSRegularExpression(pattern: #"^with\s+([A-Za-z_][\w.]*)\s*\(.*\)\s+as\s+([A-Za-z_]\w*)\s*:"#)

    static func scan(_ text: String, index: FirmwareAPIIndex) -> ScriptBindings {
        var b = ScriptBindings()
        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("import ") {
                for item in line.dropFirst("import ".count).split(separator: ",") {
                    let words = item.split(separator: " ").map(String.init)
                    guard let path = words.first else { continue }
                    if words.count == 3, words[1] == "as" {
                        b.aliases[words[2]] = path
                    } else if words.count == 1, let head = path.split(separator: ".").first {
                        b.aliases[String(head)] = String(head)
                    }
                }
            } else if line.hasPrefix("from "), let importRange = line.range(of: " import ") {
                let module = line[line.index(line.startIndex, offsetBy: 5)..<importRange.lowerBound]
                    .trimmingCharacters(in: .whitespaces)
                let names = line[importRange.upperBound...]
                    .replacingOccurrences(of: "(", with: "")
                    .replacingOccurrences(of: ")", with: "")
                for item in names.split(separator: ",") {
                    let words = item.split(separator: " ").map(String.init)
                    guard let name = words.first else { continue }
                    if name == "*", let mod = index.module(module) {
                        let members = mod.functions.map(\.name) + mod.classes.map(\.name) + mod.constants.map(\.name)
                        for member in members { b.aliases[member] = "\(module).\(member)" }
                    } else if words.count == 3, words[1] == "as" {
                        b.aliases[words[2]] = "\(module).\(name)"
                    } else if words.count == 1 {
                        b.aliases[name] = "\(module).\(name)"
                    }
                }
            } else if let (variable, callee) = Self.boundCall(line) {
                if let cls = index.instanceType(producedBy: b.qualify(callee)) { b.instances[variable] = cls }
            }
        }
        return b
    }

    /// `bb.Channel` → `bugbuster.Channel`; `Channel` (from-imported) → `bugbuster.Channel`.
    func qualify(_ dotted: String) -> String {
        var parts = dotted.split(separator: ".").map(String.init)
        guard let head = parts.first, let alias = aliases[head] else { return dotted }
        parts[0] = alias
        return parts.joined(separator: ".")
    }

    /// (variable, callee) for `x = callee(` and `with callee(…) as x:`.
    private static func boundCall(_ line: String) -> (String, String)? {
        let range = NSRange(location: 0, length: (line as NSString).length)
        if let m = assignment.firstMatch(in: line, range: range) {
            let ns = line as NSString
            return (ns.substring(with: m.range(at: 1)), ns.substring(with: m.range(at: 2)))
        }
        if let m = withAs.firstMatch(in: line, range: range) {
            let ns = line as NSString
            return (ns.substring(with: m.range(at: 2)), ns.substring(with: m.range(at: 1)))
        }
        return nil
    }
}

// MARK: - Where the caret is

enum ScriptCompletionContextFinder {
    private static let fromImport = try! NSRegularExpression(
        pattern: #"^\s*from\s+([A-Za-z_][\w.]*)\s+import\s+\(?\s*(?:[A-Za-z_]\w*(?:\s+as\s+\w+)?\s*,\s*)*$"#)
    private static let importList = try! NSRegularExpression(
        pattern: #"^\s*(?:import\s+(?:[A-Za-z_][\w.]*(?:\s+as\s+\w+)?\s*,\s*)*|from\s+)$"#)
    private static let keywordArgument = try! NSRegularExpression(pattern: #"^\s*([A-Za-z_]\w*)\s*=(?!=)"#)

    /// The completion context at `caret` (UTF-16) and the range of the typed partial.
    static func find(text: String, caret: Int) -> (ScriptCompletionContext, NSRange) {
        let ns = text as NSString
        let c = min(max(0, caret), ns.length)
        let prefix = ns.substring(to: c)
        let partial = trailing(prefix, allowDots: false)
        let range = NSRange(location: c - partial.utf16.count, length: partial.utf16.count)
        let before = String(prefix.dropLast(partial.count))
        let line = String(before.split(separator: "\n", omittingEmptySubsequences: false).last ?? "")

        if isInsideStringOrComment(line) || partial.first?.isNumber == true { return (.nothing, range) }

        if before.hasSuffix(".") {
            let receiver = trailing(String(before.dropLast()), allowDots: true)
            guard let first = receiver.first, first.isLetter || first == "_" else { return (.nothing, range) }
            return (.member(receiver: receiver, partial: partial), range)
        }
        if let module = firstCapture(fromImport, in: line) { return (.member(receiver: module, partial: partial), range) }
        if matches(importList, line) { return (.importModule(partial: partial), range) }
        if let call = enclosingCall(before) {
            return (.argument(callee: call.callee, partial: partial, positionalCount: call.positional,
                              usedKeywords: call.keywords), range)
        }
        if partial.count >= 2 { return (.identifier(partial: partial), range) }
        return (.nothing, range)
    }

    /// Trailing `[A-Za-z0-9_]` run (plus `.` when `allowDots`). ASCII only, so
    /// character count == UTF-16 count.
    static func trailing(_ s: String, allowDots: Bool) -> String {
        String(s.reversed().prefix { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_" || (allowDots && $0 == ".")) }.reversed())
    }

    /// Single-line scan: an open quote or a `#` outside quotes. (Triple-quoted
    /// strings spanning lines are not tracked; completion may show inside them.)
    static func isInsideStringOrComment(_ line: String) -> Bool {
        var quote: Character?
        var escaped = false
        for ch in line {
            if let q = quote {
                if escaped { escaped = false } else if ch == "\\" { escaped = true } else if ch == q { quote = nil }
            } else if ch == "#" {
                return true
            } else if ch == "\"" || ch == "'" {
                quote = ch
            }
        }
        return quote != nil
    }

    struct Call: Equatable {
        let callee: String
        let positional: Int
        let keywords: Set<String>
    }

    /// The innermost unclosed `callee(` before the caret when the caret sits at the
    /// start of an argument. Looks back at most 2000 characters, across lines.
    static func enclosingCall(_ before: String) -> Call? {
        let window = Array(before.suffix(2000))
        guard let open = openParenIndex(window) else { return nil }
        let callee = trailing(String(window[..<open]), allowDots: true)
        guard let first = callee.first, first.isLetter || first == "_" else { return nil }
        let args = splitTopLevel(Array(window[(open + 1)...]))
        guard let current = args.last, current.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
        var positional = 0
        var keywords = Set<String>()
        for arg in args.dropLast() {
            if let keyword = firstCapture(keywordArgument, in: arg) {
                keywords.insert(keyword)
            } else if !arg.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                positional += 1
            }
        }
        return Call(callee: callee, positional: positional, keywords: keywords)
    }

    private static func openParenIndex(_ chars: [Character]) -> Int? {
        var stack: [(Character, Int)] = []
        var quote: Character?
        var escaped = false
        var comment = false
        for (i, ch) in chars.enumerated() {
            if comment {
                if ch == "\n" { comment = false }
                continue
            }
            if let q = quote {
                if escaped { escaped = false } else if ch == "\\" { escaped = true } else if ch == q || ch == "\n" { quote = nil }
                continue
            }
            switch ch {
            case "#": comment = true
            case "\"", "'": quote = ch
            case "(", "[", "{": stack.append((ch, i))
            case ")", "]", "}": if !stack.isEmpty { stack.removeLast() }
            default: break
            }
        }
        guard let last = stack.last, last.0 == "(" else { return nil }
        return last.1
    }

    private static func splitTopLevel(_ chars: [Character]) -> [String] {
        var parts: [String] = []
        var current = ""
        var depth = 0
        var quote: Character?
        var escaped = false
        for ch in chars {
            if let q = quote {
                current.append(ch)
                if escaped { escaped = false } else if ch == "\\" { escaped = true } else if ch == q { quote = nil }
                continue
            }
            switch ch {
            case "\"", "'": quote = ch; current.append(ch)
            case "(", "[", "{": depth += 1; current.append(ch)
            case ")", "]", "}": depth -= 1; current.append(ch)
            case "," where depth == 0: parts.append(current); current = ""
            default: current.append(ch)
            }
        }
        parts.append(current)
        return parts
    }

    private static func firstCapture(_ re: NSRegularExpression, in s: String) -> String? {
        guard let m = re.firstMatch(in: s, range: NSRange(location: 0, length: (s as NSString).length)),
              m.numberOfRanges > 1, m.range(at: 1).location != NSNotFound else { return nil }
        return (s as NSString).substring(with: m.range(at: 1))
    }

    private static func matches(_ re: NSRegularExpression, _ s: String) -> Bool {
        re.firstMatch(in: s, range: NSRange(location: 0, length: (s as NSString).length)) != nil
    }
}

// MARK: - Ranking

/// Lower is better. Score: 0 case-sensitive prefix, 1 case-insensitive prefix,
/// 2 prefix of a later `_` segment. Then `order`, then the lower-cased label.
/// `_`-prefixed names only show when the partial starts with `_`.
enum ScriptCompletionRanker {
    static func score(_ label: String, partial: String) -> Int? {
        if partial.isEmpty || label.hasPrefix(partial) { return 0 }
        let l = label.lowercased(), p = partial.lowercased()
        if l.hasPrefix(p) { return 1 }
        if l.split(separator: "_").dropFirst().contains(where: { $0.hasPrefix(p) }) { return 2 }
        return nil
    }

    static func rank(_ candidates: [ScriptCompletionCandidate], partial: String, limit: Int) -> [ScriptCompletion] {
        let showPrivate = partial.hasPrefix("_")
        let scored: [(item: ScriptCompletion, score: Int, order: Int)] = candidates.compactMap { c in
            if c.item.label.hasPrefix("_") && !showPrivate { return nil }
            guard let s = score(c.item.label, partial: partial) else { return nil }
            return (c.item, s, c.order)
        }
        let sorted = scored.sorted { a, b in
            if a.score != b.score { return a.score < b.score }
            if a.order != b.order { return a.order < b.order }
            return a.item.label.lowercased() < b.item.label.lowercased()
        }
        return sorted.prefix(limit).map { $0.item }
    }
}

// MARK: - Engine

struct ScriptCompletionEngine {
    let index: FirmwareAPIIndex

    static let bundled: ScriptCompletionEngine? =
        FirmwareAPICatalogue.bundled.map { ScriptCompletionEngine(index: FirmwareAPIIndex($0)) }

    func complete(text: String, caret: Int, limit: Int = 40) -> ScriptCompletionResult {
        let (context, range) = ScriptCompletionContextFinder.find(text: text, caret: caret)
        if context == .nothing { return .empty }
        let bindings = ScriptBindings.scan(text, index: index)
        let partial: String
        var candidates: [ScriptCompletionCandidate]
        switch context {
        case .nothing:
            return .empty
        case .member(let receiver, let p):
            partial = p
            guard let target = index.resolve(receiver, bindings: bindings) else { return .empty }
            candidates = index.members(of: target)
        case .importModule(let p):
            partial = p
            candidates = index.moduleNames.map { .init(item: moduleItem($0, label: $0), order: 0) }
        case .argument(let callee, let p, let positional, let used):
            partial = p
            candidates = index.callable(callee, bindings: bindings)
                .map { Self.keywordItems($0, positionalCount: positional, used: used) } ?? []
            if ScriptCompletionRanker.rank(candidates, partial: p, limit: 1).isEmpty {
                guard p.count >= 2 else { return .empty }
                candidates = identifierCandidates(bindings)     // a positional name, not a kwarg
            }
        case .identifier(let p):
            partial = p
            candidates = identifierCandidates(bindings)
        }
        let items = ScriptCompletionRanker.rank(candidates, partial: partial, limit: limit)
        if items.isEmpty || (items.count == 1 && items[0].insertText == partial) { return .empty }
        return ScriptCompletionResult(replaceRange: range, items: items)
    }

    /// `name=` items for the parameters still open in a call.
    static func keywordItems(_ fn: FirmwareFunction, positionalCount: Int, used: Set<String>) -> [ScriptCompletionCandidate] {
        var out: [ScriptCompletionCandidate] = []
        var positionalSeen = 0
        for (i, param) in fn.params.enumerated() where param.name != "self" {
            if param.kind == .positional {
                positionalSeen += 1
                if positionalSeen <= positionalCount { continue }      // already passed by position
            }
            guard param.acceptsKeyword, !used.contains(param.name) else { continue }
            let label = param.name + "="
            var detail = param.annotation ?? ""
            if let value = param.defaultValue { detail += detail.isEmpty ? "= \(value)" : " = \(value)" }
            out.append(.init(item: ScriptCompletion(label: label, insertText: label, caretOffset: label.utf16.count,
                                                    detail: detail, doc: fn.doc, kind: .keyword),
                             order: (param.isRequired ? 1000 : 0) + i))
        }
        return out
    }

    private func moduleItem(_ module: String, label: String) -> ScriptCompletion {
        ScriptCompletion(label: label, insertText: label, caretOffset: label.utf16.count,
                         detail: label == module ? "module" : "module \(module)",
                         doc: index.module(module)?.doc ?? "", kind: .module)
    }

    /// Names bound by imports, then modules not imported yet.
    private func identifierCandidates(_ bindings: ScriptBindings) -> [ScriptCompletionCandidate] {
        var out: [ScriptCompletionCandidate] = []
        for (local, qualified) in bindings.aliases {
            switch index.target(forQualified: qualified) {
            case .module?:
                out.append(.init(item: moduleItem(qualified, label: local), order: 0))
            case .type(let path)?:
                if let cls = index.type(path), cls.kind == .class {
                    out.append(.init(item: FirmwareAPIIndex.callItem(label: local, function: cls.constructor,
                                                                     detail: cls.constructorSignature, doc: cls.doc, kind: .type),
                                     order: 0))
                } else {
                    out.append(.init(item: ScriptCompletion(label: local, insertText: local, caretOffset: local.utf16.count,
                                                            detail: "namespace", doc: index.type(path)?.doc ?? "",
                                                            kind: .namespace), order: 0))
                }
            case .instance?:
                break
            case nil:
                if let fn = index.callable(qualified: qualified) {
                    out.append(.init(item: FirmwareAPIIndex.callItem(label: local, function: fn, detail: fn.signature,
                                                                     doc: fn.doc, kind: .function), order: 1000))
                } else {
                    out.append(.init(item: ScriptCompletion(label: local, insertText: local, caretOffset: local.utf16.count,
                                                            detail: qualified, doc: "", kind: .constant), order: 2000))
                }
            }
        }
        for module in index.moduleNames where bindings.aliases[module] == nil {
            out.append(.init(item: moduleItem(module, label: module), order: 3000))
        }
        return out
    }
}
