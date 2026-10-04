import XCTest
@testable import BugBuster

final class FirmwareDocsTests: XCTestCase {
    private var entries: [FirmwareDocEntry] = []

    override func setUpWithError() throws {
        entries = FirmwareDocs.entries(try XCTUnwrap(FirmwareAPICatalogue.bundled))
    }

    private func entry(_ id: String) throws -> FirmwareDocEntry {
        try XCTUnwrap(entries.first { $0.id == id }, id)
    }

    func testNamespaceMethodEntry() throws {
        let e = try entry("daq.run.new")
        XCTAssertEqual(e.kind, .method)
        XCTAssertEqual(e.title, "daq.run.new")
        XCTAssertTrue(e.signature.hasPrefix("daq.run.new(name: str"))
        XCTAssertEqual(e.snippet, "daq.run.new(name, chem, cells, capacity_mah)")
        XCTAssertEqual(e.requiredImport, "daq")
    }

    func testClassAndInstanceMethodEntries() throws {
        let cls = try entry("bugbuster.Channel")
        XCTAssertEqual(cls.kind, .type)
        XCTAssertEqual(cls.signature, "bugbuster.Channel(channel: int)")
        XCTAssertEqual(cls.snippet, "bugbuster.Channel(channel)")
        let method = try entry("bugbuster.Channel.set_voltage")
        XCTAssertEqual(method.title, "Channel.set_voltage")
        XCTAssertEqual(method.snippet, "channel.set_voltage(voltage)")
        XCTAssertNil(method.requiredImport)
        XCTAssertFalse(entries.contains { $0.id.hasSuffix(".__init__") || $0.id.hasSuffix(".__enter__") })
    }

    func testFunctionModuleAndConstantEntries() throws {
        XCTAssertEqual(try entry("daq.vdut").snippet, "daq.vdut()")
        XCTAssertEqual(try entry("bugbuster.FUNC_VOUT").signature, "bugbuster.FUNC_VOUT: int")
        XCTAssertEqual(try entry("bugbuster").snippet, "import bugbuster\n")
        XCTAssertEqual(try entry("daq.run").kind, .namespace)
    }

    func testExamplesCarryTheirSource() throws {
        let ex = try entry("example:background_logger.py")
        XCTAssertEqual(ex.kind, .example)
        XCTAssertTrue(ex.snippet.contains("import bugbuster"))
        XCTAssertNil(ex.requiredImport)
    }

    func testSectionsGroupByModuleThenExamples() {
        XCTAssertEqual(FirmwareDocs.sections(entries, query: "").map(\.title),
                       ["bugbuster", "daq", "machine", "bb_helpers", "bb_devices", "bb_logging", "Examples"])
    }

    func testSearchRanksNameMatchesFirst() throws {
        XCTAssertEqual(FirmwareDocs.search(entries, query: "vdut").first?.id, "daq.vdut")
        XCTAssertEqual(FirmwareDocs.search(entries, query: "  VDUT ").first?.id, "daq.vdut")
        let volt = FirmwareDocs.search(entries, query: "volt").map(\.id)
        let byName = try XCTUnwrap(volt.firstIndex(of: "bugbuster.Channel.set_voltage"))
        let bySignature = try XCTUnwrap(volt.firstIndex(of: "daq.vdut"))      // "volts" only in its signature
        XCTAssertLessThan(byName, bySignature)
        XCTAssertTrue(FirmwareDocs.search(entries, query: "zzzz").isEmpty)
        XCTAssertEqual(FirmwareDocs.search(entries, query: "").count, entries.count)
        XCTAssertEqual(FirmwareDocs.sections(entries, query: "vdut").map(\.title), ["Results"])
        XCTAssertEqual(FirmwareDocs.sections(entries, query: "zzzz"), [])
    }

    func testCallSnippetUsesRequiredParamsOnly() {
        let fn = FirmwareFunction(name: "f", signature: "f(x, *, y, z=1)", params: [
            FirmwareParam(name: "x", kind: .positional, annotation: nil, defaultValue: nil),
            FirmwareParam(name: "y", kind: .keywordOnly, annotation: nil, defaultValue: nil),
            FirmwareParam(name: "z", kind: .keywordOnly, annotation: nil, defaultValue: "1")
        ], returns: nil, doc: "")
        XCTAssertEqual(FirmwareDocs.callSnippet("m.f", fn), "m.f(x, y=y)")
        XCTAssertEqual(FirmwareDocs.callSnippet("m.g", nil), "m.g()")
    }

    func testNeedsImport() {
        XCTAssertTrue(FirmwareDocs.needsImport("daq", in: ""))
        XCTAssertFalse(FirmwareDocs.needsImport("daq", in: "import daq\n"))
        XCTAssertFalse(FirmwareDocs.needsImport("daq", in: "import bugbuster, daq as d\n"))
        XCTAssertFalse(FirmwareDocs.needsImport("daq", in: "x = 1\nfrom daq import vdut\n"))
        XCTAssertTrue(FirmwareDocs.needsImport("daq", in: "import daqx\n"))
        XCTAssertTrue(FirmwareDocs.needsImport("daq", in: "# import daq\n"))
    }

    func testDetailEntriesCarryParametersReturnsAndExamples() throws {
        let cat = try XCTUnwrap(FirmwareAPICatalogue.bundled)
        let entries = FirmwareDocs.entries(cat)
        let vdut = try XCTUnwrap(entries.first { $0.id == "daq.vdut" })
        XCTAssertEqual(vdut.params.map(\.name), ["enable", "volts", "amps_limit"])
        XCTAssertTrue(vdut.params.allSatisfy { !$0.doc.isEmpty })
        XCTAssertEqual(vdut.returnType, "dict")
        XCTAssertEqual(vdut.detail.returnKeys.first?.name, "present")
        XCTAssertFalse(vdut.detail.notes.isEmpty)
        XCTAssertFalse(vdut.detail.examples.isEmpty)
        XCTAssertFalse(vdut.summary.isEmpty)
        let channel = try XCTUnwrap(entries.first { $0.id == "bugbuster.Channel" })
        XCTAssertEqual(channel.params.map(\.name), ["channel"], "a class shows its constructor parameters")
        XCTAssertFalse(channel.detail.examples.isEmpty)
        let constant = try XCTUnwrap(entries.first { $0.id == "bugbuster.FUNC_VOUT" })
        XCTAssertEqual(constant.value, "1")
        XCTAssertFalse(constant.summary.isEmpty)
    }

    func testDocDetailFormatting() {
        let p = FirmwareParam(name: "freq", kind: .keywordOnly, annotation: "int", defaultValue: "400000")
        XCTAssertEqual(ScriptDocDetail.parameterName(p), "freq=")
        XCTAssertEqual(ScriptDocDetail.requirement(p), "default 400000")
        let required = FirmwareParam(name: "ms", kind: .positional, annotation: nil, defaultValue: nil)
        XCTAssertEqual(ScriptDocDetail.requirement(required), "required")
        let rest = FirmwareParam(name: "params", kind: .varKeyword, annotation: nil, defaultValue: nil)
        XCTAssertEqual(ScriptDocDetail.parameterName(rest), "**params")
        XCTAssertEqual(ScriptDocDetail.requirement(rest), "optional")
    }
}
