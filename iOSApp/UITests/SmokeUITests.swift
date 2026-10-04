import XCTest

final class SmokeUITests: UITestBase {
    func testOverviewScreenSmoke() {
        let app = launchApp(initialSection: 0)

        let overview = app.descendants(matching: .any)["overview_tab_view"]
        waitForExistence(overview)

        // Verify key sections render with mock data
        XCTAssertTrue(app.staticTexts["I/V Characterization"].waitForExistence(timeout: 5))
        // Verify channels are populated (e.g. Channel 0, High Impedance or Voltage)
        XCTAssertTrue(app.staticTexts["Channel A"].exists || app.staticTexts["VLOGIC"].exists || app.staticTexts["Raw: 0"].exists || app.staticTexts["ADC"].exists)
    }

    func testDiagnosticsScreenSmoke() {
        let app = launchApp(initialSection: 3)

        let diagnostics = app.descendants(matching: .any)["diagnostics_tab_view"]
        waitForExistence(diagnostics)

        // Verify diagnostic cards render with mock data
        waitForExistence(app.staticTexts["Diagnostics & Config"])
        waitForExistence(app.staticTexts["BOOT SELF-TEST"])
    }
}
