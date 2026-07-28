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
}
