// =============================================================================
// cmd_streaming.cpp — Registry handlers for scope stream + waveform generator
//
// ADC stream (0x60/0x61) was already migrated in cmd_adc.cpp (slice 3).
// This file covers the remaining streaming-control opcodes:
//
//   BBP_CMD_START_SCOPE_STREAM  (0x62)  — state-machine: syncs seq, sets active
//   BBP_CMD_STOP_SCOPE_STREAM   (0x63)  — state-machine: clears active flag
//   BBP_CMD_START_WAVEGEN       (0xD0)  — configure + kick s_wavegenTask
//   BBP_CMD_STOP_WAVEGEN        (0xD1)  — stop + restore channel to HIGH_IMP
//
// Wire-format is byte-for-byte identical to the legacy handlers in bbp.cpp:
//   handleStartScopeStream  (bbp.cpp:2695)
//   handleStopScopeStream   (bbp.cpp:2711)
//   handleStartWavegen      (bbp.cpp:2723)
//   handleStopWavegen       (bbp.cpp:2796)
//
// State-machine side effects are preserved via thin public wrappers added to
// bbp.cpp: bbpStartScopeStream, bbpStopScopeStream, bbpStartWavegen.
// bbpStopWavegen already existed (used on disconnect/reset).
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "tasks.h"

// ---------------------------------------------------------------------------
// START_SCOPE_STREAM  payload: (none)  resp: (none, 0 bytes)
//
// Legacy (bbp.cpp:2695-2709):
//   - Rejects with BBP_ERR_STREAM_ACTIVE if s_scopeStreamActive
//   - Takes g_stateMutex (50 ms) to sync s_scopeLastSeq = g_deviceState.scope.seq
//   - Sets s_scopeStreamActive = true
//   - Returns 0 bytes
//
// Registry handler calls bbpStartScopeStream() which reproduces the same
// side effects. BUSY mapped to CMD_ERR_BUSY (adapter sends BBP_ERR_STREAM_ACTIVE).
// ---------------------------------------------------------------------------
static int handler_start_scope_stream(const uint8_t *payload, size_t len,
                                       uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;

    if (bbpScopeStreamActive()) return -CMD_ERR_BUSY;  // BBP_ERR_STREAM_ACTIVE

    bbpStartScopeStream();

    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// STOP_SCOPE_STREAM  payload: (none)  resp: (none, 0 bytes)
//
// Legacy (bbp.cpp:2711-2715):
//   - Sets s_scopeStreamActive = false
//   - Returns 0 bytes
// ---------------------------------------------------------------------------
static int handler_stop_scope_stream(const uint8_t *payload, size_t len,
                                      uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;

    bbpStopScopeStream();

    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// START_WAVEGEN  payload: u8 channel, u8 waveform, f32 freq_hz, f32 amplitude,
//                         f32 offset, u8 mode   (15 bytes total)
//   resp: u8 channel, u8 waveform, f32 freq_hz, f32 amplitude, f32 offset,
//          u8 mode   (reconstruct-echo — same field order as request, 15 bytes)
//
// Wire format matches legacy handleStartWavegen (bbp.cpp:2723-2778).
// Validation mirrors legacy exactly:
//   channel >= 4         -> BBP_ERR_INVALID_CH  -> CMD_ERR_OUT_OF_RANGE
//   waveform > 3         -> BBP_ERR_INVALID_PARAM -> CMD_ERR_BAD_ARG
//   mode > 1             -> BBP_ERR_INVALID_PARAM -> CMD_ERR_BAD_ARG
//   freq < 0.1 or > 100  -> BBP_ERR_INVALID_PARAM -> CMD_ERR_OUT_OF_RANGE
//
// Side effects via bbpStartWavegen():
//   - tasks_apply_channel_function() synchronously (avoids scheduler race)
//   - g_deviceState.wavegen.* updated under g_stateMutex (50 ms)
//   - xTaskNotifyGive(s_wavegenTask) to wake the generator task
// ---------------------------------------------------------------------------
static int handler_start_wavegen(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 15) return -CMD_ERR_BAD_ARG;

    size_t rpos = 0;
    uint8_t channel  = bbp_get_u8(payload, &rpos);
    uint8_t waveform = bbp_get_u8(payload, &rpos);
    float   freq_hz  = bbp_get_f32(payload, &rpos);
    float   amplitude = bbp_get_f32(payload, &rpos);
    float   offset   = bbp_get_f32(payload, &rpos);
    uint8_t mode     = bbp_get_u8(payload, &rpos);

    if (channel >= 4)                        return -CMD_ERR_OUT_OF_RANGE;  // BBP_ERR_INVALID_CH
    if (waveform > 3)                        return -CMD_ERR_BAD_ARG;       // BBP_ERR_INVALID_PARAM
    if (mode > 1)                            return -CMD_ERR_BAD_ARG;       // BBP_ERR_INVALID_PARAM
    if (freq_hz < 0.1f || freq_hz > 100.0f) return -CMD_ERR_OUT_OF_RANGE;  // BBP_ERR_INVALID_PARAM

    bbpStartWavegen(channel, waveform, freq_hz, amplitude, offset, mode);

    // Reconstruct-echo: same field order as request (matches legacy put_u8/put_f32 sequence)
    size_t pos = 0;
    bbp_put_u8(resp, &pos, channel);
    bbp_put_u8(resp, &pos, waveform);
    bbp_put_f32(resp, &pos, freq_hz);
    bbp_put_f32(resp, &pos, amplitude);
    bbp_put_f32(resp, &pos, offset);
    bbp_put_u8(resp, &pos, mode);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// STOP_WAVEGEN  payload: (none)  resp: (none, 0 bytes)
//
// Legacy (bbp.cpp:2796-2801):
//   - Calls wavegen_stop_and_reset() which:
//       * Takes g_stateMutex, reads channel, sets wavegen.active = false, releases
//       * Enqueues CMD_SET_CHANNEL_FUNC with CH_FUNC_HIGH_IMP via sendCommand()
//   - Returns 0 bytes
//
// bbpStopWavegen() was already public (used on disconnect/reset) and
// encapsulates the same wavegen_stop_and_reset() call.
// ---------------------------------------------------------------------------
static int handler_stop_wavegen(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;

    bbpStopWavegen();

    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
static const ArgSpec s_start_wavegen_args[] = {
    { "channel",   ARG_U8,  true, 0, 3 },
    { "waveform",  ARG_U8,  true, 0, 3 },
    { "freq_hz",   ARG_F32, true, 0.1f, 100.0f },
    { "amplitude", ARG_F32, true, -12.0f, 12.0f },
    { "offset",    ARG_F32, true, -12.0f, 12.0f },
    { "mode",      ARG_U8,  true, 0, 1 },
};
static const ArgSpec s_start_wavegen_rsp[] = {
    { "channel",   ARG_U8,  true, 0, 0 },
    { "waveform",  ARG_U8,  true, 0, 0 },
    { "freq_hz",   ARG_F32, true, 0, 0 },
    { "amplitude", ARG_F32, true, 0, 0 },
    { "offset",    ARG_F32, true, 0, 0 },
    { "mode",      ARG_U8,  true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_streaming_cmds[] = {
    { BBP_CMD_START_SCOPE_STREAM, "start_scope_stream",
      NULL,                   0, NULL,                  0, handler_start_scope_stream, CMD_FLAG_STREAMING },
    { BBP_CMD_STOP_SCOPE_STREAM,  "stop_scope_stream",
      NULL,                   0, NULL,                  0, handler_stop_scope_stream,  CMD_FLAG_STREAMING },
    { BBP_CMD_START_WAVEGEN,      "start_wavegen",
      s_start_wavegen_args,   6, s_start_wavegen_rsp,  6, handler_start_wavegen,      CMD_FLAG_STREAMING },
    { BBP_CMD_STOP_WAVEGEN,       "stop_wavegen",
      NULL,                   0, NULL,                  0, handler_stop_wavegen,       CMD_FLAG_STREAMING },
};

extern "C" void register_cmds_streaming(void)
{
    cmd_registry_register_block(s_streaming_cmds,
        sizeof(s_streaming_cmds) / sizeof(s_streaming_cmds[0]));
}
