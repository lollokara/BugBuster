import Foundation

private var failures = 0
private func expect<T: Equatable>(_ got: T, _ want: T, _ what: String, line: UInt = #line) {
    if got != want { failures += 1; print("FAIL (line \(line)): \(what) - got \(got), want \(want)") }
}

@main
struct RunWallMapTestMain {
    static func main() {
        let ev: [(t: Double, code: Int, b: Int)] = [(0, 2, 1_791_000_000), (600, 3, 0), (600, 2, 1_791_003_600), (1200, 4, 0)]
        let m = RunWallMap(created: 1_790_999_000, events: ev)
        expect(m.unix(300), 1_791_000_300, "inside the first active stretch")
        expect(m.unix(600), 1_791_000_600, "an interval ending at the pause belongs to the first stretch")
        expect(m.unix(601), 1_791_003_601, "after the second START")
        expect(m.runTime(1_791_000_300), 300, "inverse, first stretch")
        expect(m.runTime(1_791_003_601), 601, "inverse, second stretch")
        let none = RunWallMap(created: 1000, events: [(0, 2, 0)])
        expect(none.unix(50), 1050, "a START without a wall clock falls back to created + t")
        expect(m.source(300).src, "P4_EPOCH", "first stretch clock source")
        expect(m.source(300).uncMs, 215, "first stretch uncertainty")
        let est = RunWallMap(created: 0, events: [(0, 2, 0)])
        expect(est.source(50).src, "EST", "no wall clock falls back to EST")
        expect(est.source(50).uncMs, 3_600_000, "EST uncertainty is 1h")
        exit(failures == 0 ? 0 : 1)
    }
}
