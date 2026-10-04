import XCTest
import SwiftUI
@testable import BugBuster

final class ScriptLogStyleTests: XCTestCase {
    func testLevelColours() {
        XCTAssertEqual(ScriptLogStyle.color(for: .error), .red)
        XCTAssertEqual(ScriptLogStyle.color(for: .warning), .orange)
        XCTAssertEqual(ScriptLogStyle.color(for: .info), .primary)
        XCTAssertEqual(ScriptLogStyle.color(for: .debug), .secondary)
    }

    func testTimestampIsSecondsWithMillis() {
        XCTAssertEqual(ScriptLogStyle.timestamp(120_500), "120.500")
        XCTAssertEqual(ScriptLogStyle.timestamp(7), "0.007")
        XCTAssertEqual(ScriptLogStyle.timestamp(nil), "")
    }

    func testShareFileName() {
        let date = Date(timeIntervalSince1970: 1_760_000_000)     // 2025-10-09 08:53:20 UTC
        XCTAssertEqual(ScriptLogStyle.shareFileName(device: "bugbuster-a1", date: date, timeZone: TimeZone(identifier: "UTC")!),
                       "bugbuster-a1-script-log-20251009-085320.txt")
    }
}
