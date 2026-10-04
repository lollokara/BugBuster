import XCTest

class UITestBase: XCTestCase {
    var app: XCUIApplication!

    override func setUp() {
        super.setUp()
        continueAfterFailure = false
    }

    override func tearDown() {
        app = nil
        super.tearDown()
    }

    var isPad: Bool {
        UIDevice.current.userInterfaceIdiom == .pad
    }

    func launchApp(initialSection: Int? = nil, extraEnv: [String: String] = [:], extraArgs: [String] = []) -> XCUIApplication {
        let application = XCUIApplication()
        application.launchEnvironment["BB_MOCK_MODE"] = "1"
        if let initialSection = initialSection {
            application.launchEnvironment["BB_INITIAL_SECTION"] = String(initialSection)
        }
        for (key, value) in extraEnv {
            application.launchEnvironment[key] = value
        }
        for arg in extraArgs {
            application.launchArguments.append(arg)
        }
        application.launch()
        XCTAssertTrue(application.wait(for: .runningForeground, timeout: 10), "App failed to launch into foreground")
        self.app = application
        return application
    }

    @discardableResult
    func waitForExistence(_ element: XCUIElement, timeout: TimeInterval = 6, file: StaticString = #file, line: UInt = #line) -> Bool {
        let exists = element.waitForExistence(timeout: timeout)
        if !exists { print("UIDUMP>>>\n" + (app?.debugDescription ?? "") + "\n<<<UIDUMP") }
        XCTAssertTrue(exists, "Element '\(element.description)' did not exist within \(timeout)s", file: file, line: line)
        return exists
    }

    /// Scrolls the main list until `element` exists and is hittable (lazy rows are only
    /// created on screen; the mock device lists every catalogue example).
    @discardableResult
    func scrollTo(_ element: XCUIElement, maxSwipes: Int = 12, file: StaticString = #file, line: UInt = #line) -> Bool {
        if element.waitForExistence(timeout: 3) && element.isHittable { return true }
        for direction in [true, false] {
            for _ in 0..<maxSwipes {
                if element.exists && element.isHittable { return true }
                if direction { app.swipeUp() } else { app.swipeDown() }
                _ = element.waitForExistence(timeout: 0.5)
            }
        }
        return waitForExistence(element, file: file, line: line)
    }

    func navigateTo(section: String, index: Int) {
        if isPad {
            let sidebarItem = app.descendants(matching: .any).matching(identifier: "sidebar_\(section)").firstMatch
            if sidebarItem.waitForExistence(timeout: 3) {
                sidebarItem.tap()
            } else {
                // If sidebar was collapsed, open it
                let toggleBtn = app.buttons["ToggleSidebar"]
                if toggleBtn.exists {
                    toggleBtn.tap()
                    _ = sidebarItem.waitForExistence(timeout: 3)
                    sidebarItem.tap()
                }
            }
        } else {
            let tab = app.descendants(matching: .any).matching(identifier: "tab_\(section)").firstMatch
            if tab.waitForExistence(timeout: 3) {
                tab.tap()
            }
        }
    }

    /// Scrolls the Scripts browser list until `element` exists and is hittable (lazy rows are not in the hierarchy otherwise).
    func scrollIntoView(_ element: XCUIElement, maxSwipes: Int = 8) {
        var swipes = 0
        while !(element.exists && element.isHittable) && swipes < maxSwipes {
            // Drag in the upper-middle of the screen, clear of the status pill and tab bar.
            let start = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.62))
            let end = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.28))
            start.press(forDuration: 0.05, thenDragTo: end)
            swipes += 1
        }
        waitForExistence(element)
    }

    /// The Scripts mock launches with background_logger.py running (autorun); stop it so tests start from idle.
    func stopRunningScriptIfAny() {
        let headerStop = app.descendants(matching: .any)["script_header_stop_button"]
        if headerStop.waitForExistence(timeout: 3) {
            headerStop.tap()
            XCTAssertTrue(app.descendants(matching: .any)["script_status_pill"].waitForNonExistence(timeout: 6), "Pill should disappear after Stop")
        }
    }

    /// The log console: a sheet on iPhone, the log column on iPad (its identifier wins over the console's).
    var logConsole: XCUIElement {
        app.descendants(matching: .any).matching(identifier: isPad ? "script_log_column" : "script_log_console").firstMatch
    }
}
