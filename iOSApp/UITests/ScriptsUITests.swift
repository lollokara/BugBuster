import XCTest

final class ScriptsUITests: UITestBase {

    func testScriptsFileListLoads() {
        let app = launchApp(initialSection: 4)

        let header = app.descendants(matching: .any)["script_status_header"]
        waitForExistence(header)

        let helloRow = app.descendants(matching: .any)["script_row_hello.py"]
        scrollTo(helloRow)

        let loggerRow = app.descendants(matching: .any)["script_row_background_logger.py"]
        scrollTo(loggerRow)
    }

    func testEditorOpenTypeAutocompleteAndInsert() {
        let app = launchApp(initialSection: 4)

        let helloRow = app.descendants(matching: .any)["script_row_hello.py"]
        scrollTo(helloRow)
        helloRow.tap()

        let saveButton = app.buttons["editor_save_button"]
        waitForExistence(saveButton)

        let textView = app.textViews.firstMatch
        waitForExistence(textView)
        textView.tap()

        // Type to trigger autocomplete
        textView.typeText("\nimport daq\ndaq.")

        // Verify completion popover items exist
        let runItem = app.descendants(matching: .any)["completion_item_run"]
        waitForExistence(runItem)

        // Tap completion item to accept suggestion
        runItem.tap()

        // Verify suggestion was inserted (editor text now contains 'daq.run')
        let value = textView.value as? String ?? ""
        XCTAssertTrue(value.contains("daq.run") || value.contains("run"), "Editor text should contain inserted completion")

        let backButton = app.buttons["editor_back_button"]
        waitForExistence(backButton)
        backButton.tap()
    }

    func testDocsBrowserSearchAndInsert() {
        let app = launchApp(initialSection: 4)

        let helloRow = app.descendants(matching: .any)["script_row_hello.py"]
        scrollTo(helloRow)
        helloRow.tap()

        let docsButton = app.buttons["editor_docs_button"]
        waitForExistence(docsButton)
        docsButton.tap()

        let docsList = app.descendants(matching: .any)["docs_browser_list"]
        waitForExistence(docsList)

        // Search for 'vdut'
        let searchField = app.searchFields.firstMatch
        waitForExistence(searchField)
        searchField.tap()
        searchField.typeText("vdut")

        // Open the daq.vdut entry
        let docRow = app.descendants(matching: .any)["doc_row_daq.vdut"]
        waitForExistence(docRow)
        docRow.tap()

        // Insert snippet (scroll down in detail scrollview if needed)
        let insertButton = app.descendants(matching: .any)["doc_insert_button"]
        if !insertButton.isHittable {
            app.scrollViews.firstMatch.swipeUp()
        }
        waitForExistence(insertButton)
        insertButton.tap()

        // Verify docs sheet dismissed and the snippet landed in the editor
        let saveButton = app.buttons["editor_save_button"]
        waitForExistence(saveButton)
        let editorValue = app.textViews.firstMatch.value as? String ?? ""
        XCTAssertTrue(editorValue.contains("daq.vdut"), "Inserted docs snippet should appear in editor, got: \(editorValue)")

        let backButton = app.buttons["editor_back_button"]
        waitForExistence(backButton)
        backButton.tap()
    }

    func testSaveShowsLintBanner() {
        let app = launchApp(initialSection: 4)
        // Lint is skipped while a script runs ("Syntax check skipped"), so start idle.
        stopRunningScriptIfAny()

        let helloRow = app.descendants(matching: .any)["script_row_hello.py"]
        scrollTo(helloRow)
        helloRow.tap()

        let textView = app.textViews.firstMatch
        waitForExistence(textView)
        textView.tap()

        // Introduce syntax error
        textView.typeText("\nsyntax_error = True")

        let saveButton = app.buttons["editor_save_button"]
        waitForExistence(saveButton)
        saveButton.tap()

        // Lint failure banner appears
        let lintBanner = app.descendants(matching: .any)["script_lint_banner"]
        waitForExistence(lintBanner)
        waitForExistence(app.staticTexts["line 1: invalid syntax"])

        waitForExistence(app.buttons["editor_back_button"])
        app.buttons["editor_back_button"].tap()
    }

    func testSaveCleanFileShowsSyntaxOK() {
        let app = launchApp(initialSection: 4)
        stopRunningScriptIfAny()

        let helloRow = app.descendants(matching: .any)["script_row_hello.py"]
        scrollTo(helloRow)
        helloRow.tap()

        let saveButton = app.buttons["editor_save_button"]
        waitForExistence(saveButton)
        saveButton.tap()

        waitForExistence(app.descendants(matching: .any)["script_lint_banner"])
        waitForExistence(app.staticTexts["Saved · syntax OK"])

        app.buttons["editor_back_button"].tap()
    }

    func testRunForegroundOpensLogConsole() {
        let app = launchApp(initialSection: 4)

        // Stop background_logger.py if running so replace dialog is avoided
        let headerStop = app.descendants(matching: .any)["script_header_stop_button"]
        if headerStop.waitForExistence(timeout: 3) {
            headerStop.tap()
        }

        let runHello = app.descendants(matching: .any)["script_run_hello.py"]
        scrollTo(runHello)
        runHello.tap()

        // Console opens
        let console = logConsole
        waitForExistence(console)

        if !isPad {
            let sheet = app.descendants(matching: .any)["script_log_console"]
            XCTAssertTrue(sheet.exists)
            let closeBtn = app.descendants(matching: .any)["console_close_button"]
            waitForExistence(closeBtn)
            closeBtn.tap()
        }
    }

    func testRunInBackgroundViaMenu() {
        let app = launchApp(initialSection: 4)

        // Stop running script first
        let headerStop = app.descendants(matching: .any)["script_header_stop_button"]
        if headerStop.waitForExistence(timeout: 3) {
            headerStop.tap()
        }

        let runHello = app.descendants(matching: .any)["script_run_hello.py"]
        scrollTo(runHello)
        runHello.press(forDuration: 1.5)

        let runBackgroundButton = app.buttons["Run in background"]
        if runBackgroundButton.waitForExistence(timeout: 3) {
            runBackgroundButton.tap()
        }

        // Script is running
        let stopButton = app.descendants(matching: .any)["script_header_stop_button"]
        waitForExistence(stopButton)

        // Console should not open automatically for background runs on iPhone
        if !isPad {
            let sheet = app.descendants(matching: .any)["script_log_console"]
            XCTAssertFalse(sheet.exists)
        }
    }

    func testReplace409AlertAndConfirm() {
        let app = launchApp(initialSection: 4)

        // background_logger.py is already running at launch
        let runHello = app.descendants(matching: .any)["script_run_hello.py"]
        scrollTo(runHello)
        runHello.tap()

        // 409 replace alert with exact text
        let alert = app.alerts["Stop 'background_logger.py' and run 'hello.py'?"]
        waitForExistence(alert)

        let stopAndRunButton = alert.buttons["Stop and run"]
        waitForExistence(stopAndRunButton)
        stopAndRunButton.tap()

        // Alert dismisses and hello.py runs
        XCTAssertFalse(alert.exists)
        let header = app.descendants(matching: .any)["script_status_header"]
        XCTAssertTrue(header.staticTexts["hello.py"].waitForExistence(timeout: 4))
    }

    func testStopFromHeaderAndFromPill() {
        let app = launchApp(initialSection: 4)

        // 1. Stop from header
        let headerStop = app.descendants(matching: .any)["script_header_stop_button"]
        waitForExistence(headerStop)
        headerStop.tap()

        let pill = app.descendants(matching: .any)["script_status_pill"]
        XCTAssertFalse(pill.waitForExistence(timeout: 2))

        // Start hello.py to bring pill back
        let runHello = app.descendants(matching: .any)["script_run_hello.py"]
        scrollTo(runHello)
        runHello.tap()

        // If console sheet opened on iPhone, close it
        if !isPad {
            let closeConsole = app.descendants(matching: .any)["console_close_button"]
            if closeConsole.waitForExistence(timeout: 3) {
                closeConsole.tap()
            }
        }

        // 2. Stop from pill
        let pillStop = app.descendants(matching: .any)["script_pill_stop_button"]
        waitForExistence(pillStop)
        pillStop.tap()

        XCTAssertFalse(pillStop.waitForExistence(timeout: 3))
    }

    func testPillVisibleOnEveryTabWhileScriptRuns() {
        let app = launchApp(initialSection: 0)

        let pill = app.descendants(matching: .any)["script_status_pill"]
        waitForExistence(pill)

        // Signal Path
        navigateTo(section: "signal_path", index: 1)
        waitForExistence(pill)

        // Scope
        navigateTo(section: "scope", index: 2)
        waitForExistence(pill)

        // Diagnostics
        navigateTo(section: "diagnostics", index: 3)
        waitForExistence(pill)

        // Scripts
        navigateTo(section: "scripts", index: 4)
        waitForExistence(pill)
    }

    func testIPadLogColumnTogglePersistsWhenSwitchingSections() {
        guard isPad else { return }

        let app = launchApp(initialSection: 4)

        let toggleButton = app.descendants(matching: .any)["script_log_toggle_button"]
        waitForExistence(toggleButton)
        toggleButton.tap()

        let logColumn = app.descendants(matching: .any)["script_log_column"]
        waitForExistence(logColumn)

        // Switch to Overview
        navigateTo(section: "overview", index: 0)
        waitForExistence(logColumn)

        // Switch to Scope
        navigateTo(section: "scope", index: 2)
        waitForExistence(logColumn)

        // Switch to Diagnostics
        navigateTo(section: "diagnostics", index: 3)
        waitForExistence(logColumn)

        // Switch back to Scripts
        navigateTo(section: "scripts", index: 4)
        waitForExistence(logColumn)

        // Close log column
        toggleButton.tap()
        XCTAssertFalse(logColumn.waitForExistence(timeout: 2))
    }

    func testIPhoneConsoleSheet() {
        guard !isPad else { return }

        let app = launchApp(initialSection: 4)

        let headerLogButton = app.descendants(matching: .any)["script_header_log_button"]
        waitForExistence(headerLogButton)
        headerLogButton.tap()

        let sheet = app.descendants(matching: .any)["script_log_console"]
        waitForExistence(sheet)

        let closeButton = app.descendants(matching: .any)["console_close_button"]
        waitForExistence(closeButton)
        closeButton.tap()

        XCTAssertFalse(sheet.waitForExistence(timeout: 2))
    }

    func testConsoleFilterSearchAndPause() {
        let app = launchApp(initialSection: 4)

        // Open console
        if isPad {
            let toggle = app.descendants(matching: .any)["script_log_toggle_button"]
            waitForExistence(toggle)
            toggle.tap()
        } else {
            let logBtn = app.descendants(matching: .any)["script_header_log_button"]
            waitForExistence(logBtn)
            logBtn.tap()
        }

        let console = logConsole
        waitForExistence(console)

        // Toggle filter text field
        let searchButton = app.descendants(matching: .any)["console_filter_text_button"]
        waitForExistence(searchButton)
        searchButton.tap()

        let filterTextField = app.descendants(matching: .any)["console_filter_text_field"]
        waitForExistence(filterTextField)
        filterTextField.tap()
        filterTextField.typeText("tick")

        // Toggle auto-scroll pause
        let pauseButton = app.descendants(matching: .any)["console_pause_button"]
        waitForExistence(pauseButton)
        pauseButton.tap()
        pauseButton.tap()

        // Filter levels menu exists
        let levelsMenu = app.descendants(matching: .any)["console_filter_levels_menu"]
        waitForExistence(levelsMenu)
    }

    func testREPLViewOpensWifiMock() {
        let app = launchApp(initialSection: 4)

        let replButton = app.descendants(matching: .any)["scripts_repl_button"]
        waitForExistence(replButton)
        replButton.tap()

        let replView = app.descendants(matching: .any)["repl_view"]
        waitForExistence(replView)
        XCTAssertTrue(app.staticTexts["MicroPython shell"].exists)

        // Banner indicates background_logger.py holds the file slot
        let banner = app.descendants(matching: .any)["repl_banner"]
        waitForExistence(banner)

        let exitButton = app.descendants(matching: .any)["repl_exit_button"]
        waitForExistence(exitButton)
        exitButton.tap()

        XCTAssertFalse(replView.waitForExistence(timeout: 2))
    }
}
