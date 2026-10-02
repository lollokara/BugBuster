// battsim.rs - Battery Simulator view (DAQ HAT). Live run status + controls,
// device / cached run browser, zoomable V / I / P / SOC history and window stats.
// Backend: src-tauri/src/battsim.rs (history is mirrored to the app data dir).

use crate::components::icons::Icon;
use crate::tauri_bridge::{invoke, listen, DeviceState};
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Arc;
use wasm_bindgen::prelude::*;

use super::battsim_chart::{self as chart, fmt_charge_ah, fmt_duration, fmt_energy_wh, fmt_si, Lane};

// --------------------------------------------------------------------------
// Types (mirror src-tauri/src/battsim.rs)
// --------------------------------------------------------------------------
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
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

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
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
    #[serde(default)]
    pub e_dut_j: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct BsMeta {
    pub run_id: u16,
    pub version: u16,
    pub created_epoch: u32,
    pub name: String,
    pub params: BsParams,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct BsRunSummary {
    pub run_id: u16,
    pub active: bool,
    pub meta: Option<BsMeta>,
    pub bytes: u64,
    pub cached_dir: Option<String>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct BsEvent {
    pub t_s: u32,
    pub code: u16,
    pub a: i32,
    pub b: i32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
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

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct BsOpenInfo {
    pub meta: BsMeta,
    pub events: Vec<BsEvent>,
    pub points: usize,
    pub t_start_s: f64,
    pub t_end_s: f64,
    pub energy_exact: bool,
    pub stats: BsStats,
    pub dir: String,
    pub fetched_bytes: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct BsView {
    pub t: Vec<f64>,
    #[serde(default)]
    pub dt: Vec<f64>,
    pub v_min: Vec<f64>,
    pub v_avg: Vec<f64>,
    pub v_max: Vec<f64>,
    pub i_min: Vec<f64>,
    pub i_avg: Vec<f64>,
    pub i_max: Vec<f64>,
    pub p_avg: Vec<f64>,
    pub soc: Vec<f64>,
    pub tier: Vec<u8>,
    pub events: Vec<BsEvent>,
    pub stats: BsStats,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SyncProgress {
    run_id: u16,
    file: String,
    done: u64,
    total: u64,
}

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

const STATE_NAMES: [&str; 5] = ["No run", "Paused", "Active", "Depleted", "Stopped"];
const CHEM_NAMES: [&str; 4] = ["LiPo", "LiFePO4", "NiMH", "Lead-acid"];
const ERRORS: [&str; 10] = ["", "battlog partition missing", "no run loaded", "busy", "invalid parameters",
    "not allowed in this state", "USB-PD contract below 9 V / 3 A", "acquisition not running", "flash I/O error", "run not found"];
// DAQ_ACT_BS_*
const ACT_DEFAULTS: u8 = 14;
const ACT_NEW: u8 = 7;
const ACT_START: u8 = 8;
const ACT_PAUSE: u8 = 9;
const ACT_STOP: u8 = 10;
const ACT_UNLOAD: u8 = 11;
const ACT_LOAD: u8 = 12;
const ACT_DELETE: u8 = 13;

fn chem_name(c: u8) -> &'static str {
    CHEM_NAMES.get(c as usize).copied().unwrap_or("?")
}

async fn call<T: for<'de> Deserialize<'de>>(cmd: &str, args: impl Serialize) -> Result<T, String> {
    let a = serde_wasm_bindgen::to_value(&args).map_err(|e| e.to_string())?;
    match invoke(cmd, a).await {
        Ok(v) => serde_wasm_bindgen::from_value(v).map_err(|e| e.to_string()),
        Err(e) => Err(e.as_string().unwrap_or_else(|| format!("{cmd} failed"))),
    }
}

#[derive(Serialize)]
struct NoArgs {}

async fn sleep_ms(ms: i32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        if let Some(w) = web_sys::window() {
            let _ = w.set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms);
        }
    });
    let _ = wasm_bindgen_futures::JsFuture::from(p).await;
}

fn fmt_date(epoch: u32) -> String {
    if epoch == 0 {
        return "date unknown".into();
    }
    let d = js_sys::Date::new(&JsValue::from_f64(epoch as f64 * 1000.0));
    format!("{}-{:02}-{:02} {:02}:{:02}", d.get_full_year(), d.get_month() + 1, d.get_date(), d.get_hours(), d.get_minutes())
}

fn fmt_bytes(b: u64) -> String {
    if b >= 1 << 20 { format!("{:.2} MiB", b as f64 / 1048576.0) } else { format!("{:.0} KiB", b as f64 / 1024.0) }
}

// --------------------------------------------------------------------------
// View
// --------------------------------------------------------------------------
#[component]
pub fn BattSimTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let _ = state;
    let status = RwSignal::new(None::<BsStatus>);
    let status_err = RwSignal::new(None::<String>);
    let runs = RwSignal::new(Vec::<BsRunSummary>::new());
    let cached = RwSignal::new(Vec::<BsRunSummary>::new());
    let open = RwSignal::new(None::<BsOpenInfo>);
    let view_data = RwSignal::new(None::<BsView>);
    let t0 = RwSignal::new(0.0f64);
    let t1 = RwSignal::new(1.0f64);
    let sync = RwSignal::new(None::<SyncProgress>);
    let busy = RwSignal::new(false);
    let error = RwSignal::new(None::<String>);
    let log_i = RwSignal::new(true);
    let wall = RwSignal::new(false);
    let follow = RwSignal::new(true);
    let show_new = RwSignal::new(false);
    let confirm_delete = RwSignal::new(None::<u16>);
    let hover_x = RwSignal::new(None::<f64>);
    let drag = RwSignal::new(None::<(f64, f64, f64, f64, bool)>); // (x0, t0, t1, x_now, box)
    let lanes = vec![Lane::Voltage, Lane::Current, Lane::Power, Lane::Soc];
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    let alive = Arc::new(AtomicBool::new(true));
    {
        let a = alive.clone();
        on_cleanup(move || a.store(false, Ordering::Relaxed));
    }

    // Sync progress events from the backend.
    spawn_local(async move {
        let cb = Closure::<dyn FnMut(JsValue)>::new(move |ev: JsValue| {
            if let Ok(p) = js_sys::Reflect::get(&ev, &"payload".into()) {
                if let Ok(sp) = serde_wasm_bindgen::from_value::<SyncProgress>(p) {
                    sync.set(Some(sp));
                }
            }
        });
        listen("battsim-sync", &cb).await;
        cb.forget();
    });

    let refresh_runs = move || {
        spawn_local(async move {
            match call::<Vec<BsRunSummary>>("bs_list_runs", NoArgs {}).await {
                Ok(r) => runs.set(r),
                Err(e) => error.set(Some(e)),
            }
            if let Ok(c) = call::<Vec<BsRunSummary>>("bs_list_cached", NoArgs {}).await {
                cached.set(c);
            }
        });
    };

    // View fetch, latest request wins.
    let gen = Arc::new(AtomicU32::new(0));
    let fetch_view = {
        let gen = gen.clone();
        move || {
            if open.get_untracked().is_none() {
                return;
            }
            let my = gen.fetch_add(1, Ordering::Relaxed) + 1;
            let gen = gen.clone();
            let buckets = canvas_ref.get_untracked().map(|c| c.client_width().max(200) as u32).unwrap_or(1200);
            let (a, b) = (t0.get_untracked(), t1.get_untracked());
            spawn_local(async move {
                #[derive(Serialize)]
                struct A { t0: f64, t1: f64, buckets: u32 }
                if let Ok(v) = call::<BsView>("bs_view", A { t0: a, t1: b, buckets }).await {
                    if gen.load(Ordering::Relaxed) == my {
                        view_data.set(Some(v));
                    }
                }
            });
        }
    };

    let open_run = {
        let fetch_view = fetch_view.clone();
        move |run_id: Option<u16>, dir: Option<String>| {
            let fetch_view = fetch_view.clone();
            busy.set(true);
            error.set(None);
            spawn_local(async move {
                #[derive(Serialize)]
                #[serde(rename_all = "camelCase")]
                struct ById { run_id: u16 }
                #[derive(Serialize)]
                struct ByDir { dir: String }
                let r = match (run_id, dir) {
                    (Some(id), _) => call::<BsOpenInfo>("bs_open_run", ById { run_id: id }).await,
                    (None, Some(d)) => call::<BsOpenInfo>("bs_open_cached", ByDir { dir: d }).await,
                    _ => Err("nothing to open".into()),
                };
                busy.set(false);
                sync.set(None);
                match r {
                    Ok(info) => {
                        let (a, b) = (info.t_start_s, info.t_end_s.max(info.t_start_s + 60.0));
                        let keep = open.get_untracked().map(|o| o.meta.run_id == info.meta.run_id && o.meta.created_epoch == info.meta.created_epoch).unwrap_or(false);
                        let pinned = keep && (t1.get_untracked() - open.get_untracked().map(|o| o.t_end_s).unwrap_or(0.0)).abs() < 1.0;
                        if !keep {
                            t0.set(a);
                            t1.set(b);
                        } else if pinned && follow.get_untracked() {
                            let w = t1.get_untracked() - t0.get_untracked();
                            t1.set(b);
                            t0.set((b - w).max(a));
                        }
                        open.set(Some(info));
                        fetch_view();
                    }
                    Err(e) => error.set(Some(e)),
                }
            });
        }
    };

    // Status poll (2 s) + incremental re-sync of the open active run (60 s).
    {
        let a = alive.clone();
        let open_run = open_run.clone();
        spawn_local(async move {
            let _ = call::<()>("bs_set_epoch", NoArgs {}).await;
            refresh_runs();
            let mut tick = 0u32;
            while a.load(Ordering::Relaxed) {
                match call::<BsStatus>("bs_status", NoArgs {}).await {
                    Ok(s) => {
                        status_err.set(None);
                        let live_open = open.get_untracked().map(|o| o.meta.run_id == s.run_id && o.dir.contains(&format!("r{:05}-", s.run_id))).unwrap_or(false);
                        if tick % 30 == 29 && live_open && s.state == 2 && !busy.get_untracked() {
                            open_run(Some(s.run_id), None);
                        }
                        status.set(Some(s));
                    }
                    Err(e) => status_err.set(Some(e)),
                }
                tick = tick.wrapping_add(1);
                sleep_ms(2000).await;
            }
        });
    }

    // Redraw on any visual input.
    Effect::new(move |_| {
        let v = view_data.get();
        let (a, b) = (t0.get(), t1.get());
        let hx = hover_x.get();
        let d = drag.get();
        let lg = log_i.get();
        let wall_epoch = if wall.get() { open.get().map(|o| o.meta.created_epoch as f64).filter(|e| *e > 0.0) } else { None };
        if let Some(c) = canvas_ref.get() {
            let opts = chart::ChartOpts {
                t0: a,
                t1: b,
                lanes: vec![Lane::Voltage, Lane::Current, Lane::Power, Lane::Soc],
                log_current: lg,
                wall_epoch,
                hover_x: hx,
                box_sel: d.filter(|d| d.4).map(|d| (d.0, d.3)),
            };
            chart::draw(&c, v.as_ref(), &opts);
        }
    });

    let x_to_t = move |x: f64| -> f64 {
        let Some(c) = canvas_ref.get_untracked() else { return 0.0 };
        let lay = chart::layout(c.client_width() as f64, c.client_height() as f64, &[Lane::Voltage]);
        let (a, b) = (t0.get_untracked(), t1.get_untracked());
        a + (x - lay.plot_x) / lay.plot_w * (b - a)
    };
    let bounds = move || open.get_untracked().map(|o| (o.t_start_s, o.t_end_s.max(o.t_start_s + 60.0))).unwrap_or((0.0, 1.0));
    let set_window = {
        let fetch_view = fetch_view.clone();
        move |a: f64, b: f64| {
            let (lo, hi) = bounds();
            let w = (b - a).clamp(30.0, (hi - lo).max(30.0));
            let a = a.clamp(lo, (hi - w).max(lo));
            t0.set(a);
            t1.set(a + w);
            fetch_view();
        }
    };

    let on_wheel = {
        let set_window = set_window.clone();
        move |ev: web_sys::WheelEvent| {
            ev.prevent_default();
            let tc = x_to_t(ev.offset_x() as f64);
            let k = if ev.delta_y() > 0.0 { 1.25 } else { 0.8 };
            let (a, b) = (t0.get_untracked(), t1.get_untracked());
            set_window(tc - (tc - a) * k, tc + (b - tc) * k);
        }
    };
    let on_down = move |ev: web_sys::MouseEvent| {
        drag.set(Some((ev.offset_x() as f64, t0.get_untracked(), t1.get_untracked(), ev.offset_x() as f64, ev.shift_key())));
    };
    let on_move = {
        let set_window = set_window.clone();
        move |ev: web_sys::MouseEvent| {
            let x = ev.offset_x() as f64;
            hover_x.set(Some(x));
            if let Some((x0, a, b, _, boxed)) = drag.get_untracked() {
                if boxed {
                    drag.set(Some((x0, a, b, x, true)));
                } else if let Some(c) = canvas_ref.get_untracked() {
                    let lay = chart::layout(c.client_width() as f64, c.client_height() as f64, &[Lane::Voltage]);
                    let dt = (x - x0) / lay.plot_w * (b - a);
                    set_window(a - dt, b - dt);
                    drag.set(Some((x0, t0.get_untracked() + dt, t1.get_untracked() + dt, x, false)));
                }
            }
        }
    };
    let on_up = {
        let set_window = set_window.clone();
        move |_ev: web_sys::MouseEvent| {
            if let Some((x0, _, _, x1, true)) = drag.get_untracked() {
                if (x1 - x0).abs() > 4.0 {
                    let (ta, tb) = (x_to_t(x0.min(x1)), x_to_t(x0.max(x1)));
                    set_window(ta, tb);
                }
            }
            drag.set(None);
        }
    };
    let on_dbl = {
        let set_window = set_window.clone();
        move |_ev: web_sys::MouseEvent| {
            let (lo, hi) = bounds();
            set_window(lo, hi);
        }
    };
    let preset = {
        let set_window = set_window.clone();
        move |secs: f64| {
            let (lo, hi) = bounds();
            if secs <= 0.0 { set_window(lo, hi) } else { set_window(hi - secs, hi) }
        }
    };

    // Redraw on window resize.
    {
        let cb = Closure::<dyn FnMut()>::new(move || hover_x.set(hover_x.get_untracked()));
        if let Some(w) = web_sys::window() {
            let _ = w.add_event_listener_with_callback("resize", cb.as_ref().unchecked_ref());
        }
        cb.forget();
    }

    let act = move |action: u8, run_id: Option<u16>| {
        spawn_local(async move {
            #[derive(Serialize)]
            #[serde(rename_all = "camelCase")]
            struct A { action: u8, run_id: Option<u16>, slot: Option<u8> }
            if let Err(e) = call::<()>("bs_action", A { action, run_id, slot: None }).await {
                let hint = status.get_untracked().and_then(|s| ERRORS.get(s.last_error as usize).copied()).filter(|s| !s.is_empty());
                error.set(Some(match hint { Some(h) => format!("{e} ({h})"), None => e }));
            }
            sleep_ms(400).await;
            if let Ok(s) = call::<BsStatus>("bs_status", NoArgs {}).await {
                if s.last_error != 0 && error.get_untracked().is_none() {
                    if let Some(h) = ERRORS.get(s.last_error as usize) { error.set(Some(h.to_string())); }
                }
                status.set(Some(s));
            }
            if matches!(action, ACT_NEW | ACT_DELETE | ACT_STOP | ACT_UNLOAD | ACT_LOAD) {
                refresh_runs();
            }
        });
    };

    let export = move |fmt: &'static str| {
        spawn_local(async move {
            let name = open.get_untracked().map(|o| format!("battsim_run{}.{}", o.meta.run_id, fmt)).unwrap_or_default();
            let args = serde_wasm_bindgen::to_value(&serde_json::json!({
                "title": "Export battery-sim run", "defaultPath": name,
                "filters": [{"name": fmt.to_uppercase(), "extensions": [fmt]}]
            })).unwrap();
            let Ok(p) = invoke("plugin:dialog|save", args).await else { return };
            let Some(path) = p.as_string() else { return };
            #[derive(Serialize)]
            struct A { path: String, format: String }
            if let Err(e) = call::<()>("bs_export", A { path, format: fmt.into() }).await {
                error.set(Some(e));
            }
        });
    };

    // Hover readout: nearest bucket to the cursor.
    let hover_info = move || {
        let x = hover_x.get()?;
        let v = view_data.get()?;
        let t = x_to_t(x);
        let i = v.t.partition_point(|&tt| tt < t);
        let i = if i >= v.t.len() { v.t.len().checked_sub(1)? } else if i > 0 && (t - v.t[i - 1]).abs() < (v.t[i] - t).abs() { i - 1 } else { i };
        Some((v.t[i], v.v_avg[i], v.v_min[i], v.v_max[i], v.i_avg[i], v.i_min[i], v.i_max[i], v.p_avg[i], v.soc[i], v.tier[i]))
    };

    let open_run_list = open_run.clone();
    let open_run_cached = open_run.clone();
    let lanes_count = lanes.len();

    view! {
        <div class="bs-root">
            // ---- Live status bar ----
            <div class="bs-bar">
                {move || match status.get() {
                    None => view! {
                        <span class="bs-muted">{move || status_err.get().unwrap_or_else(|| "Reading battery simulator...".into())}</span>
                    }.into_any(),
                    Some(s) => {
                        let st = STATE_NAMES.get(s.state as usize).copied().unwrap_or("?");
                        let cls = format!("badge bs-state s{}", s.state);
                        let soc_w = format!("width:{:.1}%", s.soc_pct.clamp(0.0, 100.0));
                        let remain = s.remaining_s.map(|r| fmt_duration(r as f64)).unwrap_or_else(|| "-".into());
                        let prov = s.flags & 0x01 != 0;
                        let store_txt = format!("{} / {}", fmt_bytes(s.fs_used as u64), fmt_bytes(s.fs_total as u64));
                        let store_low = s.fs_total > 0 && (s.fs_total - s.fs_used) < 2_600_000;
                        view! {
                            <span class=cls>{st}</span>
                            {(s.state != 0).then(|| view! {
                                <span class="bs-kv"><span>"Run"</span><b>{format!("#{}", s.run_id)}</b></span>
                                <span class="bs-kv"><span>{chem_name(s.chem)}</span><b>{format!("{}S {} mAh", s.cells, s.capacity_mah)}</b></span>
                                <span class="bs-soc" title="State of charge"><span class="bs-soc-fill" style=soc_w></span><b>{format!("{:.1} %", s.soc_pct)}</b></span>
                                <span class="bs-kv"><span>"V"</span><b>{fmt_si(s.v_meas as f64, "V")}</b></span>
                                <span class="bs-kv"><span>"I"</span><b>{fmt_si(s.i_meas as f64, "A")}</b></span>
                                <span class="bs-kv"><span>"Elapsed"</span><b>{fmt_duration(s.elapsed_s as f64)}</b></span>
                                <span class="bs-kv" title="From the last 30 min of total drain"><span>"Remaining"</span><b>{if prov { format!("~{remain}") } else { remain }}</b></span>
                                {s.e_dut_j.map(|e| view! { <span class="bs-kv" title="Integrated DUT energy"><span>"Energy"</span><b>{fmt_energy_wh(e)}</b></span> })}
                            })}
                            <span class="bs-spacer"></span>
                            <span class="bs-kv" class:bs-warn=store_low title="battlog flash usage"><span>"Storage"</span><b>{store_txt}</b></span>
                        }.into_any()
                    }
                }}
                <span class="bs-sep"></span>
                {move || {
                    let s = status.get().unwrap_or_default();
                    let loaded = matches!(s.state, 1 | 2);
                    let finished = matches!(s.state, 3 | 4);
                    let (paused, active) = (s.state == 1, s.state == 2);
                    view! {
                        <button class="btn btn-sm btn-primary" disabled=!paused on:click=move |_| act(ACT_START, None)><Icon name="play" size=14 />"Start"</button>
                        <button class="btn btn-sm" disabled=!active on:click=move |_| act(ACT_PAUSE, None)><Icon name="pause" size=14 />"Pause"</button>
                        <button class="btn btn-sm" disabled=!loaded on:click=move |_| act(ACT_STOP, None)><Icon name="square" size=14 />"Stop"</button>
                        <button class="btn btn-sm btn-ghost" disabled=!(loaded || finished) title="Leave battery-sim mode (run stays on flash)" on:click=move |_| act(ACT_UNLOAD, None)>"Unload"</button>
                        <button class="btn btn-sm btn-tinted" disabled=active on:click=move |_| show_new.set(true)><Icon name="plus" size=14 />"New run"</button>
                    }
                }}
            </div>
            {move || error.get().map(|e| view! {
                <div class="bs-error" role="alert"><Icon name="triangle-alert" size=14 /><span>{e}</span>
                    <button class="btn btn-xs btn-ghost" on:click=move |_| error.set(None)>"Dismiss"</button></div>
            })}

            <div class="bs-body">
                // ---- Chart (full width) ----
                <section class="bs-main">
                    <div class="bs-toolbar">
                        <select class="dropdown bs-pick" aria-label="Run"
                            on:change={
                                let open_run = open_run_list.clone();
                                move |ev| {
                                    let v = event_target_value(&ev);
                                    if let Some(id) = v.strip_prefix("d:").and_then(|s| s.parse::<u16>().ok()) {
                                        open_run(Some(id), None);
                                    } else if let Some(dir) = v.strip_prefix("c:") {
                                        open_run(None, Some(dir.to_string()));
                                    }
                                }
                            }>
                            <option value="" prop:selected=move || open.get().is_none()>
                                {move || if runs.get().is_empty() && cached.get().is_empty() { "No runs stored" } else { "Open a run..." }}
                            </option>
                            <optgroup label="On device">
                                {move || runs.get().into_iter().rev().map(|r| {
                                    let id = r.run_id;
                                    let name = r.meta.as_ref().map(|m| m.name.clone()).unwrap_or_default();
                                    let sel = move || open.get().map(|o| o.meta.run_id == id && o.dir.contains(&format!("r{id:05}-"))).unwrap_or(false);
                                    view! { <option value=format!("d:{id}") prop:selected=sel>{format!("#{id} {name}{}", if r.active { " (loaded)" } else { "" })}</option> }
                                }).collect_view()}
                            </optgroup>
                            <optgroup label="On this PC">
                                {move || cached.get().into_iter().map(|r| {
                                    let m = r.meta.clone().unwrap_or_default();
                                    let dir = r.cached_dir.clone().unwrap_or_default();
                                    view! { <option value=format!("c:{dir}")>{format!("#{} {} - {}", m.run_id, m.name, fmt_date(m.created_epoch))}</option> }
                                }).collect_view()}
                            </optgroup>
                        </select>
                        {move || open.get().map(|o| view! {
                            <span class="bs-muted">{format!("{} points{}", o.points, if o.energy_exact { "" } else { " - energy estimated (format 1)" })}</span>
                        })}
                        <span class="bs-spacer"></span>
                        <div class="seg" role="group" aria-label="Window">
                            <button on:click={let p = preset.clone(); move |_| p(3600.0)}>"1h"</button>
                            <button on:click={let p = preset.clone(); move |_| p(21600.0)}>"6h"</button>
                            <button on:click={let p = preset.clone(); move |_| p(86400.0)}>"1d"</button>
                            <button on:click={let p = preset.clone(); move |_| p(604800.0)}>"7d"</button>
                            <button on:click={let p = preset.clone(); move |_| p(2592000.0)}>"30d"</button>
                            <button on:click={let p = preset.clone(); move |_| p(0.0)}>"All"</button>
                        </div>
                        <span class="bs-sep"></span>
                        <button class="btn btn-sm" class:btn-tinted=move || log_i.get() on:click=move |_| log_i.update(|v| *v = !*v) title="Logarithmic current axis">"log I"</button>
                        <button class="btn btn-sm" class:btn-tinted=move || wall.get() on:click=move |_| wall.update(|v| *v = !*v) title="Wall-clock axis (approximate: assumes no pauses)">"Clock"</button>
                        <button class="btn btn-sm" class:btn-tinted=move || follow.get() on:click=move |_| follow.update(|v| *v = !*v) title="Keep the newest data in view while the run is active">"Follow"</button>
                        <span class="bs-sep"></span>
                        <button class="btn btn-sm" disabled=move || open.get().is_none() on:click=move |_| export("csv")><Icon name="file-down" size=13 />"CSV"</button>
                        <button class="btn btn-sm" disabled=move || open.get().is_none() on:click=move |_| export("json")><Icon name="file-down" size=13 />"JSON"</button>
                    </div>
                    <div class="bs-plot" data-lanes=lanes_count>
                        <canvas node_ref=canvas_ref class="bs-canvas"
                            on:wheel=on_wheel on:mousedown=on_down on:mousemove=on_move on:mouseup=on_up
                            on:mouseleave=move |_| { hover_x.set(None); drag.set(None); }
                            on:dblclick=on_dbl></canvas>
                        {move || (open.get().is_none()).then(|| view! {
                            <div class="bs-empty">
                                {move || match sync.get() {
                                    Some(p) => view! { <span>{format!("Downloading {} - {} / {}", p.file, fmt_bytes(p.done), fmt_bytes(p.total))}</span> }.into_any(),
                                    None => view! { <span>{if busy.get() { "Opening run..." } else { "Pick a run above. Wheel to zoom, drag to pan, Shift+drag to zoom to a range, double-click for the whole run." }}</span> }.into_any(),
                                }}
                            </div>
                        })}
                        {move || (open.get().is_some() && busy.get()).then(|| view! {
                            <div class="bs-sync">{move || sync.get().map(|p| format!("Syncing {} {:.0} %", p.file, p.done as f64 / p.total.max(1) as f64 * 100.0)).unwrap_or_else(|| "Syncing...".into())}</div>
                        })}
                        {move || hover_info().map(|(t, va, vn, vx, ia, imn, imx, p, soc, tier)| {
                            let tl = if wall.get() { open.get().map(|o| chart::fmt_wall(o.meta.created_epoch as f64 + t, true)).unwrap_or_default() } else { format!("+{}", fmt_duration(t)) };
                            let res = ["15 min", "1 min", "1 s"][tier as usize % 3];
                            let hx = hover_x.get().unwrap_or(0.0);
                            let w = canvas_ref.get().map(|c| c.client_width() as f64).unwrap_or(0.0);
                            let pos = if hx > w - 280.0 { format!("right:{}px", w - hx + 14.0) } else { format!("left:{}px", hx + 14.0) };
                            view! {
                                <div class="bs-tip" style=pos>
                                    <div class="bs-tip-t">{tl}<span class="bs-muted">{format!(" ({res})")}</span></div>
                                    <div><i class="dq-dot s-voltage"></i>{format!("{}  [{} .. {}]", fmt_si(va, "V"), fmt_si(vn, "V"), fmt_si(vx, "V"))}</div>
                                    <div><i class="dq-dot s-current"></i>{format!("{}  [{} .. {}]", fmt_si(ia, "A"), fmt_si(imn, "A"), fmt_si(imx, "A"))}</div>
                                    <div><i class="dq-dot s-power"></i>{fmt_si(p, "W")}</div>
                                    <div><i class="dq-dot bs-dot-soc"></i>{format!("SOC {soc:.2} %")}</div>
                                </div>
                            }
                        })}
                    </div>
                </section>

                // ---- Window statistics + run management ----
                <div class="bs-lower">
                    <section class="bs-stats">
                        <div class="bs-sec-head">
                            <span class="group-title">"Window statistics"</span>
                            {move || view_data.get().map(|v| view! {
                                <span class="bs-muted">{format!("{} from +{} - {} points", fmt_duration(v.stats.duration_s), fmt_duration(v.stats.t_start_s), v.stats.points)}</span>
                            })}
                        </div>
                        {move || {
                            let Some(v) = view_data.get() else {
                                return view! { <div class="bs-muted">"Open a run to see statistics for the visible window."</div> }.into_any();
                            };
                            let s = v.stats;
                            let tile = |title: &'static str, cls: &'static str, rows: Vec<(&'static str, String)>| view! {
                                <div class=format!("bs-tile {cls}")>
                                    <div class="bs-tile-t">{title}</div>
                                    {rows.into_iter().map(|(k, val)| view! { <div class="bs-tile-r"><span>{k}</span><b>{val}</b></div> }).collect_view()}
                                </div>
                            };
                            let duty = if s.i_max > 0.0 { format!("{:.2} %", s.i_avg / s.i_max * 100.0) } else { "-".into() };
                            view! {
                                <div class="bs-tiles">
                                    {tile("Voltage", "c-v", vec![("min", fmt_si(s.v_min, "V")), ("avg", fmt_si(s.v_avg, "V")), ("max", fmt_si(s.v_max, "V"))])}
                                    {tile("Current", "c-i", vec![("min", fmt_si(s.i_min, "A")), ("avg", fmt_si(s.i_avg, "A")), ("max", fmt_si(s.i_max, "A"))])}
                                    {tile("Current profile", "c-i", vec![("baseline P10", fmt_si(s.i_p10, "A")), ("median", fmt_si(s.i_median, "A")), ("busy P99", fmt_si(s.i_p99, "A"))])}
                                    {tile("Power", "c-p", vec![("avg", fmt_si(s.p_avg, "W")), ("peak", fmt_si(s.p_max, "W")), ("duty avg/peak", duty)])}
                                    {tile("Consumed", "c-p", vec![("charge", fmt_charge_ah(s.charge_c)), (if s.energy_estimated { "energy (est.)" } else { "energy" }, fmt_energy_wh(s.energy_j))])}
                                    {tile("State of charge", "c-s", vec![("start", format!("{:.2} %", s.soc_start)), ("end", format!("{:.2} %", s.soc_end)), ("rate", format!("{:.3} %/day", s.soc_rate_pct_per_day))])}
                                    {tile("Projection at window avg", "c-s", vec![("full battery", fmt_duration(s.projected_life_s)), ("remaining", fmt_duration(s.remaining_at_avg_s))])}
                                    {(s.gaps > 0 || s.clamped > 0).then(|| tile("Data quality", "", vec![("gaps", s.gaps.to_string()), ("clamped", s.clamped.to_string())]))}
                                    {(!v.events.is_empty()).then(|| tile("Events in window", "", v.events.iter().take(4).map(|e| (chart::event_name(e), format!("+{}", fmt_duration(e.t_s as f64)))).collect()))}
                                </div>
                            }.into_any()
                        }}
                    </section>

                    <aside class="bs-runs">
                        <div class="bs-sec-head">
                            <span class="group-title">"Runs on device"</span>
                            <button class="btn btn-xs btn-ghost" title="Refresh" on:click=move |_| refresh_runs()><Icon name="refresh-cw" size=13 /></button>
                        </div>
                        <div class="bs-run-list">
                            {move || {
                                let list = runs.get();
                                if list.is_empty() {
                                    return view! { <div class="bs-muted">"No runs stored."</div> }.into_any();
                                }
                                let open_run = open_run_cached.clone();
                                list.into_iter().rev().map(|r| {
                                    let open_run = open_run.clone();
                                    let id = r.run_id;
                                    let loaded = r.active;
                                    let bytes = r.bytes;
                                    let m = r.meta.unwrap_or_default();
                                    let is_open = move || open.get().map(|o| o.meta.run_id == id && o.dir.contains(&format!("r{id:05}-"))).unwrap_or(false);
                                    let title = if m.name.is_empty() { format!("Run {id}") } else { m.name.clone() };
                                    view! {
                                        <div class="bs-run" class:open=is_open class:active=loaded>
                                            <div class="bs-run-main" title="Open history" on:click=move |_| open_run(Some(id), None)>
                                                <div class="bs-run-title"><b>{format!("#{id}")}</b><span>{title}</span>
                                                    {loaded.then(|| view! { <span class="badge bs-live">"loaded"</span> })}</div>
                                                <div class="bs-run-sub">{format!("{} {}S {} mAh - {} - {}", chem_name(m.params.chem), m.params.cells, m.params.capacity_mah, fmt_date(m.created_epoch), fmt_bytes(bytes))}</div>
                                            </div>
                                            <button class="btn btn-xs" title="Load on device (PAUSED)" disabled=loaded on:click=move |_| act(ACT_LOAD, Some(id))>"Load"</button>
                                            <button class="btn btn-xs btn-ghost" title="Delete from device" disabled=loaded on:click=move |_| confirm_delete.set(Some(id))><Icon name="trash-2" size=13 /></button>
                                        </div>
                                    }
                                }).collect_view().into_any()
                            }}
                        </div>
                    </aside>
                </div>
            </div>

            {move || confirm_delete.get().map(|id| view! {
                <div class="scrim" on:click=move |_| confirm_delete.set(None)></div>
                <div class="dialog popover bs-dialog" role="dialog" aria-modal="true">
                    <div class="group-title">{format!("Delete run #{id} from the device?")}</div>
                    <p class="bs-muted">"The copy on this PC (if any) is kept."</p>
                    <div class="bs-dialog-actions">
                        <button class="btn btn-sm" on:click=move |_| confirm_delete.set(None)>"Cancel"</button>
                        <button class="btn btn-sm btn-danger" on:click=move |_| { confirm_delete.set(None); act(ACT_DELETE, Some(id)); }>"Delete"</button>
                    </div>
                </div>
            })}
            <Show when=move || show_new.get()>
                <NewRunDialog show=show_new status=status error=error on_done=Callback::new(move |_| refresh_runs()) />
            </Show>
        </div>
    }
}

#[component]
fn NewRunDialog(
    show: RwSignal<bool>,
    status: RwSignal<Option<BsStatus>>,
    error: RwSignal<Option<String>>,
    on_done: Callback<()>,
) -> impl IntoView {
    let chem = RwSignal::new(0u8);
    let cells = RwSignal::new(1u8);
    let cap = RwSignal::new(2000u32);
    let soc = RwSignal::new(100.0f64);
    let name = RwSignal::new(String::new());
    let sd = RwSignal::new(false);
    let sd_pct = RwSignal::new(3.0f64);
    let ext = RwSignal::new(false);
    let ext_ua = RwSignal::new(0u32);
    let dither = RwSignal::new(false);
    let working = RwSignal::new(false);
    let _ = status;

    let create = move |_| {
        working.set(true);
        spawn_local(async move {
            #[derive(Serialize)]
            struct C { cfg: BsConfig }
            #[derive(Serialize)]
            #[serde(rename_all = "camelCase")]
            struct A { action: u8, run_id: Option<u16>, slot: Option<u8> }
            let ident = BsConfig { chem: Some(chem.get_untracked()), cells: Some(cells.get_untracked()), capacity_mah: Some(cap.get_untracked()), ..Default::default() };
            let rest = BsConfig {
                start_soc_pct: Some(soc.get_untracked()),
                sd_enable: Some(sd.get_untracked()),
                sd_pct_month: Some(sd_pct.get_untracked()),
                ext_enable: Some(ext.get_untracked()),
                ext_load_ua: Some(ext_ua.get_untracked()),
                dither: Some(dither.get_untracked()),
                name: Some(name.get_untracked()),
                ..Default::default()
            };
            // Chemistry defaults (cutoff, R_int, Peukert, self-discharge) after identity, before overrides.
            let r = async {
                call::<()>("bs_configure", C { cfg: ident }).await?;
                call::<()>("bs_action", A { action: ACT_DEFAULTS, run_id: None, slot: None }).await?;
                call::<()>("bs_configure", C { cfg: rest }).await?;
                call::<()>("bs_action", A { action: ACT_NEW, run_id: None, slot: None }).await
            }.await;
            working.set(false);
            match r {
                Ok(()) => { show.set(false); on_done.run(()); }
                Err(e) => error.set(Some(format!("New run: {e}"))),
            }
        });
    };

    let num = |sig: RwSignal<f64>| move |ev: web_sys::Event| {
        if let Ok(v) = event_target_value(&ev).parse::<f64>() { sig.set(v); }
    };
    view! {
        <div class="scrim" on:click=move |_| show.set(false)></div>
        <div class="dialog popover bs-dialog bs-new" role="dialog" aria-modal="true" aria-labelledby="bs-new-title">
            <div class="group-title" id="bs-new-title">"New battery-sim run"</div>
            <p class="bs-muted">"Creates the run PAUSED with the output off. Press Start when the DUT is connected. A USB-PD contract of 9 V / 3 A is required to start."</p>
            <div class="bs-form">
                <label>"Name"<input type="text" maxlength="23" prop:value=move || name.get() on:input=move |ev| name.set(event_target_value(&ev)) /></label>
                <label>"Chemistry"
                    <select on:change=move |ev| chem.set(event_target_value(&ev).parse().unwrap_or(0))>
                        {CHEM_NAMES.iter().enumerate().map(|(i, n)| view! { <option value=i.to_string() prop:selected=move || chem.get() as usize == i>{*n}</option> }).collect_view()}
                    </select></label>
                <label>"Series cells"<input type="number" min="1" max="14" prop:value=move || cells.get().to_string() on:input=move |ev| cells.set(event_target_value(&ev).parse().unwrap_or(1)) /></label>
                <label>"Capacity (mAh)"<input type="number" min="1" prop:value=move || cap.get().to_string() on:input=move |ev| cap.set(event_target_value(&ev).parse().unwrap_or(1)) /></label>
                <label>"Start SOC (%)"<input type="number" min="0" max="100" step="0.1" prop:value=move || soc.get().to_string() on:input=num(soc) /></label>
                <label class="bs-check"><input type="checkbox" prop:checked=move || sd.get() on:change=move |ev| sd.set(event_target_checked(&ev)) />"Self-discharge"</label>
                <label>"Self-discharge (%/month)"<input type="number" min="0" step="0.1" disabled=move || !sd.get() prop:value=move || sd_pct.get().to_string() on:input=num(sd_pct) /></label>
                <label class="bs-check"><input type="checkbox" prop:checked=move || ext.get() on:change=move |ev| ext.set(event_target_checked(&ev)) />"Virtual external load"</label>
                <label>"External load (uA)"<input type="number" min="0" disabled=move || !ext.get() prop:value=move || ext_ua.get().to_string() on:input=move |ev| ext_ua.set(event_target_value(&ev).parse().unwrap_or(0)) /></label>
                <label class="bs-check"><input type="checkbox" prop:checked=move || dither.get() on:change=move |ev| dither.set(event_target_checked(&ev)) />"Sub-step voltage dithering"</label>
            </div>
            <div class="bs-dialog-actions">
                <button class="btn btn-sm" on:click=move |_| show.set(false)>"Cancel"</button>
                <button class="btn btn-sm btn-primary" disabled=move || working.get() on:click=create>{move || if working.get() { "Creating..." } else { "Create run" }}</button>
            </div>
        </div>
    }
}
