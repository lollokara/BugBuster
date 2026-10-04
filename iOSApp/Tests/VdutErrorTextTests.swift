import XCTest
@testable import BugBuster

final class VdutErrorTextTests: XCTestCase {
    func testDecodesRunOwnsVdutError() {
        let body = Data(#"{"ok":false,"error":"battery simulator run 7 is loaded and owns VDUT; unload it first","runId":7}"#.utf8)
        XCTAssertEqual(ConnectionManager.firmwareErrorText(from: body),
                       "battery simulator run 7 is loaded and owns VDUT; unload it first")
    }

    func testNoErrorKeyYieldsNil() {
        XCTAssertNil(ConnectionManager.firmwareErrorText(from: Data(#"{"ok":true}"#.utf8)))
        XCTAssertNil(ConnectionManager.firmwareErrorText(from: Data("garbage".utf8)))
        XCTAssertNil(ConnectionManager.firmwareErrorText(from: Data(#"{"error":""}"#.utf8)))
    }
}
