import XCTest
@testable import BugBuster

final class ScriptLogStoreTests: XCTestCase {
    private func page(_ text: String, since: UInt64, next: UInt64? = nil, dropped: UInt64 = 0) -> ScriptLogPage {
        let data = Data(text.utf8)
        return ScriptLogPage(data: data, since: since, next: next ?? since + UInt64(data.count), dropped: dropped)
    }

    // MARK: parse

    func testParsesStructuredLine() {
        let l = ScriptLogLine.parse("120500 W mpy ch0 above 2.6 V", id: 1)
        XCTAssertEqual(l.tsMs, 120_500)
        XCTAssertEqual(l.level, .warning)
        XCTAssertEqual(l.source, "mpy")
        XCTAssertEqual(l.text, "ch0 above 2.6 V")
        XCTAssertFalse(l.isMarker)
        XCTAssertEqual(l.plainText, "120500 W mpy ch0 above 2.6 V")
    }

    func testParsesEmptyText() {
        XCTAssertEqual(ScriptLogLine.parse("5 I mpy ", id: 1).text, "")
        XCTAssertEqual(ScriptLogLine.parse("5 E sys", id: 1).text, "")
        XCTAssertEqual(ScriptLogLine.parse("5 E sys", id: 1).level, .error)
    }

    func testUnstructuredLineKeepsText() {
        for raw in ["Traceback (most recent call last):", "123 X mpy odd level", "hello"] {
            let l = ScriptLogLine.parse(raw, id: 9)
            XCTAssertNil(l.tsMs, raw)
            XCTAssertEqual(l.level, .info, raw)
            XCTAssertEqual(l.source, "", raw)
            XCTAssertEqual(l.text, raw, raw)
        }
    }

    // MARK: assembler

    func testAssemblerCarriesPartialLine() {
        var a = ScriptLogAssembler()
        XCTAssertEqual(a.feed(Data("1 I mpy hel".utf8)), [])
        XCTAssertTrue(a.hasPartial)
        XCTAssertEqual(a.feed(Data("lo\n2 I mpy x\n".utf8)), ["1 I mpy hello", "2 I mpy x"])
        XCTAssertFalse(a.hasPartial)
    }

    func testAssemblerStripsCR() {
        var a = ScriptLogAssembler()
        XCTAssertEqual(a.feed(Data("1 I mpy a\r\n".utf8)), ["1 I mpy a"])
    }

    func testAssemblerKeepsSplitUTF8Intact() {
        let bytes = Array("1 I mpy 25 °C\n".utf8)
        let cut = bytes.firstIndex(of: 0xC2)! + 1          // between 0xC2 and 0xB0 of "°"
        var a = ScriptLogAssembler()
        XCTAssertEqual(a.feed(Data(bytes[..<cut])), [])
        XCTAssertEqual(a.feed(Data(bytes[cut...])), ["1 I mpy 25 °C"])
    }

    func testAssemblerFlushesRunawayPartial() {
        var a = ScriptLogAssembler()
        let out = a.feed(Data(repeating: 0x41, count: ScriptLogAssembler.maxPartial + 1))
        XCTAssertEqual(out.count, 1)
        XCTAssertFalse(a.hasPartial)
    }

    // MARK: store

    @MainActor
    func testIngestParsesAndAdvancesCursor() {
        let store = ScriptLogStore()
        store.ingest(page("1 I mpy a\n2 E mpy b\n", since: 0))
        XCTAssertEqual(store.lines.map(\.text), ["a", "b"])
        XCTAssertEqual(store.lines.map(\.level), [.info, .error])
        XCTAssertEqual(store.cursor, 20)
        XCTAssertEqual(store.lastLine?.text, "b")
    }

    @MainActor
    func testRestartedPageResetsCursorAndMarks() {
        let store = ScriptLogStore()
        store.ingest(page("1 I mpy a\n", since: 0, next: 900))
        store.ingest(ScriptLogPage(data: Data(), since: 900, next: 40, dropped: 0))
        XCTAssertEqual(store.cursor, 0)
        XCTAssertTrue(store.lines.last?.isMarker == true)
        XCTAssertEqual(store.lines.last?.text, "Device log restarted")
    }

    @MainActor
    func testDroppedMarkerOnlyAfterFirstAttach() {
        let store = ScriptLogStore()
        store.ingest(page("1 I mpy first\n", since: 0, next: 5000, dropped: 4986))   // first attach to a wrapped ring
        XCTAssertFalse(store.lines.contains { $0.isMarker })
        store.ingest(page("2 I mpy later\n", since: 5000, next: 9000, dropped: 3986))
        XCTAssertEqual(store.lines.filter(\.isMarker).map(\.text), ["3986 bytes of log dropped (ring overflow)"])
        XCTAssertEqual(store.lines.last?.text, "later")
    }

    @MainActor
    func testDroppedBytesDiscardThePartialLine() {
        let store = ScriptLogStore()
        store.ingest(page("1 I mpy half", since: 0))
        store.ingest(page("2 I mpy whole\n", since: 12, next: 600, dropped: 574))
        XCTAssertEqual(store.lines.filter { !$0.isMarker }.map(\.text), ["whole"])
    }

    @MainActor
    func testCapacityKeepsNewest() {
        let store = ScriptLogStore(capacity: 3)
        store.ingest(page((1...5).map { "\($0) I mpy l\($0)\n" }.joined(), since: 0))
        XCTAssertEqual(store.lines.map(\.text), ["l3", "l4", "l5"])
        XCTAssertEqual(store.lines.map(\.id), store.lines.map(\.id).sorted())
    }

    @MainActor
    func testClearKeepsCursorResetDoesNot() {
        let store = ScriptLogStore()
        store.ingest(page("1 I mpy a\n", since: 0))
        store.clear()
        XCTAssertTrue(store.lines.isEmpty)
        XCTAssertEqual(store.cursor, 10)
        store.isPaused = true
        store.reset()
        XCTAssertEqual(store.cursor, 0)
        XCTAssertFalse(store.isPaused)
    }

    @MainActor
    func testPlainTextForCopy() {
        let store = ScriptLogStore()
        store.ingest(page("1 I mpy a\n", since: 0))
        store.appendMarker("Started 'x.py'")
        XCTAssertEqual(store.plainText, "1 I mpy a\n--- Started 'x.py' ---")
    }

    // MARK: filter

    func testFilterByLevelAndQueryKeepsMarkers() {
        let lines = [ScriptLogLine.parse("1 E mpy Boom", id: 1),
                     ScriptLogLine.parse("2 I mpy fine", id: 2),
                     ScriptLogLine.marker("Started", id: 3)]
        var f = ScriptLogFilter()
        XCTAssertEqual(f.apply(lines).count, 3)
        f.levels = [.error]
        XCTAssertEqual(f.apply(lines).map(\.id), [1, 3])
        f.levels = Set(ScriptLogLevel.allCases)
        f.query = "boom"
        XCTAssertEqual(f.apply(lines).map(\.id), [1, 3])
        f.query = "   "
        XCTAssertEqual(f.apply(lines).count, 3)
    }
}
