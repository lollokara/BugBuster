// =============================================================================
// daq_store.rs — DAQ measurement-stream data model (host side).
//
// Holds the fused per-sample tracks (current / voltage / power + range/source
// flags) for the live P4 stream, plus the latest STATS / ENERGY / FFT / STATUS
// snapshots. Provides viewport extraction (min/max decimation per pixel
// bucket), a dI/dt heatmap, and exact integrals over a sample range for the
// shift-select panel.
// =============================================================================

use crate::daq_proto::{
    meta_range, meta_source, EnergyRecord, FftRecord, MarkerRecord, StatsRecord, StatusRecord,
    WaveIRecord, WaveVRecord, META_SATURATED, META_SETTLING, SRC_BLEND, SRC_COARSE, SRC_FINE,
};
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;

/// Absolute hard ceiling on the recent raw window (RAM safety net). The actual
/// caps are chosen adaptively from available memory (see `adaptive_budget`).
pub const DAQ_STORE_MAX_SAMPLES: usize = 400_000_000;
/// Ceiling on total pyramid-covered samples (history length).
pub const DAQ_TOTAL_MAX_SAMPLES: usize = 2_000_000_000;

/// Bytes per raw sample: i/v/p (3×f32 = 12) + range/source/flags (3).
const RAW_BYTES_PER_SAMPLE: u64 = 15;
/// Amortised pyramid bytes per covered sample (all levels, ~size_of::<Bin>/15).
const PYR_BYTES_PER_SAMPLE: u64 = 3;
/// Memory kept free for the OS/UI regardless of capture size.
const RESERVED_BYTES: u64 = 1536 * 1024 * 1024; // 1.5 GB

fn available_memory_bytes() -> u64 {
    let mut sys = sysinfo::System::new();
    sys.refresh_memory();
    let avail = sys.available_memory();
    if avail == 0 {
        4 * 1024 * 1024 * 1024 // fallback: assume 4 GB
    } else {
        avail
    }
}

/// Choose `(raw_cap, total_cap)` from available RAM. The capture keeps the most
/// recent `raw_cap` samples at full resolution (zoom-in detail) and a
/// full-length min/max pyramid up to `total_cap` samples (overview + coarse
/// zoom). ~60 % of the budget goes to the raw window, ~40 % to pyramid history.
fn adaptive_budget() -> (usize, usize) {
    let avail = available_memory_bytes();
    let half = avail / 100 * 55;
    let after_reserve = avail.saturating_sub(RESERVED_BYTES);
    let budget = half.min(after_reserve).max(256 * 1024 * 1024); // ≥ 256 MB
    let raw_cap = ((budget * 60 / 100) / RAW_BYTES_PER_SAMPLE) as usize;
    let raw_cap = raw_cap.clamp(1_000_000, DAQ_STORE_MAX_SAMPLES);
    let total_cap = ((budget * 40 / 100) / PYR_BYTES_PER_SAMPLE) as usize;
    let total_cap = total_cap.clamp(raw_cap, DAQ_TOTAL_MAX_SAMPLES);
    (raw_cap, total_cap)
}

/// One digital event marker held by the store / returned in a view. Absolute
/// `sample_index` is in `total` space (never decimated).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqMarker {
    pub sample_index: u64,
    pub timestamp_us: u64,
    pub channel: u8,
    pub edge: u8,
    pub kind: u8,
}

#[derive(Debug, Clone, Default)]
pub struct DaqStore {
    pub sample_rate_hz: u32,
    pub decimation: u8,
    /// Total samples ever appended (== pyramid coverage). Raw arrays hold only
    /// the most recent window; older raw is evicted but stays in the pyramid.
    pub total: u64,
    /// Recent raw window cap (samples kept at full resolution).
    pub raw_cap: usize,
    /// Pyramid history cap (`max_samples`): appending stops past this.
    pub max_samples: usize,
    /// Parallel per-sample arrays (most recent `raw_cap` samples).
    pub i: Vec<f32>,
    pub v: Vec<f32>,
    pub p: Vec<f32>,
    pub range: Vec<u8>,
    pub source: Vec<u8>,
    pub flags: Vec<u8>,
    pub overflow: bool,
    /// Digital event markers (flags + triggers). Stored as discrete events with
    /// their absolute sample index, so they are NEVER decimated away — they
    /// survive every zoom level untouched (requirement: flags preserved through
    /// compression). Kept sorted by `sample_index`.
    pub markers: Vec<DaqMarker>,
    /// Latest device-pushed aggregate snapshots.
    pub last_stats: Option<StatsRecord>,
    pub last_energy: Option<EnergyRecord>,
    pub last_fft: Option<FftRecord>,
    pub last_status: Option<StatusRecord>,
    /// Multi-resolution min/max pyramid. `levels[k]` reduces raw by
    /// PYR_FACTOR^(k+1); `built[k]` counts the immutable (complete) bins so
    /// updates only recompute the small tail on each append.
    levels: Vec<Vec<Bin>>,
    built: Vec<usize>,

    // ── v2 dual-timeline state ──────────────────────────────────────────────
    /// Absolute device index of raw[0], anchored from the first WAVE_I record.
    first_index: Option<u64>,
    /// Cumulative count of index-gap samples that were filled with NaN (or,
    /// past the fill cap, skipped by re-anchoring `first_index`).
    dropped_samples: u64,
    /// Last held voltage sample (device-time hold), NaN until the first
    /// WAVE_V arrives.
    last_v: f32,
    /// Pending voltage samples not yet consumed by the current axis, as
    /// `(timestamp_us, v)` pairs in device time, oldest first.
    volt_fifo: VecDeque<(u64, f32)>,
    /// Ring of the last 64 `(timestamp_us, start_index)` WAVE_I header pairs,
    /// used for the online least-squares timebase fit.
    timebase_ring: VecDeque<(u64, u64)>,
    /// Fitted actual sample rate (Hz), falls back to `sample_rate_hz` until a
    /// fit is available / while the fit is out of the ±5% sanity band.
    actual_rate_hz: f64,
}

/// One decimated column of the trace view: min/max envelope per track.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqViewData {
    pub sample_rate_hz: u32,
    pub total_samples: u64,
    pub view_start: u64,
    pub view_end: u64,
    pub decimated: bool,
    pub overflow: bool,
    /// Fitted actual sample rate (Hz); falls back to nominal `sample_rate_hz`.
    pub actual_rate_hz: f64,
    /// Per-bucket min/max for each analog track. Length = number of columns.
    pub i_min: Vec<f32>,
    pub i_max: Vec<f32>,
    pub v_min: Vec<f32>,
    pub v_max: Vec<f32>,
    pub p_min: Vec<f32>,
    pub p_max: Vec<f32>,
    /// Dominant fusion source per bucket (0=FINE,1=COARSE,2=BLEND) for tinting.
    pub source: Vec<u8>,
    /// Per-bucket peak |dI/dt| in A/s for the bottom heatmap.
    pub didt: Vec<f32>,
    /// 1 = bucket had no finite sample (index gap / not-yet-held voltage).
    pub gap: Vec<u8>,
    /// Event markers whose absolute sample index falls inside the view window.
    /// Always sent at full fidelity regardless of decimation.
    pub markers: Vec<DaqMarker>,
}

/// Exact integrals over a selected sample range, for the shift-select panel.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DaqIntegral {
    pub start: u64,
    pub end: u64,
    pub duration_s: f64,
    pub charge_c: f64,
    pub charge_mah: f64,
    pub energy_j: f64,
    pub energy_mwh: f64,
    pub avg_i: f64,
    pub avg_v: f64,
    pub avg_p: f64,
    pub min_i: f64,
    pub max_i: f64,
    /// Extrapolated "if this pattern ran for 1 hour" consumption (avg_p × 1 h).
    pub projected_mwh_per_hour: f64,
}

// ── Display filters (raw-domain, applied before min/max decimation) ──────────
// Used only when the "hi-res" filter toggle is on and the viewport is in the
// raw path (span small enough that raw samples back every column). Filtering the
// true signal before decimation gives zoom-stable, undistorted smoothing.

/// Centered box (moving-average) low-pass; O(n) via prefix sum.
fn moving_avg(x: &[f32], win: usize) -> Vec<f32> {
    let n = x.len();
    if n == 0 || win <= 1 {
        return x.to_vec();
    }
    let half = (win / 2).max(1);
    let mut pref = vec![0.0f64; n + 1];
    for i in 0..n {
        pref[i + 1] = pref[i] + x[i] as f64;
    }
    let mut out = vec![0.0f32; n];
    for (i, slot) in out.iter_mut().enumerate() {
        let a = i.saturating_sub(half);
        let b = (i + half + 1).min(n);
        *slot = ((pref[b] - pref[a]) / (b - a) as f64) as f32;
    }
    out
}

/// Zero-phase exponential moving average (forward + backward pass).
fn ema(x: &[f32], win: usize) -> Vec<f32> {
    let n = x.len();
    if n == 0 || win <= 1 {
        return x.to_vec();
    }
    let alpha = 2.0 / (win as f64 + 1.0);
    let mut fwd = vec![0.0f32; n];
    let mut acc = x[0] as f64;
    for i in 0..n {
        acc += alpha * (x[i] as f64 - acc);
        fwd[i] = acc as f32;
    }
    let mut out = vec![0.0f32; n];
    let mut acc2 = fwd[n - 1] as f64;
    for i in (0..n).rev() {
        acc2 += alpha * (fwd[i] as f64 - acc2);
        out[i] = acc2 as f32;
    }
    out
}

/// Windowed median (spike rejection); window capped for cost.
fn median_filter(x: &[f32], win: usize) -> Vec<f32> {
    let n = x.len();
    if n == 0 || win <= 1 {
        return x.to_vec();
    }
    let w = win.clamp(1, 127);
    let half = w / 2;
    let mut out = vec![0.0f32; n];
    let mut buf: Vec<f32> = Vec::with_capacity(w);
    for i in 0..n {
        buf.clear();
        let a = i.saturating_sub(half);
        let b = (i + half + 1).min(n);
        buf.extend_from_slice(&x[a..b]);
        buf.sort_by(|p, q| p.partial_cmp(q).unwrap_or(std::cmp::Ordering::Equal));
        out[i] = buf[buf.len() / 2];
    }
    out
}

/// High-pass = signal minus its moving average (AC ripple, removes DC).
fn highpass(x: &[f32], win: usize) -> Vec<f32> {
    let lp = moving_avg(x, win);
    x.iter().zip(lp.iter()).map(|(a, b)| a - b).collect()
}

/// Dispatch one filter. `kind`: 1=avg, 2=EMA, 3=median, 4=high-pass.
fn apply_filter(x: &[f32], win: usize, kind: u8) -> Vec<f32> {
    match kind {
        1 => moving_avg(x, win),
        2 => ema(x, win),
        3 => median_filter(x, win),
        4 => highpass(x, win),
        _ => x.to_vec(),
    }
}

// ── Multi-resolution min/max pyramid ────────────────────────────────────────
// Each level reduces the one below by PYR_FACTOR, so a viewport query only ever
// scans O(columns) bins regardless of total capture length. Zoom-in below the
// finest reduction reads raw samples for full detail.
const PYR_FACTOR: usize = 16;
const PYR_MAX_LEVELS: usize = 6; // 16^6 ≈ 16.7M raw samples per top-level bin

/// One min/max bin summarising a contiguous run of samples.
#[derive(Debug, Clone, Copy)]
struct Bin {
    i_min: f32,
    i_max: f32,
    v_min: f32,
    v_max: f32,
    p_min: f32,
    p_max: f32,
    didt: f32,
    source: u8,
}

impl Bin {
    fn empty() -> Self {
        Bin {
            i_min: f32::INFINITY,
            i_max: f32::NEG_INFINITY,
            v_min: f32::INFINITY,
            v_max: f32::NEG_INFINITY,
            p_min: f32::INFINITY,
            p_max: f32::NEG_INFINITY,
            didt: 0.0,
            source: 0,
        }
    }
    fn merge(&mut self, o: &Bin) {
        // Keep the fusion source of the higher-current child (most informative
        // for tinting at coarse zoom).
        if o.i_max > self.i_max {
            self.source = o.source;
        }
        self.i_min = self.i_min.min(o.i_min);
        self.i_max = self.i_max.max(o.i_max);
        self.v_min = self.v_min.min(o.v_min);
        self.v_max = self.v_max.max(o.v_max);
        self.p_min = self.p_min.min(o.p_min);
        self.p_max = self.p_max.max(o.p_max);
        self.didt = self.didt.max(o.didt);
    }
}

/// Build a bin from raw sample arrays over the absolute range [a_abs, b_abs),
/// where the raw arrays start at absolute index `raw_start` (older samples may
/// have been evicted).
fn build_bin_raw(
    i: &[f32],
    v: &[f32],
    p: &[f32],
    src: &[u8],
    a_abs: usize,
    b_abs: usize,
    raw_start: usize,
    dt: f32,
) -> Bin {
    let mut bin = Bin::empty();
    let a = a_abs.saturating_sub(raw_start);
    let b = b_abs.saturating_sub(raw_start).min(i.len());
    if a >= b {
        return bin;
    }
    let mut prev_i = if a > 0 { i[a - 1] } else { i[a] };
    let mut peak_i = f32::NEG_INFINITY;
    for k in a..b {
        let iv = i[k];
        bin.i_min = bin.i_min.min(iv);
        bin.i_max = bin.i_max.max(iv);
        let vv = v[k];
        bin.v_min = bin.v_min.min(vv);
        bin.v_max = bin.v_max.max(vv);
        let pv = p[k];
        bin.p_min = bin.p_min.min(pv);
        bin.p_max = bin.p_max.max(pv);
        let d = ((iv - prev_i) / dt).abs();
        if d > bin.didt {
            bin.didt = d;
        }
        prev_i = iv;
        if iv > peak_i {
            peak_i = iv;
            bin.source = src[k];
        }
    }
    bin
}

/// Build a bin by merging lower-level bins over [a, b).
fn build_bin_merge(lower: &[Bin], a: usize, b: usize) -> Bin {
    let mut bin = Bin::empty();
    let b = b.min(lower.len());
    for e in a..b {
        bin.merge(&lower[e]);
    }
    bin
}

impl DaqStore {
    pub fn new(sample_rate_hz: u32) -> Self {
        let (raw_cap, total_cap) = adaptive_budget();
        let rate = sample_rate_hz.max(1);
        Self {
            sample_rate_hz: rate,
            decimation: 1,
            total: 0,
            raw_cap,
            max_samples: total_cap,
            levels: vec![Vec::new(); PYR_MAX_LEVELS],
            built: vec![0; PYR_MAX_LEVELS],
            last_v: f32::NAN,
            actual_rate_hz: rate as f64,
            ..Default::default()
        }
    }

    /// Fitted actual sample rate (Hz), from the online least-squares timebase
    /// fit; falls back to the nominal `sample_rate_hz` until enough headers
    /// have been seen or the fit falls outside the ±5% sanity band.
    pub fn actual_rate_hz(&self) -> f64 {
        self.actual_rate_hz
    }

    /// Cumulative count of index-gap samples filled with NaN (or skipped past
    /// the fill cap, in which case `first_index` was advanced instead).
    pub fn dropped_samples(&self) -> u64 {
        self.dropped_samples
    }

    /// Estimated resident bytes of the stored capture (raw window + pyramid).
    pub fn mem_used_bytes(&self) -> u64 {
        let raw = self.i.len() as u64 * RAW_BYTES_PER_SAMPLE;
        let pyr: u64 = self
            .levels
            .iter()
            .map(|l| l.len() as u64 * std::mem::size_of::<Bin>() as u64)
            .sum();
        raw + pyr
    }

    /// Absolute index of the oldest sample still held in the raw arrays.
    fn raw_start(&self) -> usize {
        self.total as usize - self.i.len()
    }

    pub fn total_samples(&self) -> u64 {
        self.total
    }

    pub fn clear(&mut self) {
        self.i.clear();
        self.v.clear();
        self.p.clear();
        self.range.clear();
        self.source.clear();
        self.flags.clear();
        self.overflow = false;
        self.total = 0;
        self.markers.clear();
        for lvl in self.levels.iter_mut() {
            lvl.clear();
        }
        for b in self.built.iter_mut() {
            *b = 0;
        }
        self.first_index = None;
        self.dropped_samples = 0;
        self.last_v = f32::NAN;
        self.volt_fifo.clear();
        self.timebase_ring.clear();
        self.actual_rate_hz = self.sample_rate_hz.max(1) as f64;
    }

    /// Record a digital event marker (flag / trigger). Kept sorted by absolute
    /// sample index and never decimated, so it survives every zoom level.
    pub fn push_marker(&mut self, m: &MarkerRecord) {
        let dm = DaqMarker {
            sample_index: m.sample_index as u64,
            timestamp_us: m.timestamp_us,
            channel: m.channel,
            edge: m.edge,
            kind: m.kind,
        };
        // Markers usually arrive in order; binary-search keeps the vec sorted.
        let pos = self
            .markers
            .partition_point(|x| x.sample_index <= dm.sample_index);
        self.markers.insert(pos, dm);
        // Bound memory: keep at most the most recent 100k markers.
        const MAX_MARKERS: usize = 100_000;
        if self.markers.len() > MAX_MARKERS {
            let drop = self.markers.len() - MAX_MARKERS;
            self.markers.drain(0..drop);
        }
    }

    /// Markers whose absolute sample index falls within `[start, end)`.
    fn markers_in_range(&self, start: u64, end: u64) -> Vec<DaqMarker> {
        let lo = self.markers.partition_point(|m| m.sample_index < start);
        let hi = self.markers.partition_point(|m| m.sample_index < end);
        self.markers[lo..hi].to_vec()
    }

    /// Evict the oldest raw samples once the recent window exceeds `raw_cap`.
    /// The pyramid still covers them, so overview/coarse zoom is unaffected;
    /// only full-resolution zoom into the evicted region degrades to the finest
    /// pyramid level. Eviction happens in chunks to amortise the front-drain.
    fn evict_raw(&mut self) {
        let margin = self.raw_cap / 4 + 1;
        if self.i.len() >= self.raw_cap + margin {
            let evict = self.i.len() - self.raw_cap;
            self.i.drain(0..evict);
            self.v.drain(0..evict);
            self.p.drain(0..evict);
            self.range.drain(0..evict);
            self.source.drain(0..evict);
            self.flags.drain(0..evict);
        }
    }

    /// Incrementally rebuild the pyramid tails after new samples were appended.
    fn update_pyramid(&mut self) {
        if self.levels.len() != PYR_MAX_LEVELS {
            self.levels = vec![Vec::new(); PYR_MAX_LEVELS];
            self.built = vec![0; PYR_MAX_LEVELS];
        }
        let total = self.total as usize;
        let raw_start = total - self.i.len();
        let dt = 1.0_f32 / self.sample_rate_hz.max(1) as f32;
        // Level 0 from raw samples (absolute indices, offset by raw_start).
        {
            let complete = total / PYR_FACTOR;
            let lvl = &mut self.levels[0];
            lvl.truncate(self.built[0]); // drop the old partial tail
            while self.built[0] < complete {
                let a = self.built[0] * PYR_FACTOR;
                lvl.push(build_bin_raw(
                    &self.i,
                    &self.v,
                    &self.p,
                    &self.source,
                    a,
                    a + PYR_FACTOR,
                    raw_start,
                    dt,
                ));
                self.built[0] += 1;
            }
            if total > complete * PYR_FACTOR {
                lvl.push(build_bin_raw(
                    &self.i,
                    &self.v,
                    &self.p,
                    &self.source,
                    complete * PYR_FACTOR,
                    total,
                    raw_start,
                    dt,
                ));
            }
        }
        // Higher levels from the level below.
        for k in 1..PYR_MAX_LEVELS {
            let (low_s, high_s) = self.levels.split_at_mut(k);
            let lower = &low_s[k - 1];
            let lvl = &mut high_s[0];
            let complete = self.built[k - 1] / PYR_FACTOR;
            lvl.truncate(self.built[k]);
            while self.built[k] < complete {
                let a = self.built[k] * PYR_FACTOR;
                lvl.push(build_bin_merge(lower, a, a + PYR_FACTOR));
                self.built[k] += 1;
            }
            if lower.len() > complete * PYR_FACTOR {
                lvl.push(build_bin_merge(lower, complete * PYR_FACTOR, lower.len()));
            }
        }
    }

    /// Push one sample onto the fused-current axis, honouring the history
    /// cap. Returns `false` (and sets `overflow`) once the cap is hit.
    fn try_push_sample(&mut self, i: f32, v: f32, p: f32, range: u8, source: u8, flags: u8) -> bool {
        if self.total as usize >= self.max_samples.max(1) {
            if !self.overflow {
                self.overflow = true;
                log::warn!(
                    "daq_store: history cap ({}) reached — capture truncated",
                    self.max_samples
                );
            }
            return false;
        }
        self.i.push(i);
        self.v.push(v);
        self.p.push(p);
        self.range.push(range);
        self.source.push(source);
        self.flags.push(flags);
        self.total += 1;
        true
    }

    /// Online least-squares fit of `timestamp_us` vs `start_index` over the
    /// 64-header ring, clamped to ±5% of the nominal rate to reject nonsense
    /// during startup (too few points / noisy early headers).
    fn fit_timebase(&mut self) {
        let n = self.timebase_ring.len();
        let nominal = self.sample_rate_hz.max(1) as f64;
        if n < 2 {
            self.actual_rate_hz = nominal;
            return;
        }
        let (mut sx, mut sy, mut sxx, mut sxy) = (0.0f64, 0.0f64, 0.0f64, 0.0f64);
        for &(t_us, idx) in &self.timebase_ring {
            let x = idx as f64;
            let y = t_us as f64;
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        let nf = n as f64;
        let denom = nf * sxx - sx * sx;
        if denom.abs() < 1e-9 {
            return;
        }
        let slope_us_per_sample = (nf * sxy - sx * sy) / denom;
        if slope_us_per_sample <= 0.0 {
            return;
        }
        let rate = 1e6 / slope_us_per_sample;
        if rate >= nominal * 0.95 && rate <= nominal * 1.05 {
            self.actual_rate_hz = rate;
        }
    }

    /// Append a decoded WAVE_V (voltage) record. Voltage samples are held in a
    /// device-timestamp FIFO and consumed by `append_wave_i` (voltage hold
    /// onto the current axis), tolerating any current/voltage rate ratio.
    pub fn append_wave_v(&mut self, rec: &WaveVRecord) {
        let rate = rec.sample_rate.max(1) as u64;
        for (k, &vv) in rec.v.iter().enumerate() {
            let t_us = rec.timestamp_us + (k as u64 * 1_000_000) / rate;
            self.volt_fifo.push_back((t_us, vv));
        }
        // Bound memory in case current samples stop arriving; the FIFO should
        // normally drain well before this in a healthy stream.
        const MAX_VOLT_FIFO: usize = 1_000_000;
        if self.volt_fifo.len() > MAX_VOLT_FIFO {
            let drop = self.volt_fifo.len() - MAX_VOLT_FIFO;
            self.volt_fifo.drain(0..drop);
        }
    }

    /// Append a decoded WAVE_I (fused current) record. Handles index-gap
    /// filling/re-anchoring, device-restart detection, voltage hold, and the
    /// timebase fit before appending the samples themselves.
    pub fn append_wave_i(&mut self, rec: &WaveIRecord) {
        if rec.sample_rate > 0 {
            self.sample_rate_hz = rec.sample_rate;
        }
        if rec.decimation > 0 {
            self.decimation = rec.decimation;
        }
        let rate = self.sample_rate_hz.max(1) as u64;

        let expected = match self.first_index {
            None => {
                self.first_index = Some(rec.start_index);
                rec.start_index
            }
            Some(fi) => fi + self.total,
        };

        if rec.start_index > expected {
            let gap_len = rec.start_index - expected;
            let cap = 4 * rate; // 4 seconds worth of samples at the nominal rate
            let fill = gap_len.min(cap);
            for _ in 0..fill {
                if !self.try_push_sample(f32::NAN, f32::NAN, f32::NAN, 0, 0, 0) {
                    break;
                }
            }
            self.dropped_samples += gap_len;
            if gap_len > cap {
                if let Some(fi) = self.first_index.as_mut() {
                    *fi += gap_len - fill;
                }
                log::warn!(
                    "daq_store: index gap of {gap_len} samples exceeds the {cap}-sample fill cap; re-anchoring first_index"
                );
            }
        } else if rec.start_index < expected {
            // Device restarted mid-stream: indices no longer align with what
            // we've stored, so drop everything and re-anchor from here.
            self.clear();
            self.first_index = Some(rec.start_index);
        }

        // Timebase fit: keep the last 64 (timestamp_us, start_index) header
        // pairs and recompute the slope on every append.
        self.timebase_ring.push_back((rec.timestamp_us, rec.start_index));
        if self.timebase_ring.len() > 64 {
            self.timebase_ring.pop_front();
        }
        self.fit_timebase();

        for (k, &iv) in rec.i.iter().enumerate() {
            // Device time of this sample, used to advance the voltage hold
            // regardless of the current/voltage rate ratio.
            let t_us = rec.timestamp_us + (k as u64 * 1_000_000) / rate;
            while let Some(&(vt, vv)) = self.volt_fifo.front() {
                if vt <= t_us {
                    self.last_v = vv;
                    self.volt_fifo.pop_front();
                } else {
                    break;
                }
            }
            let vv = self.last_v;
            let pv = iv * vv;
            let m = rec.meta.get(k).copied().unwrap_or(0);
            let range = meta_range(m);
            let source = meta_source(m);
            let flags = m & (META_SATURATED | META_SETTLING);
            if !self.try_push_sample(iv, vv, pv, range, source, flags) {
                break;
            }
        }
        self.update_pyramid();
        self.evict_raw();
    }

    /// Extract a decimated view between [start, end) sample indices, at most
    /// `max_points` columns wide. Uses min/max envelope decimation. When
    /// `filter_type != 0` and `smooth > 1` and the viewport is in the raw path,
    /// the selected filter is applied to the raw signal before decimation
    /// ("hi-res" smoothing); otherwise the raw envelope is returned and any
    /// client-side display filter is applied in the frontend.
    pub fn get_view(
        &self,
        start: u64,
        end: u64,
        max_points: u32,
        smooth: u32,
        filter_type: u8,
    ) -> DaqViewData {
        let total = self.total_samples();
        let start = start.min(total);
        let end = end.clamp(start, total);
        let span = (end - start) as usize;
        let max_points = max_points.max(1) as usize;

        let mut view = DaqViewData {
            sample_rate_hz: self.sample_rate_hz,
            total_samples: total,
            view_start: start,
            view_end: end,
            overflow: self.overflow,
            actual_rate_hz: self.actual_rate_hz(),
            ..Default::default()
        };
        // Markers are independent of the decimation path — always attach the
        // full set within the window so flags/triggers render at every zoom.
        view.markers = self.markers_in_range(start, end);
        if span == 0 {
            return view;
        }

        let s0 = start as usize;
        let s1 = end as usize;
        let raw_start = self.raw_start();
        let columns = span.min(max_points);
        let raw_per_col = (span / columns).max(1) as u64;
        let dt = 1.0_f32 / self.sample_rate_hz.max(1) as f32;

        // Pick the coarsest pyramid level whose bins are still finer than a
        // column. m == 0 means read raw samples (full detail).
        let mut m = 0usize;
        let mut f_pow: u64 = 1;
        while m < PYR_MAX_LEVELS
            && f_pow * (PYR_FACTOR as u64) <= raw_per_col
            && !self.levels[m].is_empty()
        {
            f_pow *= PYR_FACTOR as u64;
            m += 1;
        }

        // The raw path is only usable when the requested range is still resident
        // (not evicted); otherwise drop to the finest pyramid level.
        let raw_ok = s0 >= raw_start;

        if m == 0 && raw_ok {
            let lo = s0 - raw_start; // local index of the view start
            view.decimated = span > columns;
            // Optional raw-domain pre-filter over the visible window. The raw
            // path is only chosen for small spans (each column backed by < ~16
            // samples), so filtering `span` samples here is cheap. Min/max is
            // taken from the filtered signal; dI/dt and source stay on the raw
            // signal so the heatmap and tint reflect the true device behaviour.
            let win = smooth.clamp(1, 8192) as usize;
            let (fi, fv, fp) = if filter_type != 0 && win > 1 {
                let end = (lo + span).min(self.i.len());
                (
                    Some(apply_filter(&self.i[lo..end], win, filter_type)),
                    Some(apply_filter(&self.v[lo..end], win, filter_type)),
                    Some(apply_filter(&self.p[lo..end], win, filter_type)),
                )
            } else {
                (None, None, None)
            };
            for col in 0..columns {
                let b0 = (col * span) / columns;
                let mut b1 = ((col + 1) * span) / columns;
                if b1 <= b0 {
                    b1 = b0 + 1;
                }
                b1 = b1.min(span);
                let mut imin = f32::INFINITY;
                let mut imax = f32::NEG_INFINITY;
                let mut vmin = f32::INFINITY;
                let mut vmax = f32::NEG_INFINITY;
                let mut pmin = f32::INFINITY;
                let mut pmax = f32::NEG_INFINITY;
                let mut peak_didt = 0.0_f32;
                let mut src_counts = [0u32; 3];
                let mut any_finite = false;
                let mut prev_i = if lo + b0 > 0 {
                    self.i[lo + b0 - 1]
                } else {
                    self.i[lo + b0]
                };
                for k in b0..b1 {
                    let idx = lo + k;
                    let raw_i = self.i[idx];
                    if raw_i.is_finite() {
                        any_finite = true;
                    }
                    let iv = fi.as_ref().map_or(raw_i, |a| a[k]);
                    if iv.is_finite() {
                        imin = imin.min(iv);
                        imax = imax.max(iv);
                    }
                    let vv = fv.as_ref().map_or(self.v[idx], |a| a[k]);
                    if vv.is_finite() {
                        vmin = vmin.min(vv);
                        vmax = vmax.max(vv);
                    }
                    let pv = fp.as_ref().map_or(self.p[idx], |a| a[k]);
                    if pv.is_finite() {
                        pmin = pmin.min(pv);
                        pmax = pmax.max(pv);
                    }
                    // dI/dt is computed on the raw signal (the heatmap should
                    // reflect true slew, not the smoothed trace). NaN samples
                    // (index gaps) never contribute a slew value.
                    if raw_i.is_finite() && prev_i.is_finite() {
                        let d = ((raw_i - prev_i) / dt).abs();
                        if d > peak_didt {
                            peak_didt = d;
                        }
                    }
                    prev_i = raw_i;
                    if raw_i.is_finite() {
                        match self.source[idx] {
                            SRC_FINE => src_counts[0] += 1,
                            SRC_COARSE => src_counts[1] += 1,
                            SRC_BLEND => src_counts[2] += 1,
                            _ => {}
                        }
                    }
                }
                let dom = {
                    let mut best = 0usize;
                    for k in 1..3 {
                        if src_counts[k] > src_counts[best] {
                            best = k;
                        }
                    }
                    best as u8
                };
                view.i_min.push(if imin.is_finite() { imin } else { 0.0 });
                view.i_max.push(if imax.is_finite() { imax } else { 0.0 });
                view.v_min.push(if vmin.is_finite() { vmin } else { 0.0 });
                view.v_max.push(if vmax.is_finite() { vmax } else { 0.0 });
                view.p_min.push(if pmin.is_finite() { pmin } else { 0.0 });
                view.p_max.push(if pmax.is_finite() { pmax } else { 0.0 });
                view.source.push(dom);
                view.didt.push(peak_didt);
                view.gap.push(if any_finite { 0 } else { 1 });
            }
        } else {
            // Pyramid path — O(columns) bin decimation, scales to any capture.
            // Evicted raw regions also land here at the finest pyramid level.
            if m == 0 {
                m = 1;
                f_pow = PYR_FACTOR as u64;
            }
            let lvl = &self.levels[m - 1];
            let fp = f_pow as usize;
            let e0 = s0 / fp;
            let e1 = ((s1 + fp - 1) / fp).min(lvl.len());
            let ecount = e1.saturating_sub(e0);
            if ecount == 0 {
                return view;
            }
            let cols = ecount.min(max_points);
            view.decimated = true;
            for col in 0..cols {
                let b0 = e0 + (col * ecount) / cols;
                let mut b1 = e0 + ((col + 1) * ecount) / cols;
                if b1 <= b0 {
                    b1 = b0 + 1;
                }
                b1 = b1.min(e1);
                let mut bin = Bin::empty();
                for e in b0..b1 {
                    bin.merge(&lvl[e]);
                }
                let fi = bin.i_min.is_finite();
                view.i_min.push(if fi { bin.i_min } else { 0.0 });
                view.i_max.push(if fi { bin.i_max } else { 0.0 });
                let fv = bin.v_min.is_finite();
                view.v_min.push(if fv { bin.v_min } else { 0.0 });
                view.v_max.push(if fv { bin.v_max } else { 0.0 });
                let fpp = bin.p_min.is_finite();
                view.p_min.push(if fpp { bin.p_min } else { 0.0 });
                view.p_max.push(if fpp { bin.p_max } else { 0.0 });
                view.source.push(bin.source);
                view.didt.push(bin.didt);
                view.gap.push(if fi { 0 } else { 1 });
            }
        }

        // Display filtering: when a filter is requested it is applied in the
        // raw path above (pre-decimation, hi-res). The pyramid path leaves the
        // envelope unfiltered — there the window is finer than one bin, so there
        // is nothing extra to smooth. The lightweight client-side envelope
        // filter (used when the hi-res toggle is off) runs in the frontend.
        view
    }

    /// Exact trapezoidal integrals over [start, end). Only the resident raw
    /// window can be integrated exactly, so the range is clamped to it; a
    /// selection that reaches into evicted history integrates the available
    /// (most recent) portion.
    pub fn integrate(&self, start: u64, end: u64) -> DaqIntegral {
        let total = self.total_samples();
        let raw_start = self.raw_start() as u64;
        let start = start.clamp(raw_start, total);
        let end = end.clamp(start, total);
        let n = (end - start) as usize;
        let mut out = DaqIntegral {
            start,
            end,
            ..Default::default()
        };
        if n == 0 {
            return out;
        }
        let dt = 1.0_f64 / self.sample_rate_hz.max(1) as f64;
        let s0 = (start - raw_start) as usize; // local index
        let s1 = (end - raw_start) as usize;

        let mut sum_i = 0.0_f64;
        let mut sum_v = 0.0_f64;
        let mut sum_p = 0.0_f64;
        let mut charge_c = 0.0_f64; // ∫ I dt
        let mut energy_j = 0.0_f64; // ∫ P dt
        let mut min_i = f64::INFINITY;
        let mut max_i = f64::NEG_INFINITY;
        let mut finite_n: u64 = 0;

        // NaN samples come from index-gap fills (see append_wave_i). They
        // must not poison the integral: skip them entirely for averages and
        // for charge/energy contribution (treated as zero span), while
        // `duration_s` below stays the true wall-clock span of the
        // selection (gaps still took real time on the device).
        for idx in s0..s1 {
            let iv = self.i[idx] as f64;
            if !iv.is_finite() {
                continue;
            }
            let pv = self.p[idx] as f64;
            let vv = self.v[idx] as f64;
            finite_n += 1;
            sum_i += iv;
            sum_v += if vv.is_finite() { vv } else { 0.0 };
            sum_p += if pv.is_finite() { pv } else { 0.0 };
            min_i = min_i.min(iv);
            max_i = max_i.max(iv);
            // Trapezoid with the next sample where available and finite,
            // else rectangle.
            if idx + 1 < s1 && self.i[idx + 1].is_finite() {
                let i_next = self.i[idx + 1] as f64;
                let p_next = self.p[idx + 1] as f64;
                charge_c += 0.5 * (iv + i_next) * dt;
                energy_j += 0.5 * (pv + p_next) * dt;
            } else {
                charge_c += iv * dt;
                energy_j += pv * dt;
            }
        }
        let nf = n as f64;
        out.duration_s = nf * dt;
        let fc = finite_n.max(1) as f64;
        out.avg_i = sum_i / fc;
        out.avg_v = sum_v / fc;
        out.avg_p = sum_p / fc;
        out.min_i = if min_i.is_finite() { min_i } else { 0.0 };
        out.max_i = if max_i.is_finite() { max_i } else { 0.0 };
        out.charge_c = charge_c;
        out.charge_mah = charge_c / 3.6; // C → mAh (1 mAh = 3.6 C)
        out.energy_j = energy_j;
        out.energy_mwh = energy_j / 3.6; // J → mWh (1 mWh = 3.6 J)
        out.projected_mwh_per_hour = out.avg_p * 1000.0; // avg_P (W) × 1 h → mWh
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::daq_proto::WaveVRecord;

    /// Build a WAVE_I record and append it (as in the brief's Design block).
    fn push_wave_i(store: &mut DaqStore, start: u64, i_vals: &[f32], rate: u32, ts_us: u64) {
        store.append_wave_i(&WaveIRecord {
            start_index: start,
            timestamp_us: ts_us,
            sample_rate: rate,
            decimation: 1,
            i: i_vals.to_vec(),
            meta: vec![0; i_vals.len()],
        });
    }

    /// Append `n` contiguous constant-current samples (with a matching
    /// constant-voltage hold established up front) at the store's current
    /// rate/index, exactly as the legacy `append_waveform` tests exercised.
    fn push_constant(store: &mut DaqStore, i: f32, v: f32, n: usize) {
        if store.total_samples() == 0 {
            store.append_wave_v(&WaveVRecord {
                start_index: 0,
                timestamp_us: 0,
                sample_rate: 100,
                v: vec![v],
            });
        }
        let rate = store.sample_rate_hz;
        let start = store.total_samples();
        let ts = (start as f64 * 1e6 / rate.max(1) as f64) as u64;
        push_wave_i(store, start, &vec![i; n], rate, ts);
    }

    #[test]
    fn integrate_constant_load() {
        let mut store = DaqStore::new(1000);
        push_constant(&mut store, 1.0, 1.0, 1000);
        let r = store.integrate(0, 1000);
        assert!((r.duration_s - 1.0).abs() < 1e-6);
        assert!((r.avg_i - 1.0).abs() < 1e-6);
        assert!((r.avg_p - 1.0).abs() < 1e-6);
        // 1 W for 1 s = 1 J ≈ 0.2778 mWh.
        assert!((r.energy_j - 1.0).abs() < 1e-2);
        assert!((r.energy_mwh - (1.0 / 3.6)).abs() < 1e-3);
        // 1 W sustained 1 h = 1000 mWh.
        assert!((r.projected_mwh_per_hour - 1000.0).abs() < 1e-6);
    }

    #[test]
    fn view_decimates_to_columns() {
        let mut store = DaqStore::new(1000);
        push_constant(&mut store, 0.5, 3.3, 10_000);
        let view = store.get_view(0, 10_000, 200, 1, 0);
        assert!(view.decimated);
        assert_eq!(view.i_min.len(), 200);
        assert!((view.i_min[0] - 0.5).abs() < 1e-6);
        assert_eq!(view.didt.len(), 200);
    }

    #[test]
    fn pyramid_view_brackets_extremes() {
        // 500k-sample sawtooth (period 1000), appended in chunks to exercise
        // the incremental pyramid; a coarse full-capture view must still
        // bracket the [0,1) range via the min/max envelope.
        let mut store = DaqStore::new(1_000_000);
        store.append_wave_v(&WaveVRecord {
            start_index: 0,
            timestamp_us: 0,
            sample_rate: 100,
            v: vec![3.3],
        });
        let n = 500_000usize;
        let all: Vec<f32> = (0..n).map(|k| (k % 1000) as f32 / 1000.0).collect();
        let mut start = 0u64;
        for chunk in all.chunks(40_000) {
            let ts = (start as f64 * 1e6 / 1_000_000.0) as u64;
            push_wave_i(&mut store, start, chunk, 1_000_000, ts);
            start += chunk.len() as u64;
        }
        assert_eq!(store.total_samples(), n as u64);

        // Full-capture coarse view forces the pyramid path.
        let view = store.get_view(0, n as u64, 500, 1, 0);
        assert!(view.decimated);
        assert!(!view.i_min.is_empty());
        let gmax = view.i_max.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
        let gmin = view.i_min.iter().cloned().fold(f32::INFINITY, f32::min);
        assert!(gmax > 0.95, "envelope max too low: {gmax}");
        assert!(gmin < 0.05, "envelope min too high: {gmin}");

        // A tight zoom into a single sawtooth period uses the raw path and
        // returns full per-sample detail.
        let zoom = store.get_view(1000, 2000, 800, 1, 0);
        assert!(zoom.i_max.iter().cloned().fold(0.0, f32::max) > 0.95);
    }

    #[test]
    fn compaction_evicts_raw_keeps_overview() {
        let mut store = DaqStore::new(1_000_000);
        store.raw_cap = 50_000; // force a small raw window to trigger eviction
        store.append_wave_v(&WaveVRecord {
            start_index: 0,
            timestamp_us: 0,
            sample_rate: 100,
            v: vec![3.3],
        });
        let n = 400_000usize;
        let mut k = 0usize;
        while k < n {
            let end = (k + 20_000).min(n);
            let vals: Vec<f32> = (k..end).map(|j| (j % 1000) as f32 / 1000.0).collect();
            let ts = (k as f64 * 1e6 / 1_000_000.0) as u64;
            push_wave_i(&mut store, k as u64, &vals, 1_000_000, ts);
            k = end;
        }
        // Full history is still counted, but the raw window is bounded.
        assert_eq!(store.total_samples(), n as u64);
        assert!(store.i.len() < n, "raw should have been evicted");
        assert!(store.i.len() <= store.raw_cap + store.raw_cap / 4 + 1);

        // Overview across the whole capture still brackets the range.
        let view = store.get_view(0, n as u64, 500, 1, 0);
        assert!(view.i_max.iter().cloned().fold(f32::NEG_INFINITY, f32::max) > 0.95);

        // Zoom into an EVICTED region (near the start) still returns data from
        // the finest pyramid level.
        let zoom = store.get_view(0, 2_000, 800, 1, 0);
        assert!(!zoom.i_max.is_empty());
        assert!(
            zoom.i_max.iter().cloned().fold(f32::NEG_INFINITY, f32::max) > 0.9,
            "evicted-region zoom should still show the sawtooth"
        );

        // Integral clamps to the resident window without panicking.
        let _ = store.integrate(0, n as u64);
    }

    #[test]
    fn markers_survive_decimation() {
        use crate::daq_proto::{MarkerRecord, MARK_KIND_FLAG, MARK_KIND_TRIGGER};
        let mut store = DaqStore::new(1_000_000);
        push_constant(&mut store, 0.5, 3.3, 100_000);
        // A flag at sample 1000 and a trigger at 50_000.
        store.push_marker(&MarkerRecord {
            sample_index: 1_000,
            timestamp_us: 4_000,
            channel: 2,
            edge: 1,
            kind: MARK_KIND_FLAG,
        });
        store.push_marker(&MarkerRecord {
            sample_index: 50_000,
            timestamp_us: 200_000,
            channel: 6,
            edge: 0,
            kind: MARK_KIND_TRIGGER,
        });

        // Heavily decimated overview (200 columns over 100k samples) must still
        // carry both markers untouched.
        let view = store.get_view(0, 100_000, 200, 1, 0);
        assert!(view.decimated);
        assert_eq!(view.markers.len(), 2);
        assert_eq!(view.markers[0].sample_index, 1_000);
        assert_eq!(view.markers[0].kind, MARK_KIND_FLAG);
        assert_eq!(view.markers[1].sample_index, 50_000);
        assert_eq!(view.markers[1].kind, MARK_KIND_TRIGGER);

        // A narrow window only returns the markers inside it.
        let zoom = store.get_view(0, 2_000, 800, 1, 0);
        assert_eq!(zoom.markers.len(), 1);
        assert_eq!(zoom.markers[0].channel, 2);
    }

    #[test]
    fn voltage_held_onto_current_axis() {
        let mut s = DaqStore::new(1000);
        s.append_wave_v(&WaveVRecord {
            start_index: 0,
            timestamp_us: 0,
            sample_rate: 100,
            v: vec![3.3],
        });
        s.append_wave_v(&WaveVRecord {
            start_index: 1,
            timestamp_us: 500_000,
            sample_rate: 100,
            v: vec![2.0],
        });
        // 1000 current samples over 1 s: first half sees 3.3 V, second half 2.0 V.
        let iv: Vec<f32> = vec![1.0; 1000];
        push_wave_i(&mut s, 0, &iv, 1000, 0);
        assert!((s.v[100] - 3.3).abs() < 1e-6);
        assert!((s.v[600] - 2.0).abs() < 1e-6);
        assert!((s.p[600] - 2.0).abs() < 1e-6); // p = v_held * i
    }

    #[test]
    fn index_gap_becomes_nan_and_gap_column() {
        let mut s = DaqStore::new(1000);
        s.append_wave_v(&WaveVRecord {
            start_index: 0,
            timestamp_us: 0,
            sample_rate: 100,
            v: vec![3.3],
        });
        push_wave_i(&mut s, 0, &vec![1.0; 500], 1000, 0);
        push_wave_i(&mut s, 700, &vec![1.0; 300], 1000, 700_000); // 200-sample gap
        assert_eq!(s.dropped_samples(), 200);
        assert_eq!(s.total_samples(), 1000);
        assert!(s.i[600].is_nan());
        let view = s.get_view(0, 1000, 100, 1, 0);
        assert!(view.gap.iter().any(|&g| g == 1), "gap column expected");
        // Non-gap columns keep real data.
        assert!(view.i_max.iter().cloned().fold(f32::NEG_INFINITY, f32::max) > 0.99);
    }

    #[test]
    fn timebase_fit_recovers_actual_rate() {
        let mut s = DaqStore::new(256_000);
        // Device actually runs at 255,000 Hz: 1000-sample blocks every 3921.57 µs.
        for b in 0..64u64 {
            let ts = (b * 1000) as f64 * 1e6 / 255_000.0;
            push_wave_i(&mut s, b * 1000, &vec![0.0f32; 1000], 256_000, ts as u64);
        }
        let r = s.actual_rate_hz();
        assert!((r - 255_000.0).abs() < 500.0, "fitted {r}");
    }

    #[test]
    fn integrate_skips_gap_samples() {
        let mut s = DaqStore::new(1000);
        s.append_wave_v(&WaveVRecord {
            start_index: 0,
            timestamp_us: 0,
            sample_rate: 100,
            v: vec![1.0],
        });
        push_wave_i(&mut s, 0, &vec![1.0; 400], 1000, 0);
        push_wave_i(&mut s, 600, &vec![1.0; 400], 1000, 600_000);
        let r = s.integrate(0, 1000);
        // 800 finite 1 A samples at 1 kHz ≈ 0.8 C, no NaN poisoning.
        assert!(r.charge_c.is_finite() && (r.charge_c - 0.8).abs() < 0.01);
    }
}
