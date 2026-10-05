import SwiftUI
import UIKit
import Runestone
import TreeSitterPythonRunestone

/// Editor + caret-anchored completion popover.
struct ScriptEditorView: View {
    @ObservedObject var model: ScriptEditorModel
    var isEditable = true

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .topLeading) {
                ScriptCodeEditor(model: model, isEditable: isEditable)
                if !model.completion.isEmpty {
                    let size = ScriptCompletionPopoverLayout.size(itemCount: model.completion.items.count,
                                                                  containerWidth: geo.size.width)
                    let origin = ScriptCompletionPopoverLayout.origin(caret: model.caretRect, container: geo.size, size: size)
                    ScriptCompletionPopover(items: model.completion.items) { model.accept($0) }
                        .frame(width: size.width, height: size.height)
                        .offset(x: origin.x, y: origin.y)
                        .accessibilityIdentifier("completion_popover")
                        .transition(.opacity)
                }
            }
        }
    }
}

/// Runestone `TextView` with tree-sitter Python highlighting, line numbers,
/// 4-space indentation, bracket pairing and the BugBuster key bar.
struct ScriptCodeEditor: UIViewRepresentable {
    @ObservedObject var model: ScriptEditorModel
    var isEditable = true

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeUIView(context: Context) -> TextView {
        let tv = TextView()
        tv.editorDelegate = context.coordinator
        tv.backgroundColor = .clear
        tv.showLineNumbers = true
        tv.lineSelectionDisplayType = .line
        tv.isLineWrappingEnabled = false
        tv.indentStrategy = .space(length: 4)
        tv.characterPairs = ScriptCharacterPair.python
        tv.gutterLeadingPadding = 6
        tv.gutterTrailingPadding = 6
        tv.textContainerInset = UIEdgeInsets(top: 8, left: 4, bottom: 8, right: 8)
        tv.autocorrectionType = .no
        tv.autocapitalizationType = .none
        tv.smartQuotesType = .no
        tv.smartDashesType = .no
        tv.smartInsertDeleteType = .no
        tv.spellCheckingType = .no
        tv.keyboardType = .asciiCapable
        tv.keyboardAppearance = .dark
        tv.isEditable = isEditable
        tv.accessibilityIdentifier = "script_editor_text_view"
        tv.inputAccessoryView = context.coordinator.makeAccessoryBar()
        tv.setState(TextViewState(text: model.text, theme: ScriptEditorTheme(), language: .python))
        context.coordinator.textView = tv
        model.textView = tv
        #if DEBUG
        // Layout checks: BB_EDITOR_FOCUS=1 raises the keyboard, BB_EDITOR_TYPE=<text> then types it.
        let env = ProcessInfo.processInfo.environment
        if env["BB_EDITOR_FOCUS"] == "1" {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                tv.becomeFirstResponder()
                if let text = env["BB_EDITOR_TYPE"] {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { tv.insertText(text) }
                }
            }
        }
        #endif
        return tv
    }

    func updateUIView(_ tv: TextView, context: Context) {
        // Text flows model → view only through ScriptEditorModel.load/apply
        // (setState here would reset undo and re-parse on every SwiftUI pass).
        if tv.isEditable != isEditable { tv.isEditable = isEditable }
    }

    static func dismantleUIView(_ tv: TextView, coordinator: Coordinator) {
        MainActor.assumeIsolated {
            if coordinator.model.textView === tv { coordinator.model.textView = nil }
        }
    }

    /// Runestone's delegate protocol is not actor-annotated; UIKit calls it on main.
    final class Coordinator: NSObject, TextViewDelegate {
        let model: ScriptEditorModel
        weak var textView: TextView?
        private var keyBar: ScriptKeyBar?

        init(model: ScriptEditorModel) {
            self.model = model
        }

        func textViewDidChange(_ textView: TextView) {
            MainActor.assumeIsolated {
                model.textDidChange(textView.text, caret: textView.selectedRange.location)
                publishCaret(textView)
                refreshBar()
            }
        }

        func textViewDidChangeSelection(_ textView: TextView) {
            MainActor.assumeIsolated {
                model.selectionDidChange(caret: textView.selectedRange.location, length: textView.selectedRange.length)
                publishCaret(textView)
                refreshBar()
            }
        }

        func textViewDidEndEditing(_ textView: TextView) {
            MainActor.assumeIsolated { model.dismissCompletion() }
        }

        func textView(_ textView: TextView, shouldChangeTextIn range: NSRange, replacementText text: String) -> Bool {
            MainActor.assumeIsolated {
                if text == "\t" {                           // hardware Tab: suggestion, next placeholder, or indent
                    pressTab()
                    return false
                }
                let clean = ScriptTextSanitizer.sanitize(text)
                model.textWillChange(in: range, replacement: clean)
                guard clean != text else { return true }
                textView.replace(range, withText: clean)
                return false
            }
        }

        @MainActor
        private func publishCaret(_ tv: TextView) {
            guard let position = tv.selectedTextRange?.end else { return }
            // caretRect is in content coordinates; the popover lives in visible ones.
            model.caretRect = tv.caretRect(for: position).offsetBy(dx: -tv.contentOffset.x, dy: -tv.contentOffset.y)
        }

        func makeAccessoryBar() -> UIView {
            let bar = ScriptKeyBar()
            bar.actions.tab = { [weak self] in self?.pressTab() }
            bar.actions.indent = { [weak self] in self.map { m in MainActor.assumeIsolated { m.model.indent() } } }
            bar.actions.outdent = { [weak self] in self.map { m in MainActor.assumeIsolated { m.model.outdent() } } }
            bar.actions.insert = { [weak self] text in self?.textView?.insertText(text) }
            bar.actions.undo = { [weak self] in self?.textView?.undoManager?.undo() }
            bar.actions.redo = { [weak self] in self?.textView?.undoManager?.redo() }
            bar.actions.nextPlaceholder = { [weak self] in
                self.map { m in MainActor.assumeIsolated { _ = m.model.nextPlaceholder() } }
            }
            bar.actions.dismiss = { [weak self] in self?.textView?.resignFirstResponder() }
            keyBar = bar
            return bar
        }

        /// Tab: accept the top suggestion, else the next placeholder, else indent.
        func pressTab() {
            MainActor.assumeIsolated {
                if model.handleTab() == .indent { model.indent() }
            }
        }

        @MainActor
        func refreshBar() {
            keyBar?.refresh(canUndo: textView?.undoManager?.canUndo ?? false,
                            canRedo: textView?.undoManager?.canRedo ?? false,
                            hasPlaceholder: model.placeholders != nil)
        }
    }
}

struct ScriptCharacterPair: CharacterPair {
    let leading: String
    let trailing: String

    static let python: [CharacterPair] = [
        ScriptCharacterPair(leading: "(", trailing: ")"),
        ScriptCharacterPair(leading: "[", trailing: "]"),
        ScriptCharacterPair(leading: "{", trailing: "}"),
        ScriptCharacterPair(leading: "\"", trailing: "\""),
        ScriptCharacterPair(leading: "'", trailing: "'")
    ]
}

/// Dark theme matching the app's obsidian/cyan palette. tree-sitter-python
/// captures (highlights.scm) are matched on their first dotted component.
final class ScriptEditorTheme: Runestone.Theme {
    enum Palette {
        static let keyword = UIColor.cyan
        static let string = UIColor(red: 0.95, green: 0.65, blue: 0.35, alpha: 1)
        static let comment = UIColor(red: 0.47, green: 0.56, blue: 0.47, alpha: 1)
        static let number = UIColor(red: 0.70, green: 0.90, blue: 0.50, alpha: 1)
        static let function = UIColor(red: 0.50, green: 0.75, blue: 1.00, alpha: 1)
        static let type = UIColor(red: 0.35, green: 0.90, blue: 0.80, alpha: 1)
        static let constant = UIColor(red: 0.70, green: 0.55, blue: 1.00, alpha: 1)
        static let property = UIColor(white: 0.85, alpha: 1)
        static let punctuation = UIColor(white: 0.60, alpha: 1)
    }

    let font = UIFont.monospacedSystemFont(ofSize: 13, weight: .regular)
    let textColor = UIColor(white: 0.92, alpha: 1)
    let gutterBackgroundColor = UIColor.clear
    let gutterHairlineColor = UIColor(white: 1, alpha: 0.08)
    let lineNumberColor = UIColor(white: 1, alpha: 0.35)
    let lineNumberFont = UIFont.monospacedSystemFont(ofSize: 11, weight: .regular)
    let selectedLineBackgroundColor = UIColor(white: 1, alpha: 0.05)
    let selectedLinesLineNumberColor = UIColor.cyan
    let selectedLinesGutterBackgroundColor = UIColor.clear
    let invisibleCharactersColor = UIColor(white: 1, alpha: 0.2)
    let pageGuideHairlineColor = UIColor(white: 1, alpha: 0.08)
    let pageGuideBackgroundColor = UIColor.clear
    let markedTextBackgroundColor = UIColor.cyan.withAlphaComponent(0.2)

    func textColor(for highlightName: String) -> UIColor? {
        Self.color(for: highlightName)
    }

    func fontTraits(for highlightName: String) -> FontTraits {
        highlightName.hasPrefix("keyword") ? .bold : []
    }

    static func color(for highlightName: String) -> UIColor? {
        switch highlightName.split(separator: ".").first.map(String.init) ?? highlightName {
        case "keyword": return Palette.keyword
        case "string", "escape": return Palette.string
        case "comment": return Palette.comment
        case "number": return Palette.number
        case "function": return Palette.function
        case "type", "constructor": return Palette.type
        case "constant": return Palette.constant
        case "property": return Palette.property
        case "operator", "punctuation": return Palette.punctuation
        default: return nil                      // "variable", "embedded": default text colour
        }
    }
}

enum ScriptCompletionPopoverLayout {
    static let rowHeight: CGFloat = 40
    static let maxVisibleRows = 6

    static func size(itemCount: Int, containerWidth: CGFloat) -> CGSize {
        CGSize(width: max(160, min(340, containerWidth - 8)),
               height: CGFloat(min(itemCount, maxVisibleRows)) * rowHeight + 8)
    }

    /// Below the caret line, clamped inside the container; flipped above when it would overflow.
    static func origin(caret: CGRect, container: CGSize, size: CGSize) -> CGPoint {
        var x = caret.minX
        if x + size.width > container.width - 4 { x = max(4, container.width - size.width - 4) }
        var y = caret.maxY + 4
        if y + size.height > container.height - 4 { y = max(4, caret.minY - size.height - 4) }
        return CGPoint(x: x, y: y)
    }
}

struct ScriptCompletionPopover: View {
    let items: [ScriptCompletion]
    let onPick: (ScriptCompletion) -> Void

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(items) { item in
                    Button { onPick(item) } label: {
                        HStack(spacing: 8) {
                            Text(Self.glyph(item.kind))
                                .font(.caption2.monospaced().weight(.bold))
                                .foregroundStyle(.cyan)
                                .frame(width: 18)
                            VStack(alignment: .leading, spacing: 1) {
                                Text(item.label)
                                    .font(.subheadline.monospaced().weight(.semibold))
                                    .foregroundStyle(.primary)
                                if !item.detail.isEmpty {
                                    Text(item.detail)
                                        .font(.caption2.monospaced())
                                        .foregroundStyle(.secondary)
                                        .lineLimit(1)
                                }
                            }
                            Spacer(minLength: 0)
                        }
                        .padding(.horizontal, 10)
                        .frame(height: ScriptCompletionPopoverLayout.rowHeight)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("\(item.label), \(item.detail)")
                    .accessibilityIdentifier("completion_item_\(item.label)")
                }
            }
            .padding(.vertical, 4)
        }
        .accessibilityIdentifier("completion_popover")
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).stroke(Color.white.opacity(0.12), lineWidth: 1))
        .shadow(color: .black.opacity(0.35), radius: 12, y: 6)
    }

    static func glyph(_ kind: ScriptCompletionKind) -> String {
        switch kind {
        case .module: return "M"
        case .function: return "ƒ"
        case .method: return "m"
        case .type: return "C"
        case .namespace: return "N"
        case .constant: return "K"
        case .keyword: return "="
        case .builtin: return "ƒ"
        case .exception: return "E"
        case .reserved: return "k"
        }
    }
}
