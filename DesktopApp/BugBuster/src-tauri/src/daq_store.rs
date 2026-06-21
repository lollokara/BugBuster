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
    EnergyRecord, FftRecord, StatsRecord, StatusRecord, WaveformRecord, SRC_BLEND, SRC_COARSE,
    SRC_FINE,
};
use serde::{Deserialize, Serialize};

/// Absolute hard ceiling on stored samples (RAM safety net). The actual cap is
/// chosen adaptively from available memory (see `adaptive_max_samples`). ~400 M
/// samples ≈ 7 GB of raw arrays; only reached on a large machine.
pub const DAQ_STORE_MAX_SAMPLES: usize = 400_000_000;

/// Approximate bytes per stored sample: raw i/v/p (3×f32 = 12) + range/source/
/// flags (3) + amortised pyramid (~3) + slack.
const BYTES_PER_SAMPLE: u64 = 18;

/// Bytes available for the OS/UI to keep free regardless of capture size.
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

/// Choose the capture sample cap from available RAM: use the smaller of ~55 %
/// of available memory or (available − reserved), then clamp. This squeezes as
/// much raw resolution as the host can hold while leaving headroom.
fn adaptive_max_samples() -> usize {
    let avail = available_memory_bytes();
    let half = avail / 100 * 55;
    let after_reserve = avail.saturating_sub(RESERVED_BYTES);
    let budget = half.min(after_reserve).max(256 * 1024 * 1024); // ≥ 256 MB
    let n = (budget / BYTES_PER_SAMPLE) as usize;
    n.clamp(2_000_000, DAQ_STORE_MAX_SAMPLES)
}

#[derive(Debug, Clone, Default)]
pub struct DaqStore {
    pub sample_rate_hz: u32,
    pub decimation: u8,
    /// RAM-adaptive retention cap (samples), chosen from available memory.
    pub max_samples: usize,
    /// Parallel per-sample arrays.
    pub i: Vec<f32>,
    pub v: Vec<f32>,
    pub p: Vec<f32>,
    pub range: Vec<u8>,
    pub source: Vec<u8>,
    pub flags: Vec<u8>,
    pub overflow: bool,
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

/// Centered box (moving-average) low-pass over `x` with the given window in
/// samples. Returns a vector the same length as `x`.
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

/// Windowed median (spike/impulse rejection). Cost-bounded: the window is
/// capped and very large views fall back to the moving average.
fn median_filter(x: &[f32], win: usize) -> Vec<f32> {
    let n = x.len();
    if n == 0 || win <= 1 {
        return x.to_vec();
    }
    if n > 300_000 {
        return moving_avg(x, win);
    }
    let w = win.clamp(1, 63);
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

/// High-pass = signal minus its moving average (shows AC / ripple, removes DC).
fn highpass(x: &[f32], win: usize) -> Vec<f32> {
    let lp = moving_avg(x, win);
    x.iter().zip(lp.iter()).map(|(a, b)| a - b).collect()
}

/// Apply the selected display filter. `kind`: 0=none, 1=moving avg, 2=EMA,
/// 3=median, 4=high-pass.
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

/// Build a bin directly from raw sample arrays over [a, b).
fn build_bin_raw(i: &[f32], v: &[f32], p: &[f32], src: &[u8], a: usize, b: usize, dt: f32) -> Bin {
    let mut bin = Bin::empty();
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
        Self {
            sample_rate_hz: sample_rate_hz.max(1),
            decimation: 1,
            max_samples: adaptive_max_samples(),
            levels: vec![Vec::new(); PYR_MAX_LEVELS],
            built: vec![0; PYR_MAX_LEVELS],
            ..Default::default()
        }
    }

    /// Estimated resident bytes of the stored capture (raw + pyramid).
    pub fn mem_used_bytes(&self) -> u64 {
        let raw = self.i.len() as u64 * 15;
        let pyr: u64 = self
            .levels
            .iter()
            .map(|l| l.len() as u64 * std::mem::size_of::<Bin>() as u64)
            .sum();
        raw + pyr
    }

    pub fn total_samples(&self) -> u64 {
        self.i.len() as u64
    }

    pub fn clear(&mut self) {
        self.i.clear();
        self.v.clear();
        self.p.clear();
        self.range.clear();
        self.source.clear();
        self.flags.clear();
        self.overflow = false;
        for lvl in self.levels.iter_mut() {
            lvl.clear();
        }
        for b in self.built.iter_mut() {
            *b = 0;
        }
    }

    /// Incrementally rebuild the pyramid tails after new samples were appended.
    fn update_pyramid(&mut self) {
        if self.levels.len() != PYR_MAX_LEVELS {
            self.levels = vec![Vec::new(); PYR_MAX_LEVELS];
            self.built = vec![0; PYR_MAX_LEVELS];
        }
        let n = self.i.len();
        let dt = 1.0_f32 / self.sample_rate_hz.max(1) as f32;
        // Level 0 from raw samples.
        {
            let complete = n / PYR_FACTOR;
            let lvl = &mut self.levels[0];
            lvl.truncate(self.built[0]); // drop the old partial tail
            while self.built[0] < complete {
                let a = self.built[0] * PYR_FACTOR;
                lvl.push(build_bin_raw(
                    &self.i, &self.v, &self.p, &self.source, a, a + PYR_FACTOR, dt,
                ));
                self.built[0] += 1;
            }
            if n > complete * PYR_FACTOR {
                lvl.push(build_bin_raw(
                    &self.i,
                    &self.v,
                    &self.p,
                    &self.source,
                    complete * PYR_FACTOR,
                    n,
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

    /// Append a decoded WAVEFORM record to the store.
    pub fn append_waveform(&mut self, rec: &WaveformRecord) {
        if rec.sample_rate > 0 {
            self.sample_rate_hz = rec.sample_rate;
        }
        if rec.decimation > 0 {
            self.decimation = rec.decimation;
        }
        for s in &rec.samples {
            if self.i.len() >= self.max_samples.max(1) {
                if !self.overflow {
                    self.overflow = true;
                    log::warn!(
                        "daq_store: adaptive cap ({}) reached — capture truncated",
                        self.max_samples
                    );
                }
                break;
            }
            self.i.push(s.i);
            self.v.push(s.v);
            self.p.push(s.p);
            self.range.push(s.range);
            self.source.push(s.source);
            self.flags.push(s.flags);
        }
        self.update_pyramid();
    }

    /// Extract a decimated view between [start, end) sample indices, at most
    /// `max_points` columns wide. Uses min/max envelope decimation. When a
    /// `filter_type` (1=avg, 2=EMA, 3=median, 4=high-pass) is selected with a
    /// `smooth` window > 1, that filter is applied to the visible I/V/P before
    /// decimation.
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
            ..Default::default()
        };
        if span == 0 {
            return view;
        }

        let s0 = start as usize;
        let s1 = end as usize;
        let columns = span.min(max_points);
        let raw_per_col = (span / columns).max(1) as u64;
        let dt = 1.0_f32 / self.sample_rate_hz.max(1) as f32;

        // Pick the coarsest pyramid level whose bins are still finer than a
        // column. m == 0 means read raw samples (full detail + filtering).
        let mut m = 0usize;
        let mut f_pow: u64 = 1;
        while m < PYR_MAX_LEVELS
            && f_pow * (PYR_FACTOR as u64) <= raw_per_col
            && !self.levels[m].is_empty()
        {
            f_pow *= PYR_FACTOR as u64;
            m += 1;
        }

        if m == 0 {
            // Raw path — bounded span (< columns * PYR_FACTOR), full detail.
            let win = smooth.clamp(1, 8192) as usize;
            let active = filter_type != 0 && win > 1;
            let sm_i = active.then(|| apply_filter(&self.i[s0..s1], win, filter_type));
            let sm_v = active.then(|| apply_filter(&self.v[s0..s1], win, filter_type));
            let sm_p = active.then(|| apply_filter(&self.p[s0..s1], win, filter_type));
            let gi = |k: usize| sm_i.as_ref().map(|a| a[k]).unwrap_or(self.i[s0 + k]);
            let gv = |k: usize| sm_v.as_ref().map(|a| a[k]).unwrap_or(self.v[s0 + k]);
            let gp = |k: usize| sm_p.as_ref().map(|a| a[k]).unwrap_or(self.p[s0 + k]);
            view.decimated = span > columns;
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
                let mut prev_i = if b0 > 0 { gi(b0 - 1) } else { gi(b0) };
                for k in b0..b1 {
                    let iv = gi(k);
                    imin = imin.min(iv);
                    imax = imax.max(iv);
                    let vv = gv(k);
                    vmin = vmin.min(vv);
                    vmax = vmax.max(vv);
                    let pv = gp(k);
                    pmin = pmin.min(pv);
                    pmax = pmax.max(pv);
                    let d = ((iv - prev_i) / dt).abs();
                    if d > peak_didt {
                        peak_didt = d;
                    }
                    prev_i = iv;
                    match self.source[s0 + k] {
                        SRC_FINE => src_counts[0] += 1,
                        SRC_COARSE => src_counts[1] += 1,
                        SRC_BLEND => src_counts[2] += 1,
                        _ => {}
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
                view.i_min.push(imin);
                view.i_max.push(imax);
                view.v_min.push(vmin);
                view.v_max.push(vmax);
                view.p_min.push(pmin);
                view.p_max.push(pmax);
                view.source.push(dom);
                view.didt.push(peak_didt);
            }
        } else {
            // Pyramid path — O(columns) bin decimation, scales to any capture.
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
            }
        }
        view
    }

    /// Exact trapezoidal integrals over [start, end) at full sample resolution.
    pub fn integrate(&self, start: u64, end: u64) -> DaqIntegral {
        let total = self.total_samples();
        let start = start.min(total);
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
        let dt = 1.0_f64 / self.sample_rate_hz as f64;
        let s0 = start as usize;
        let s1 = end as usize;

        let mut sum_i = 0.0_f64;
        let mut sum_v = 0.0_f64;
        let mut sum_p = 0.0_f64;
        let mut charge_c = 0.0_f64; // ∫ I dt
        let mut energy_j = 0.0_f64; // ∫ P dt
        let mut min_i = f64::INFINITY;
        let mut max_i = f64::NEG_INFINITY;

        for idx in s0..s1 {
            let iv = self.i[idx] as f64;
            let pv = self.p[idx] as f64;
            sum_i += iv;
            sum_v += self.v[idx] as f64;
            sum_p += pv;
            min_i = min_i.min(iv);
            max_i = max_i.max(iv);
            // Trapezoid with the next sample where available, else rectangle.
            if idx + 1 < s1 {
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
        out.avg_i = sum_i / nf;
        out.avg_v = sum_v / nf;
        out.avg_p = sum_p / nf;
        out.min_i = min_i;
        out.max_i = max_i;
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
    use crate::daq_proto::{WaveSample, WaveformRecord};

    fn push_constant(store: &mut DaqStore, i: f32, v: f32, n: usize) {
        let samples: Vec<WaveSample> = (0..n)
            .map(|_| WaveSample {
                i,
                v,
                p: i * v,
                range: 1,
                source: SRC_FINE,
                flags: 0,
            })
            .collect();
        store.append_waveform(&WaveformRecord {
            start_seq: 0,
            sample_rate: store.sample_rate_hz,
            decimation: 1,
            samples,
        });
    }

    #[test]
    fn integrate_constant_load() {
        // 1 A, 1 V, 1 W for 1 s at 1 kSPS.
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
        let n = 500_000usize;
        let all: Vec<WaveSample> = (0..n)
            .map(|k| {
                let ph = (k % 1000) as f32 / 1000.0;
                WaveSample {
                    i: ph,
                    v: 3.3,
                    p: ph * 3.3,
                    range: 1,
                    source: SRC_FINE,
                    flags: 0,
                }
            })
            .collect();
        for chunk in all.chunks(40_000) {
            store.append_waveform(&WaveformRecord {
                start_seq: 0,
                sample_rate: 1_000_000,
                decimation: 1,
                samples: chunk.to_vec(),
            });
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
}
