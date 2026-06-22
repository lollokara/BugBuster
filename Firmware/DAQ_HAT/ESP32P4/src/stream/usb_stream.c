// =============================================================================
// usb_stream.c — frame builder / transport manager for the measurement stream.
// =============================================================================

#include "usb_stream.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "usb_stream";

// -----------------------------------------------------------------------------
// CRC-16/CCITT-FALSE
// -----------------------------------------------------------------------------
uint16_t usb_proto_crc16(const uint8_t *data, uint32_t len, uint16_t init)
{
    uint16_t crc = init;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void usb_stream_init(usb_stream_t *s)
{
    memset(s, 0, sizeof(*s));
    s->wave_decim = 1;
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

// -----------------------------------------------------------------------------
// Frame assembly
// -----------------------------------------------------------------------------
static esp_err_t emit_frame(usb_stream_t *s, usb_rec_type_t type,
                            const void *payload, uint16_t len)
{
    if (len > USB_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s->have_transport) {
        s->dropped_frames++;
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
    if (len && payload) {
        memcpy(&f[USB_FRAME_HEADER_LEN], payload, len);
    }
    // CRC over header tail [2 .. 12+len).
    uint16_t crc = usb_proto_crc16(&f[2], (uint32_t)(USB_FRAME_HEADER_LEN - 2) + len, 0xFFFF);
    uint32_t crc_off = USB_FRAME_HEADER_LEN + len;
    f[crc_off]     = (uint8_t)(crc);
    f[crc_off + 1] = (uint8_t)(crc >> 8);

    uint32_t total = crc_off + USB_FRAME_CRC_LEN;

    // Back-pressure: drop the frame if the TX FIFO cannot take it whole.
    if (s->transport.writable &&
        s->transport.writable(s->transport.ctx) < total) {
        s->dropped_frames++;
        return ESP_ERR_NO_MEM;
    }
    uint32_t wrote = s->transport.write(f, total, s->transport.ctx);
    s->tx_seq++;
    if (wrote != total) {
        s->dropped_frames++;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t usb_stream_send_frame(usb_stream_t *s, usb_rec_type_t type,
                                const void *payload, uint16_t len)
{
    return emit_frame(s, type, payload, len);
}

// -----------------------------------------------------------------------------
// WAVEFORM batching
// -----------------------------------------------------------------------------
esp_err_t usb_stream_flush_waveform(usb_stream_t *s)
{
    if (s->wave_count == 0) {
        return ESP_OK;
    }
    // Compose payload = header + samples into the frame staging area directly.
    uint8_t pl[sizeof(usb_wave_header_t) +
               sizeof(usb_wave_sample_t) * 256];
    usb_wave_header_t hdr = {
        .start_seq   = s->wave_start_seq,
        .sample_rate = s->wave_rate,
        .count       = s->wave_count,
        .decimation  = s->wave_decim,
        ._pad        = 0,
    };
    memcpy(pl, &hdr, sizeof(hdr));
    memcpy(pl + sizeof(hdr), s->wave,
           (size_t)s->wave_count * sizeof(usb_wave_sample_t));
    uint16_t len = (uint16_t)(sizeof(hdr) +
                              (size_t)s->wave_count * sizeof(usb_wave_sample_t));

    esp_err_t err = emit_frame(s, USB_REC_WAVEFORM, pl, len);
    s->wave_count = 0;
    return err;
}

void usb_stream_push_sample(usb_stream_t *s, const fusion_output_t *fo,
                            float v, float p,
                            uint32_t sample_rate, uint8_t decimation)
{
    uint32_t idx = s->sample_seq++;
    if (!s->streaming) {
        return;
    }
    if (s->wave_count == 0) {
        s->wave_start_seq = idx;
        s->wave_rate      = sample_rate;
        s->wave_decim     = decimation ? decimation : 1;
    }
    usb_wave_sample_t *w = &s->wave[s->wave_count++];
    w->i      = fo->amps;
    w->v      = v;
    w->p      = p;
    w->range  = (uint8_t)fo->range;
    w->source = (uint8_t)fo->source;
    w->flags  = fo->saturated ? 0x01 : 0x00;
    w->_pad   = 0;

    if (s->wave_count >= (uint16_t)(sizeof(s->wave) / sizeof(s->wave[0]))) {
        usb_stream_flush_waveform(s);
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
    power_dsp_get_stats(d, PDSP_SIG_I, &r); fill_stat(&p.i, &r);
    power_dsp_get_stats(d, PDSP_SIG_V, &r); fill_stat(&p.v, &r);
    power_dsp_get_stats(d, PDSP_SIG_P, &r); fill_stat(&p.p, &r);
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
                                 uint8_t kind, uint32_t sample_index)
{
    usb_marker_payload_t m = {
        .sample_index = (sample_index == 0xFFFFFFFFu) ? s->sample_seq
                                                       : sample_index,
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .channel      = channel,
        .edge         = edge ? 1u : 0u,
        .kind         = kind,
        ._pad         = 0,
    };
    return emit_frame(s, USB_REC_MARKER, &m, sizeof(m));
}

void usb_stream_set_arm(usb_stream_t *s, bool armed, uint32_t pre_samples)
{
    s->armed       = armed;
    s->pre_samples = pre_samples;
}

uint32_t usb_stream_sample_seq(const usb_stream_t *s)
{
    return s->sample_seq;
}

esp_err_t usb_stream_send_fft(usb_stream_t *s, const float *mags, uint16_t nbins,
                              uint32_t sample_rate, uint8_t source, uint8_t window)
{
    // Clamp so header + bins fit one frame.
    uint16_t max_bins = (uint16_t)((USB_MAX_PAYLOAD - sizeof(usb_fft_header_t))
                                   / sizeof(float));
    if (nbins > max_bins) nbins = max_bins;

    // Build payload into the frame staging area is not accessible here, so use a
    // local buffer sized to the protocol maximum.
    static uint8_t pl[USB_MAX_PAYLOAD];
    usb_fft_header_t hdr = {
        .sample_rate = sample_rate,
        .nbins       = nbins,
        .source      = source,
        .window      = window,
    };
    memcpy(pl, &hdr, sizeof(hdr));
    memcpy(pl + sizeof(hdr), mags, (size_t)nbins * sizeof(float));
    uint16_t len = (uint16_t)(sizeof(hdr) + (size_t)nbins * sizeof(float));
    return emit_frame(s, USB_REC_FFT, pl, len);
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
        if (s->rx_len == 1 && byte != USB_PROTO_MAGIC1) { s->rx_len = 0; continue; }

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
