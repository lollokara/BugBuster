import XCTest
import SwiftUI
@testable import BugBuster

final class ScriptStatusTextTests: XCTestCase {
    func testStateLabels() {
        XCTAssertEqual(ScriptStatusText.stateLabel(ScriptRunStatus(state: .running)), "Running")
        XCTAssertEqual(ScriptStatusText.stateLabel(ScriptRunStatus(state: .stopping)), "Stopping…")
        XCTAssertEqual(ScriptStatusText.stateLabel(ScriptRunStatus(state: .error)), "Error")
        XCTAssertEqual(ScriptStatusText.stateLabel(ScriptRunStatus(state: .done, lastExit: .stopped)), "Stopped")
        XCTAssertEqual(ScriptStatusText.stateLabel(ScriptRunStatus(state: .done, lastExit: .ok)), "Finished")
        XCTAssertEqual(ScriptStatusText.stateLabel(ScriptRunStatus()), "Idle")
    }

    func testSourceLabelsAndColours() {
        XCTAssertEqual(ScriptStatusText.sourceLabel(.autorun), "Autorun")
        XCTAssertEqual(ScriptStatusText.sourceLabel(.repl), "REPL")
        XCTAssertEqual(ScriptStatusText.sourceLabel(.manual), "Manual")
        XCTAssertEqual(ScriptStatusText.stateColor(.running), .green)
        XCTAssertEqual(ScriptStatusText.stateColor(.error), .red)
        XCTAssertEqual(ScriptStatusText.stateColor(.stopping), .orange)
    }

    func testElapsedLabel() {
        XCTAssertNil(ScriptStatusText.elapsedLabel(nil))
        XCTAssertEqual(ScriptStatusText.elapsedLabel(65), "1m 05s")
    }

    func testAutorunStates() {
        XCTAssertEqual(ScriptStatusText.autorunState(ScriptAutorunStatus(enabled: true, ranThisBoot: true, running: true)), "Running")
        XCTAssertEqual(ScriptStatusText.autorunState(ScriptAutorunStatus(enabled: true, lastRunOk: true, ranThisBoot: true)), "Finished OK")
        XCTAssertEqual(ScriptStatusText.autorunState(ScriptAutorunStatus(enabled: true, lastRunOk: false, ranThisBoot: true)), "Failed")
        XCTAssertEqual(ScriptStatusText.autorunState(ScriptAutorunStatus(enabled: true, io12High: true)), "Runs at next boot")
        XCTAssertEqual(ScriptStatusText.autorunState(ScriptAutorunStatus(enabled: true, io12High: false)), "Suppressed: IO12 held low")
        XCTAssertEqual(ScriptStatusText.autorunState(ScriptAutorunStatus(enabled: false)), "Off")
    }

    func testAutorunRows() {
        let a = ScriptAutorunStatus(enabled: true, hasScript: true, io12High: true, lastRunOk: true, lastRunId: 3,
                                    scriptName: "boot.py", ranThisBoot: true, running: false)
        let rows = ScriptStatusText.autorunRows(a)
        XCTAssertEqual(rows.map(\.label), ["Script", "Ran this boot", "State", "IO12 gate"])
        XCTAssertEqual(rows.map(\.value), ["boot.py", "Yes", "Finished OK", "High: autorun allowed"])
        XCTAssertEqual(ScriptStatusText.autorunRows(ScriptAutorunStatus(hasScript: true)).first?.value, "autorun.py")
        XCTAssertEqual(ScriptStatusText.autorunRows(ScriptAutorunStatus()).first?.value, "None")
    }
}
