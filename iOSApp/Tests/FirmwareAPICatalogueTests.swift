import XCTest
@testable import BugBuster

final class FirmwareAPICatalogueTests: XCTestCase {
    func testBundledCatalogueLoads() throws {
        let cat = try XCTUnwrap(FirmwareAPICatalogue.bundled, "firmware_api.json missing from the app bundle")
        XCTAssertEqual(cat.schema, FirmwareAPICatalogue.supportedSchema)
        XCTAssertEqual(Set(cat.modules.map(\.name)),
                       ["bugbuster", "daq", "machine", "bb_helpers", "bb_devices", "bb_logging"])
        XCTAssertFalse(cat.examples.isEmpty)
    }

    func testDaqRunIsANamespaceWithTheSpecMethods() throws {
        let daq = try XCTUnwrap(FirmwareAPICatalogue.bundled?.module(named: "daq"))
        let run = try XCTUnwrap(daq.classes.first { $0.name == "run" })
        XCTAssertEqual(run.kind, .namespace)
        XCTAssertTrue(Set(run.methods.map(\.name)).isSuperset(of:
            ["status", "list", "new", "start", "pause", "stop", "edit", "delete"]))
    }

    func testParamKindsAndDefaults() throws {
        let daq = try XCTUnwrap(FirmwareAPICatalogue.bundled?.module(named: "daq"))
        let vdut = try XCTUnwrap(daq.functions.first { $0.name == "vdut" })
        XCTAssertEqual(vdut.params.map(\.name), ["enable", "volts", "amps_limit"])
        XCTAssertEqual(vdut.params.map(\.kind), [.positional, .keywordOnly, .keywordOnly])
        XCTAssertEqual(vdut.params[1].defaultValue, "None")
        XCTAssertFalse(vdut.params[1].isRequired)
        XCTAssertTrue(vdut.params[1].acceptsKeyword)
    }

    func testClassesKeepTheirConstructor() throws {
        let bb = try XCTUnwrap(FirmwareAPICatalogue.bundled?.module(named: "bugbuster"))
        let channel = try XCTUnwrap(bb.classes.first { $0.name == "Channel" })
        XCTAssertEqual(channel.kind, .class)
        XCTAssertEqual(channel.methods.first?.name, "__init__")
        XCTAssertEqual(channel.methods.first?.params.map(\.name), ["channel"])
        XCTAssertEqual(channel.constructor?.name, "__init__")
        XCTAssertEqual(channel.constructorSignature, "Channel(channel: int)")
        let run = try XCTUnwrap(FirmwareAPICatalogue.bundled?.module(named: "daq")?.classes.first { $0.name == "run" })
        XCTAssertNil(run.constructor)
    }

    func testUnknownKindsDegradeInsteadOfFailing() throws {
        let json = """
        {"schema":"bugbuster.firmware-api/1","modules":[{"name":"m","doc":"",
         "functions":[{"name":"f","signature":"f(x, /)","params":[{"name":"x","kind":"positional_only","annotation":null,"default":null}],"returns":null,"doc":""}],
         "classes":[{"name":"K","kind":"protocol","doc":"","methods":[],"constants":[]}],
         "constants":[{"name":"C","annotation":"int","value":null}]}],"examples":[]}
        """
        let cat = try FirmwareAPICatalogue.decode(Data(json.utf8))
        XCTAssertEqual(cat.modules[0].functions[0].params[0].kind, .positional)
        XCTAssertEqual(cat.modules[0].classes[0].kind, .class)
        XCTAssertNil(cat.modules[0].functions[0].returns)
        XCTAssertNil(cat.modules[0].constants[0].value)
    }

    func testRejectsAnotherSchema() {
        let json = #"{"schema":"bugbuster.firmware-api/2","modules":[],"examples":[]}"#
        XCTAssertThrowsError(try FirmwareAPICatalogue.decode(Data(json.utf8))) { error in
            XCTAssertEqual(error as? FirmwareAPICatalogue.UnsupportedSchema,
                           FirmwareAPICatalogue.UnsupportedSchema(schema: "bugbuster.firmware-api/2"))
        }
    }

    func testExamplesCarrySource() throws {
        let ex = try XCTUnwrap(FirmwareAPICatalogue.bundled?.examples.first { $0.name == "background_logger.py" })
        XCTAssertTrue(ex.source.contains("import bugbuster"))
        XCTAssertFalse(ex.title.isEmpty)
    }

    func testMissingResourceYieldsNil() {
        XCTAssertNil(FirmwareAPICatalogue.load(bundle: Bundle(for: FirmwareAPICatalogueTests.self)))
    }
}
