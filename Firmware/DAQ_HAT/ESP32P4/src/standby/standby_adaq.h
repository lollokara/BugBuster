#pragma once

// =============================================================================
// standby_adaq.h - keep an ADAQ7769 configuration across an analog power cycle.
//
// A converter loses every register when its supply is removed. Before the rails
// are switched off the driver shadow (clock, power mode, filter, decimation,
// Sinc3, reference buffers, PGA/GPIO, read format) is copied and the two
// runtime calibration registers are READ back; after the rails return the same
// values are written, using the existing adaq7769 API. Nothing here runs a
// calibration, writes NVS, or consults the settings registry - so a CLI-chosen
// output data rate survives a standby exactly as it was.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "adaq7769.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool          valid;          // the copy below is complete
    bool          was_ok;         // the device was usable when it was taken (else it is skipped)
    adaq_config_t cfg;
    uint8_t       gpio_control;
    uint8_t       gpio_write;
    int32_t       offset24;       // runtime OFFSET registers (read back, never computed)
    uint32_t      gain24;         // runtime GAIN registers
    uint8_t       diag_last_status;
    uint8_t       diag_sticky;    // fault history: restored untouched, never cleared by a standby
    uint32_t      diag_err_count;
    uint32_t      diag_status_reads;
} sb_adaq_shadow_t;

// Read-only on the converter apart from the register reads. Call with acquisition
// stopped and the ADC gate open.
esp_err_t sb_adaq_save(adaq7769_t *dev, bool usable, sb_adaq_shadow_t *s);

// Reset + identify + full configure from @p s, then read the calibration
// registers back and compare. ESP_OK only when every step and the readback agree.
esp_err_t sb_adaq_restore(adaq7769_t *dev, const sb_adaq_shadow_t *s);

// Internal readiness AFTER a restore, with the analog inputs still disconnected:
// the converter must hand out a conversion result and its status header must not
// report an ADC / digital / clock fault. The VALUE is discarded - with the muxes
// open it measures nothing and must never reach a status, readout or stream.
// ESP_OK for a part that was not usable at boot (nothing to check).
esp_err_t sb_adaq_check_conversion(adaq7769_t *dev, bool was_ok);

#ifdef __cplusplus
}
#endif
