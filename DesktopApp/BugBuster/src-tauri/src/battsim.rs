// =============================================================================
// battsim.rs - DAQ HAT battery simulator: status, run list, incremental history
// sync into the app data dir, decimated views and window statistics.
//
// Wire format mirrors python/bugbuster/battsim.py (the reference decoder) and
// Firmware/DAQ_HAT/ESP32P4/src/battsim/battsim_store.h. History record v1 (32 B)
// and v2 (48 B, cumulative charge + energy) are both decoded from meta.version.
// Transport: BBP DAQ_CONFIG sub-op 0x0B over USB, /api/daq/bs* over HTTP.
// =============================================================================

use crate::bbp;
use crate::connection_manager::ConnectionManager;
use crate::daq_commands::{http_json, vdut_http};
use base64::Engine;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use tauri::{AppHandle, Emitter, Manager, State};

type CmdResult<T> = Result<T, String>;
fn map_err(e: impl std::fmt::Display) -> String {
    e.to_string()
}

const CFG_BATTSIM: u8 = 0x0B;
const CFG_SET: u8 = 0x01;
const CFG_ACTION: u8 = 0x04;
const OP_STATUS: u8 = 0;
const OP_LIST_RUNS: u8 = 1;
const OP_RUN_DIR: u8 = 2;
const OP_READ: u8 = 3;
const OP_PROFILE: u8 = 4;
const OP_SET_EPOCH: u8 = 5;
const CHUNK_USB: usize = 236;
const CHUNK_HTTP: usize = 2048;
const FILE_META: u16 = 0;
const FILE_CKPT: u16 = 1;
const FILE_S1: u16 = 4;
const M1_BASE: u16 = 0x100;
const STATUS_SIZE: usize = 88;
const META_SIZE: usize = 68;
const CKPT_SIZE: usize = 352;
const RF_GAP: u16 = 0x0001;
const RF_I_CLAMP: u16 = 0x0002;
const FLAG_REMAIN_OK: u8 = 0x40;

// --------------------------------------------------------------------------
// Little-endian readers
// --------------------------------------------------------------------------
fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn i32le(b: &[u8], o: usize) -> i32 {
    u32le(b, o) as i32
}
fn i64le(b: &[u8], o: usize) -> i64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[o..o + 8]);
    i64::from_le_bytes(a)
}
fn f32le(b: &[u8], o: usize) -> f32 {
    f32::from_bits(u32le(b, o))
}

// --------------------------------------------------------------------------
// Decoded structures
// --------------------------------------------------------------------------
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsParams {
    pub chem: u8,
    pub cells: u8,
    pub sd_enable: bool,
    pub ext_enable: bool,
    pub dither: bool,
    pub capacity_mah: u32,
    pub start_soc_pct: f64,
    pub cutoff_mv_cell: u16,
    pub rint_uohm_cell: u32,
    pub peukert: f64,
    pub sd_pct_month: f64,
    pub ext_load_ua: u32,
}

fn parse_params(b: &[u8]) -> BsParams {
    BsParams {
        chem: b[0],
        cells: b[1],
        sd_enable: b[2] != 0,
        ext_enable: b[3] != 0,
        dither: b[4] != 0,
        capacity_mah: u32le(b, 8),
        start_soc_pct: u16le(b, 12) as f64 / 10.0,
        cutoff_mv_cell: u16le(b, 14),
        rint_uohm_cell: u32le(b, 16),
        peukert: u16le(b, 20) as f64 / 1000.0,
        sd_pct_month: u16le(b, 22) as f64 / 100.0,
        ext_load_ua: u32le(b, 24),
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsStatus {
    pub state: u8,
    pub flags: u8,
    pub last_error: u8,
    pub run_id: u16,
    pub chem: u8,
    pub cells: u8,
    pub capacity_mah: u32,
    pub soc_pct: f64,
    pub profile_mask: u16,
    pub elapsed_s: u32,
    pub remaining_s: Option<u32>,
    pub v_target: f32,
    pub v_meas: f32,
    pub i_meas: f32,
    pub i_avg: f32,
    pub q_used_c: f64,
    pub q_dut_c: f64,
    pub q_ext_c: f64,
    pub q_sd_c: f64,
    pub q_peuk_c: f64,
    pub fs_total: u32,
    pub fs_used: u32,
    /// Status v2: integrated DUT energy (J).
    pub e_dut_j: Option<f64>,
}

fn parse_status(b: &[u8]) -> CmdResult<BsStatus> {
    if b.len() < STATUS_SIZE {
        return Err(format!("short battsim status: {} < {STATUS_SIZE}", b.len()));
    }
    let flags = b[2];
    Ok(BsStatus {
        state: b[1],
        flags,
        last_error: b[3],
        run_id: u16le(b, 4),
        chem: b[6],
        cells: b[7],
        capacity_mah: u32le(b, 8),
        soc_pct: u16le(b, 12) as f64 / 100.0,
        profile_mask: u16le(b, 14),
        elapsed_s: u32le(b, 16),
        remaining_s: (flags & FLAG_REMAIN_OK != 0).then(|| u32le(b, 20)),
        v_target: f32le(b, 24),
        v_meas: f32le(b, 28),
        i_meas: f32le(b, 32),
        i_avg: f32le(b, 36),
        q_used_c: i64le(b, 40) as f64 * 1e-9,
        q_dut_c: i64le(b, 48) as f64 * 1e-9,
        q_ext_c: i64le(b, 56) as f64 * 1e-9,
        q_sd_c: i64le(b, 64) as f64 * 1e-9,
        q_peuk_c: i64le(b, 72) as f64 * 1e-9,
        fs_total: u32le(b, 80),
        fs_used: u32le(b, 84),
        e_dut_j: (b.len() >= 96).then(|| i64le(b, 88) as f64 * 1e-6),
    })
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsMeta {
    pub run_id: u16,
    pub version: u16,
    pub created_epoch: u32,
    pub name: String,
    pub params: BsParams,
}

fn parse_meta(b: &[u8]) -> CmdResult<BsMeta> {
    if b.len() < META_SIZE || u32le(b, 0) != 0x4E52_5342 {
        return Err("bad run meta".into());
    }
    let name_raw = &b[12..36];
    let end = name_raw.iter().position(|&c| c == 0).unwrap_or(name_raw.len());
    Ok(BsMeta {
        version: u16le(b, 4),
        run_id: u16le(b, 6),
        created_epoch: u32le(b, 8),
        name: String::from_utf8_lossy(&name_raw[..end]).into_owned(),
        params: parse_params(&b[36..64]),
    })
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsCkpt {
    pub state: u8,
    pub seq: u32,
    pub elapsed_s: f64,
    pub q_dut_c: f64,
    pub q_ext_c: f64,
    pub q_sd_c: f64,
    pub q_peuk_c: f64,
    pub q_init_c: f64,
    pub q15_interval_s: u32,
    pub q15_count: u32,
}

fn parse_ckpt(b: &[u8]) -> Option<BsCkpt> {
    if b.len() < CKPT_SIZE || u32le(b, 0) != 0x4B43_5342 {
        return None;
    }
    Some(BsCkpt {
        state: b[6],
        seq: u32le(b, 8),
        q_dut_c: i64le(b, 16) as f64 * 1e-12,
        elapsed_s: i64le(b, 24) as f64 / 16_384_000.0,
        q_ext_c: i64le(b, 32) as f64 * 1e-12,
        q_sd_c: i64le(b, 40) as f64 * 1e-12,
        q_peuk_c: i64le(b, 48) as f64 * 1e-12,
        q_init_c: i64le(b, 56) as f64 * 1e-12,
        q15_interval_s: u32le(b, 336),
        q15_count: u32le(b, 340),
    })
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsEvent {
    pub t_s: u32,
    pub code: u16,
    pub a: i32,
    pub b: i32,
}

fn parse_events(b: &[u8]) -> Vec<BsEvent> {
    b.chunks_exact(16)
        .map(|c| BsEvent { t_s: u32le(c, 0), code: u16le(c, 4), a: i32le(c, 8), b: i32le(c, 12) })
        .collect()
}

/// One history point; cumulative fields are absolute since run start.
#[derive(Debug, Clone, Default)]
struct Rec {
    t: f64,
    dt: f64,
    v_avg: f64,
    v_min: f64,
    v_max: f64,
    soc: f64,
    i_avg: f64,
    i_min: f64,
    i_max: f64,
    flags: u16,
    q_dut: Option<f64>,
    q_used: f64,
    e_dut: Option<f64>,
    tier: u8, // 0 q15, 1 m1, 2 s1
}

fn parse_records(b: &[u8], version: u16, nominal_dt: f64, tier: u8) -> CmdResult<Vec<Rec>> {
    let mut out = Vec::new();
    let mut prev_t: Option<f64> = None;
    match version {
        1 => {
            for c in b.chunks_exact(32) {
                let t = u32le(c, 0) as f64;
                let dt = match prev_t {
                    Some(p) if t > p => t - p,
                    _ => t.min(nominal_dt),
                };
                let ia = i32le(c, 12) as f64 * 1e-9;
                out.push(Rec {
                    t,
                    dt,
                    v_avg: u16le(c, 4) as f64 / 1e3,
                    v_min: u16le(c, 6) as f64 / 1e3,
                    v_max: u16le(c, 8) as f64 / 1e3,
                    soc: u16le(c, 10) as f64 / 100.0,
                    i_avg: ia,
                    i_min: i32le(c, 16) as f64 * 1e-9,
                    i_max: i32le(c, 20) as f64 * 1e-9,
                    flags: 0,
                    q_dut: None,
                    q_used: i64le(c, 24) as f64 * 1e-9,
                    e_dut: None,
                    tier,
                });
                prev_t = Some(t);
            }
        }
        2 => {
            for c in b.chunks_exact(48) {
                out.push(Rec {
                    t: u32le(c, 0) as f64,
                    v_avg: u16le(c, 4) as f64 / 1e3,
                    v_min: u16le(c, 6) as f64 / 1e3,
                    v_max: u16le(c, 8) as f64 / 1e3,
                    soc: u16le(c, 10) as f64 / 100.0,
                    i_min: i32le(c, 12) as f64 * 1e-9,
                    i_max: i32le(c, 16) as f64 * 1e-9,
                    flags: u16le(c, 20),
                    dt: u16le(c, 22) as f64,
                    q_dut: Some(i64le(c, 24) as f64 * 1e-9),
                    q_used: i64le(c, 32) as f64 * 1e-9,
                    e_dut: Some(i64le(c, 40) as f64 * 1e-6),
                    tier,
                    ..Default::default()
                });
            }
        }
        v => return Err(format!("unsupported battsim history version {v}")),
    }
    Ok(out)
}

/// v2: exact interval current from consecutive cumulative DUT charge.
fn derive_current(recs: &mut [Rec]) {
    let mut prev: Option<(f64, f64)> = None; // (t_end, q_dut)
    for r in recs.iter_mut() {
        let Some(q) = r.q_dut else { continue };
        if r.dt > 0.0 {
            let start = r.t - r.dt;
            r.i_avg = match prev {
                Some((pt, pq)) if (pt - start).abs() <= 1.0 => (q - pq) / r.dt,
                _ if start <= 0.0 => q / r.dt,
                _ => (r.i_min + r.i_max) / 2.0,
            };
        }
        prev = Some((r.t, q));
    }
}

fn merge_tiers(q15: Vec<Rec>, m1: Vec<Rec>, s1: Vec<Rec>) -> Vec<Rec> {
    let m1_start = m1.first().map(|r| r.t - r.dt).unwrap_or(f64::INFINITY);
    let s1_start = s1.first().map(|r| r.t - r.dt).unwrap_or(f64::INFINITY);
    let mut out: Vec<Rec> = q15.into_iter().filter(|r| r.t <= m1_start.min(s1_start)).collect();
    out.extend(m1.into_iter().filter(|r| r.t <= s1_start));
    out.extend(s1);
    out
}

struct History {
    meta: BsMeta,
    ckpt: Option<BsCkpt>,
    events: Vec<BsEvent>,
    recs: Vec<Rec>,
    dir: PathBuf,
}

fn load_history(dir: &Path) -> CmdResult<History> {
    let read = |n: &str| std::fs::read(dir.join(n)).unwrap_or_default();
    let meta = parse_meta(&read("meta.bin"))?;
    let ckpt = parse_ckpt(&read("ckpt.bin"));
    let q15_dt = ckpt.as_ref().map(|c| c.q15_interval_s as f64).filter(|v| *v > 0.0).unwrap_or(900.0);
    let q15 = parse_records(&read("q15.bin"), meta.version, q15_dt, 0)?;
    let mut days: Vec<String> = std::fs::read_dir(dir)
        .map_err(map_err)?
        .filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
        .filter(|n| n.len() == 9 && n.starts_with('m') && n.ends_with(".bin") && n[1..5].chars().all(|c| c.is_ascii_digit()))
        .collect();
    days.sort();
    let mut m1 = Vec::new();
    for d in days {
        m1.extend(parse_records(&read(&d), meta.version, 60.0, 1)?);
    }
    let s1 = parse_records(&read("s1.bin"), meta.version, 1.0, 2)?;
    let mut recs = merge_tiers(q15, m1, s1);
    if meta.version >= 2 {
        derive_current(&mut recs);
    }
    Ok(History { meta, ckpt, events: parse_events(&read("ev.bin")), recs, dir: dir.to_path_buf() })
}

// --------------------------------------------------------------------------
// Statistics + decimated view
// --------------------------------------------------------------------------
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsStats {
    pub points: usize,
    pub t_start_s: f64,
    pub t_end_s: f64,
    pub duration_s: f64,
    pub v_min: f64,
    pub v_max: f64,
    pub v_avg: f64,
    pub i_min: f64,
    pub i_max: f64,
    pub i_avg: f64,
    pub i_p10: f64,
    pub i_median: f64,
    pub i_p99: f64,
    pub p_avg: f64,
    pub p_max: f64,
    pub charge_c: f64,
    pub energy_j: f64,
    pub energy_estimated: bool,
    pub soc_start: f64,
    pub soc_end: f64,
    pub soc_rate_pct_per_day: f64,
    pub projected_life_s: f64,
    pub remaining_at_avg_s: f64,
    pub gaps: usize,
    pub clamped: usize,
}

fn pct(sorted: &[f64], p: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    let k = (sorted.len() - 1) as f64 * p;
    let (lo, hi) = (k.floor() as usize, k.ceil() as usize);
    sorted[lo] + (sorted[hi] - sorted[lo]) * (k - lo as f64)
}

fn finite(x: f64) -> f64 {
    if x.is_finite() { x } else { 0.0 }
}

fn window_range(h: &History, t0: f64, t1: f64) -> (usize, usize) {
    let lo = h.recs.partition_point(|r| r.t <= t0);
    let hi = h.recs.partition_point(|r| r.t - r.dt < t1);
    (lo, hi.max(lo))
}

fn stats(h: &History, t0: f64, t1: f64) -> BsStats {
    let (lo, hi) = window_range(h, t0, t1);
    let rs = &h.recs[lo..hi];
    if rs.is_empty() {
        return BsStats::default();
    }
    let tw: f64 = rs.iter().map(|r| r.dt).sum::<f64>().max(1e-9);
    let mut is: Vec<f64> = rs.iter().map(|r| r.i_avg).collect();
    is.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let charge: f64 = rs.iter().map(|r| r.i_avg * r.dt).sum();
    let exact = h.meta.version >= 2;
    let energy = if exact {
        let e1 = rs.last().and_then(|r| r.e_dut).unwrap_or(0.0);
        let e0 = if lo > 0 { h.recs[lo - 1].e_dut.unwrap_or(0.0) } else { 0.0 };
        e1 - e0
    } else {
        rs.iter().map(|r| r.v_avg * r.i_avg * r.dt).sum()
    };
    let first = &rs[0];
    let last = &rs[rs.len() - 1];
    let span = last.t - (first.t - first.dt);
    let i_mean = charge / tw;
    let cap_c = h.meta.params.capacity_mah as f64 * 3.6;
    let fold = |f: fn(&Rec) -> f64, init: f64, op: fn(f64, f64) -> f64| rs.iter().map(f).fold(init, op);
    BsStats {
        points: rs.len(),
        t_start_s: first.t - first.dt,
        t_end_s: last.t,
        duration_s: span,
        v_min: fold(|r| r.v_min, f64::INFINITY, f64::min),
        v_max: fold(|r| r.v_max, f64::NEG_INFINITY, f64::max),
        v_avg: rs.iter().map(|r| r.v_avg * r.dt).sum::<f64>() / tw,
        i_min: fold(|r| r.i_min, f64::INFINITY, f64::min),
        i_max: fold(|r| r.i_max, f64::NEG_INFINITY, f64::max),
        i_avg: i_mean,
        i_p10: pct(&is, 0.10),
        i_median: pct(&is, 0.50),
        i_p99: pct(&is, 0.99),
        p_avg: energy / tw,
        p_max: fold(|r| r.v_max * r.i_max, f64::NEG_INFINITY, f64::max),
        charge_c: charge,
        energy_j: energy,
        energy_estimated: !exact,
        soc_start: first.soc,
        soc_end: last.soc,
        soc_rate_pct_per_day: finite((first.soc - last.soc) / span * 86400.0),
        projected_life_s: finite(cap_c / i_mean),
        remaining_at_avg_s: finite(last.soc / 100.0 * cap_c / i_mean),
        gaps: rs.iter().filter(|r| r.flags & RF_GAP != 0).count(),
        clamped: rs.iter().filter(|r| r.flags & RF_I_CLAMP != 0).count(),
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsView {
    pub t: Vec<f64>,
    /// Interval covered by each point (s): record dt, or the bucket width.
    pub dt: Vec<f64>,
    pub v_min: Vec<f64>,
    pub v_avg: Vec<f64>,
    pub v_max: Vec<f64>,
    pub i_min: Vec<f64>,
    pub i_avg: Vec<f64>,
    pub i_max: Vec<f64>,
    pub p_avg: Vec<f64>,
    pub soc: Vec<f64>,
    /// Finest source tier in each bucket: 0 = 15 min, 1 = 1 min, 2 = 1 s.
    pub tier: Vec<u8>,
    pub events: Vec<BsEvent>,
    pub stats: BsStats,
}

fn view(h: &History, t0: f64, t1: f64, buckets: usize) -> BsView {
    let (lo, hi) = window_range(h, t0, t1);
    let rs = &h.recs[lo..hi];
    let mut v = BsView {
        events: h.events.iter().filter(|e| (e.t_s as f64) >= t0 && (e.t_s as f64) <= t1).cloned().collect(),
        stats: stats(h, t0, t1),
        ..Default::default()
    };
    let mut push = |t: f64, r: &Rec, p: f64, dt: f64| {
        v.t.push(t);
        v.dt.push(dt);
        v.v_min.push(r.v_min);
        v.v_avg.push(r.v_avg);
        v.v_max.push(r.v_max);
        v.i_min.push(r.i_min);
        v.i_avg.push(r.i_avg);
        v.i_max.push(r.i_max);
        v.p_avg.push(p);
        v.soc.push(r.soc);
        v.tier.push(r.tier);
    };
    let buckets = buckets.clamp(16, 8000);
    if rs.len() <= buckets {
        for r in rs {
            push(r.t - r.dt / 2.0, r, r.v_avg * r.i_avg, r.dt);
        }
        return v;
    }
    // Min/max envelope + dt-weighted mean per bucket; empty buckets are skipped.
    let w = (t1 - t0) / buckets as f64;
    let mut i = 0;
    for b in 0..buckets {
        let be = t0 + w * (b + 1) as f64;
        let mut acc = Rec { v_min: f64::INFINITY, v_max: f64::NEG_INFINITY, i_min: f64::INFINITY, i_max: f64::NEG_INFINITY, ..Default::default() };
        let (mut tw, mut ew, mut n) = (0.0, 0.0, 0usize);
        let (mut lo, mut hi) = (f64::INFINITY, f64::NEG_INFINITY);
        while i < rs.len() && rs[i].t - rs[i].dt / 2.0 < be {
            let r = &rs[i];
            lo = lo.min(r.t - r.dt);
            hi = hi.max(r.t);
            acc.v_min = acc.v_min.min(r.v_min);
            acc.v_max = acc.v_max.max(r.v_max);
            acc.i_min = acc.i_min.min(r.i_min);
            acc.i_max = acc.i_max.max(r.i_max);
            acc.v_avg += r.v_avg * r.dt;
            acc.i_avg += r.i_avg * r.dt;
            ew += r.v_avg * r.i_avg * r.dt;
            acc.soc = r.soc;
            acc.tier = acc.tier.max(r.tier);
            tw += r.dt;
            n += 1;
            i += 1;
        }
        if n == 0 || tw <= 0.0 {
            continue;
        }
        acc.v_avg /= tw;
        acc.i_avg /= tw;
        // Span the records actually covered, so sparse data stays continuous instead of w-wide dots.
        push((lo + hi) / 2.0, &acc, ew / tw, hi - lo);
    }
    v
}

// --------------------------------------------------------------------------
// Transport
// --------------------------------------------------------------------------
fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn b64_field(j: &serde_json::Value) -> CmdResult<Vec<u8>> {
    let s = j.get("data").and_then(|v| v.as_str()).unwrap_or("");
    base64::engine::general_purpose::STANDARD.decode(s).map_err(map_err)
}

async fn bs_req(mgr: &ConnectionManager, op: u8, args: &[u8]) -> CmdResult<Vec<u8>> {
    if let Some((url, client)) = vdut_http(mgr).await {
        let body = serde_json::json!({ "op": op, "args": hex(args) });
        let j = http_json(client.post(format!("{url}/api/daq/bs")).json(&body), "/api/daq/bs").await?;
        return b64_field(&j);
    }
    let mut p = vec![CFG_BATTSIM, op];
    p.extend_from_slice(args);
    mgr.send_command(bbp::CMD_DAQ_CONFIG, &p).await.map_err(map_err)
}

/// Registry passthrough (sub-op 0..4) over BBP or /api/daq/config.
async fn cfg_raw(mgr: &ConnectionManager, op: u8, args: &[u8]) -> CmdResult<Vec<u8>> {
    if let Some((url, client)) = vdut_http(mgr).await {
        let body = serde_json::json!({ "op": op, "args": hex(args) });
        let j = http_json(client.post(format!("{url}/api/daq/config")).json(&body), "/api/daq/config").await?;
        return b64_field(&j);
    }
    let mut p = vec![op];
    p.extend_from_slice(args);
    mgr.send_command(bbp::CMD_DAQ_CONFIG, &p).await.map_err(map_err)
}

async fn read_chunk(mgr: &ConnectionManager, run: u16, file: u16, off: u32, n: usize) -> CmdResult<Vec<u8>> {
    if let Some((url, client)) = vdut_http(mgr).await {
        let body = serde_json::json!({ "run": run, "file": file, "off": off, "len": n.min(CHUNK_HTTP) });
        let j = http_json(client.post(format!("{url}/api/daq/bs/read")).json(&body), "/api/daq/bs/read").await?;
        return b64_field(&j);
    }
    let mut a = Vec::with_capacity(9);
    a.extend_from_slice(&run.to_le_bytes());
    a.extend_from_slice(&file.to_le_bytes());
    a.extend_from_slice(&off.to_le_bytes());
    a.push(n.min(CHUNK_USB) as u8);
    bs_req(mgr, OP_READ, &a).await
}

async fn read_range(
    mgr: &ConnectionManager,
    run: u16,
    file: u16,
    off: u32,
    size: usize,
    mut progress: impl FnMut(usize),
) -> CmdResult<Vec<u8>> {
    let mut buf = Vec::with_capacity(size);
    while buf.len() < size {
        let want = size - buf.len();
        let chunk = read_chunk(mgr, run, file, off + buf.len() as u32, want).await?;
        let got = chunk.len();
        buf.extend_from_slice(&chunk);
        progress(buf.len());
        if got == 0 || got < want.min(CHUNK_USB) {
            break;
        }
    }
    Ok(buf)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsFile {
    pub file_id: u16,
    pub size: u32,
    pub name: String,
}

fn file_name(id: u16) -> String {
    match id {
        0 => "meta.bin".into(),
        1 => "ckpt.bin".into(),
        2 => "ev.bin".into(),
        3 => "q15.bin".into(),
        4 => "s1.bin".into(),
        d if d >= M1_BASE => format!("m{:04}.bin", d - M1_BASE),
        o => format!("f{o}.bin"),
    }
}

async fn run_dir(mgr: &ConnectionManager, run: u16) -> CmdResult<Vec<BsFile>> {
    let mut files = Vec::new();
    loop {
        let mut a = run.to_le_bytes().to_vec();
        a.push(files.len() as u8);
        let raw = bs_req(mgr, OP_RUN_DIR, &a).await?;
        if raw.len() < 4 {
            return Err("short run dir".into());
        }
        let total = raw[2] as usize;
        let n = (raw.len() - 4) / 6;
        for i in 0..n {
            let id = u16le(&raw, 4 + 6 * i);
            files.push(BsFile { file_id: id, size: u32le(&raw, 6 + 6 * i), name: file_name(id) });
        }
        if n == 0 || files.len() >= total {
            return Ok(files);
        }
    }
}

// --------------------------------------------------------------------------
// Tauri state + commands
// --------------------------------------------------------------------------
#[derive(Default)]
pub struct BattSimState {
    open: Mutex<Option<History>>,
}

fn cache_root(app: &AppHandle) -> CmdResult<PathBuf> {
    let d = app.path().app_data_dir().map_err(map_err)?.join("battsim");
    std::fs::create_dir_all(&d).map_err(map_err)?;
    Ok(d)
}

#[tauri::command]
pub async fn bs_status(mgr: State<'_, ConnectionManager>) -> CmdResult<BsStatus> {
    parse_status(&bs_req(&mgr, OP_STATUS, &[]).await?)
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsRunSummary {
    pub run_id: u16,
    pub active: bool,
    pub meta: Option<BsMeta>,
    pub bytes: u64,
    pub cached_dir: Option<String>,
}

/// Every run on the device with its meta and total stored size.
#[tauri::command]
pub async fn bs_list_runs(mgr: State<'_, ConnectionManager>) -> CmdResult<Vec<BsRunSummary>> {
    let mut ids: Vec<u16> = Vec::new();
    let mut active: u16;
    loop {
        let raw = bs_req(&mgr, OP_LIST_RUNS, &(ids.len() as u16).to_le_bytes()).await?;
        if raw.len() < 4 {
            return Err("short run list".into());
        }
        let total = u16le(&raw, 0) as usize;
        active = u16le(&raw, 2);
        let page: Vec<u16> = (0..(raw.len() - 4) / 2).map(|i| u16le(&raw, 4 + 2 * i)).collect();
        let empty = page.is_empty();
        ids.extend(page);
        if empty || ids.len() >= total {
            break;
        }
    }
    let mut out = Vec::with_capacity(ids.len());
    for id in ids {
        let files = run_dir(&mgr, id).await.unwrap_or_default();
        let meta = read_range(&mgr, id, FILE_META, 0, META_SIZE, |_| {}).await.ok().and_then(|b| parse_meta(&b).ok());
        out.push(BsRunSummary {
            run_id: id,
            active: id == active,
            bytes: files.iter().map(|f| f.size as u64).sum(),
            meta,
            cached_dir: None,
        });
    }
    Ok(out)
}

/// Runs mirrored on this PC (openable without a device).
#[tauri::command]
pub fn bs_list_cached(app: AppHandle) -> CmdResult<Vec<BsRunSummary>> {
    let root = cache_root(&app)?;
    let mut out = Vec::new();
    for e in std::fs::read_dir(&root).map_err(map_err)?.flatten() {
        let p = e.path();
        let Ok(meta) = parse_meta(&std::fs::read(p.join("meta.bin")).unwrap_or_default()) else { continue };
        let bytes = std::fs::read_dir(&p).map(|d| d.flatten().filter_map(|f| f.metadata().ok()).map(|m| m.len()).sum()).unwrap_or(0);
        out.push(BsRunSummary { run_id: meta.run_id, active: false, meta: Some(meta), bytes, cached_dir: Some(p.to_string_lossy().into_owned()) });
    }
    out.sort_by_key(|r| std::cmp::Reverse(r.meta.as_ref().map(|m| m.created_epoch).unwrap_or(0)));
    Ok(out)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SyncProgress {
    run_id: u16,
    file: String,
    done: u64,
    total: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsOpenInfo {
    pub meta: BsMeta,
    pub ckpt: Option<BsCkpt>,
    pub events: Vec<BsEvent>,
    pub points: usize,
    pub t_start_s: f64,
    pub t_end_s: f64,
    pub energy_exact: bool,
    pub stats: BsStats,
    pub dir: String,
    pub fetched_bytes: u64,
}

fn open_info(h: &History, fetched: u64) -> BsOpenInfo {
    BsOpenInfo {
        meta: h.meta.clone(),
        ckpt: h.ckpt.clone(),
        events: h.events.clone(),
        points: h.recs.len(),
        t_start_s: h.recs.first().map(|r| r.t - r.dt).unwrap_or(0.0),
        t_end_s: h.recs.last().map(|r| r.t).unwrap_or(0.0),
        energy_exact: h.meta.version >= 2,
        stats: stats(h, f64::NEG_INFINITY, f64::INFINITY),
        dir: h.dir.to_string_lossy().into_owned(),
        fetched_bytes: fetched,
    }
}

/// Mirror a run into the app data dir (only appended bytes are fetched), then
/// open it. Emits `battsim-sync` progress events.
#[tauri::command]
pub async fn bs_open_run(
    run_id: u16,
    app: AppHandle,
    mgr: State<'_, ConnectionManager>,
    st: State<'_, BattSimState>,
) -> CmdResult<BsOpenInfo> {
    let meta_raw = read_range(&mgr, run_id, FILE_META, 0, META_SIZE, |_| {}).await?;
    let meta = parse_meta(&meta_raw)?;
    let dir = cache_root(&app)?.join(format!("r{:05}-{}", run_id, meta.created_epoch));
    std::fs::create_dir_all(&dir).map_err(map_err)?;
    let remote = run_dir(&mgr, run_id).await?;
    for e in std::fs::read_dir(&dir).map_err(map_err)?.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        if !remote.iter().any(|f| f.name == n) {
            let _ = std::fs::remove_file(e.path());
        }
    }
    let total: u64 = remote.iter().map(|f| f.size as u64).sum();
    let mut fetched = 0u64;
    for f in &remote {
        let path = dir.join(&f.name);
        let mut local = if matches!(f.file_id, FILE_META | FILE_CKPT | FILE_S1) {
            Vec::new()
        } else {
            std::fs::read(&path).unwrap_or_default()
        };
        // History files are append-only except q15 (compacted in place): a
        // shrink or a changed head means refetch from zero.
        if local.len() > f.size as usize {
            local.clear();
        } else if !local.is_empty() {
            let n = local.len().min(CHUNK_USB);
            let head = read_range(&mgr, run_id, f.file_id, 0, n, |_| {}).await?;
            if head[..] != local[..head.len().min(n)] {
                local.clear();
            }
        }
        let base = local.len();
        if base < f.size as usize {
            let name = f.name.clone();
            let app2 = app.clone();
            let tail = read_range(&mgr, run_id, f.file_id, base as u32, f.size as usize - base, |n| {
                let _ = app2.emit("battsim-sync", SyncProgress { run_id, file: name.clone(), done: (base + n) as u64, total: total.max(1) });
            })
            .await?;
            fetched += tail.len() as u64;
            local.extend_from_slice(&tail);
        }
        std::fs::write(&path, &local).map_err(map_err)?;
    }
    let h = load_history(&dir)?;
    let info = open_info(&h, fetched);
    *st.open.lock().map_err(map_err)? = Some(h);
    Ok(info)
}

/// Open a previously mirrored run from disk (no device needed).
#[tauri::command]
pub fn bs_open_cached(dir: String, app: AppHandle, st: State<'_, BattSimState>) -> CmdResult<BsOpenInfo> {
    let root = cache_root(&app)?;
    let p = PathBuf::from(&dir);
    let canon = p.canonicalize().map_err(map_err)?;
    if !canon.starts_with(root.canonicalize().map_err(map_err)?) {
        return Err("not a battery-sim cache directory".into());
    }
    let h = load_history(&canon)?;
    let info = open_info(&h, 0);
    *st.open.lock().map_err(map_err)? = Some(h);
    Ok(info)
}

#[tauri::command]
pub fn bs_view(t0: f64, t1: f64, buckets: u32, st: State<'_, BattSimState>) -> CmdResult<BsView> {
    let g = st.open.lock().map_err(map_err)?;
    let h = g.as_ref().ok_or("no run open")?;
    Ok(view(h, t0, t1, buckets as usize))
}

#[tauri::command]
pub fn bs_export(path: String, format: String, st: State<'_, BattSimState>) -> CmdResult<()> {
    let g = st.open.lock().map_err(map_err)?;
    let h = g.as_ref().ok_or("no run open")?;
    let rows = h.recs.iter();
    if format == "csv" {
        let mut w = csv::Writer::from_path(&path).map_err(map_err)?;
        w.write_record(["t_s", "dt_s", "v_avg", "v_min", "v_max", "soc_pct", "i_avg_a", "i_min_a", "i_max_a", "p_avg_w", "flags", "q_dut_c", "q_used_c", "e_dut_j", "tier"]).map_err(map_err)?;
        for r in rows {
            let opt = |x: Option<f64>| x.map(|v| v.to_string()).unwrap_or_default();
            w.write_record([
                r.t.to_string(), r.dt.to_string(), r.v_avg.to_string(), r.v_min.to_string(), r.v_max.to_string(),
                r.soc.to_string(), r.i_avg.to_string(), r.i_min.to_string(), r.i_max.to_string(),
                (r.v_avg * r.i_avg).to_string(), r.flags.to_string(), opt(r.q_dut), r.q_used.to_string(),
                opt(r.e_dut), ["q15", "m1", "s1"][r.tier as usize % 3].to_string(),
            ]).map_err(map_err)?;
        }
        return w.flush().map_err(map_err);
    }
    let j = serde_json::json!({
        "schema": "bugbuster.battsim.run/1",
        "meta": h.meta, "checkpoint": h.ckpt, "events": h.events,
        "energyExact": h.meta.version >= 2,
        "stats": stats(h, f64::NEG_INFINITY, f64::INFINITY),
        "columns": ["t_s","dt_s","v_avg","v_min","v_max","soc_pct","i_avg","i_min","i_max","flags","q_dut_c","q_used_c","e_dut_j"],
        "rows": h.recs.iter().map(|r| serde_json::json!([r.t, r.dt, r.v_avg, r.v_min, r.v_max, r.soc, r.i_avg, r.i_min, r.i_max, r.flags, r.q_dut, r.q_used, r.e_dut])).collect::<Vec<_>>(),
    });
    std::fs::write(&path, serde_json::to_vec(&j).map_err(map_err)?).map_err(map_err)
}

#[tauri::command]
pub async fn bs_set_epoch(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_err(map_err)?.as_secs() as u32;
    bs_req(&mgr, OP_SET_EPOCH, &now.to_le_bytes()).await.map(|_| ())
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsProfile {
    pub slot: u8,
    pub name: String,
    pub params: BsParams,
}

#[tauri::command]
pub async fn bs_profiles(mask: u16, mgr: State<'_, ConnectionManager>) -> CmdResult<Vec<BsProfile>> {
    let mut out = Vec::new();
    for slot in 0..16u8 {
        if mask & (1 << slot) == 0 {
            continue;
        }
        let raw = bs_req(&mgr, OP_PROFILE, &[slot]).await?;
        if raw.len() < 52 {
            continue;
        }
        let end = raw[..24].iter().position(|&c| c == 0).unwrap_or(24);
        out.push(BsProfile { slot, name: String::from_utf8_lossy(&raw[..end]).into_owned(), params: parse_params(&raw[24..52]) });
    }
    Ok(out)
}

/// Battery definition to write before NEW RUN / profile save. Unset fields are left alone.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BsConfig {
    pub chem: Option<u8>,
    pub cells: Option<u8>,
    pub capacity_mah: Option<u32>,
    pub start_soc_pct: Option<f64>,
    pub cutoff_mv_cell: Option<u16>,
    pub rint_uohm_cell: Option<u32>,
    pub peukert: Option<f64>,
    pub sd_enable: Option<bool>,
    pub sd_pct_month: Option<f64>,
    pub ext_enable: Option<bool>,
    pub ext_load_ua: Option<u32>,
    pub dither: Option<bool>,
    pub name: Option<String>,
}

async fn cfg_set(mgr: &ConnectionManager, key: u16, ty: u8, val: &[u8]) -> CmdResult<()> {
    let mut a = key.to_le_bytes().to_vec();
    a.push(ty);
    a.push(val.len() as u8);
    a.extend_from_slice(val);
    cfg_raw(mgr, CFG_SET, &a).await.map(|_| ())
}

#[tauri::command]
pub async fn bs_configure(cfg: BsConfig, mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    const BOOL: u8 = 1;
    const U8: u8 = 2;
    const U16: u8 = 4;
    const U32: u8 = 6;
    const ENUM: u8 = 9;
    const STR: u8 = 10;
    if let Some(v) = cfg.chem { cfg_set(&mgr, 0x0801, ENUM, &[v]).await?; }
    if let Some(v) = cfg.cells { cfg_set(&mgr, 0x0802, U8, &[v]).await?; }
    if let Some(v) = cfg.capacity_mah { cfg_set(&mgr, 0x0803, U32, &v.to_le_bytes()).await?; }
    if let Some(v) = cfg.start_soc_pct { cfg_set(&mgr, 0x0804, U16, &((v * 10.0).round() as u16).to_le_bytes()).await?; }
    if let Some(v) = cfg.cutoff_mv_cell { cfg_set(&mgr, 0x0805, U16, &v.to_le_bytes()).await?; }
    if let Some(v) = cfg.rint_uohm_cell { cfg_set(&mgr, 0x0806, U32, &v.to_le_bytes()).await?; }
    if let Some(v) = cfg.peukert { cfg_set(&mgr, 0x0807, U16, &((v * 1000.0).round() as u16).to_le_bytes()).await?; }
    if let Some(v) = cfg.sd_enable { cfg_set(&mgr, 0x0808, BOOL, &[v as u8]).await?; }
    if let Some(v) = cfg.sd_pct_month { cfg_set(&mgr, 0x0809, U16, &((v * 100.0).round() as u16).to_le_bytes()).await?; }
    if let Some(v) = cfg.ext_enable { cfg_set(&mgr, 0x080A, BOOL, &[v as u8]).await?; }
    if let Some(v) = cfg.ext_load_ua { cfg_set(&mgr, 0x080B, U32, &v.to_le_bytes()).await?; }
    if let Some(v) = cfg.dither { cfg_set(&mgr, 0x080C, BOOL, &[v as u8]).await?; }
    if let Some(v) = cfg.name {
        let b: Vec<u8> = v.bytes().take(23).collect();
        cfg_set(&mgr, 0x080F, STR, &b).await?;
    }
    Ok(())
}

/// Run / profile action (DAQ_ACT_BS_*, 4..14). `run_id` / `slot` select the target first.
#[tauri::command]
pub async fn bs_action(
    action: u8,
    run_id: Option<u16>,
    slot: Option<u8>,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    if !(4..=14).contains(&action) {
        return Err(format!("not a battery-sim action: {action}"));
    }
    if let Some(r) = run_id {
        cfg_set(&mgr, 0x080E, 4, &r.to_le_bytes()).await?;
    }
    if let Some(s) = slot {
        cfg_set(&mgr, 0x080D, 2, &[s]).await?;
    }
    cfg_raw(&mgr, CFG_ACTION, &[action]).await.map(|_| ())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn meta(version: u16) -> Vec<u8> {
        let mut b = vec![0u8; META_SIZE];
        b[0..4].copy_from_slice(&0x4E52_5342u32.to_le_bytes());
        b[4..6].copy_from_slice(&version.to_le_bytes());
        b[6..8].copy_from_slice(&7u16.to_le_bytes());
        b[12..17].copy_from_slice(b"bench");
        b[44..48].copy_from_slice(&2000u32.to_le_bytes()); // params.capacity_mah
        b
    }

    fn rec_v2(t: u32, dt: u16, q_nc: i64, e_uj: i64) -> Vec<u8> {
        let mut b = vec![0u8; 48];
        b[0..4].copy_from_slice(&t.to_le_bytes());
        b[4..6].copy_from_slice(&3700u16.to_le_bytes());
        b[6..8].copy_from_slice(&3695u16.to_le_bytes());
        b[8..10].copy_from_slice(&3705u16.to_le_bytes());
        b[10..12].copy_from_slice(&9000u16.to_le_bytes());
        b[12..16].copy_from_slice(&9_000_000i32.to_le_bytes());
        b[16..20].copy_from_slice(&11_000_000i32.to_le_bytes());
        b[22..24].copy_from_slice(&dt.to_le_bytes());
        b[24..32].copy_from_slice(&q_nc.to_le_bytes());
        b[32..40].copy_from_slice(&q_nc.to_le_bytes());
        b[40..48].copy_from_slice(&e_uj.to_le_bytes());
        b
    }

    #[test]
    fn v2_history_exact_current_energy_and_view() {
        let dir = std::env::temp_dir().join(format!("bs_test_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("meta.bin"), meta(2)).unwrap();
        let m: Vec<u8> = (1..=1000).flat_map(|k| rec_v2(60 * k, 60, 600_000_000 * k as i64, 2_220_000 * k as i64)).collect();
        std::fs::write(dir.join("m0000.bin"), m).unwrap();
        let h = load_history(&dir).unwrap();
        assert_eq!(h.recs.len(), 1000);
        assert!(h.recs.iter().all(|r| (r.i_avg - 0.010).abs() < 1e-9));
        let s = stats(&h, f64::NEG_INFINITY, f64::INFINITY);
        assert!((s.energy_j - 2220.0).abs() < 1e-6);
        assert!((s.charge_c - 600.0).abs() < 1e-6);
        let v = view(&h, 0.0, 60_000.0, 100);
        assert_eq!(v.t.len(), 100);
        assert!(v.i_avg.iter().all(|i| (i - 0.010).abs() < 1e-9));
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn status_layout() {
        let mut b = vec![0u8; STATUS_SIZE];
        b[1] = 2;
        b[2] = FLAG_REMAIN_OK;
        b[12..14].copy_from_slice(&8123u16.to_le_bytes());
        b[20..24].copy_from_slice(&7200u32.to_le_bytes());
        let s = parse_status(&b).unwrap();
        assert_eq!(s.state, 2);
        assert_eq!(s.remaining_s, Some(7200));
        assert!((s.soc_pct - 81.23).abs() < 1e-9);
    }
}
