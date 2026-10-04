import XCTest

final class DynamicTypeUITests: UITestBase {
    func testScriptsTabControlsStayHittableAtLargeDynamicType() {
        let app = launchApp(
            initialSection: 4,
            extraArgs: ["-UIPreferredContentSizeCategoryName", "UICTContentSizeCategoryAccessibilityXXXL"]
        )

        // Stored scripts browser controls
        let header = app.descendants(matching: .any)["script_status_header"]
        waitForExistence(header)

        let newButton = app.buttons["scripts_new_button"]
        waitForExistence(newButton)
        XCTAssertTrue(newButton.isHittable, "New script button must stay hittable at XXXL dynamic type")

        let replButton = app.buttons["scripts_repl_button"]
        waitForExistence(replButton)
        XCTAssertTrue(replButton.isHittable, "REPL button must stay hittable at XXXL dynamic type")

        let helloRow = app.descendants(matching: .any)["script_row_hello.py"]
        scrollIntoView(helloRow)
        XCTAssertTrue(helloRow.isHittable, "Script row must stay hittable at XXXL dynamic type")

        let runButton = app.descendants(matching: .any)["script_run_hello.py"]
        scrollIntoView(runButton)
        XCTAssertTrue(runButton.isHittable, "Script run button must stay hittable at XXXL dynamic type")

        // Open editor
        helloRow.tap()

        let backButton = app.buttons["editor_back_button"]
        waitForExistence(backButton)
        XCTAssertTrue(backButton.isHittable, "Editor back button must stay hittable at XXXL dynamic type")

        let docsButton = app.buttons["editor_docs_button"]
        waitForExistence(docsButton)
        XCTAssertTrue(docsButton.isHittable, "Editor docs button must stay hittable at XXXL dynamic type")

        let saveButton = app.buttons["editor_save_button"]
        waitForExistence(saveButton)
        XCTAssertTrue(saveButton.isHittable, "Editor save button must stay hittable at XXXL dynamic type")

        let runMenu = app.descendants(matching: .any)["editor_run_menu"]
        waitForExistence(runMenu)
        XCTAssertTrue(runMenu.isHittable, "Editor run menu must stay hittable at XXXL dynamic type")

        backButton.tap()
    }
}
