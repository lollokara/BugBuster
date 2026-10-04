import XCTest
import UIKit
@testable import BugBuster

final class ScriptEditorModelTests: XCTestCase {
    // The model runs headless here (no TextView): edits apply to `text` directly.

    @MainActor
    func testTypingADotOffersMembers() {
        let m = ScriptEditorModel()
        m.load("import daq\n")
        XCTAssertFalse(m.isDirty)
        m.textDidChange("import daq\ndaq.", caret: 15)
        XCTAssertTrue(m.isDirty)
        XCTAssertEqual(m.completion.items.first?.label, "run")
    }

    @MainActor
    func testAcceptReplacesThePartialAndOpensKwargs() throws {
        let m = ScriptEditorModel()
        m.load("import daq\n")
        m.textDidChange("import daq\ndaq.vd", caret: 17)
        let vdut = try XCTUnwrap(m.completion.items.first { $0.label == "vdut" })
        m.accept(vdut)
        XCTAssertEqual(m.text, "import daq\ndaq.vdut()")
        XCTAssertEqual(m.caret, 20)                                      // between the parens
        XCTAssertEqual(m.completion.items.first?.label, "enable=")        // kwargs follow at once
    }

    @MainActor
    func testAcceptFirstIsTheTabKey() {
        let m = ScriptEditorModel()
        m.load("import daq\n")
        m.textDidChange("import daq\ndaq.run.sta", caret: 22)
        XCTAssertTrue(m.acceptFirst())
        XCTAssertEqual(m.text, "import daq\ndaq.run.start()")
        m.dismissCompletion()
        XCTAssertFalse(m.acceptFirst())
    }

    @MainActor
    func testInsertAtCaretAddsAMissingImport() {
        let m = ScriptEditorModel()
        m.load("x = 1\n")
        m.selectionDidChange(caret: 6)
        m.insertAtCaret("daq.vdut()", ensuringImport: "daq")
        XCTAssertEqual(m.text, "import daq\nx = 1\ndaq.vdut()")
        XCTAssertEqual(m.caret, 27)
        XCTAssertTrue(m.isDirty)
    }

    @MainActor
    func testInsertAtCaretKeepsAnExistingImport() {
        let m = ScriptEditorModel()
        m.load("import daq\n")
        m.selectionDidChange(caret: 11)
        m.insertAtCaret("daq.read()", ensuringImport: "daq")
        XCTAssertEqual(m.text, "import daq\ndaq.read()")
    }

    @MainActor
    func testInsertIntoAnEmptyDocument() {
        let m = ScriptEditorModel()
        m.load("")
        m.insertAtCaret("daq.read()", ensuringImport: "daq")
        XCTAssertEqual(m.text, "import daq\ndaq.read()")
        XCTAssertEqual(m.caret, 21)
    }

    @MainActor
    func testLoadClearsDirtyAndCompletion() {
        let m = ScriptEditorModel()
        m.load("import daq\n")
        m.textDidChange("import daq\ndaq.", caret: 15)
        m.load("print(1)\n")
        XCTAssertFalse(m.isDirty)
        XCTAssertTrue(m.completion.isEmpty)
        m.textDidChange("print(2)\n", caret: 9)
        m.markSaved()
        XCTAssertFalse(m.isDirty)
    }

    @MainActor
    func testWithoutAnEngineThereIsNoCompletion() {
        let m = ScriptEditorModel(engine: nil)
        m.textDidChange("import daq\ndaq.", caret: 15)
        XCTAssertTrue(m.completion.isEmpty)
    }

    func testTextEditAppliesInArrayOrder() {
        let edit = ScriptTextEdit(replacements: [
            .init(range: NSRange(location: 5, length: 0), text: "B"),
            .init(range: NSRange(location: 0, length: 0), text: "A")], caret: 7)
        XCTAssertEqual(edit.applied(to: "hello"), "AhelloB")
    }

    func testSanitizerReplacesSmartPunctuation() {
        XCTAssertEqual(ScriptTextSanitizer.sanitize("“a” ‘b’ — –"), "\"a\" 'b' -- -")
        XCTAssertEqual(ScriptTextSanitizer.sanitize("plain"), "plain")
    }

    func testSanitizerNormalizesCRLFAndLoneCRToLF() {
        XCTAssertEqual(ScriptTextSanitizer.sanitize("line1\r\nline2\r\nline3"), "line1\nline2\nline3")
        XCTAssertEqual(ScriptTextSanitizer.sanitize("line1\rline2\rline3"), "line1\nline2\nline3")
        XCTAssertEqual(ScriptTextSanitizer.sanitize("line1\r\nline2\rline3\nline4"), "line1\nline2\nline3\nline4")
    }

    func testThemeColours() {
        XCTAssertEqual(ScriptEditorTheme.color(for: "keyword"), ScriptEditorTheme.Palette.keyword)
        XCTAssertEqual(ScriptEditorTheme.color(for: "function.builtin"), ScriptEditorTheme.Palette.function)
        XCTAssertEqual(ScriptEditorTheme.color(for: "punctuation.special"), ScriptEditorTheme.Palette.punctuation)
        XCTAssertEqual(ScriptEditorTheme.color(for: "constant.builtin"), ScriptEditorTheme.Palette.constant)
        XCTAssertNil(ScriptEditorTheme.color(for: "variable"))
    }

    func testPopoverClampsRightAndFlipsAbove() {
        let size = CGSize(width: 300, height: 200)
        let container = CGSize(width: 400, height: 600)
        XCTAssertEqual(ScriptCompletionPopoverLayout.origin(caret: CGRect(x: 250, y: 20, width: 2, height: 16),
                                                            container: container, size: size),
                       CGPoint(x: 96, y: 40))
        XCTAssertEqual(ScriptCompletionPopoverLayout.origin(caret: CGRect(x: 10, y: 500, width: 2, height: 16),
                                                            container: container, size: size),
                       CGPoint(x: 10, y: 296))
        XCTAssertEqual(ScriptCompletionPopoverLayout.size(itemCount: 20, containerWidth: 1000).height,
                       ScriptCompletionPopoverLayout.rowHeight * 6 + 8)
    }

    // MARK: placeholders

    @MainActor
    private func modelAfterAccepting(_ typed: String, label: String) throws -> ScriptEditorModel {
        let m = ScriptEditorModel()
        m.load("")
        m.textDidChange(typed, caret: (typed as NSString).length)
        let item = try XCTUnwrap(m.completion.items.first { $0.label == label })
        m.accept(item)
        return m
    }

    @MainActor
    func testAcceptingACallSelectsTheFirstPlaceholder() throws {
        let m = try modelAfterAccepting("import bugbuster\nbugbuster.sle", label: "sleep")
        XCTAssertEqual(m.text, "import bugbuster\nbugbuster.sleep(ms)")
        XCTAssertEqual(m.selection, NSRange(location: 33, length: 2))
        XCTAssertEqual(m.caret, 33)
        XCTAssertNotNil(m.placeholders)
        XCTAssertTrue(m.completion.isEmpty, "no popup over a selected placeholder")
    }

    @MainActor
    func testTabWalksThePlaceholdersThenLeavesTheCall() throws {
        let m = try modelAfterAccepting("import daq\ndaq.run.ne", label: "new")
        XCTAssertEqual(m.text, "import daq\ndaq.run.new(name, chem, cells, capacity_mah)")
        XCTAssertEqual(m.selection, NSRange(location: 23, length: 4))
        XCTAssertEqual(m.handleTab(), .nextPlaceholder)
        XCTAssertEqual(m.selection, NSRange(location: 29, length: 4))
        XCTAssertTrue(m.nextPlaceholder())
        XCTAssertEqual(m.selection, NSRange(location: 35, length: 5))
        XCTAssertTrue(m.nextPlaceholder())
        XCTAssertEqual(m.selection, NSRange(location: 42, length: 12))
        XCTAssertTrue(m.nextPlaceholder())                              // past the last: after the ")"
        XCTAssertEqual(m.selection, NSRange(location: 55, length: 0))
        XCTAssertNil(m.placeholders)
        XCTAssertFalse(m.nextPlaceholder())
        XCTAssertEqual(m.handleTab(), .indent)
    }

    @MainActor
    func testTypingInAPlaceholderKeepsTheLaterOnesAligned() throws {
        let m = try modelAfterAccepting("import daq\ndaq.run.ne", label: "new")
        // Type "demo" over `name` (the editor reports the edit, then the new text).
        m.textWillChange(in: NSRange(location: 23, length: 4), replacement: "demo run")
        m.textDidChange("import daq\ndaq.run.new(demo run, chem, cells, capacity_mah)", caret: 31)
        XCTAssertNotNil(m.placeholders)
        XCTAssertTrue(m.nextPlaceholder())
        XCTAssertEqual(m.selection, NSRange(location: 33, length: 4))         // `chem`, shifted by 4
        let text = m.text as NSString
        XCTAssertEqual(text.substring(with: m.selection), "chem")
    }

    @MainActor
    func testEditingOutsideThePlaceholderEndsTheSession() throws {
        let m = try modelAfterAccepting("import bugbuster\nbugbuster.sle", label: "sleep")
        m.textWillChange(in: NSRange(location: 0, length: 0), replacement: "#")
        XCTAssertNil(m.placeholders)
        XCTAssertEqual(m.handleTab(), .indent)
    }

    @MainActor
    func testMovingTheCaretAwayEndsTheSession() throws {
        let m = try modelAfterAccepting("import bugbuster\nbugbuster.sle", label: "sleep")
        m.selectionDidChange(caret: 3, length: 0)
        XCTAssertNil(m.placeholders)
    }

    @MainActor
    func testTabAcceptsASuggestionBeforeAnyPlaceholder() {
        let m = ScriptEditorModel()
        m.load("")
        m.textDidChange("import daq\ndaq.run.sta", caret: 22)
        XCTAssertEqual(m.handleTab(), .acceptedCompletion)
        XCTAssertEqual(m.text, "import daq\ndaq.run.start()")
    }

    @MainActor
    func testNoParenthesisDoublingThroughTheModel() throws {
        let m = ScriptEditorModel()
        m.load("")
        let text = "import bugbuster\nbugbuster.sle(10)"
        m.textDidChange(text, caret: 30)
        let sleep = try XCTUnwrap(m.completion.items.first)
        m.accept(sleep)
        XCTAssertEqual(m.text, "import bugbuster\nbugbuster.sleep(10)")
        XCTAssertEqual(m.caret, 32)
    }

    // MARK: indentation

    func testIndentInsertsOneUnitAtTheCaret() {
        let edit = ScriptIndentation.indent(text: "x = 1", selection: NSRange(location: 0, length: 0))
        XCTAssertEqual(edit.applied(to: "x = 1"), "    x = 1")
        XCTAssertEqual(edit.caret, 4)
    }

    func testIndentPrefixesEveryTouchedLine() {
        let text = "a\nb\nc\n"
        let edit = ScriptIndentation.indent(text: text, selection: NSRange(location: 0, length: 3))   // "a\nb"
        XCTAssertEqual(edit.applied(to: text), "    a\n    b\nc\n")
        XCTAssertEqual(edit.caret, 4)
        XCTAssertEqual(edit.selectionLength, 3 + 4)
    }

    func testSelectionEndingAfterANewlineDoesNotTouchTheNextLine() {
        let text = "a\nb\n"
        let edit = ScriptIndentation.indent(text: text, selection: NSRange(location: 0, length: 2))   // "a\n"
        XCTAssertEqual(edit.applied(to: text), "    a\nb\n")
    }

    func testOutdentRemovesUpToOneUnit() throws {
        let text = "        deep\n  two\nnone\n\tTab\n"
        let edit = try XCTUnwrap(ScriptIndentation.outdent(text: text, selection: NSRange(location: 0, length: (text as NSString).length)))
        XCTAssertEqual(edit.applied(to: text), "    deep\ntwo\nnone\nTab\n")
    }

    func testOutdentTheCaretLineAndKeepTheCaretOnTheText() throws {
        let text = "x\n    y = 1"
        let edit = try XCTUnwrap(ScriptIndentation.outdent(text: text, selection: NSRange(location: 8, length: 0)))
        XCTAssertEqual(edit.applied(to: text), "x\ny = 1")
        XCTAssertEqual(edit.caret, 4)
        XCTAssertNil(ScriptIndentation.outdent(text: "flush", selection: NSRange(location: 2, length: 0)))
    }

    @MainActor
    func testModelIndentAndOutdent() {
        let m = ScriptEditorModel()
        m.load("")
        m.textDidChange("a\nb", caret: 3)
        m.selectionDidChange(caret: 0, length: 3)
        m.indent()
        XCTAssertEqual(m.text, "    a\n    b")
        m.outdent()
        XCTAssertEqual(m.text, "a\nb")
    }

    // MARK: keyboard bar and layout

    func testEditorGapIsTheSpaceAboveTheKeyboardBeyondTheChrome() {
        XCTAssertEqual(ScriptsKeyboard.editorGap(editorBottom: 513, keyboardTop: 525), 0)
        XCTAssertEqual(ScriptsKeyboard.editorGap(editorBottom: 489, keyboardTop: 525), 24)
        XCTAssertEqual(ScriptsKeyboard.editorGap(editorBottom: 548, keyboardTop: 525), -35)
    }

    @MainActor
    func testKeyBarHasTheSpecifiedGroupsAtTheTouchTargetSize() throws {
        XCTAssertEqual(ScriptKeyBar.symbols, ["(", ")", "[", "]", ":", "=", "\"", "'", "#"])
        XCTAssertEqual(ScriptKeyBar.buttonSize, 44)
        let bar = ScriptKeyBar()
        let buttons = Self.buttons(in: bar)
        let labels = buttons.compactMap(\.accessibilityLabel)
        XCTAssertEqual(labels.prefix(3), ["Outdent", "Indent", "Tab"])
        for symbol in ScriptKeyBar.symbols { XCTAssertTrue(labels.contains(ScriptKeyBar.spoken(symbol)), symbol) }
        XCTAssertTrue(Set(["Undo", "Redo", "Next placeholder", "Dismiss keyboard"]).isSubset(of: Set(labels)))
        XCTAssertEqual(bar.intrinsicContentSize.height, ScriptKeyBar.height)
        for button in buttons {
            let sizes = button.constraints.filter { $0.firstAttribute == .width || $0.firstAttribute == .height }.map(\.constant)
            XCTAssertEqual(sizes, [44, 44], button.accessibilityLabel ?? "?")
        }
    }

    @MainActor
    func testKeyBarButtonsCallTheirActions() throws {
        let bar = ScriptKeyBar()
        var log: [String] = []
        bar.actions.tab = { log.append("tab") }
        bar.actions.insert = { log.append("insert \($0)") }
        bar.actions.indent = { log.append("indent") }
        bar.actions.dismiss = { log.append("dismiss") }
        bar.refresh(canUndo: true, canRedo: false, hasPlaceholder: true)
        let buttons = Self.buttons(in: bar)
        func tap(_ label: String) { buttons.first { $0.accessibilityLabel == label }?.sendActions(for: .touchUpInside) }
        tap("Tab"); tap("Open parenthesis"); tap("Indent"); tap("Dismiss keyboard")
        XCTAssertEqual(log, ["tab", "insert (", "indent", "dismiss"])
        XCTAssertTrue(try XCTUnwrap(buttons.first { $0.accessibilityLabel == "Undo" }).isEnabled)
        XCTAssertFalse(try XCTUnwrap(buttons.first { $0.accessibilityLabel == "Redo" }).isEnabled)
        XCTAssertTrue(try XCTUnwrap(buttons.first { $0.accessibilityLabel == "Next placeholder" }).isEnabled)
    }

    private static func buttons(in view: UIView) -> [UIButton] {
        view.subviews.flatMap { sub -> [UIButton] in (sub as? UIButton).map { [$0] } ?? buttons(in: sub) }
    }
}
