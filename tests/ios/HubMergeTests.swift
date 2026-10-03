import Foundation

private var failures = 0
private func expect<T: Equatable>(_ got: T, _ want: T, _ what: String, line: UInt = #line) {
    if got != want { failures += 1; print("FAIL (line \(line)): \(what) - got \(got), want \(want)") }
}
private func check(_ cond: Bool, _ what: String, line: UInt = #line) {
    if !cond { failures += 1; print("FAIL (line \(line)): \(what)") }
}

private func pt(_ t: Double, _ dt: Double, _ src: DataSource, res: Int? = nil) -> SeriesPoint {
    SeriesPoint(t: t, dt: dt, vAvg: 3.7, vMin: 3.6, vMax: 3.8, iAvg: 0.1, iMin: 0.05, iMax: 0.2, soc: 50,
                res: res ?? Int(dt), source: src)
}

func runHubMergeTests() {
    // run_uid
    expect(HubRunUid.make(mac: "A0:B7:65:11:22:33", runId: 7, createdEpoch: 1_791_000_000), "a0b765112233-7-1791000000", "uid from STA MAC")
    expect(HubRunUid.make(mac: "a0b765112233", runId: 7, createdEpoch: 0), nil, "no uid while the device clock was unset")
    expect(HubRunUid.make(mac: "a0b7651122", runId: 7, createdEpoch: 5), nil, "short mac")
    expect(HubRunUid.make(mac: "zz:b7:65:11:22:33", runId: 7, createdEpoch: 5), nil, "non-hex mac")
    expect(HubRunUid.make(mac: "a0b765112233", runId: 70000, createdEpoch: 5), nil, "run id is u16")
    expect(HubRunUid.parse("a0b765112233-7-1791000000"), HubRunKey(mac: "a0b765112233", runId: 7, createdEpoch: 1_791_000_000), "parse")
    expect(HubRunUid.parse("A0b765112233-7-1"), nil, "uppercase is not a hub uid")

    // series decode: hole marker dropped, interval end = bucket start + max(bucket, res)
    let json = """
    {"run_uid":"x","bucket":60,"t":[600,660,720],"v_min":[3.5,null,3.4],"v_avg":[3.7,null,3.6],"v_max":[3.9,null,3.8],
     "i_min":[0.1,null,0.1],"i_avg":[0.2,null,0.2],"i_max":[0.3,null,0.3],"soc":[80,null,79],"p_avg":[1,null,1],"state":[2,null,2],"res":[60,null,900]}
    """
    let s = try! HubSeries.decode(Data(json.utf8))
    expect(s.points.count, 2, "hole row skipped")
    expect(s.points[0].t, 660, "bucket 60 res 60: interval end")
    expect(s.points[1].t, 720 + 900, "a 900 s row spans 900 s even in a 60 s bucket")
    expect(s.points[1].dt, 900, "dt follows the source resolution")
    expect(s.points[0].source, .hub, "hub source")
    do { _ = try HubSeries.decode(Data("[]".utf8)); check(false, "bad body must throw") } catch { check(true, "throws") }

    let cov = try! HubCoverage.decode(Data(#"{"ranges":[{"from":10,"to":20.5,"res":60,"points":3}]}"#.utf8))
    expect(cov.ranges, [HubCoverage.Range(from: 10, to: 20.5, res: 60)], "coverage decode")

    // gap detection by expected cadence
    let hub1s = (100...160).map { pt(Double($0), 1, .hub) } + (200...260).map { pt(Double($0), 1, .hub) }
    let gaps = HubMerge.gaps(hub: hub1s, from: 90, to: 270, expected: 1)
    expect(gaps.count, 3, "leading, middle and trailing gap")
    expect(gaps[1].from, 160, "middle gap starts after the last hub point")
    expect(gaps[1].to, 199, "middle gap ends before the next hub interval")
    expect(HubMerge.gaps(hub: (100...160).map { pt(Double($0), 1, .hub) }, from: 99, to: 160, expected: 1).count, 0, "contiguous hub data has no gap")

    // merge: hub wins, device is cut to what the hub lacks
    let hub = (100...200).map { pt(Double($0), 1, .hub) }
    let dev = [60, 120, 180, 240, 300].map { pt(Double($0), 60, .device) }
    let merged = HubMerge.merge(hub: hub, device: dev)
    let devOut = merged.filter { $0.source == .device }.map { [$0.t, $0.dt] }
    expect(devOut, [[60, 60], [99, 39], [240, 40], [300, 60]], "device points trimmed to the hub gaps")
    expect(Set(merged.map { $0.t }).count, merged.count, "no duplicate timestamps")
    expect(merged.map { $0.t }, merged.map { $0.t }.sorted(), "sorted")
    let total = merged.reduce(0) { $0 + $1.dt }
    expect(total, 60 + 39 + 101 + 40 + 60, "no overlap and no hole")

    // hub unreachable (phone on the DAQ hotspot): device data passes through untouched
    expect(HubMerge.merge(hub: [], device: dev), dev, "device only")
    expect(HubMerge.indicator(dev), "Device 1 min", "indicator device only")
    expect(HubMerge.indicator(hub), "Hub 1 s", "indicator hub only")
    expect(HubMerge.indicator(merged), "Hub 1 s + Device 1 min", "indicator mixed")
    expect(HubMerge.indicator([]), "", "indicator empty")
    expect(HubMerge.resLabel(900), "15 min", "15 min label")

    if failures > 0 { print("\(failures) failure(s)") }
}

@main
struct HubMergeTestMain {
    static func main() {
        runHubMergeTests()
        exit(failures == 0 ? 0 : 1)
    }
}
