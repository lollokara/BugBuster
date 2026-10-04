import XCTest
@testable import BugBuster

/// Regression for the "SOC spikes back to ~100 %" artifact (run #2, Lead-acid 6S 10000mAh): hub points
/// from early in the run were mapped hours late through a wall map that has no anchor after a reboot.
final class BattSimSocMergeTests: XCTestCase {
    private func le(_ v: Int, _ n: Int) -> [UInt8] { (0..<n).map { UInt8(truncatingIfNeeded: v >> (8 * $0)) } }

    private func meta() -> Data {
        var b = le(0x4E52_5342, 4) + le(2, 2) + le(2, 2) + le(1_790_954_740, 4)
        b += Array(repeating: 0, count: 68 - b.count)
        return Data(b)
    }

    /// v2 record: t, v, soc (0.01 %), dt.
    private func rec(t: Int, soc: Int, dt: Int = 60) -> [UInt8] {
        var b = le(t, 4) + le(12_700, 2) + le(12_700, 2) + le(12_700, 2) + le(soc, 2)
        b += le(0, 4) + le(0, 4) + le(0, 2) + le(dt, 2)
        b += Array(repeating: 0, count: 48 - b.count)
        return b
    }

    private func device(minutes: Int = 600) -> BsHistory {
        var m1: [UInt8] = []
        for k in 1...minutes { m1 += rec(t: k * 60, soc: 10_000 - k * 5) }   // 100 % -> 70 %
        var q: [UInt8] = []
        for k in stride(from: 15, through: minutes, by: 15) { q += rec(t: k * 60, soc: 10_000 - k * 5, dt: 900) }
        let files = ["meta.bin": meta(), "q15.bin": Data(q), "m0000.bin": Data(m1)]
        return try! BattSim.buildHistory(files)
    }

    private func assertNonIncreasing(_ recs: [BsRec], file: StaticString = #filePath, line: UInt = #line) {
        for i in recs.indices.dropFirst() where recs[i].soc > recs[i - 1].soc + 1e-9 {
            XCTFail("SOC rises \(recs[i - 1].soc) -> \(recs[i].soc) at t=\(recs[i].t)", file: file, line: line)
            return
        }
    }

    func testDeviceTiersMergeToMonotoneSoc() {
        let h = device()
        XCTAssertEqual(h.recs.count, 600)          // q15 fully replaced by m1, no duplicates
        assertNonIncreasing(h.recs)
    }

    func testMisplacedHubPointsAreRejectedAndMergeStaysMonotone() {
        let h = device()
        // Wall map with one anchor only (as after a reboot START with b = 0).
        let map = RunWallMap(created: 1_790_954_740, events: [(0, 2, 1_790_954_819)])
        // Hub holds correct early samples (SOC ~99 %), but they sit at wall times the map sends to hour 8+.
        var hub: [SeriesPoint] = []
        for k in 0..<40 {
            let runT = 28_800.0 + Double(k) * 60 + 60            // where the map places them
            hub.append(SeriesPoint(t: map.unix(runT), dt: 60, vAvg: 12.7, vMin: 12.7, vMax: 12.7,
                                   iAvg: 0.03, iMin: 0.03, iMax: 0.03, soc: 99.0 - Double(k) * 0.01,
                                   res: 60, source: .hub))
        }
        // Without the guard the hub points would win and re-introduce spikes.
        let naive = HubMerge.merge(hub: hub, device: BattSimHub.points(h.recs, map: map))
        XCTAssertFalse(BattSimHub.recs(naive, device: h.recs, map: map).map(\.soc).isMonotoneDown)

        let kept = BattSimHub.consistent(hub, device: h.recs, map: map)
        XCTAssertTrue(kept.isEmpty)
        let merged = HubMerge.merge(hub: kept, device: BattSimHub.points(h.recs, map: map))
        assertNonIncreasing(BattSimHub.recs(merged, device: h.recs, map: map))
    }

    func testConsistentHubPointsAreKept() {
        let h = device()
        let map = RunWallMap(created: 1_790_954_740, events: [(0, 2, 1_790_954_819)])
        let k = 100
        let good = SeriesPoint(t: map.unix(Double(k * 60)), dt: 60, vAvg: 12.7, vMin: 12.7, vMax: 12.7,
                               iAvg: 0.03, iMin: 0.03, iMax: 0.03, soc: Double(10_000 - k * 5) / 100,
                               res: 1, source: .hub)
        XCTAssertEqual(BattSimHub.consistent([good], device: h.recs, map: map).count, 1)
    }
}

private extension Array where Element == Double {
    var isMonotoneDown: Bool { indices.dropFirst().allSatisfy { self[$0] <= self[$0 - 1] + 1e-9 } }
}
