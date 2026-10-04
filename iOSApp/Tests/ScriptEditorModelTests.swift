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
}
