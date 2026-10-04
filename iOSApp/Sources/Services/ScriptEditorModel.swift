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
    /// Caret (UTF-16) after all replacements.
    let caret: Int

    func applied(to text: String) -> String {
        let out = NSMutableString(string: text)
        for r in replacements { out.replaceCharacters(in: r.range, with: r.text) }
        return out as String
    }

    static func completion(_ item: ScriptCompletion, range: NSRange) -> ScriptTextEdit {
        ScriptTextEdit(replacements: [Replacement(range: range, text: item.insertText)],
                       caret: range.location + item.caretOffset)
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

/// iOS keyboards and pasted text bring typographic punctuation that MicroPython rejects.
enum ScriptTextSanitizer {
    static func sanitize(_ text: String) -> String {
        text.replacingOccurrences(of: "\u{201C}", with: "\"")
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
        isDirty = true
        recompute()
    }

    func selectionDidChange(caret newCaret: Int) {
        guard newCaret != caret || completion.isEmpty else { return }
        caret = newCaret
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

    /// Docs browser "Insert".
    func insertAtCaret(_ snippet: String, ensuringImport module: String?) {
        completion = .empty
        apply(.insertion(snippet, at: caret, importing: module, in: text))
    }

    private func apply(_ edit: ScriptTextEdit) {
        if let tv = textView {
            for r in edit.replacements { tv.replace(r.range, withText: r.text) }
            tv.selectedRange = NSRange(location: edit.caret, length: 0)
            text = tv.text
        } else {
            text = edit.applied(to: text)
        }
        caret = edit.caret
        isDirty = true
        recompute()
    }

    private func recompute() {
        guard let engine else { completion = .empty; return }
        completion = engine.complete(text: text, caret: caret)
    }
}
