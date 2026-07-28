import Foundation

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
        expect(c.odr, 16000.0, "doubling ADC decimation halves the ODR")
        expect(c.sampleRate, 16000.0, "sample rate tracks the ODR exactly")
    }
    do {
        // Asking for a rate picks the nearest achievable decimation rather than
        // silently reporting a rate the hardware never reached.
        let c = DaqAcquisitionConfig(filter: .wideband, adcDec: .x32, odr: 32000)
            .settingRequestedRate(8000)
        expect(c.adcDec, .x128, "32k/x32 -> 8k needs x128")
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
}
