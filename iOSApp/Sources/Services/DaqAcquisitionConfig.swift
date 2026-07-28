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
public struct DaqAcquisitionConfig: Equatable {
    /// The ADAQ7769 output rate at x32 decimation (1.024 MSPS — see the
    /// ADAQ_FILTER_SINC5_X8 comment in adaq7769_regs.h, "1.024MSPS"). Every
    /// decimation change scales down from this fixed point rather than from
    /// whatever `odr` happens to already hold, so repeated changes can't
    /// drift from the true hardware rate table.
    public static let baseRateHz: Double = 1_024_000.0

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

    /// Doubling/halving the ADC decimation moves the ODR inversely off the
    /// fixed base rate, not off whatever `odr` was last requested.
    public func settingAdcDec(_ newDec: DaqAdcDecimation) -> DaqAcquisitionConfig {
        var copy = self
        copy.adcDec = newDec
        copy.odr = Self.baseRateHz / newDec.ratio
        return copy
    }

    public func settingFilter(_ newFilter: DaqFilter) -> DaqAcquisitionConfig {
        var copy = self
        copy.filter = newFilter
        return copy
    }

    /// Picks the nearest decimation the hardware can actually hit for a
    /// requested rate, rather than silently reporting a rate the device
    /// never reached.
    public func settingRequestedRate(_ requestedHz: Double) -> DaqAcquisitionConfig {
        guard requestedHz > 0 else { return self }
        let best = DaqAdcDecimation.allCases.min { a, b in
            abs(Self.baseRateHz / a.ratio - requestedHz) < abs(Self.baseRateHz / b.ratio - requestedHz)
        } ?? adcDec
        return settingAdcDec(best)
    }

    /// Device readback always wins: the driver clamps combinations the part
    /// cannot hit, so a UI echoing its own request would misreport the
    /// rate. Overwrites filter, decimation, and ODR verbatim from STATUS.
    public func applyingDeviceReadback(filter: DaqFilter, adcDec: DaqAdcDecimation,
                                        odr: Double) -> DaqAcquisitionConfig {
        DaqAcquisitionConfig(filter: filter, adcDec: adcDec, odr: odr)
    }
}
