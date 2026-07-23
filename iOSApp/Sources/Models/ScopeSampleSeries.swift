import SwiftUI

struct ScopeSeriesPoint {
    let t: Double
    let v: Double
    let color: Color
}

struct ScopeSeriesChannel {
    let label: String
    let defaultColor: Color
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
                return ScopeSeriesPoint(t: sample[0], v: sample[ch + 1], color: color)
            }
            channels.append(ScopeSeriesChannel(label: "CH\(ch + 1)", defaultColor: color, points: points))
        }
        return ScopeSampleSeries(channels: channels)
    }

    /// Builds a series from DAQ's separate voltage/current sample arrays.
    /// Current samples are colored per-point by their FINE/COARSE/BLEND source byte.
    static func fromDAQ(
        voltageSamples: [(t: Double, value: Float)],
        currentSamples: [(t: Double, value: Float)],
        currentSampleSources: [UInt8],
        showVoltage: Bool,
        showCurrent: Bool
    ) -> ScopeSampleSeries {
        var channels: [ScopeSeriesChannel] = []
        if showVoltage {
            let points = voltageSamples.map {
                ScopeSeriesPoint(t: $0.t, v: Double($0.value), color: ScopeColors.daqVoltage)
            }
            channels.append(ScopeSeriesChannel(label: "Voltage", defaultColor: ScopeColors.daqVoltage, points: points))
        }
        if showCurrent {
            let points = currentSamples.enumerated().map { idx, sample -> ScopeSeriesPoint in
                let source = idx < currentSampleSources.count ? currentSampleSources[idx] : 0
                return ScopeSeriesPoint(t: sample.t, v: Double(sample.value), color: ScopeColors.daqCurrentColor(forSource: source))
            }
            channels.append(ScopeSeriesChannel(label: "Current", defaultColor: ScopeColors.daqCurrentFine, points: points))
        }
        return ScopeSampleSeries(channels: channels)
    }
}
