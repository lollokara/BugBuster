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

/// Sample cap so a long unattended capture can't OOM the host. Past this the
/// store stops appending and sets `overflow`.
pub const DAQ_STORE_MAX_SAMPLES: usize = 16_000_000;

#[derive(Debug, Clone, Default)]
pub struct DaqStore {
    pub sample_rate_hz: u32,
    pub decimation: u8,
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

impl DaqStore {
    pub fn new(sample_rate_hz: u32) -> Self {
        Self {
            sample_rate_hz: sample_rate_hz.max(1),
            decimation: 1,
            ..Default::default()
        }
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
            if self.i.len() >= DAQ_STORE_MAX_SAMPLES {
                if !self.overflow {
                    self.overflow = true;
                    log::warn!(
                        "daq_store: sample cap ({}) reached — capture truncated",
                        DAQ_STORE_MAX_SAMPLES
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
    }

    /// Extract a decimated view between [start, end) sample indices, at most
    /// `max_points` columns wide. Uses min/max envelope decimation.
    pub fn get_view(&self, start: u64, end: u64, max_points: u32) -> DaqViewData {
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

        let columns = span.min(max_points);
        view.decimated = span > columns;
        let dt = 1.0_f32 / self.sample_rate_hz as f32;

        view.i_min.reserve(columns);
        view.i_max.reserve(columns);
        view.v_min.reserve(columns);
        view.v_max.reserve(columns);
        view.p_min.reserve(columns);
        view.p_max.reserve(columns);
        view.source.reserve(columns);
        view.didt.reserve(columns);

        let s0 = start as usize;
        for col in 0..columns {
            let b0 = s0 + (col * span) / columns;
            let mut b1 = s0 + ((col + 1) * span) / columns;
            if b1 <= b0 {
                b1 = b0 + 1;
            }
            b1 = b1.min(self.i.len());

            let mut imin = f32::INFINITY;
            let mut imax = f32::NEG_INFINITY;
            let mut vmin = f32::INFINITY;
            let mut vmax = f32::NEG_INFINITY;
            let mut pmin = f32::INFINITY;
            let mut pmax = f32::NEG_INFINITY;
            let mut peak_didt = 0.0_f32;
            let mut src_counts = [0u32; 3];
            let mut prev_i = if b0 > 0 { self.i[b0 - 1] } else { self.i[b0] };

            for idx in b0..b1 {
                let iv = self.i[idx];
                imin = imin.min(iv);
                imax = imax.max(iv);
                let vv = self.v[idx];
                vmin = vmin.min(vv);
                vmax = vmax.max(vv);
                let pv = self.p[idx];
                pmin = pmin.min(pv);
                pmax = pmax.max(pv);
                let d = ((iv - prev_i) / dt).abs();
                if d > peak_didt {
                    peak_didt = d;
                }
                prev_i = iv;
                match self.source[idx] {
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
        let view = store.get_view(0, 10_000, 200);
        assert!(view.decimated);
        assert_eq!(view.i_min.len(), 200);
        assert!((view.i_min[0] - 0.5).abs() < 1e-6);
        assert_eq!(view.didt.len(), 200);
    }
}
