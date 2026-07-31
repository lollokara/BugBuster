import SwiftUI

struct ScopeSeriesPoint {
    let t: Double
    let v: Double
    /// Compact source/trace discriminator, NOT a resolved `Color`. `Color` is
    /// a heap-backed existential; storing one per point meant tens of
    /// thousands of heap allocations/sec at real sample rates and a 20 Hz
    /// tick. `source` is the same 0...3 discriminator DAQ current already
    /// carried per sample (see `ScopeColors.daqCurrentColor(forSource:)`) —
    /// callers resolve it to a real `Color` ONCE per contiguous same-source
    /// run when the canvas draws, not once per point. The legacy ADC path
    /// never varies color within a channel, so it always uses source 0.
    let source: UInt8

    init(t: Double, v: Double, source: UInt8 = 0) {
        self.t = t
        self.v = v
        self.source = source
    }
}

struct ScopeSeriesChannel {
    let label: String
    let defaultColor: Color
    /// Physical unit for this trace's values (used for per-trace autoranging,
    /// e.g. "V" -> mV/µV/nV, "A" -> mA/µA/nA). Traces sharing a unit can share
    /// an axis; traces with different units need independent autoscale.
    let unit: String
    let points: [ScopeSeriesPoint]
}

struct ScopeSampleSeries {
    let channels: [ScopeSeriesChannel]
}

enum ScopeColors {
    static let accents: [Color] = [
        Color(red: 0.23, green: 0.51, blue: 0.96), // Blue
        Color(red: 0.06, green: 0.73, blue: 0.51), // Emerald
        Color(red: 0.96, green: 0.62, blue: 0.04), // Amber
        Color(red: 0.66, green: 0.33, blue: 0.97)  // Purple
    ]

    static let daqVoltage       = Color(red: 0.06, green: 0.73, blue: 0.51)
    static let daqCurrentFine   = Color(red: 0.23, green: 0.51, blue: 0.96)
    static let daqCurrentCoarse = Color(red: 0.96, green: 0.62, blue: 0.12)
    static let daqCurrentBlend  = Color(red: 0.66, green: 0.33, blue: 0.97)
    static let daqPower         = Color(red: 0.96, green: 0.42, blue: 0.42)

    /// Autoranges a raw magnitude into an SI-prefixed unit (V -> mV/µV/nV, etc).
    /// Returns the multiplier to apply to a raw value and the resulting unit string.
    static func autoUnit(_ maxAbs: Double, base: String) -> (scale: Double, unit: String) {
        let m = abs(maxAbs)
        if m >= 1 || m == 0 { return (1, base) }
        if m >= 1e-3 { return (1e3, "m" + base) }
        if m >= 1e-6 { return (1e6, "µ" + base) }
        return (1e9, "n" + base)
    }

    static func daqCurrentColor(forSource source: UInt8) -> Color {
        switch source {
        case 1: return daqCurrentCoarse
        case 2: return daqCurrentBlend
        default: return daqCurrentFine
        }
    }
}

extension ScopeSampleSeries {
    /// Builds a series from ADC's `[[Double]]` sample rows: index 0 is time,
    /// index `ch + 1` is that channel's voltage.
    static func fromADC(sampleBuffer: [[Double]], activeChannels: [Bool]) -> ScopeSampleSeries {
        var channels: [ScopeSeriesChannel] = []
        for ch in 0..<4 where activeChannels[ch] {
            let color = ScopeColors.accents[ch]
            let points: [ScopeSeriesPoint] = sampleBuffer.compactMap { sample in
                guard sample.count > ch + 1 else { return nil }
                return ScopeSeriesPoint(t: sample[0], v: sample[ch + 1])
            }
            channels.append(ScopeSeriesChannel(label: "CH\(ch + 1)", defaultColor: color, unit: "V", points: points))
        }
        return ScopeSampleSeries(channels: channels)
    }
}
