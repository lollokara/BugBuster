import XCTest
@testable import BugBuster

final class FakeBLEAPI: BLEAPITransport {
    var requests: [(path: String, body: [String: Any]?)] = []
    var reply: Data?

    func apiRequest(path: String, body: [String: Any]?, timeout: TimeInterval) async -> Data? {
        requests.append((path, body))
        return reply
    }
}

final class DiagnosticsTransportTests: XCTestCase {
    // Captured from handle_get_selftest_supplies() (webserver.cpp).
    private let suppliesJSON = """
    {"valid":true,"suppliesOk":true,"avddHiV":5.021,"dvccV":1.201,"avccV":3.298,"avssV":-0.012,"tempC":31.5}
    """

    func testDecodesFirmwareSuppliesShape() throws {
        let resp = try JSONDecoder().decode(InternalSuppliesResponse.self, from: Data(suppliesJSON.utf8))
        XCTAssertTrue(resp.valid)
        XCTAssertTrue(resp.suppliesOk)
        XCTAssertEqual(resp.supplies.map(\.name), ["AVDD_HI", "DVCC", "AVCC", "AVSS", "Temp"])
        XCTAssertEqual(resp.supplies.map(\.value), [5.021, 1.201, 3.298, -0.012, 31.5])
        XCTAssertEqual(resp.supplies.last?.unit, "°C")
    }

    @MainActor
    func testGetRequestOverBLEWithEmptyIPUsesTunnel() async throws {
        let cm = ConnectionManager()
        let fake = FakeBLEAPI()
        fake.reply = Data(suppliesJSON.utf8)
        cm.bleAPI = fake
        cm.transport = .ble
        cm.activeDevice = DiscoveredDevice(hostname: "bb", ip: "", port: 0, isBle: true, bleId: UUID())

        let resp: InternalSuppliesResponse = try await cm.getRequest(path: "/api/selftest/supplies")

        XCTAssertEqual(fake.requests.map(\.path), ["/api/selftest/supplies"])
        XCTAssertEqual(resp.avddHiV, 5.021)
    }

    @MainActor
    func testBLEUnknownPathSurfacesFirmwareError() async {
        let cm = ConnectionManager()
        let fake = FakeBLEAPI()
        fake.reply = Data(#"{"error":"unknown path"}"#.utf8)
        cm.bleAPI = fake
        cm.transport = .ble
        cm.activeDevice = DiscoveredDevice(hostname: "bb", ip: "", port: 0, isBle: true, bleId: UUID())

        do {
            let _: OtaUpdateStatus = try await cm.getRequest(path: "/api/update/status")
            XCTFail("expected an error")
        } catch let error as ConnectionAPIError {
            guard case .bleRejected(_, let message) = error else { return XCTFail("wrong case \(error)") }
            XCTAssertEqual(message, "unknown path")
        } catch {
            XCTFail("unexpected error \(error)")
        }
    }
}
