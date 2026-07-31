import Foundation

// Behavioral tests for ScopeAxis. Run from main.swift so swiftc gets exactly
// one top-level entry point.
func runScopeAxisTests() {
    // --- The bug this exists to fix -----------------------------------------
    do {
        // A single glitch sample used to define the whole axis: the real band
        // is 1.81..1.83 V, one dropout to 0 V, and the lane rescaled to 0..1.83
        // so the actual signal collapsed into the top 1% of the view.
        var v = [Double](repeating: 1.82, count: 1000)
        for i in 0..<1000 { v[i] = 1.81 + Double(i % 20) * 0.001 }
        v[500] = 0.0                       // dropout glitch

        let b = ScopeAxis.bounds(v)
        check(b.min > 1.5, "one dropout must not drag the axis to 0 (got \(b.min))")
        check(b.max < 2.0, "axis must stay near the real band (got \(b.max))")
        check(b.min < 1.81 && b.max > 1.829,
              "the real signal band must still fit inside the axis")
    }

    do {
        // Symmetric case: a single positive spike.
        var v = [Double](repeating: 0.0, count: 1000)
        for i in 0..<1000 { v[i] = Double(i % 10) * 1e-6 }
        v[123] = 5.0
        let b = ScopeAxis.bounds(v)
        check(b.max < 1.0, "a lone positive spike must not own the axis (got \(b.max))")
    }

    // --- Must not over-clip -------------------------------------------------
    do {
        // A genuine wide-range signal is NOT outliers: nothing should be
        // trimmed away when the data is spread evenly.
        let v = (0..<1000).map { Double($0) }
        let b = ScopeAxis.bounds(v)
        check(b.min <= 10.0, "evenly spread data must not be trimmed at the low end")
        check(b.max >= 989.0, "evenly spread data must not be trimmed at the high end")
    }

    do {
        // Small samples have no statistical basis for trimming — use raw
        // min/max so a 3-point trace isn't mangled.
        let b = ScopeAxis.bounds([1.0, 5.0, 100.0])
        check(b.max > 100.0, "tiny datasets must keep their true max (got \(b.max))")
        check(b.min < 1.0, "tiny datasets must keep their true min (got \(b.min))")
    }

    // --- Degenerate inputs must never produce an invalid axis ---------------
    do {
        let b = ScopeAxis.bounds([])
        check(b.min < b.max, "empty input must still yield a usable axis")

        let flat = ScopeAxis.bounds([Double](repeating: 2.5, count: 500))
        check(flat.min < flat.max, "a flat trace must not collapse to a zero-height axis")
        check(flat.min < 2.5 && flat.max > 2.5, "a flat trace must be centred in view")

        let zero = ScopeAxis.bounds([Double](repeating: 0.0, count: 500))
        check(zero.min < zero.max, "an all-zero trace must not collapse")
    }

    do {
        // NaN/inf can reach us from a desynced frame; they must never poison
        // the axis (a NaN comparison silently defeats min/max).
        let v = [1.0, 2.0, Double.nan, 3.0, Double.infinity, -Double.infinity, 2.5]
        let b = ScopeAxis.bounds(v)
        check(b.min.isFinite && b.max.isFinite, "axis must stay finite with NaN/inf present")
        check(b.min < b.max, "axis must stay ordered with NaN/inf present")
    }

    do {
        // nA-scale data must keep nA-scale bounds (regression: a fixed absolute
        // pad once blew a nA trace up to a ±1 A axis).
        let v = (0..<1000).map { 1e-9 + Double($0 % 7) * 1e-12 }
        let b = ScopeAxis.bounds(v)
        check(b.max < 1e-6, "nA data must not be padded up to µA/A scale (got \(b.max))")
    }

    do {
        // The axis must always be ordered, whatever the input ordering.
        let b = ScopeAxis.bounds([5.0, -5.0, 5.0, -5.0, 0.0, 0.0, 0.0, 0.0])
        check(b.min < b.max, "bounds must be ordered regardless of input order")
    }

    // --- Quickselect rewrite: must match the old sort-based percentiles ----

    /// Sort-based reference implementation of the trimmed-percentile step,
    /// mirroring the pre-quickselect `ScopeAxis.bounds` exactly (same padding
    /// logic), so it can be diffed against the real implementation.
    func referenceBounds(_ values: [Double]) -> (min: Double, max: Double) {
        let finite = values.filter { $0.isFinite }
        guard !finite.isEmpty else { return (-1, 1) }
        let lo: Double
        let hi: Double
        if finite.count >= ScopeAxis.minSamplesForTrim {
            let sorted = finite.sorted()
            let k = Swift.min(Int(Double(sorted.count) * ScopeAxis.trimFraction),
                              (sorted.count - 1) / 2)
            lo = sorted[k]
            hi = sorted[sorted.count - 1 - k]
        } else {
            lo = finite.min()!
            hi = finite.max()!
        }
        let span = hi - lo
        let mag = Swift.max(abs(lo), abs(hi))
        let pad: Double
        if span > mag * 1e-6, span > 0 {
            pad = span * 0.1
        } else {
            pad = mag > 0 ? mag * 0.1 : 1e-12
        }
        return (lo - pad, hi + pad)
    }

    do {
        // Duplicate values clustered around the trim boundary: quickselect
        // must still land on the exact same k-th element as a full sort.
        var v = [Double](repeating: 3.0, count: 200)
        for i in 0..<200 where i % 3 == 0 { v[i] = 3.0 + Double(i) * 0.0001 }
        let b = ScopeAxis.bounds(v)
        let ref = referenceBounds(v)
        check(b == ref, "duplicate-heavy input must match the sort-based reference (got \(b), want \(ref))")
    }

    do {
        // All-equal input: every element is the same value, well above the
        // trim threshold.
        let v = [Double](repeating: 42.0, count: 500)
        let b = ScopeAxis.bounds(v)
        let ref = referenceBounds(v)
        check(b == ref, "all-equal input must match the sort-based reference (got \(b), want \(ref))")
        check(b.min < b.max, "all-equal input must still produce a non-zero-height axis")
    }

    do {
        // Exactly `minSamplesForTrim` elements: the boundary where trimming
        // first kicks in.
        let v = (0..<ScopeAxis.minSamplesForTrim).map { Double($0) }
        let b = ScopeAxis.bounds(v)
        let ref = referenceBounds(v)
        check(b == ref, "exactly-minSamplesForTrim input must match the sort-based reference (got \(b), want \(ref))")
    }

    do {
        // Randomised differential test: many random inputs, comparing the
        // quickselect implementation against the sort-based reference. This
        // is the guarantee that matters most — the rewrite must be a pure
        // performance change, never a behavior change.
        var rng = SystemRandomNumberGenerator()
        for trial in 0..<500 {
            let count = Int.random(in: 0...400, using: &rng)
            var v = [Double]()
            v.reserveCapacity(count)
            for _ in 0..<count {
                let choice = Int.random(in: 0...9, using: &rng)
                switch choice {
                case 0: v.append(.nan)
                case 1: v.append(.infinity)
                case 2: v.append(-.infinity)
                default:
                    v.append(Double.random(in: -1e6...1e6, using: &rng))
                }
            }
            let b = ScopeAxis.bounds(v)
            let ref = referenceBounds(v)
            check(b == ref, "trial \(trial) (n=\(count)) must match reference (got \(b), want \(ref))")
        }
    }
}
