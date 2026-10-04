import XCTest
import SwiftUI
import CoreTransferable
@testable import BugBuster

final class ScriptsViewsAuditFixesTests: XCTestCase {
    // MARK: - Finding 17: Lazy Log Export
    func testScriptLogExportIsLazy() {
        var called = false
        let export = ScriptLogExport(fileName: "test-log.txt") {
            called = true
            return "line 1\nline 2"
        }
        XCTAssertFalse(called, "ScriptLogExport must not evaluate textProvider at creation time")
        XCTAssertEqual(export.textProvider(), "line 1\nline 2")
        XCTAssertTrue(called)
    }

    // MARK: - Finding 25: iPad Log Column Responsive Width
    func testLogColumnWidthShrinksForNarrowDetail() {
        // When detail pane available width is constrained (e.g. portrait with sidebar),
        // column width must shrink below the default 380 pt to preserve editor room.
        let narrowWidth = ScriptLogColumnLayout.columnWidth(for: 650)
        XCTAssertLessThanOrEqual(narrowWidth, 320)
        XCTAssertGreaterThanOrEqual(narrowWidth, 260)

        // When plenty of space is available, it uses the standard width.
        let fullWidth = ScriptLogColumnLayout.columnWidth(for: 1100)
        XCTAssertEqual(fullWidth, ScriptLogColumnLayout.width)
    }

    // MARK: - Finding 27: Minimum Tap Target
    func testMinimumTapTargetConstant() {
        XCTAssertGreaterThanOrEqual(ScriptViewMetrics.minTouchTarget, 44)
    }
}
