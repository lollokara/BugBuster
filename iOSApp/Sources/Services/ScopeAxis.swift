import Foundation

// =============================================================================
// ScopeAxis.swift — robust vertical-axis bounds for a scope lane.
//
// Foundation-only (no SwiftUI) so it is host-testable without a simulator;
// see tests/ios/ScopeAxisTests.swift.
//
// Why this exists: the axis used to be the raw min/max over every point, so a
// SINGLE glitch sample defined the whole lane. On a real capture the voltage
// band was ~1.81-1.83 V with occasional dropouts, and the dropouts rescaled
// the lane until the actual signal collapsed into a sliver. The current lane
// showed the same thing: a handful of outliers stretched the range while the
// real trace sat flat against the bottom edge.
//
// Outliers are still DRAWN (the caller clamps them to the lane edge) — they
// just no longer get to define the view.
// =============================================================================

public enum ScopeAxis {
    /// Fraction trimmed from each end before the axis is computed. 0.5% is
    /// enough to reject isolated dropouts/spikes while leaving genuine signal
    /// excursions — which are never this rare — fully in view.
    public static let trimFraction = 0.005

    /// Below this many samples there is no statistical basis for calling
    /// anything an outlier, so the raw extremes are used. A 3-point trace must
    /// not have a third of itself trimmed away.
    public static let minSamplesForTrim = 50

    /// Vertical bounds for `values`, robust to isolated outliers.
    ///
    /// Guarantees, for any input including empty/NaN/infinite/flat:
    ///   - both bounds are finite, and `min < max` (never a zero-height axis)
    ///   - padding is magnitude-relative, so a nA-scale trace keeps nA-scale
    ///     bounds instead of being blown up to a ±1 A axis
    public static func bounds(_ values: [Double]) -> (min: Double, max: Double) {
        // NaN silently defeats `<`/`>` comparisons, and a single infinity would
        // make the axis useless, so neither may reach the percentile step.
        let finite = values.filter { $0.isFinite }
        guard !finite.isEmpty else { return (-1, 1) }

        let lo: Double
        let hi: Double
        if finite.count >= minSamplesForTrim {
            let sorted = finite.sorted()
            // `k` is clamped below count/2 so the low index can never cross the
            // high index on small-but-trimmable inputs.
            let k = Swift.min(Int(Double(sorted.count) * trimFraction),
                              (sorted.count - 1) / 2)
            lo = sorted[k]
            hi = sorted[sorted.count - 1 - k]
        } else {
            lo = finite.min()!
            hi = finite.max()!
        }

        let span = hi - lo
        let mag = Swift.max(abs(lo), abs(hi))
        // Magnitude-RELATIVE padding: a fixed absolute floor blew a nA-scale
        // trace up to a ±1 A axis, defeating the per-lane SI autoranging.
        let pad: Double
        if span > mag * 1e-6, span > 0 {
            pad = span * 0.1
        } else {
            // Flat (or effectively flat) trace: give it a visible band centred
            // on the value rather than a zero-height axis. The all-zero case
            // has no magnitude to scale from, hence the absolute fallback.
            pad = mag > 0 ? mag * 0.1 : 1e-12
        }
        return (lo - pad, hi + pad)
    }
}
