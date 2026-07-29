import Foundation

// =============================================================================
// DaqAcquisitionConfig.swift — ADAQ7769-1 filter/ODR configuration model.
//
// RULING (human, do not reintroduce the alternative): the sample rate IS the
// ODR. There is NO stream-decimation conversion anywhere in the client.
// Changing the rate re-tunes filter + ADC hardware decimation on the device
// itself — never a client-side keep-1-of-N drop, which would alias (the
// device's own stream_decim is a naive drop with no anti-alias filter; ADC
// decimation runs through the digital filter instead). STATUS still reports
// stream_decim read-only; nothing in the UI drives it.
//
// Foundation-only by design (no SwiftUI/Network import) so this host-tests
// via swiftc — see tests/ios/DaqAcquisitionConfigTests.swift, wired into
// tests/unit/test_daq_link_state_machine.py the way ScopeAxis was wired.
//
// SCOPE LIMIT: filter/ODR here apply to the FINE and COARSE current ADAQs
// only. The VOLTAGE ADAQ stays pinned at VOLTAGE_ODR_TARGET_SPS (50 kSPS,
// see Firmware include/config.h:356-362) because it shares SPI bus B and a
// common SYNC line with COARSE and must run well below the current ODR
// (bench-verified). This type never represents the voltage rate.
// =============================================================================

/// Mirrors `ADAQ_FILTER_*` in
/// Firmware/DAQ_HAT/ESP32P4/src/adaq7769/adaq7769_regs.h.
public enum DaqFilter: UInt8, CaseIterable, Codable, Equatable {
    case sinc5 = 0
    case sinc5x8 = 1
    case sinc5x16 = 2
    case sinc3 = 3
    case wideband = 4

    public var label: String {
        switch self {
        case .sinc5: return "Sinc5"
        case .sinc5x8: return "Sinc5 x8"
        case .sinc5x16: return "Sinc5 x16"
        case .sinc3: return "Sinc3"
        case .wideband: return "Wideband"
        }
    }
}

/// Mirrors `ADAQ_DEC_*` in
/// Firmware/DAQ_HAT/ESP32P4/src/adaq7769/adaq7769_regs.h.
public enum DaqAdcDecimation: UInt8, CaseIterable, Codable, Equatable {
    case x32 = 0
    case x64 = 1
    case x128 = 2
    case x256 = 3
    case x512 = 4
    case x1024 = 5

    /// Decimation ratio as a plain multiplier (x32 -> 32, ...).
    public var ratio: Double {
        switch self {
        case .x32: return 32
        case .x64: return 64
        case .x128: return 128
        case .x256: return 256
        case .x512: return 512
        case .x1024: return 1024
        }
    }

    public var label: String { "x\(Int(ratio))" }
}

/// Requested (or device-confirmed) ADAQ7769 acquisition configuration for
/// the FINE/COARSE current ADCs.
///
/// `sampleRate` is always identically `odr` per the ruling above — there is
/// no separate "displayed rate" derived from stream decimation.
///
/// NOTE ON ACCURACY: this type mirrors the firmware's ENUM VALUES verbatim
/// (`DaqFilter`/`DaqAdcDecimation` raw values match `ADAQ_FILTER_*`/
/// `ADAQ_DEC_*` exactly), but the rate FORMULA is filter-dependent and is
/// derived from `adaq7769_output_data_rate()` (adaq7769.c:353-366) plus the
/// SINC3 scale-factor reinterpretation in `daq_board.c` (~512-521) — it does
/// not use one universal division for all five filters:
///   - SINC5, WIDEBAND: odr = baseRateHz / decimation ratio (32...1024) —
///     `adc_dec` is the direct DEC_RATE register value.
///   - SINC5_X8:  odr = baseRateHz / 8, FIXED — `adc_dec` has no effect.
///   - SINC5_X16: odr = baseRateHz / 16, FIXED — `adc_dec` has no effect.
///   - SINC3: `adc_dec` is reinterpreted as a scale factor, not a ratio:
///     dec = (adc_dec != 0 ? adc_dec : 1) * 32; odr = baseRateHz / dec.
///     (This is why the iOS ADC-Dec picker is disabled for SINC3 in
///     ScopeTab — its x32...x1024 labels would describe a decimation the
///     device never applies under this filter.)
public struct DaqAcquisitionConfig: Equatable {
    /// fMOD at MCLK_DIV_2 — the modulator rate every ODR formula divides down
    /// from. This board clocks the ADAQ at ADAQ_MCLK_HZ = 16_384_000
    /// (config.h:130, SiT8208 Y1 via a CDCLVC1104 fan-out) and runs
    /// MCLK_DIV = 2, so fMOD = 8.192 MHz — NOT the 1.024 MHz suggested by the
    /// "1.024MSPS" comment on ADAQ_FILTER_SINC5_X8 in adaq7769_regs.h, which
    /// describes the part's reset default and not this hardware.
    ///
    /// Verified against the device on 2026-07-29: SET_ACQ_CONFIG with
    /// filter=SINC5, adc_dec=x128 logged `fine_odr=64000`, and
    /// 8_192_000 / 128 == 64_000 exactly. The previous 1_024_000 made every
    /// derived rate 8x too low.
    ///
    /// Fixed rather than derived from whatever `odr` happens to already hold,
    /// so repeated changes can't drift from the true hardware rate table.
    public static let baseRateHz: Double = 8_192_000.0

    public var filter: DaqFilter
    public var adcDec: DaqAdcDecimation
    /// Output data rate in samples/sec. Holds the requested value until a
    /// device STATUS readback overwrites it via `applyingDeviceReadback` —
    /// the device's actual applied rate always wins over the local guess.
    public var odr: Double

    public init(filter: DaqFilter, adcDec: DaqAdcDecimation, odr: Double) {
        self.filter = filter
        self.adcDec = adcDec
        self.odr = odr
    }

    /// The sample rate IS the ODR (no stream-decimation conversion in the
    /// client). Clamped so a degenerate/zero ODR can never read as negative.
    public var sampleRate: Double { max(odr, 0) }

    /// The ODR the device would apply for a given filter/decimation pair,
    /// per `adaq7769_output_data_rate()` and the SINC3 scale-factor
    /// reinterpretation in `daq_board.c`. SINC5_X8/SINC5_X16 ignore `adcDec`
    /// entirely (fixed dividers); SINC3 treats it as a raw scale factor,
    /// not a ratio.
    private static func computedOdr(filter: DaqFilter, adcDec: DaqAdcDecimation) -> Double {
        switch filter {
        case .sinc5x8:
            return baseRateHz / 8.0
        case .sinc5x16:
            return baseRateHz / 16.0
        case .sinc3:
            let raw = Double(adcDec.rawValue)
            let scale = raw == 0 ? 1.0 : raw
            return baseRateHz / (scale * 32.0)
        case .sinc5, .wideband:
            return baseRateHz / adcDec.ratio
        }
    }

    /// Changing the ADC decimation moves the ODR per `computedOdr` for the
    /// CURRENT filter — a no-op on the resulting rate for SINC5_X8/X16,
    /// whose ODR is fixed regardless of decimation.
    public func settingAdcDec(_ newDec: DaqAdcDecimation) -> DaqAcquisitionConfig {
        var copy = self
        copy.adcDec = newDec
        copy.odr = Self.computedOdr(filter: filter, adcDec: newDec)
        return copy
    }

    /// Changing the filter also changes the rate formula, so the ODR is
    /// recomputed against the (unchanged) decimation under the new filter.
    public func settingFilter(_ newFilter: DaqFilter) -> DaqAcquisitionConfig {
        var copy = self
        copy.filter = newFilter
        copy.odr = Self.computedOdr(filter: newFilter, adcDec: adcDec)
        return copy
    }

    /// Picks the nearest decimation the hardware can actually hit for a
    /// requested rate under the CURRENT filter, rather than silently
    /// reporting a rate the device never reached. SINC5_X8/X16 have exactly
    /// one achievable rate regardless of decimation, so there is nothing to
    /// search — the request is a no-op on those two filters.
    public func settingRequestedRate(_ requestedHz: Double) -> DaqAcquisitionConfig {
        guard requestedHz > 0 else { return self }
        switch filter {
        case .sinc5x8, .sinc5x16:
            return self
        case .sinc5, .wideband, .sinc3:
            let best = DaqAdcDecimation.allCases.min { a, b in
                abs(Self.computedOdr(filter: filter, adcDec: a) - requestedHz) <
                abs(Self.computedOdr(filter: filter, adcDec: b) - requestedHz)
            } ?? adcDec
            return settingAdcDec(best)
        }
    }

    /// Device readback always wins: the driver clamps combinations the part
    /// cannot hit, so a UI echoing its own request would misreport the
    /// rate. Overwrites filter, decimation, and ODR verbatim from STATUS —
    /// never recomputed, always copied from what the device reported.
    public func applyingDeviceReadback(filter: DaqFilter, adcDec: DaqAdcDecimation,
                                        odr: Double) -> DaqAcquisitionConfig {
        DaqAcquisitionConfig(filter: filter, adcDec: adcDec, odr: odr)
    }
}
