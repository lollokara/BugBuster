import Foundation
import UIKit
import Runestone
import TreeSitterPythonRunestone

/// A pure description of one editor change, applied either to a string (tests,
/// headless) or to the Runestone view. Replacements apply in array order, so
/// callers list them from the end of the document backwards.
struct ScriptTextEdit: Equatable {
    struct Replacement: Equatable {
        let range: NSRange
        let text: String
    }

    let replacements: [Replacement]
    /// Caret (UTF-16) after all replacements; with `selectionLength` the selected span starts here.
    let caret: Int
    var selectionLength = 0
    /// Argument placeholders in document coordinates, in call order (the first is the selection).
    var placeholders: [NSRange] = []
    /// Where the caret goes after the last placeholder: just past the call's closing parenthesis.
    var endCaret: Int?

    func applied(to text: String) -> String {
        let out = NSMutableString(string: text)
        for r in replacements { out.replaceCharacters(in: r.range, with: r.text) }
        return out as String
    }

    static func completion(_ item: ScriptCompletion, range: NSRange) -> ScriptTextEdit {
        let placeholders = item.placeholders.map { NSRange(location: range.location + $0.location, length: $0.length) }
        return ScriptTextEdit(replacements: [Replacement(range: range, text: item.insertText)],
                              caret: range.location + item.caretOffset,
                              selectionLength: placeholders.first?.length ?? 0,
                              placeholders: placeholders,
                              endCaret: placeholders.isEmpty ? nil : range.location + item.insertText.utf16.count)
    }

    /// `snippet` at `caret`, plus `import <module>` on line 1 when `text` lacks it.
    static func insertion(_ snippet: String, at caret: Int, importing module: String?, in text: String) -> ScriptTextEdit {
        var replacements = [Replacement(range: NSRange(location: caret, length: 0), text: snippet)]
        var newCaret = caret + snippet.utf16.count
        if let module, FirmwareDocs.needsImport(module, in: text) {
            let line = "import \(module)\n"
            replacements.append(Replacement(range: NSRange(location: 0, length: 0), text: line))
            newCaret += line.utf16.count
        }
        return ScriptTextEdit(replacements: replacements, caret: newCaret)
    }
}

/// Indent / outdent of the lines a selection touches (4 spaces, the editor's indent unit).
enum ScriptIndentation {
    static let unit = "    "

    /// A selection: indent every line it touches. A bare caret: insert one unit there.
    static func indent(text: String, selection: NSRange) -> ScriptTextEdit {
        let ns = text as NSString
        let starts = lineStarts(ns, selection)
        guard selection.length > 0 else {
            return ScriptTextEdit(replacements: [.init(range: selection, text: unit)],
                                  caret: selection.location + unit.utf16.count)
        }
        let replacements = starts.reversed().map { ScriptTextEdit.Replacement(range: NSRange(location: $0, length: 0), text: unit) }
        return ScriptTextEdit(replacements: replacements, caret: selection.location + unit.utf16.count,
                              selectionLength: selection.length + unit.utf16.count * (starts.count - 1))
    }

    /// Remove up to one unit of leading spaces (or one tab) from every touched line; nil when none has any.
    static func outdent(text: String, selection: NSRange) -> ScriptTextEdit? {
        let ns = text as NSString
        let starts = lineStarts(ns, selection)
        var removals: [(start: Int, count: Int)] = []
        for start in starts {
            var n = 0
            while n < unit.utf16.count, start + n < ns.length, ns.character(at: start + n) == 0x20 { n += 1 }
            if n == 0, start < ns.length, ns.character(at: start) == 0x09 { n = 1 }
            if n > 0 { removals.append((start, n)) }
        }
        guard !removals.isEmpty else { return nil }
        func mapped(_ position: Int) -> Int {
            position - removals.reduce(0) { $0 + min($1.count, max(0, position - $1.start)) }
        }
        let newStart = mapped(selection.location)
        let newEnd = mapped(NSMaxRange(selection))
        let replacements = removals.reversed().map {
            ScriptTextEdit.Replacement(range: NSRange(location: $0.start, length: $0.count), text: "")
        }
        return ScriptTextEdit(replacements: replacements, caret: newStart, selectionLength: newEnd - newStart)
    }

    /// Start offsets of the lines touched by `selection` (a selection ending right after a newline
    /// does not touch the next line).
    private static func lineStarts(_ ns: NSString, _ selection: NSRange) -> [Int] {
        let lines = ns.lineRange(for: selection)
        var starts: [Int] = []
        var position = lines.location
        repeat {
            starts.append(position)
            position = NSMaxRange(ns.lineRange(for: NSRange(location: position, length: 0)))
        } while position < NSMaxRange(lines)
        return starts
    }
}

/// The arguments of an inserted call, walked with Tab: each is selected in turn (typing
/// replaces it) and the caret leaves after the closing parenthesis from the last one.
struct ScriptPlaceholderSession: Equatable {
    /// Document ranges (UTF-16), in call order.
    private(set) var ranges: [NSRange]
    private(set) var current = 0
    private(set) var endCaret: Int

    init(ranges: [NSRange], endCaret: Int) {
        self.ranges = ranges
        self.endCaret = endCaret
    }

    var currentRange: NSRange { ranges[current] }
    var isLast: Bool { current + 1 >= ranges.count }

    mutating func advance() { if !isLast { current += 1 } }

    /// Follow an edit made by typing. False when it left the current placeholder, which ends the session.
    mutating func adjust(forEdit range: NSRange, replacementLength: Int) -> Bool {
        let cur = ranges[current]
        guard range.location >= cur.location, NSMaxRange(range) <= NSMaxRange(cur) else { return false }
        let delta = replacementLength - range.length
        ranges[current].length += delta
        for i in (current + 1)..<ranges.count { ranges[i].location += delta }
        endCaret += delta
        return true
    }

    /// The caret or selection moved: follow it into another placeholder, or false when it left them all.
    mutating func follow(selection: NSRange) -> Bool {
        guard let index = ranges.firstIndex(where: {
            selection.location >= $0.location && NSMaxRange(selection) <= NSMaxRange($0)
        }) else { return false }
        current = index
        return true
    }
}

/// iOS keyboards and pasted text bring typographic punctuation that MicroPython rejects.
enum ScriptTextSanitizer {
    static func sanitize(_ text: String) -> String {
        text.replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")
            .replacingOccurrences(of: "\u{201C}", with: "\"")
            .replacingOccurrences(of: "\u{201D}", with: "\"")
            .replacingOccurrences(of: "\u{2018}", with: "'")
            .replacingOccurrences(of: "\u{2019}", with: "'")
            .replacingOccurrences(of: "\u{2014}", with: "--")
            .replacingOccurrences(of: "\u{2013}", with: "-")
    }
}

/// Editor state: the document, caret, dirty flag and the live completion list.
/// Works headless (tests) and drives the Runestone `TextView` when one is attached.
@MainActor
final class ScriptEditorModel: ObservableObject {
    @Published private(set) var text = ""
    @Published private(set) var completion: ScriptCompletionResult = .empty
    @Published private(set) var isDirty = false
    @Published private(set) var caret = 0
    /// Selected span (a selected placeholder, or the caret with length 0).
    @Published private(set) var selection = NSRange(location: 0, length: 0)
    /// Argument placeholders of the call that was just inserted; nil when none is being filled.
    @Published private(set) var placeholders: ScriptPlaceholderSession?
    /// Caret rectangle in the editor's visible coordinates (anchors the popover).
    @Published var caretRect: CGRect = .zero

    let engine: ScriptCompletionEngine?
    /// Set by `ScriptCodeEditor` while on screen.
    weak var textView: TextView?

    init(engine: ScriptCompletionEngine? = ScriptCompletionEngine.bundled) {
        self.engine = engine
    }

    /// A file was opened: replace the document. Not an edit, so it's not dirty.
    func load(_ newText: String) {
        text = newText
        caret = 0
        selection = NSRange(location: 0, length: 0)
        placeholders = nil
        isDirty = false
        completion = .empty
        if let tv = textView {
            tv.setState(TextViewState(text: newText, theme: tv.theme, language: .python))
        }
    }

    func markSaved() {
        isDirty = false
    }

    func textDidChange(_ newText: String, caret newCaret: Int) {
        guard newText != text else { selectionDidChange(caret: newCaret); return }
        text = newText
        caret = newCaret
        selection = NSRange(location: newCaret, length: 0)
        isDirty = true
        recompute()
    }

    /// The editor is about to apply a typed edit (not called for programmatic ones).
    func textWillChange(in range: NSRange, replacement: String) {
        guard var session = placeholders else { return }
        placeholders = session.adjust(forEdit: range, replacementLength: (replacement as NSString).length) ? session : nil
    }

    func selectionDidChange(caret newCaret: Int, length: Int = 0) {
        let newSelection = NSRange(location: newCaret, length: length)
        guard newSelection != selection || completion.isEmpty else { return }
        selection = newSelection
        caret = newCaret
        if var session = placeholders {
            placeholders = session.follow(selection: newSelection) ? session : nil
        }
        recompute()
    }

    func dismissCompletion() {
        completion = .empty
    }

    func accept(_ item: ScriptCompletion) {
        let range = completion.replaceRange
        completion = .empty
        apply(.completion(item, range: range))
    }

    /// Tab key / accessory Tab: take the top suggestion when one is showing.
    @discardableResult
    func acceptFirst() -> Bool {
        guard let first = completion.items.first else { return false }
        accept(first)
        return true
    }

    /// Jump to the next argument placeholder, or past the call's `)` from the last one.
    @discardableResult
    func nextPlaceholder() -> Bool {
        guard var session = placeholders else { return false }
        if session.isLast {
            placeholders = nil
            setSelection(NSRange(location: session.endCaret, length: 0))
        } else {
            session.advance()
            placeholders = session
            setSelection(session.currentRange)
        }
        return true
    }

    func indent() { apply(ScriptIndentation.indent(text: text, selection: selection)) }

    func outdent() {
        if let edit = ScriptIndentation.outdent(text: text, selection: selection) { apply(edit) }
    }

    enum TabAction: Equatable { case acceptedCompletion, nextPlaceholder, indent }

    /// What the Tab key does now: accept a suggestion, else walk the placeholders, else indent.
    func handleTab() -> TabAction {
        if acceptFirst() { return .acceptedCompletion }
        if nextPlaceholder() { return .nextPlaceholder }
        return .indent
    }

    private func setSelection(_ range: NSRange) {
        selection = range
        caret = range.location
        if let tv = textView { tv.selectedRange = range }
        recompute()
    }

    /// Docs browser "Insert".
    func insertAtCaret(_ snippet: String, ensuringImport module: String?) {
        completion = .empty
        apply(.insertion(snippet, at: caret, importing: module, in: text))
    }

    private func apply(_ edit: ScriptTextEdit) {
        placeholders = nil
        let target = NSRange(location: edit.caret, length: edit.selectionLength)
        if let tv = textView {
            for r in edit.replacements { tv.replace(r.range, withText: r.text) }
            tv.selectedRange = target
            text = tv.text
        } else {
            text = edit.applied(to: text)
        }
        caret = edit.caret
        selection = target
        if !edit.placeholders.isEmpty, let end = edit.endCaret {
            placeholders = ScriptPlaceholderSession(ranges: edit.placeholders, endCaret: end)
        }
        isDirty = true
        recompute()
    }

    private func recompute() {
        // A selected placeholder is about to be overwritten: no popup over it.
        guard let engine, selection.length == 0 else { completion = .empty; return }
        completion = engine.complete(text: text, caret: caret)
    }
}
