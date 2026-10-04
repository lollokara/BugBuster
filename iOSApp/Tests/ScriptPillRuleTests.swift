import XCTest
@testable import BugBuster

final class ScriptPillRuleTests: XCTestCase {
    func testPillShowsOnEveryTabWhileActive() {
        let running = ScriptRunStatus(running: true, state: .running, fileSlotName: "a.py")
        XCTAssertTrue(ScriptPillRule.isVisible(status: running, onScriptsTab: false))
        XCTAssertTrue(ScriptPillRule.isVisible(status: running, onScriptsTab: true))
        XCTAssertTrue(ScriptPillRule.isVisible(status: ScriptRunStatus(state: .stopping), onScriptsTab: false))
        XCTAssertTrue(ScriptPillRule.isVisible(status: ScriptRunStatus(state: .stopping), onScriptsTab: true))
    }

    func testPillHidesWhenIdleOrUnknown() {
        XCTAssertFalse(ScriptPillRule.isVisible(status: ScriptRunStatus(state: .done), onScriptsTab: false))
        XCTAssertFalse(ScriptPillRule.isVisible(status: ScriptRunStatus(state: .done), onScriptsTab: true))
        XCTAssertFalse(ScriptPillRule.isVisible(status: nil, onScriptsTab: false))
        XCTAssertFalse(ScriptPillRule.isVisible(status: nil, onScriptsTab: true))
    }

    func testColumnWidthLeavesRoomForTheScope() {
        XCTAssertLessThanOrEqual(ScriptLogColumnLayout.width, 400)
    }
}
