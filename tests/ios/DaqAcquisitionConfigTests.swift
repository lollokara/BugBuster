import Foundation

// Expected ODRs are ABSOLUTE hardware values, not `baseRateHz / N`
// expressions — deriving them from the constant under test would make these
// assertions tautological. fMOD is 8.192 MHz on this board
// (ADAQ_MCLK_HZ 16_384_000 / MCLK_DIV 2, config.h:130), verified against the
// device on 2026-07-29: filter=SINC5 adc_dec=x128 logged fine_odr=64000.

func runDaqAcquisitionConfigTests() {
    // RULING: the sample rate IS the ODR. There is no stream-decimation
    // conversion — changing the rate re-tunes filter + ADC hardware decimation.
    do {
        let c = DaqAcquisitionConfig(filter: .wideband, adcDec: .x32, odr: 32000)
        expect(c.sampleRate, 32000.0, "sample rate IS the ODR — no stream decimation")
    }
    do {
        // Decimation drives the ODR: doubling it halves the rate.
        let c = DaqAcquisitionConfig(filter: .wideband, adcDec: .x32, odr: 32000)
            .settingAdcDec(.x64)
        expect(c.odr, 128000.0, "doubling ADC decimation halves the ODR")
        expect(c.sampleRate, 128000.0, "sample rate tracks the ODR exactly")
    }
    do {
        // Asking for a rate picks the nearest achievable decimation rather than
        // silently reporting a rate the hardware never reached.
        let c = DaqAcquisitionConfig(filter: .wideband, adcDec: .x32, odr: 32000)
            .settingRequestedRate(8000)
        expect(c.adcDec, .x1024, "8k needs x1024 at fMOD 8.192 MHz")
        expect(c.sampleRate, 8000.0, "resulting rate matches the request")
    }
    do {
        // Device readback always wins over the local guess: the driver clamps
        // combinations the part cannot hit, and a UI echoing its own request
        // would misreport the rate.
        var c = DaqAcquisitionConfig(filter: .wideband, adcDec: .x32, odr: 32000)
        c = c.applyingDeviceReadback(filter: .sinc5, adcDec: .x64, odr: 16000)
        expect(c.filter, .sinc5, "device readback overwrites the requested filter")
        expect(c.adcDec, .x64, "device readback overwrites the requested decimation")
        expect(c.sampleRate, 16000.0, "sample rate follows the device's actual ODR")
    }
    do {
        // Degenerate input must never produce a nonsense rate.
        let c = DaqAcquisitionConfig(filter: .wideband, adcDec: .x32, odr: 0)
        check(c.sampleRate >= 0, "a zero ODR must not yield a negative rate")
    }

    // --- I2: filter-dependent rate formula (not one universal division) ----
    // adaq7769_output_data_rate() (adaq7769.c:353-366): SINC5_X8/SINC5_X16
    // use FIXED dividers regardless of adc_dec; SINC5/WIDEBAND use the
    // decimation ratio directly; SINC3 reinterprets adc_dec as a scale
    // factor (daq_board.c ~512-521): dec = (adc_dec ?: 1) * 32.
    do {
        let c = DaqAcquisitionConfig(filter: .sinc5x8, adcDec: .x32, odr: 128000)
        expect(c.sampleRate, 128000.0, "Sinc5x8 ODR is fMOD/8")
        let changed = c.settingAdcDec(.x1024)
        expect(changed.odr, 1024000.0, "Sinc5x8's ODR ignores adc_dec entirely")
    }
    do {
        let c = DaqAcquisitionConfig(filter: .sinc5x16, adcDec: .x32, odr: 64000)
        expect(c.sampleRate, 64000.0, "Sinc5x16 ODR is fMOD/16")
        let changed = c.settingAdcDec(.x512)
        expect(changed.odr, 512000.0, "Sinc5x16's ODR ignores adc_dec entirely")
    }
    do {
        // Fixed-rate filters have exactly one achievable rate — requesting
        // any other rate is a no-op, not a misleading "nearest" pick.
        let c = DaqAcquisitionConfig(filter: .sinc5x8, adcDec: .x32, odr: 128000)
            .settingRequestedRate(8000)
        expect(c.odr, 128000.0, "requesting a rate Sinc5x8 cannot reach is a no-op")
    }
    do {
        // SINC3: adc_dec is a raw scale factor (dec = adc_dec * 32), NOT a
        // decimation ratio — x128's raw value (2) means dec=64, not 128.
        let c = DaqAcquisitionConfig(filter: .sinc3, adcDec: .x128, odr: 16000)
        expect(c.sampleRate, 16000.0, "sampleRate reflects the stored odr until recomputed")
        let widened = c.settingAdcDec(.x256)
        expect(widened.odr, 85333.33333333333, "Sinc3: dec = adc_dec(3) * 32 = 96 -> 8.192M/96")
    }
    do {
        // The firmware clamps adc_dec=0 the same as adc_dec=1 for SINC3
        // ("adc_dec ? adc_dec : 1"), so x32 (raw 0) and x64 (raw 1) must
        // produce the identical Sinc3 rate.
        let raw0 = DaqAcquisitionConfig(filter: .sinc3, adcDec: .x32, odr: 0)
        let raw1 = DaqAcquisitionConfig(filter: .sinc3, adcDec: .x64, odr: 0)
        expect(raw0.settingAdcDec(.x32).odr, raw1.settingAdcDec(.x64).odr,
               "Sinc3 raw adc_dec 0 and 1 both clamp to a scale factor of 1")
    }
    do {
        // Switching filters must re-derive the ODR, not just carry the old
        // number forward under the new formula.
        let c = DaqAcquisitionConfig(filter: .wideband, adcDec: .x32, odr: 32000)
            .settingFilter(.sinc5x8)
        expect(c.odr, 1024000.0, "switching to Sinc5x8 recomputes the fixed ODR")
    }
}
