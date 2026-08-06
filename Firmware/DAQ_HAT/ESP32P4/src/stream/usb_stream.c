// =============================================================================
// usb_stream.c — frame builder / transport manager for the measurement stream.
// =============================================================================

#include "usb_stream.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "daq_perf.h"

static const char *TAG = "usb_stream";

// -----------------------------------------------------------------------------
// CRC-16/CCITT-FALSE (table-driven — this runs on every outbound WAVEFORM
// frame, up to ~1000/s of ~4 KB each at full ODR, so the bit-loop form was a
// measurable chunk of daq_fast_task's per-sample CPU budget).
// -----------------------------------------------------------------------------
static uint16_t s_crc16_table[256];
static bool     s_crc16_table_ready;

static void crc16_table_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint16_t crc = (uint16_t)(i << 8);
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
        s_crc16_table[i] = crc;
    }
    s_crc16_table_ready = true;
}

uint16_t usb_proto_crc16(const uint8_t *data, uint32_t len, uint16_t init)
{
    if (!s_crc16_table_ready) {
        crc16_table_init();
    }
    uint16_t crc = init;
    for (uint32_t i = 0; i < len; ++i) {
        crc = (uint16_t)((crc << 8) ^ s_crc16_table[((crc >> 8) ^ data[i]) & 0xFF]);
    }
    return crc;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void usb_stream_init(usb_stream_t *s)
{
    memset(s, 0, sizeof(*s));
    s->wi_decim = 1;
    if (!s_crc16_table_ready) {
        crc16_table_init();
    }
}

void usb_stream_set_transport(usb_stream_t *s, const usb_transport_t *t)
{
    s->transport      = *t;
    s->have_transport = true;
}

void usb_stream_set_cmd_cb(usb_stream_t *s, usb_cmd_cb_t cb, void *user)
{
    s->cmd_cb   = cb;
    s->cmd_user = user;
}

void usb_stream_set_streaming(usb_stream_t *s, bool on)
{
    s->streaming = on;
}

void usb_stream_reset_apply(usb_stream_t *s)
{
    s->sample_seq           = 0;
    s->volt_seq             = 0;
    s->wi_count             = 0;
    s->wv_count             = 0;
    s->dropped_frames       = 0;
    s->wi_frames            = 0;
    s->wv_frames            = 0;
    s->wi_drops             = 0;
    s->wv_drops             = 0;
    s->tx_frames            = 0;
    s->tx_bytes_window      = 0;
    s->bytes_per_sec        = 0;
    s->perf_last_us         = 0;
    s->perf_last_sample_seq = 0;
    s->reset_pending        = false;
    // Discard anything staged: it belongs to the session being torn down, and
    // emitting it after the reset would put pre-reset frames on the new stream.
    s->tx_batch_len         = 0;
    s->tx_batching          = false;
    // tx_seq is intentionally left untouched — it is the outbound frame
    // sequence and must stay monotonic across sessions.
}

void usb_stream_reset_session(usb_stream_t *s)
{
    // Single-writer handoff: only flag the reset here. The producer task
    // (daq_fast_task) applies it at the top of the next push_sample /
    // push_voltage, so the TinyUSB/ctrl task never mutates the batch state
    // or the non-atomic u64 sequences while a push is in flight.
    s->reset_pending = true;
}

// -----------------------------------------------------------------------------
// Frame assembly
// -----------------------------------------------------------------------------
// Finalize + send a frame whose payload (len bytes) has ALREADY been composed
// into s->frame_buf at offset USB_FRAME_HEADER_LEN. Fills header bytes 0..12,
// writes the CRC slot (0x0000 for data frames, real CRC-16 otherwise), applies
// back-pressure and transmits. This is the zero-copy path used by the large
// WAVE_I / WAVE_V / FFT builders so no payload-sized stack or static buffer is
// needed.
// Attribute one emitted-or-dropped frame to its record type (extension v5).
static void count_by_type(usb_stream_t *s, uint8_t type, bool sent)
{
    if (type == USB_REC_WAVE_I) {
        if (sent) s->wi_frames++; else s->wi_drops++;
    } else if (type == USB_REC_WAVE_V) {
        if (sent) s->wv_frames++; else s->wv_drops++;
    }
}

// Push whatever is staged in tx_batch out as a single transport write.
// Callers have already validated FIFO space per frame as they were staged, so
// a short write here means the FIFO drained differently than observed; the
// bytes are dropped and the loss is visible to the host as a tx_seq gap.
static void batch_flush(usb_stream_t *s)
{
    if (!s->tx_batch_len) return;
    uint16_t len = s->tx_batch_len;
    s->tx_batch_len = 0;   // clear first: transport.write must never re-enter
    if (s->have_transport && s->transport.write) {
        s->transport.write(s->tx_batch, len, s->transport.ctx);
    }
}

void usb_stream_batch_begin(usb_stream_t *s)
{
    if (!s) return;
    s->tx_batching = true;
}

void usb_stream_batch_end(usb_stream_t *s)
{
    if (!s) return;
    batch_flush(s);
    s->tx_batching = false;
}

static esp_err_t emit_frame_inplace(usb_stream_t *s, usb_rec_type_t type,
                                    uint16_t len, bool crc)
{
    if (len > USB_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s->have_transport) {
        s->dropped_frames++;
        count_by_type(s, (uint8_t)type, false);
        s->tx_seq++;   // see tx_seq contract note below
        return ESP_ERR_INVALID_STATE;
    }
    if (s->transport.connected && !s->transport.connected(s->transport.ctx)) {
        // No host attached: drop silently, no log spam.
        s->dropped_frames++;
        count_by_type(s, (uint8_t)type, false);
        s->tx_seq++;   // see tx_seq contract note below
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *f = s->frame_buf;
    f[0] = USB_PROTO_MAGIC0;
    f[1] = USB_PROTO_MAGIC1;
    f[2] = USB_PROTO_VERSION;
    f[3] = (uint8_t)type;
    f[4] = 0;                       // flags
    f[5] = 0;                       // reserved
    f[6] = (uint8_t)(s->tx_seq);
    f[7] = (uint8_t)(s->tx_seq >> 8);
    f[8] = (uint8_t)(s->tx_seq >> 16);
    f[9] = (uint8_t)(s->tx_seq >> 24);
    f[10] = (uint8_t)(len);
    f[11] = (uint8_t)(len >> 8);
    uint32_t crc_off = USB_FRAME_HEADER_LEN + len;
    if (crc) {
        // CRC over header tail [2 .. 12+len).
        uint16_t c = usb_proto_crc16(&f[2], (uint32_t)(USB_FRAME_HEADER_LEN - 2) + len, 0xFFFF);
        f[crc_off]     = (uint8_t)(c);
        f[crc_off + 1] = (uint8_t)(c >> 8);
    } else {
        // Data frames: CRC slot is zeroed and unchecked (see usb_proto.h).
        f[crc_off]     = 0;
        f[crc_off + 1] = 0;
    }

    uint32_t total = crc_off + USB_FRAME_CRC_LEN;

    // Coalescing path: stage the finished frame instead of issuing its own
    // transport write. The back-pressure test still runs per frame, against the
    // batch's cumulative size, so drop accounting is identical to the direct
    // path -- a frame is only ever counted as sent once the FIFO has proven it
    // has room for it.
    if (s->tx_batching && total <= sizeof(s->tx_batch)) {
        if (s->tx_batch_len + total > sizeof(s->tx_batch)) {
            batch_flush(s);
        }
        if (s->transport.writable &&
            s->transport.writable(s->transport.ctx) < s->tx_batch_len + total) {
            s->dropped_frames++;
            count_by_type(s, (uint8_t)type, false);
            s->tx_seq++;
            return ESP_ERR_NO_MEM;
        }
        memcpy(&s->tx_batch[s->tx_batch_len], f, total);
        s->tx_batch_len += (uint16_t)total;
        s->tx_seq++;
        s->tx_frames++;
        s->tx_bytes_window += total;
        count_by_type(s, (uint8_t)type, true);
        return ESP_OK;
    }
    // Too big to stage: anything already batched must go out first so the host
    // never sees frames out of emission order.
    if (s->tx_batching && s->tx_batch_len) {
        batch_flush(s);
    }

    // Back-pressure: drop the frame if the TX FIFO cannot take it whole. This
    // frame's tx_seq was already stamped into the header above but never
    // reaches the wire -- still bump tx_seq (matching the failed-write path
    // below) so the sequence the host DOES see has a gap here, rather than
    // silently reusing this value for the next frame and hiding the drop.
    if (s->transport.writable &&
        s->transport.writable(s->transport.ctx) < total) {
        s->dropped_frames++;
        count_by_type(s, (uint8_t)type, false);
        s->tx_seq++;
        if (s->dropped_frames <= 5 || s->dropped_frames % 1000 == 0) {
            ESP_LOGW(TAG, "frame drop #%lu: need=%lu avail=%lu",
                     (unsigned long)s->dropped_frames,
                     (unsigned long)total,
                     (unsigned long)s->transport.writable(s->transport.ctx));
        }
        return ESP_ERR_NO_MEM;
    }
    uint32_t wrote = s->transport.write(f, total, s->transport.ctx);
    s->tx_seq++;
    if (wrote != total) {
        s->dropped_frames++;
        count_by_type(s, (uint8_t)type, false);
        return ESP_FAIL;
    }
    s->tx_frames++;
    s->tx_bytes_window += total;
    count_by_type(s, (uint8_t)type, true);
    return ESP_OK;
}

// Copy an external payload into frame_buf, then finalize + send.
static esp_err_t emit_frame_ex(usb_stream_t *s, usb_rec_type_t type,
                               const void *payload, uint16_t len, bool crc)
{
    if (len > USB_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len && payload) {
        memcpy(&s->frame_buf[USB_FRAME_HEADER_LEN], payload, len);
    }
    return emit_frame_inplace(s, type, len, crc);
}

static esp_err_t emit_frame(usb_stream_t *s, usb_rec_type_t type,
                            const void *payload, uint16_t len)
{
    // Device->PC data frames (type < 0x80) skip the software CRC; loopback /
    // control-direction frames keep it (see usb_proto.h).
    return emit_frame_ex(s, type, payload, len, (uint8_t)type >= 0x80u);
}

esp_err_t usb_stream_send_frame(usb_stream_t *s, usb_rec_type_t type,
                                const void *payload, uint16_t len)
{
    return emit_frame(s, type, payload, len);
}

// -----------------------------------------------------------------------------
// WAVE_I / WAVE_V batching (SoA; matches wire layout so flush is two memcpys)

// Samples that fit in one 1/USB_WAVE_FLUSH_HZ window at @rate, clamped to the
// frame capacity. Evaluated once per batch, never per sample.
static inline uint16_t wave_flush_limit(uint32_t rate, uint16_t cap)
{
    uint32_t n = rate / USB_WAVE_FLUSH_HZ;
    if (n < 1)   n = 1;
    if (n > cap) n = cap;
    return (uint16_t)n;
}
// -----------------------------------------------------------------------------
esp_err_t usb_stream_flush_wave_i(usb_stream_t *s)
{
    if (s->wi_count == 0) {
        return ESP_OK;
    }
    // Compose payload = header + f32 array + meta array directly into the
    // frame staging buffer (no payload-sized stack allocation — this runs on
    // the daq_fast task, which has a small stack).
    uint8_t *pl = &s->frame_buf[USB_FRAME_HEADER_LEN];
    usb_wave_hdr_t hdr = {
        .start_index  = s->wi_start_index,
        .timestamp_us = s->wi_timestamp_us,
        .sample_rate  = s->wi_rate,
        .count        = s->wi_count,
        .decimation   = s->wi_decim,
        ._pad         = 0,
    };
    memcpy(pl, &hdr, sizeof(hdr));
    memcpy(pl + sizeof(hdr), s->wi_i, (size_t)s->wi_count * sizeof(float));
    memcpy(pl + sizeof(hdr) + (size_t)s->wi_count * sizeof(float),
           s->wi_meta, (size_t)s->wi_count * sizeof(uint8_t));
    uint16_t len = (uint16_t)(sizeof(hdr) +
                              (size_t)s->wi_count * (sizeof(float) + sizeof(uint8_t)));

    esp_err_t err = emit_frame_inplace(s, USB_REC_WAVE_I, len, false);
    if (err == ESP_ERR_INVALID_SIZE) {
        // Should be unreachable given USB_WAVE_I_BATCH is sized to fit
        // USB_MAX_PAYLOAD, but log loudly if it ever regresses rather than
        // silently dropping the batch.
        ESP_LOGE(TAG, "WAVE_I payload oversized (len=%u > %u), batch dropped",
                 (unsigned)len, (unsigned)USB_MAX_PAYLOAD);
    }
    s->wi_count = 0;
    return err;
}

void usb_stream_push_sample(usb_stream_t *s, const fusion_output_t *fo,
                            uint32_t sample_rate, uint8_t decimation,
                            bool settling)
{
    if (s->reset_pending) {
        usb_stream_reset_apply(s);   // consumed on the sole producer task
    }
    uint64_t idx = s->sample_seq++;
    if (!s->streaming) {
        return;
    }
    if (s->wi_count == 0) {
        s->wi_start_index  = idx;
        s->wi_timestamp_us = (uint64_t)esp_timer_get_time();
        s->wi_rate         = sample_rate;
        s->wi_decim        = decimation ? decimation : 1;
        s->wi_limit        = wave_flush_limit(sample_rate, USB_WAVE_I_BATCH);
    }
    s->wi_i[s->wi_count] = fo->amps;
    s->wi_meta[s->wi_count] =
          (uint8_t)(fo->range & 0x03)
        | (uint8_t)((fo->source & 0x03) << 2)
        | (fo->saturated ? USB_META_SATURATED : 0)
        | (settling      ? USB_META_SETTLING  : 0);
    if (++s->wi_count >= s->wi_limit) {
        usb_stream_flush_wave_i(s);
    }
}

esp_err_t usb_stream_flush_wave_v(usb_stream_t *s)
{
    if (s->wv_count == 0) {
        return ESP_OK;
    }
    // Compose the payload directly into the frame staging buffer (see
    // flush_wave_i — no payload-sized stack allocation).
    uint8_t *pl = &s->frame_buf[USB_FRAME_HEADER_LEN];
    usb_wave_hdr_t hdr = {
        .start_index  = s->wv_start_index,
        .timestamp_us = s->wv_timestamp_us,
        .sample_rate  = s->wv_rate,
        .count        = s->wv_count,
        .decimation   = 1,
        ._pad         = 0,
    };
    memcpy(pl, &hdr, sizeof(hdr));
    memcpy(pl + sizeof(hdr), s->wv_v, (size_t)s->wv_count * sizeof(float));
    uint16_t len = (uint16_t)(sizeof(hdr) + (size_t)s->wv_count * sizeof(float));

    esp_err_t err = emit_frame_inplace(s, USB_REC_WAVE_V, len, false);
    if (err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGE(TAG, "WAVE_V payload oversized (len=%u > %u), batch dropped",
                 (unsigned)len, (unsigned)USB_MAX_PAYLOAD);
    }
    s->wv_count = 0;
    return err;
}

void usb_stream_push_voltage(usb_stream_t *s, float v, uint32_t sample_rate)
{
    if (s->reset_pending) {
        usb_stream_reset_apply(s);   // consumed on the sole producer task
    }
    uint64_t idx = s->volt_seq++;
    if (!s->streaming) {
        return;
    }
    if (s->wv_count == 0) {
        s->wv_start_index  = idx;
        s->wv_timestamp_us = (uint64_t)esp_timer_get_time();
        s->wv_rate         = sample_rate;
        s->wv_limit        = wave_flush_limit(sample_rate, USB_WAVE_V_BATCH);
    }
    s->wv_v[s->wv_count] = v;
    if (++s->wv_count >= s->wv_limit) {
        usb_stream_flush_wave_v(s);
    }
}

// -----------------------------------------------------------------------------
// STATS / ENERGY / STATUS
// -----------------------------------------------------------------------------
static void fill_stat(usb_stat_block_t *dst, const stat_result_t *src)
{
    dst->min   = src->min;
    dst->max   = src->max;
    dst->mean  = src->mean;
    dst->rms   = src->rms;
    dst->std   = src->std;
    dst->count = src->count;
}

esp_err_t usb_stream_send_stats(usb_stream_t *s, const power_dsp_t *d)
{
    usb_stats_payload_t p;
    stat_result_t r;
    DAQ_PERF_BEGIN(t_math);
    power_dsp_get_stats(d, PDSP_SIG_I, &r); fill_stat(&p.i, &r);
    power_dsp_get_stats(d, PDSP_SIG_V, &r); fill_stat(&p.v, &r);
    power_dsp_get_stats(d, PDSP_SIG_P, &r); fill_stat(&p.p, &r);
    DAQ_PERF_END(DAQ_PERF_SUM_MATH, t_math);
    return emit_frame(s, USB_REC_STATS, &p, sizeof(p));
}

esp_err_t usb_stream_send_energy(usb_stream_t *s, const power_dsp_t *d)
{
    usb_energy_payload_t p = {
        .energy_mwh = power_dsp_energy_mwh(d),
        .energy_j   = power_dsp_energy_j(d),
        .charge_mah = power_dsp_charge_mah(d),
        .charge_c   = power_dsp_charge_c(d),
        .elapsed_s  = power_dsp_elapsed_s(d),
        .last_i     = power_dsp_last_i(d),
        .last_v     = power_dsp_last_v(d),
        .last_p     = power_dsp_last_p(d),
    };
    return emit_frame(s, USB_REC_ENERGY, &p, sizeof(p));
}

esp_err_t usb_stream_send_status(usb_stream_t *s, const usb_status_payload_t *st)
{
    return emit_frame(s, USB_REC_STATUS, st, sizeof(*st));
}

esp_err_t usb_stream_send_marker(usb_stream_t *s, uint8_t channel, uint8_t edge,
                                 uint8_t kind, uint64_t sample_index)
{
    usb_marker_payload_t m = {
        .sample_index = (sample_index == UINT64_MAX) ? s->sample_seq
                                                       : sample_index,
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .channel      = channel,
        .edge         = edge ? 1u : 0u,
        .kind         = kind,
        ._pad         = 0,
    };
    return emit_frame_ex(s, USB_REC_MARKER, &m, sizeof(m), false);
}

void usb_stream_set_arm(usb_stream_t *s, bool armed, uint32_t pre_samples)
{
    s->armed       = armed;
    s->pre_samples = pre_samples;
}

uint64_t usb_stream_sample_seq(const usb_stream_t *s)
{
    return s->sample_seq;
}

void usb_stream_get_type_counters(const usb_stream_t *s,
                                  uint32_t *wi_frames, uint32_t *wv_frames,
                                  uint32_t *wi_drops, uint32_t *wv_drops)
{
    if (!s) return;
    if (wi_frames) *wi_frames = s->wi_frames;
    if (wv_frames) *wv_frames = s->wv_frames;
    if (wi_drops)  *wi_drops  = s->wi_drops;
    if (wv_drops)  *wv_drops  = s->wv_drops;
}

void usb_stream_perf_tick(usb_stream_t *s)
{
    int64_t now = esp_timer_get_time();
    if (s->perf_last_us == 0) {
        s->perf_last_us = now;
        return;
    }
    int64_t dt_us = now - s->perf_last_us;
    if (dt_us <= 0) {
        return;
    }
    s->bytes_per_sec = (uint32_t)((s->tx_bytes_window * 1000000ULL) / (uint64_t)dt_us);
    s->tx_bytes_window = 0;

    // sample_seq restarts at 0 on a session reset (applied on this same
    // task); clamp a backwards delta to 0 rather than wrapping huge.
    uint64_t emitted = (s->sample_seq >= s->perf_last_sample_seq)
                       ? (s->sample_seq - s->perf_last_sample_seq) : 0;
    uint32_t emit_rate = (uint32_t)((emitted * 1000000ULL) / (uint64_t)dt_us);
    s->perf_last_sample_seq = s->sample_seq;

    s->perf_last_us = now;
    ESP_LOGI(TAG, "perf: %.2f MB/s emit=%lu Sa/s frames=%lu drops=%lu",
             (double)s->bytes_per_sec / (1024.0 * 1024.0),
             (unsigned long)emit_rate,
             (unsigned long)s->tx_frames,
             (unsigned long)s->dropped_frames);
}

esp_err_t usb_stream_send_fft(usb_stream_t *s, const float *mags, uint16_t nbins,
                              uint32_t sample_rate, uint8_t source, uint8_t window)
{
    // Clamp so header + bins fit one frame.
    uint16_t max_bins = (uint16_t)((USB_MAX_PAYLOAD - sizeof(usb_fft_header_t))
                                   / sizeof(float));
    if (nbins > max_bins) nbins = max_bins;

    // Compose the payload directly into the frame staging buffer (no static /
    // stack buffer needed — frame_buf is exactly the right size).
    uint8_t *pl = &s->frame_buf[USB_FRAME_HEADER_LEN];
    usb_fft_header_t hdr = {
        .sample_rate = sample_rate,
        .nbins       = nbins,
        .source      = source,
        .window      = window,
    };
    memcpy(pl, &hdr, sizeof(hdr));
    memcpy(pl + sizeof(hdr), mags, (size_t)nbins * sizeof(float));
    uint16_t len = (uint16_t)(sizeof(hdr) + (size_t)nbins * sizeof(float));
    return emit_frame_inplace(s, USB_REC_FFT, len, false);
}

// -----------------------------------------------------------------------------
// Inbound control decoding (simple framed parser)
// -----------------------------------------------------------------------------
static void dispatch_rx_frame(usb_stream_t *s, const uint8_t *f, uint16_t total)
{
    usb_rec_type_t type = (usb_rec_type_t)f[3];
    uint16_t len = (uint16_t)(f[10] | (f[11] << 8));
    uint16_t crc_calc = usb_proto_crc16(&f[2], (uint32_t)(USB_FRAME_HEADER_LEN - 2) + len, 0xFFFF);
    uint16_t crc_rx = (uint16_t)(f[USB_FRAME_HEADER_LEN + len] |
                                 (f[USB_FRAME_HEADER_LEN + len + 1] << 8));
    if (crc_calc != crc_rx) {
        ESP_LOGW(TAG, "rx CRC mismatch (type 0x%02X)", type);
        return;
    }
    if (s->cmd_cb) {
        s->cmd_cb(type, &f[USB_FRAME_HEADER_LEN], len, s->cmd_user);
    }
}

void usb_stream_on_rx(usb_stream_t *s, const uint8_t *data, uint32_t len)
{
    for (uint32_t k = 0; k < len; ++k) {
        uint8_t byte = data[k];

        // Resync on magic at the start of the buffer.
        if (s->rx_len == 0 && byte != USB_PROTO_MAGIC0) continue;
        if (s->rx_len == 1 && byte != USB_PROTO_MAGIC1) {
            // Byte 2 isn't MAGIC1, so this candidate frame is bogus -- but
            // don't just drop the byte: if it's itself a MAGIC0, a real frame
            // may start right here (e.g. "BB BB 50 ..." -- the first BB fails
            // as a candidate, and the naive drop-and-reset also ate the second
            // BB, so the frame that actually starts there was never found).
            // Re-arm rx_len=1 with this byte as the new rx_buf[0] instead.
            if (byte == USB_PROTO_MAGIC0) {
                s->rx_buf[0] = byte;
            } else {
                s->rx_len = 0;
            }
            continue;
        }

        if (s->rx_len < sizeof(s->rx_buf)) {
            s->rx_buf[s->rx_len++] = byte;
        } else {
            s->rx_len = 0;   // overflow -> resync
            continue;
        }

        if (s->rx_len >= USB_FRAME_HEADER_LEN) {
            uint16_t plen = (uint16_t)(s->rx_buf[10] | (s->rx_buf[11] << 8));
            uint16_t total = (uint16_t)(USB_FRAME_HEADER_LEN + plen + USB_FRAME_CRC_LEN);
            if (plen > (sizeof(s->rx_buf) - USB_FRAME_OVERHEAD)) {
                s->rx_len = 0;   // implausible length -> resync
                continue;
            }
            if (s->rx_len >= total) {
                dispatch_rx_frame(s, s->rx_buf, total);
                s->rx_len = 0;
            }
        }
    }
}
