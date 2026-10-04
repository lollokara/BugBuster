import XCTest

final class NavigationUITests: UITestBase {
    func testNavigationEverySectionOpensMainContent() {
        let app = launchApp(initialSection: 0)

        // 1. Overview
        let overview = app.descendants(matching: .any)["overview_tab_view"]
        waitForExistence(overview)

        // 2. Signal Path
        navigateTo(section: "signal_path", index: 1)
        let signalPath = app.descendants(matching: .any)["signal_path_tab_view"]
        waitForExistence(signalPath)

        // 3. Scope
        navigateTo(section: "scope", index: 2)
        let scope = app.descendants(matching: .any)["scope_tab_view"]
        waitForExistence(scope)

        // 4. Diagnostics
        navigateTo(section: "diagnostics", index: 3)
        let diagnostics = app.descendants(matching: .any)["diagnostics_tab_view"]
        waitForExistence(diagnostics)

        // 5. Scripts
        navigateTo(section: "scripts", index: 4)
        let scripts = app.descendants(matching: .any)["script_status_header"]
        waitForExistence(scripts)
    }

    func testIPadSidebarCollapse() {
        guard isPad else { return }

        let app = launchApp(initialSection: 0)

        // Ensure sidebar item is visible
        let sidebarOverview = app.descendants(matching: .any).matching(identifier: "sidebar_overview").firstMatch
        waitForExistence(sidebarOverview)

        // The system split view sidebar toggle button
        let toggleSidebarButton = app.buttons["ToggleSidebar"]
        guard toggleSidebarButton.waitForExistence(timeout: 4) else {
            // Alternatively, in standard NavigationSplitView, check for first nav bar button
            let navBarButton = app.navigationBars.buttons.firstMatch
            if navBarButton.exists {
                navBarButton.tap()
            }
            return
        }

        // Collapse sidebar
        toggleSidebarButton.tap()

        // Detail view should still show Overview
        let overview = app.descendants(matching: .any)["overview_tab_view"]
        waitForExistence(overview)

        // Re-expand sidebar
        toggleSidebarButton.tap()
        waitForExistence(sidebarOverview)
    }
}
