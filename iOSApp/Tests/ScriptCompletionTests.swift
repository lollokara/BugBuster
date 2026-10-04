import XCTest
@testable import BugBuster

final class ScriptCompletionTests: XCTestCase {
    private var engine: ScriptCompletionEngine!

    override func setUpWithError() throws {
        engine = try XCTUnwrap(ScriptCompletionEngine.bundled, "catalogue missing from the app bundle")
    }

    /// `|` marks the caret.
    private func complete(_ marked: String) -> ScriptCompletionResult {
        let ns = marked as NSString
        let caret = ns.range(of: "|").location
        let text = ns.replacingCharacters(in: NSRange(location: caret, length: 1), with: "")
        return engine.complete(text: text, caret: caret)
    }

    private func labels(_ marked: String) -> [String] { complete(marked).items.map(\.label) }

    // MARK: members after "."

    func testModuleMembersAfterDot() {
        let result = complete("import daq\ndaq.|")
        XCTAssertTrue(Set(result.items.map(\.label)).isSuperset(of: ["run", "present", "vdut", "read", "samples"]))
        XCTAssertEqual(result.items.first?.label, "run")
        XCTAssertEqual(result.items.first?.kind, .namespace)
    }

    func testNamespaceMethodsArePrefixRankedAlphabetically() {
        XCTAssertEqual(labels("import daq\ndaq.run.st|"), ["start", "status", "stop"])
    }

    func testInsertTextAndCaretOffsets() throws {
        let items = complete("import daq\ndaq.|").items
        let vdut = try XCTUnwrap(items.first { $0.label == "vdut" })
        XCTAssertEqual(vdut.insertText, "vdut()")
        XCTAssertEqual(vdut.caretOffset, 5)                // inside the parens: it takes arguments
        let present = try XCTUnwrap(items.first { $0.label == "present" })
        XCTAssertEqual(present.caretOffset, 9)             // after ")": no arguments
        let run = try XCTUnwrap(items.first { $0.label == "run" })
        XCTAssertEqual(run.insertText, "run")
    }

    func testReplaceRangeCoversThePartial() {
        XCTAssertEqual(complete("import daq\ndaq.vd|").replaceRange, NSRange(location: 15, length: 2))
    }

    func testUnicodeBeforeTheCaretKeepsUTF16Offsets() {
        let text = "# café ☕️ µA\nimport daq\ndaq.v"
        let result = engine.complete(text: text, caret: (text as NSString).length)
        XCTAssertEqual(result.replaceRange, NSRange(location: (text as NSString).length - 1, length: 1))
        XCTAssertEqual(result.items.first?.label, "vdut")
    }

    func testInstanceMethodsFromConstructorAssignment() {
        let found = labels("import bugbuster\nch = bugbuster.Channel(0)\nch.|")
        XCTAssertTrue(Set(found).isSuperset(of: ["set_voltage", "read_voltage", "set_function", "set_do"]))
        XCTAssertFalse(found.contains("__init__"))
    }

    func testAliasedImport() throws {
        let first = try XCTUnwrap(complete("import bugbuster as bb\nbb.Ch|").items.first)
        XCTAssertEqual(first.label, "Channel")
        XCTAssertEqual(first.kind, .type)
        XCTAssertEqual(first.detail, "Channel(channel: int)")
    }

    func testFromImportedClass() {
        XCTAssertEqual(labels("from bugbuster import Channel\nc = Channel(1)\nc.read|"), ["read_voltage"])
    }

    func testReturnAnnotationGivesInstanceType() {
        XCTAssertEqual(labels("import bugbuster\nc = bugbuster.claim([0])\nc.|"), [])           // only dunders
        XCTAssertEqual(labels("import bugbuster\nc = bugbuster.claim([0])\nc.__|"), ["__enter__", "__exit__"])
    }

    func testUnknownReceiverGivesNothing() {
        XCTAssertTrue(complete("foo.|").isEmpty)
    }

    // MARK: kwargs inside calls

    func testKwargsInsideCall() {
        XCTAssertEqual(labels("import daq\ndaq.vdut(|"), ["enable=", "volts=", "amps_limit="])
    }

    func testKwargsSkipUsedAndPositionallyPassed() {
        XCTAssertEqual(labels("import daq\ndaq.vdut(True, volts=1.0, |"), ["amps_limit="])
    }

    func testKwargWordStartMatch() {
        XCTAssertEqual(labels("import daq\ndaq.vdut(lim|"), ["amps_limit="])
    }

    func testRequiredParamsRankAfterOptional() {
        XCTAssertEqual(labels("import bugbuster\nbugbuster.I2C(|"),
                       ["freq=", "pullups=", "supply=", "vlogic=", "allow_split_supplies=", "sda_io=", "scl_io="])
    }

    func testMultiLineCallStillFindsCallee() {
        XCTAssertEqual(labels("import daq\ndaq.run.new(\n    \"bench\",\n    \"lipo\",\n    |"),
                       ["soc=", "self_discharge=", "ext_load=", "cells=", "capacity_mah="])
    }

    func testIdentifierFallbackInsideCall() {
        XCTAssertEqual(labels("import daq\ndaq.vdut(da|").first, "daq")
    }

    // MARK: ranking

    func testPrefixBeforeWordStartThenGroups() {
        XCTAssertEqual(Array(labels("import bugbuster\nbugbuster.rail|").prefix(5)),
                       ["rail_power_up", "hat_rails", "hat_set_rail_enable", "hat_set_rail_voltage", "HAT_RAIL_3V3_ADJ"])
    }

    func testCaseInsensitivePrefix() {
        XCTAssertEqual(labels("import bugbuster\nbugbuster.func_v|"), ["FUNC_VIN", "FUNC_VOUT"])
    }

    func testScores() {
        XCTAssertEqual(ScriptCompletionRanker.score("set_voltage", partial: ""), 0)
        XCTAssertEqual(ScriptCompletionRanker.score("set_voltage", partial: "set"), 0)
        XCTAssertEqual(ScriptCompletionRanker.score("FUNC_VIN", partial: "func"), 1)
        XCTAssertEqual(ScriptCompletionRanker.score("set_voltage", partial: "volt"), 2)
        XCTAssertNil(ScriptCompletionRanker.score("set_voltage", partial: "oltage"))
    }

    func testExactNamespaceMatchIsSuppressed() {
        XCTAssertTrue(complete("import daq\ndaq.run|").isEmpty)
    }

    // MARK: imports, strings, comments, numbers

    func testImportModuleNames() {
        XCTAssertEqual(labels("import d|"), ["daq", "bb_devices"])      // prefix, then word-start
        XCTAssertEqual(labels("import bugbuster, b|"), ["bb_devices", "bb_helpers", "bb_logging", "bugbuster"])
    }

    func testFromImportListsModuleMembers() {
        XCTAssertTrue(Set(labels("from daq import |")).isSuperset(of: ["vdut", "run"]))
    }

    func testNothingInsideStringsOrComments() {
        XCTAssertTrue(complete("import daq\nprint(\"daq.|").isEmpty)
        XCTAssertTrue(complete("import daq\n# daq.|").isEmpty)
    }

    func testNumbersDoNotComplete() {
        XCTAssertTrue(complete("x = 1.|").isEmpty)
        XCTAssertTrue(complete("x = 12|").isEmpty)
    }

    // MARK: bindings

    func testBindingsScan() {
        let text = """
        import bugbuster as bb, daq
        from machine import Pin as P
        ch = bb.Channel(0)
        with bb.claim([1]) as cl:
            pass
        """
        let b = ScriptBindings.scan(text, index: engine.index)
        XCTAssertEqual(b.aliases["bb"], "bugbuster")
        XCTAssertEqual(b.aliases["daq"], "daq")
        XCTAssertEqual(b.aliases["P"], "machine.Pin")
        XCTAssertEqual(b.instances["ch"], "bugbuster.Channel")
        XCTAssertEqual(b.instances["cl"], "bugbuster.Claim")
    }

    func testStarImportAliasesEveryMember() {
        let b = ScriptBindings.scan("from daq import *\n", index: engine.index)
        XCTAssertEqual(b.aliases["vdut"], "daq.vdut")
        XCTAssertEqual(b.aliases["run"], "daq.run")
    }
}
