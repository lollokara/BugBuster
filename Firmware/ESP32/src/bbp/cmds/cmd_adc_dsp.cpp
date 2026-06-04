// =============================================================================
// cmd_adc_dsp.cpp — Registry handlers for ADC DSP stream commands
//   BBP_CMD_START_ADC_DSP_STREAM  (0x64)
//   BBP_CMD_STOP_ADC_DSP_STREAM   (0x65)
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"

// ---------------------------------------------------------------------------
// START_ADC_DSP_STREAM
//   Request:  u8 channel, u8 rate_code, u16 window_samples,
//             f32 spike_threshold, u8 n_fft_peaks
//   Response: u8 channel, u8 rate_code, u16 window_samples,
//             f32 spike_threshold, u8 n_fft_peaks, u16 effective_rate_hz
// ---------------------------------------------------------------------------
static int handler_start_adc_dsp_stream(const uint8_t *payload, size_t len,
                                         uint8_t *resp, size_t *resp_len)
{
    if (bbpAdcDspActive()) return -CMD_ERR_BUSY;
    if (len < 9) return -CMD_ERR_BAD_ARG;

    size_t rpos = 0;
    uint8_t  channel         = bbp_get_u8(payload,  &rpos);
    uint8_t  rate_code       = bbp_get_u8(payload,  &rpos);
    uint16_t window_samples  = bbp_get_u16(payload, &rpos);
    float    spike_threshold = bbp_get_f32(payload, &rpos);
    uint8_t  n_fft_peaks     = bbp_get_u8(payload,  &rpos);

    if (channel >= 4) return -CMD_ERR_OUT_OF_RANGE;
    if (n_fft_peaks > 16) n_fft_peaks = 16;

    uint16_t effective_rate = 0;
    bbpStartAdcDspStream(channel, rate_code, window_samples,
                         spike_threshold, n_fft_peaks, &effective_rate);

    size_t pos = 0;
    bbp_put_u8(resp,  &pos, channel);
    bbp_put_u8(resp,  &pos, rate_code);
    bbp_put_u16(resp, &pos, window_samples);
    bbp_put_f32(resp, &pos, spike_threshold);
    bbp_put_u8(resp,  &pos, n_fft_peaks);
    bbp_put_u16(resp, &pos, effective_rate);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// STOP_ADC_DSP_STREAM  (no payload, no response)
// ---------------------------------------------------------------------------
static int handler_stop_adc_dsp_stream(const uint8_t *payload, size_t len,
                                        uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;
    bbpStopAdcDspStream();
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
static const ArgSpec s_start_args[] = {
    { "channel",        ARG_U8,  true, 0, 3   },
    { "rateCode",       ARG_U8,  true, 0, 255 },
    { "windowSamples",  ARG_U16, true, 0, 256 },
    { "spikeThreshold", ARG_F32, true, 0, 1e6 },
    { "nFftPeaks",      ARG_U8,  true, 0, 16  },
};
static const ArgSpec s_start_rsp[] = {
    { "channel",        ARG_U8,  true, 0, 0 },
    { "rateCode",       ARG_U8,  true, 0, 0 },
    { "windowSamples",  ARG_U16, true, 0, 0 },
    { "spikeThreshold", ARG_F32, true, 0, 0 },
    { "nFftPeaks",      ARG_U8,  true, 0, 0 },
    { "effectiveRate",  ARG_U16, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_adc_dsp_cmds[] = {
    { BBP_CMD_START_ADC_DSP_STREAM, "start_adc_dsp_stream",
      s_start_args, 5, s_start_rsp, 6,
      handler_start_adc_dsp_stream, CMD_FLAG_STREAMING },
    { BBP_CMD_STOP_ADC_DSP_STREAM,  "stop_adc_dsp_stream",
      nullptr, 0, nullptr, 0,
      handler_stop_adc_dsp_stream,  CMD_FLAG_STREAMING },
};

extern "C" void register_cmds_adc_dsp(void)
{
    cmd_registry_register_block(s_adc_dsp_cmds,
        sizeof(s_adc_dsp_cmds) / sizeof(s_adc_dsp_cmds[0]));
}
