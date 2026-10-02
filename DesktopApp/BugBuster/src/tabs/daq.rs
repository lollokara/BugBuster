// =============================================================================
// daq.rs — High-Speed DAQ (ESP32-P4) instrumentation tab.
//
// Live-streams fused current / voltage / power tracks from the P4 USB-HS port.
// WebGL trace area (daq_gl.rs) with stacked I/V/P lanes, a 2D overlay for axes /
// cursor / shift-select / dI/dt heatmap, a slide-in FFT panel, and a settings
// panel. Works against real hardware or the synthetic "Demo / Mock device".
// =============================================================================

use leptos::prelude::*;
use leptos::task::spawn_local;
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use wasm_bindgen::closure::Closure;
use wasm_bindgen::JsCast;
use web_sys::{CanvasRenderingContext2d, HtmlCanvasElement};

use crate::components::icons::Icon;
use crate::components::ui::{Callout, SegmentedControl, Switch};
use crate::tabs::daq_gl::{GlRenderer, Lane};
use crate::tabs::daq_cal::CalibrationWizard;
use crate::tabs::daq_trigger_panel::TriggerPanel;
use crate::tauri_bridge::*;
use crate::theme::{css_var, use_theme};

const LABEL_W: f64 = 70.0;
const RULER_H: f64 = 20.0;
const HEATMAP_H: f64 = 34.0;
const TOP_PAD: f64 = 10.0;
const LANE_GAP: f64 = 8.0;

const SAMPLE_RATE_LABELS: [&str; 5] = ["10 kSPS", "50 kSPS", "100 kSPS", "250 kSPS", "1 MSPS"];
const SAMPLE_RATE_SHORT: [&str; 5] = ["10k", "50k", "100k", "250k", "1M"];

/// Engineering-notation formatter (e.g. 12.3 mA).
fn fmt_eng(value: f64, unit: &str) -> String {
    let a = value.abs();
    let (scaled, prefix) = if a >= 1e6 {
        (value / 1e6, "M")
    } else if a >= 1e3 {
        (value / 1e3, "k")
    } else if a >= 1.0 || a == 0.0 {
        (value, "")
    } else if a >= 1e-3 {
        (value * 1e3, "m")
    } else if a >= 1e-6 {
        (value * 1e6, "µ")
    } else {
        (value * 1e9, "n")
    };
    format!("{:.3} {}{}", scaled, prefix, unit)
}

fn fmt_time(s: f64) -> String {
    if s >= 1.0 {
        format!("{:.3} s", s)
    } else if s >= 1e-3 {
        format!("{:.3} ms", s * 1e3)
    } else {
        format!("{:.3} µs", s * 1e6)
    }
}

/// Split an engineering-formatted reading ("12.300 mA") into value and unit.
fn split_val(s: String) -> (String, String) {
    match s.split_once(' ') {
        Some((v, u)) => (v.to_string(), u.to_string()),
        None => (s, String::new()),
    }
}

/// Placeholder for a reading that is not available yet.
const NO_VALUE: &str = "\u{2013}";

fn nice_range(lo: f32, hi: f32) -> (f32, f32) {
    let mut lo = lo;
    let mut hi = hi;
    if !lo.is_finite() || !hi.is_finite() {
        return (0.0, 1.0);
    }
    if (hi - lo).abs() < 1e-9 {
        let pad = if hi.abs() < 1e-9 { 1.0 } else { hi.abs() * 0.1 };
        lo -= pad;
        hi += pad;
    } else {
        let pad = (hi - lo) * 0.08;
        lo -= pad;
        hi += pad;
    }
    (lo, hi)
}

#[derive(Clone, Copy)]
struct TrackInfo {
    name: &'static str,
    unit: &'static str,
    /// Suffix of the `.dq-dot.s-*` class that carries the series colour.
    key: &'static str,
}

const TRACK_I: TrackInfo = TrackInfo { name: "Current", unit: "A", key: "current" };
const TRACK_V: TrackInfo = TrackInfo { name: "Voltage", unit: "V", key: "voltage" };
const TRACK_P: TrackInfo = TrackInfo { name: "Power", unit: "W", key: "power" };

/// Canvas colours resolved from the active theme's CSS tokens.
#[derive(Clone)]
struct Palette {
    font: String,
    bg: String,
    grid: String,
    axis: String,
    text: String,
    surface: String,
    sep: String,
    accent: String,
    accent_tint: String,
    accent_tint_strong: String,
    marker: String,
    warn: String,
    current: String,
    voltage: String,
    power: String,
    coarse: String,
    blend: String,
}

fn tok(name: &str) -> String {
    let v = css_var(name);
    if v.is_empty() {
        "gray".to_string()
    } else {
        v
    }
}

impl Palette {
    fn read() -> Self {
        let font = css_var("--font-ui");
        Palette {
            font: if font.is_empty() { "sans-serif".to_string() } else { font },
            bg: tok("--surface-plot"),
            grid: tok("--grid-line"),
            axis: tok("--label-2"),
            text: tok("--label-1"),
            surface: tok("--surface-1"),
            sep: tok("--sep-strong"),
            accent: tok("--accent"),
            accent_tint: tok("--accent-tint"),
            accent_tint_strong: tok("--accent-tint-strong"),
            marker: tok("--series-marker"),
            warn: tok("--c-orange-text"),
            current: tok("--series-current"),
            voltage: tok("--series-voltage"),
            power: tok("--series-power"),
            coarse: tok("--c-blue"),
            blend: tok("--c-teal"),
        }
    }

    fn series(&self, name: &str) -> &str {
        match name {
            "Current" => &self.current,
            "Voltage" => &self.voltage,
            _ => &self.power,
        }
    }

    fn font(&self, size: u32) -> String {
        format!("{}px {}", size, self.font)
    }
}

thread_local! {
    static PALETTE: RefCell<Option<(String, Palette)>> = RefCell::new(None);
}

/// Theme palette, re-read from CSS only when `data-theme` changes.
fn palette() -> Palette {
    let theme = web_sys::window()
        .and_then(|w| w.document())
        .and_then(|d| d.document_element())
        .and_then(|e| e.get_attribute("data-theme"))
        .unwrap_or_default();
    PALETTE.with(|c| {
        let mut c = c.borrow_mut();
        if let Some((t, p)) = c.as_ref() {
            if *t == theme {
                return p.clone();
            }
        }
        let p = Palette::read();
        *c = Some((theme, p.clone()));
        p
    })
}

/// `#rrggbb` / `#rgb` token value to 0..1 floats for WebGL.
fn hex_rgb(c: &str) -> [f32; 3] {
    let h = c.trim().trim_start_matches('#');
    let p = |s: &str| {
        u8::from_str_radix(s, 16)
            .map(|v| v as f32 / 255.0)
            .unwrap_or(0.5)
    };
    match h.len() {
        6 | 8 => [p(&h[0..2]), p(&h[2..4]), p(&h[4..6])],
        3 => {
            let d = |i: usize| p(&format!("{0}{0}", &h[i..i + 1]));
            [d(0), d(1), d(2)]
        }
        _ => [0.5, 0.5, 0.5],
    }
}

#[component]
pub fn DaqTab(state: ReadSignal<crate::tauri_bridge::DeviceState>) -> impl IntoView {
    let _ = state; // DAQ uses its own USB transport; device_state not required.

    // ---- Stream / view state ------------------------------------------------
    let status = RwSignal::new(Option::<DaqStreamRuntimeStatus>::None);
    let snapshots = RwSignal::new(Option::<DaqSnapshots>::None);
    let view_data = RwSignal::new(Option::<DaqViewData>::None);
    let overview_data = RwSignal::new(Option::<DaqViewData>::None);
    let streaming = RwSignal::new(false);
    let total_samples = RwSignal::new(0u64);
    let rate_hz = RwSignal::new(250_000u32);
    let view_start = RwSignal::new(0u64);
    let view_end = RwSignal::new(0u64);
    // Autofocus: -2 = full capture (grows live), -1 = manual, >0 = last N seconds.
    let autofocus = RwSignal::new(-2i64);

    // ---- Track toggles ------------------------------------------------------
    let show_i = RwSignal::new(true);
    let show_v = RwSignal::new(true);
    let show_p = RwSignal::new(true);
    let tint_source = RwSignal::new(true);
    let combined = RwSignal::new(false);
    // Display low-pass filter: centered moving-average window in samples (1 = off).
    let smooth_window = RwSignal::new(1u32);
    // Display filter type: 0=none, 1=moving avg, 2=EMA, 3=median, 4=high-pass.
    let filter_type = RwSignal::new(1u8);
    // Hi-res filter: when on, the backend filters the raw signal before
    // decimation (zoom-stable, undistorted) for viewports in the raw path; when
    // off, a lightweight filter is applied to the decimated envelope in the
    // frontend (cheaper, but smooths the min/max band rather than the signal).
    let raw_filter = RwSignal::new(true);

    // ---- Selection / hover --------------------------------------------------
    let sel_anchor = RwSignal::new(Option::<u64>::None);
    let sel_start = RwSignal::new(Option::<u64>::None);
    let sel_end = RwSignal::new(Option::<u64>::None);
    let integral = RwSignal::new(Option::<DaqIntegral>::None);
    let hover = RwSignal::new(Option::<(f64, f64)>::None);
    let drag_mode = RwSignal::new(0u8); // 0 = none, 1 = select, 2 = minimap
    let drag_moved = RwSignal::new(false);

    // ---- Panels -------------------------------------------------------------
    // Right inspector content: 0 = closed, 1 = settings, 2 = FFT, 3 = triggers.
    let inspector = RwSignal::new(1u8);
    // Performance metrics overlay.
    let perf_open = RwSignal::new(false);
    let fps = RwSignal::new(0.0f64);
    let fetch_ms = RwSignal::new(0.0f64);
    let cal_open = RwSignal::new(false);

    // ---- Acquisition / source settings -------------------------------------
    let sample_rate_idx = RwSignal::new(3u8);    // 250 kSPS default
    let voltage_rate_idx = RwSignal::new(1u8);   // 50 kSPS default (voltage ADC)
    let decimation = RwSignal::new(1u16);        // USB decimation 1..256
    let hw_filter_idx = RwSignal::new(0u8);      // 0=Wideband 1=Sinc5 2=Sinc3
    let hw_decim_idx = RwSignal::new(3u8);       // 0=×32 .. 5=×1024 (default ×256)
    let sr_mode = RwSignal::new(false);          // Super Resolution (1 ksps I / 500 sps V)
    let range_lock_idx = RwSignal::new(0u8); // 0 = Auto, 1..3 = HI/MID/LO+1
    let vdut_mv = RwSignal::new(3300u32);
    let ilimit_ma = RwSignal::new(500u32);
    let source_enable = RwSignal::new(true);
    let fft_nbins = RwSignal::new(256u16);
    let fft_window = RwSignal::new(1u8);
    let fft_source = RwSignal::new(0u8);
    // Non-overridable USB-PD guard warning (DUT enable / current cal).
    let pd_warn = RwSignal::new(String::new());

    // ---- Canvas + renderer --------------------------------------------------
    let gl_canvas = NodeRef::<leptos::html::Canvas>::new();
    let overlay = NodeRef::<leptos::html::Canvas>::new();
    let renderer: Rc<RefCell<Option<GlRenderer>>> = Rc::new(RefCell::new(None));
    // Counts completed full repaints, for the render-FPS metric.
    let frame_counter = Rc::new(Cell::new(0u64));

    let alive = Arc::new(AtomicBool::new(true));
    on_cleanup({
        let alive = alive.clone();
        move || alive.store(false, Ordering::SeqCst)
    });

    // Build the GL renderer once the canvas mounts.
    {
        let renderer = renderer.clone();
        Effect::new(move |_| {
            if renderer.borrow().is_some() {
                return;
            }
            if let Some(c) = gl_canvas.get() {
                let canvas: HtmlCanvasElement = c.unchecked_into();
                *renderer.borrow_mut() = GlRenderer::new(&canvas);
            }
        });
    }

    // ---- Rendering ----------------------------------------------------------
    // GL trace pass — heavier; only re-runs on data / track / layout changes.
    let render_gl = {
        let renderer = renderer.clone();
        let alive = alive.clone();
        Rc::new(move || {
            // The window resize listener outlives the tab; never touch disposed refs.
            if !alive.load(Ordering::SeqCst) {
                return;
            }
            let Some(gl_c) = gl_canvas.get_untracked() else { return };
            let Some(ov_c) = overlay.get_untracked() else { return };
            let gl_canvas_el: HtmlCanvasElement = gl_c.unchecked_into();
            let ov_canvas: HtmlCanvasElement = ov_c.unchecked_into();
            let dpr = web_sys::window()
                .map(|w| w.device_pixel_ratio())
                .unwrap_or(1.0);
            let css_w = ov_canvas.client_width() as f64;
            let css_h = ov_canvas.client_height() as f64;
            if css_w < 2.0 || css_h < 2.0 {
                return;
            }
            let pw = (css_w * dpr) as u32;
            let ph = (css_h * dpr) as u32;
            for c in [&gl_canvas_el, &ov_canvas] {
                if c.width() != pw {
                    c.set_width(pw);
                }
                if c.height() != ph {
                    c.set_height(ph);
                }
            }
            let x0 = (LABEL_W * dpr) as f32;
            let x1 = (css_w * dpr) as f32;
            let pal = palette();
            let bg = hex_rgb(&pal.bg);
            let Some(raw) = view_data.get_untracked() else {
                if let Some(rr) = renderer.borrow().as_ref() {
                    rr.render(pw as f32, ph as f32, x0, x1, bg, &[]);
                }
                return;
            };
            // When hi-res is off the backend returned the raw envelope, so we
            // apply the lightweight envelope filter here; when on, the backend
            // already filtered the raw signal — draw it as-is.
            let vd = if raw_filter.get_untracked() {
                raw
            } else {
                apply_display_filter(
                    raw,
                    smooth_window.get_untracked(),
                    filter_type.get_untracked(),
                )
            };
            let tracks = visible_tracks(
                show_i.get_untracked(),
                show_v.get_untracked(),
                show_p.get_untracked(),
            );
            let comb = combined.get_untracked();
            let regions = lane_regions(TOP_PAD, css_h - RULER_H - HEATMAP_H, tracks.len(), comb);
            let tint_cur = tint_source.get_untracked();
            let mut gl_lanes: Vec<Lane> = Vec::new();
            for (i, t) in tracks.iter().enumerate() {
                let (vmin, vmax, src) = track_cols(&vd, t.name);
                if vmin.is_empty() {
                    continue;
                }
                let gap_slice = if vd.gap.len() == vmin.len() {
                    Some(vd.gap.as_slice())
                } else {
                    None
                };
                let (lo, hi) = col_range_gapped(t.name, vmin, vmax, gap_slice);
                let (y_top, y_bottom) = regions[i];
                let tint = t.name == "Current" && tint_cur;
                gl_lanes.push(Lane {
                    y_top: (y_top * dpr) as f32,
                    y_bottom: (y_bottom * dpr) as f32,
                    vmin,
                    vmax,
                    source: if tint { src } else { None },
                    gap: gap_slice,
                    lo,
                    hi,
                    color: hex_rgb(pal.series(t.name)),
                    coarse: hex_rgb(&pal.coarse),
                    blend: hex_rgb(&pal.blend),
                    tint,
                });
            }
            if let Some(rr) = renderer.borrow().as_ref() {
                rr.render(pw as f32, ph as f32, x0, x1, bg, &gl_lanes);
            }
        })
    };

    // Overlay (2D) pass — cheap; re-runs on hover / selection too.
    let paint_overlay = {
        let alive = alive.clone();
        Rc::new(move || {
        if !alive.load(Ordering::SeqCst) {
            return;
        }
        let Some(ov_c) = overlay.get_untracked() else { return };
        let ov_canvas: HtmlCanvasElement = ov_c.unchecked_into();
        let dpr = web_sys::window()
            .map(|w| w.device_pixel_ratio())
            .unwrap_or(1.0);
        let css_w = ov_canvas.client_width() as f64;
        let css_h = ov_canvas.client_height() as f64;
        if css_w < 2.0 || css_h < 2.0 {
            return;
        }
        let pw = (css_w * dpr) as u32;
        let ph = (css_h * dpr) as u32;
        if ov_canvas.width() != pw {
            ov_canvas.set_width(pw);
        }
        if ov_canvas.height() != ph {
            ov_canvas.set_height(ph);
        }
        let tracks = visible_tracks(
            show_i.get_untracked(),
            show_v.get_untracked(),
            show_p.get_untracked(),
        );
        let comb = combined.get_untracked();
        let regions = lane_regions(TOP_PAD, css_h - RULER_H - HEATMAP_H, tracks.len(), comb);
        let vd = view_data.get_untracked().map(|raw| {
            if raw_filter.get_untracked() {
                raw
            } else {
                apply_display_filter(
                    raw,
                    smooth_window.get_untracked(),
                    filter_type.get_untracked(),
                )
            }
        });
        let ov = overview_data.get_untracked();
        draw_overlay(
            &ov_canvas,
            dpr,
            css_w,
            css_h,
            &tracks,
            &regions,
            comb,
            vd.as_ref(),
            ov.as_ref(),
            sel_start.get_untracked(),
            sel_end.get_untracked(),
            hover.get_untracked(),
            drag_mode.get_untracked() != 0,
        );
        })
    };

    let render_all = {
        let g = render_gl.clone();
        let o = paint_overlay.clone();
        let fc = frame_counter.clone();
        Rc::new(move || {
            fc.set(fc.get() + 1);
            g();
            o();
        })
    };

    // Derive the display-filtered view from the raw envelope. Re-runs only when
    // the raw data, filter type, or window changes — never on the live polling
    // path unless `view_data` actually changed — and only touches ~1800 columns.
    // Effect A: full repaint when data / filter / tracks / layout change. The
    // filter itself is applied inside `render_gl` / `paint_overlay`, so tracking
    // `filter_type` + `smooth_window` here is what makes a filter change repaint.
    {
        let render_all = render_all.clone();
        Effect::new(move |_| {
            view_data.track();
            filter_type.track();
            smooth_window.track();
            raw_filter.track();
            combined.track();
            show_i.track();
            show_v.track();
            show_p.track();
            tint_source.track();
            render_all();
        });
    }
    // Effect B: cheap overlay-only repaint for hover / selection / minimap.
    {
        let paint = paint_overlay.clone();
        Effect::new(move |_| {
            hover.track();
            sel_start.track();
            sel_end.track();
            overview_data.track();
            paint();
        });
    }
    // A theme flip or inspector toggle changes colours / plot width without new
    // data; repaint once the new CSS (data-theme, layout) has applied.
    {
        let render_all = render_all.clone();
        let theme = use_theme();
        Effect::new(move |_| {
            theme.resolved.track();
            inspector.track();
            let render_all = render_all.clone();
            spawn_local(async move {
                slp(40).await;
                render_all();
            });
        });
    }
    // Repaint on window resize.
    {
        let render_all = render_all.clone();
        let closure = Closure::<dyn FnMut()>::new(move || render_all());
        if let Some(w) = web_sys::window() {
            let _ = w
                .add_event_listener_with_callback("resize", closure.as_ref().unchecked_ref());
        }
        closure.forget();
    }

    // ---- Data refresh helpers (drop overlapping fetches for smoothness) -----
    let view_inflight = Rc::new(Cell::new(false));
    // Last fetched view key (start, end, backend-smooth, backend-filter). The
    // backend only filters when the hi-res toggle is on (else it returns the raw
    // envelope and the frontend filters cheaply). Skipping identical keys stops
    // the backend re-decimating the same viewport ~30×/s while paused or idle.
    let view_key = Rc::new(Cell::new((u64::MAX, u64::MAX, u32::MAX, u8::MAX)));
    let refresh_view = {
        let g = view_inflight.clone();
        let key = view_key.clone();
        let alive = alive.clone();
        Rc::new(move || {
            if g.get() {
                return;
            }
            let vs = view_start.get_untracked();
            let ve = view_end.get_untracked();
            if ve <= vs {
                return;
            }
            // Only ask the backend to filter when hi-res is on; otherwise fetch
            // the raw envelope and let the frontend apply the display filter.
            let (bs, bf) = if raw_filter.get_untracked() {
                (smooth_window.get_untracked(), filter_type.get_untracked())
            } else {
                (1u32, 0u8)
            };
            let k = (vs, ve, bs, bf);
            if key.get() == k {
                return;
            }
            g.set(true);
            let g2 = g.clone();
            let key2 = key.clone();
            let alive = alive.clone();
            let t0 = js_sys::Date::now();
            spawn_local(async move {
                let vd = daq_get_view(vs, ve, 1800, bs, bf).await;
                g2.set(false);
                if !alive.load(Ordering::SeqCst) {
                    return;
                }
                if let Some(vd) = vd {
                    view_data.set(Some(vd));
                    key2.set(k);
                }
                fetch_ms.set(js_sys::Date::now() - t0);
            });
        })
    };
    let ov_inflight = Rc::new(Cell::new(false));
    let refresh_overview = {
        let g = ov_inflight.clone();
        let alive = alive.clone();
        Rc::new(move || {
            if g.get() {
                return;
            }
            let total = total_samples.get_untracked();
            if total < 2 {
                return;
            }
            g.set(true);
            let g2 = g.clone();
            let alive = alive.clone();
            spawn_local(async move {
                let vd = daq_get_view(0, total, 1000, 1, 0).await;
                g2.set(false);
                if !alive.load(Ordering::SeqCst) {
                    return;
                }
                if let Some(vd) = vd {
                    overview_data.set(Some(vd));
                }
            });
        })
    };

    // ---- Polling loop -------------------------------------------------------
    {
        let alive = alive.clone();
        let refresh_view = refresh_view.clone();
        let refresh_overview = refresh_overview.clone();
        let frame_counter = frame_counter.clone();
        spawn_local(async move {
            // Auto-connect to a real DAQ if present (mock connect handled by panel).
            if let Some(st) = daq_stream_status().await {
                if !st.connected && daq_check_usb().await {
                    daq_connect(false).await;
                }
            }
            let mut tick = 0u32;
            let mut perf_last = js_sys::Date::now();
            let mut perf_frames = 0u64;
            loop {
                if !alive.load(Ordering::SeqCst) {
                    break;
                }
                // Status + aggregate snapshots at ~5 Hz (cheaper than the view).
                // Every await below can outlive the tab, after which the signals
                // are disposed: re-check `alive` before touching them.
                if tick % 4 == 0 {
                    let st = daq_stream_status().await;
                    if !alive.load(Ordering::SeqCst) {
                        break;
                    }
                    if let Some(st) = st {
                        total_samples.set(st.total_samples);
                        streaming.set(st.active);
                        if st.sample_rate_hz > 0 {
                            rate_hz.set(st.sample_rate_hz);
                        }
                        status.set(Some(st));
                    }
                    let snap = daq_get_snapshots().await;
                    if !alive.load(Ordering::SeqCst) {
                        break;
                    }
                    if let Some(snap) = snap {
                        if snap.sample_rate_hz > 0 {
                            rate_hz.set(snap.sample_rate_hz);
                        }
                        snapshots.set(Some(snap));
                    }
                }
                let total = total_samples.get_untracked();
                if total > 0 {
                    let rate = rate_hz.get_untracked().max(1) as f64;
                    match autofocus.get_untracked() {
                        // Full capture — view the whole buffer, growing live.
                        -2 => {
                            view_start.set(0);
                            view_end.set(total);
                        }
                        // Manual — user navigates; just clamp to the buffer.
                        -1 => {
                            let ve = view_end.get_untracked();
                            if ve == 0 {
                                view_end.set(total);
                            } else if ve > total {
                                let span = ve - view_start.get_untracked();
                                view_end.set(total);
                                view_start.set(total.saturating_sub(span));
                            }
                        }
                        // Autofocus on the last N seconds.
                        secs => {
                            let win = ((secs as f64) * rate).max(2.0) as u64;
                            view_end.set(total);
                            view_start.set(total.saturating_sub(win));
                        }
                    }
                    refresh_view();
                    if tick % 6 == 0 {
                        refresh_overview();
                    }
                }
                // Render-FPS + perf log once per second.
                let now_p = js_sys::Date::now();
                if now_p - perf_last >= 1000.0 {
                    let frames = frame_counter.get();
                    let dt = (now_p - perf_last) / 1000.0;
                    let f = (frames.saturating_sub(perf_frames)) as f64 / dt.max(1e-3);
                    fps.set(f);
                    perf_frames = frames;
                    perf_last = now_p;
                    if streaming.get_untracked() {
                        let (ing, tot) = status
                            .get_untracked()
                            .map(|s| (s.ingest_sps, s.total_samples))
                            .unwrap_or((0.0, 0));
                        web_sys::console::log_1(
                            &format!(
                                "[DAQ perf] ingest={:.2} MSa/s | render={:.0} fps | fetch={:.1} ms | total={:.2} M ({:.1} MB)",
                                ing / 1e6,
                                f,
                                fetch_ms.get_untracked(),
                                tot as f64 / 1e6,
                                tot as f64 * 15.0 / 1e6,
                            )
                            .into(),
                        );
                    }
                }
                tick = tick.wrapping_add(1);
                slp(33).await;
            }
        });
    }

    // Fetch integral whenever the selection changes.
    {
        let alive = alive.clone();
        Effect::new(move |_| {
            let (s, e) = (sel_start.get(), sel_end.get());
            if let (Some(s), Some(e)) = (s, e) {
                if e > s {
                    let alive = alive.clone();
                    spawn_local(async move {
                        let r = daq_get_integral(s, e).await;
                        if !alive.load(Ordering::SeqCst) {
                            return;
                        }
                        if let Some(r) = r {
                            integral.set(Some(r));
                        }
                    });
                    return;
                }
            }
            integral.set(None);
        });
    }

    // ---- Control actions ----------------------------------------------------
    // DAQ_K_SAMPLE_RATE_IDX key = DAQ_KEY(GRP_ACQ=0x01, idx=0x03) = 0x0103.
    // CMD_SET_RATE (0x82) is currently unimplemented in the P4 firmware; the
    // only working path to change the ADAQ hardware ODR is via this BBP config
    // key which triggers apply_sample_rate() on the P4.
    const DAQ_K_SAMPLE_RATE_IDX: u16 = 0x0103;

    let start_stream = move |_| {
        let idx = sample_rate_idx.get_untracked();
        let vidx = voltage_rate_idx.get_untracked();
        let dec = decimation.get_untracked();
        // Fresh capture — view the whole thing as it grows.
        autofocus.set(-2);
        sel_start.set(None);
        sel_end.set(None);
        sel_anchor.set(None);
        view_start.set(0);
        view_end.set(0);
        spawn_local(async move {
            // Do NOT call daq_cfg_set_enum here. That BBP call triggers
            // apply_sample_rate → daq_board_stop_fast (170ms!) on the P4
            // BEFORE streaming starts, racing with USB CMD_START. Users who
            // need a specific ODR should use the Rate buttons (apply_rate)
            // which safely reconfigure hardware while the stream is idle.
            // The P4 boots with bind_settings ODR already applied.
            daq_stream_start(idx, vidx, dec).await;
        });
    };
    let stop_stream = move |_| {
        spawn_local(async move {
            daq_stream_stop().await;
        });
    };

    let apply_source = move || {
        let (v, il, en) = (
            vdut_mv.get_untracked(),
            ilimit_ma.get_untracked(),
            source_enable.get_untracked(),
        );
        spawn_local(async move {
            if en {
                // Only block if we have a definitive PD status that shows
                // insufficient power. If the BBP call times out (pd = None),
                // pass the command to the firmware — it enforces the same PD
                // requirement and will suppress the output if needed.
                if let Some(pd) = fetch_usbpd_status().await {
                    let ok = pd.attached && pd.voltage_v >= 9.0 && pd.current_a >= 3.0;
                    if !ok {
                        let msg = if !pd.present {
                            "Blocked: USB-PD controller (HUSB238) not detected on I2C".to_string()
                        } else if !pd.attached {
                            "Blocked: no USB-PD source connected. Plug in a USB-C PD adapter (at least 9 V / 3 A)".to_string()
                        } else {
                            format!(
                                "Blocked: USB-PD source too weak ({:.1} V / {:.1} A). Need at least 9 V / 3 A",
                                pd.voltage_v, pd.current_a
                            )
                        };
                        pd_warn.set(msg);
                        source_enable.set(false);
                        return;
                    }
                }
                // BBP timed out (None) — let the firmware decide.
            }
            pd_warn.set(String::new());
            daq_set_source(v, il, en).await;
        });
    };
    let apply_range = move || {
        let idx = range_lock_idx.get_untracked();
        let range = if idx == 0 { 0xFF } else { idx - 1 };
        let label = ["Auto", "HI \u{b5}A", "MID mA", "LO A"].get(idx as usize).copied().unwrap_or("Auto");
        spawn_local(async move {
            daq_set_range_lock(range).await;
            show_toast(&format!("Range lock: {}", label), "ok");
        });
    };
    let apply_rate = move || {
        let (idx, vidx, dec) = (
            sample_rate_idx.get_untracked(),
            voltage_rate_idx.get_untracked(),
            decimation.get_untracked(),
        );
        let label = ["10k", "50k", "100k", "250k", "1M"].get(idx as usize).copied().unwrap_or("?");
        spawn_local(async move {
            // Set hardware sample rate via BBP (the only working path on P4).
            let cfg_ok = daq_cfg_set_enum(DAQ_K_SAMPLE_RATE_IDX, idx).await;
            // Also send CMD_SET_RATE for future firmware compatibility.
            daq_set_rate(idx, vidx, dec).await;
            if cfg_ok {
                show_toast(&format!("Rate set: {} SPS (restart stream to apply)", label), "ok");
            } else {
                show_toast("Rate: USB command sent; BBP config unavailable - retry", "err");
            }
        });
    };
    let apply_fft = move || {
        let (n, w, s, en) = (
            fft_nbins.get_untracked(),
            fft_window.get_untracked(),
            fft_source.get_untracked(),
            inspector.get_untracked() == 2,
        );
        spawn_local(async move {
            daq_set_fft(n, s, w, en).await;
        });
    };

    // Convert a canvas-relative mouse X to a sample index within the view.
    let x_to_sample = move |mx: f64, css_w: f64| -> u64 {
        let plot_w = (css_w - LABEL_W).max(1.0);
        let frac = ((mx - LABEL_W) / plot_w).clamp(0.0, 1.0);
        let vs = view_start.get_untracked();
        let ve = view_end.get_untracked();
        vs + (frac * (ve.saturating_sub(vs)) as f64) as u64
    };
    // Center the view on a full-capture fraction (minimap navigation).
    let center_view_full = move |mx: f64, css_w: f64| {
        let plot_w = (css_w - LABEL_W).max(1.0);
        let frac = ((mx - LABEL_W) / plot_w).clamp(0.0, 1.0);
        let total = total_samples.get_untracked();
        let span = view_end
            .get_untracked()
            .saturating_sub(view_start.get_untracked())
            .max(2);
        let center = (frac * total as f64) as u64;
        let half = span / 2;
        let mut vs = center.saturating_sub(half);
        let mut ve = vs + span;
        if ve > total {
            ve = total;
            vs = total.saturating_sub(span);
        }
        view_start.set(vs);
        view_end.set(ve);
    };
    // Mouse geometry: (x, y, width, height) relative to the overlay canvas.
    let geom = move |ev: &leptos::ev::MouseEvent| -> Option<(f64, f64, f64, f64)> {
        let c = overlay.get()?;
        let canvas: HtmlCanvasElement = c.unchecked_into();
        let rect = canvas.get_bounding_client_rect();
        Some((
            ev.client_x() as f64 - rect.left(),
            ev.client_y() as f64 - rect.top(),
            rect.width(),
            rect.height(),
        ))
    };

    // ---- Mouse handlers (on overlay) ---------------------------------------
    let on_mousedown = {
        let refresh_view = refresh_view.clone();
        move |ev: leptos::ev::MouseEvent| {
            let Some((mx, my, css_w, css_h)) = geom(&ev) else {
                return;
            };
            if mx < LABEL_W {
                return;
            }
            if my >= css_h - HEATMAP_H {
                // Minimap strip: jump / navigate the whole capture.
                autofocus.set(-1);
                center_view_full(mx, css_w);
                drag_mode.set(2);
                refresh_view();
            } else {
                // Plot: drag to select a segment.
                let s = x_to_sample(mx, css_w);
                sel_anchor.set(Some(s));
                sel_start.set(Some(s));
                sel_end.set(Some(s));
                drag_mode.set(1);
                drag_moved.set(false);
            }
        }
    };
    let on_mousemove = {
        let refresh_view = refresh_view.clone();
        move |ev: leptos::ev::MouseEvent| {
            let Some((mx, my, css_w, _css_h)) = geom(&ev) else {
                return;
            };
            hover.set(Some((mx, my)));
            match drag_mode.get_untracked() {
                1 => {
                    let s = x_to_sample(mx, css_w);
                    if let Some(a) = sel_anchor.get_untracked() {
                        sel_start.set(Some(a.min(s)));
                        sel_end.set(Some(a.max(s)));
                    }
                    drag_moved.set(true);
                }
                2 => {
                    center_view_full(mx, css_w);
                    refresh_view();
                }
                _ => {}
            }
        }
    };
    let on_mouseup = move |_ev: leptos::ev::MouseEvent| {
        // A plain click (no drag) in the plot clears the selection.
        if drag_mode.get_untracked() == 1 && !drag_moved.get_untracked() {
            sel_anchor.set(None);
            sel_start.set(None);
            sel_end.set(None);
        }
        drag_mode.set(0);
    };
    let on_mouseleave = move |_ev: leptos::ev::MouseEvent| {
        hover.set(None);
        drag_mode.set(0);
    };
    let on_wheel = {
        let refresh_view = refresh_view.clone();
        move |ev: leptos::ev::WheelEvent| {
            ev.prevent_default();
            // Geometry of the cursor over the plot.
            let Some(c) = overlay.get() else { return };
            let canvas: HtmlCanvasElement = c.unchecked_into();
            let rect = canvas.get_bounding_client_rect();
            let mx = ev.client_x() as f64 - rect.left();
            let css_w = rect.width();
            let plot_w = (css_w - LABEL_W).max(1.0);
            let frac = ((mx - LABEL_W) / plot_w).clamp(0.0, 1.0);

            autofocus.set(-1);
            let total = total_samples.get_untracked();
            let vs = view_start.get_untracked();
            let ve = view_end.get_untracked();
            let span = (ve.saturating_sub(vs)).max(2) as f64;
            // Normalise the scroll delta across mice/trackpads (pixel vs line vs
            // page mode) so zoom feels consistent, then apply a gentle, smooth
            // exponential step (down = zoom out).
            let unit = match ev.delta_mode() {
                1 => 16.0,  // lines → px
                2 => 400.0, // pages → px
                _ => 1.0,   // already px
            };
            let dy = (ev.delta_y() * unit).clamp(-240.0, 240.0);
            let factor = 1.0012_f64.powf(dy);
            let new_span = (span * factor).clamp(32.0, total.max(32) as f64);
            // Keep the sample under the cursor fixed while zooming.
            let anchor = vs as f64 + frac * span;
            let ns = new_span as u64;
            let mut nvs = (anchor - frac * new_span).max(0.0) as u64;
            let mut nve = nvs + ns;
            if nve > total {
                nve = total;
                nvs = total.saturating_sub(ns);
            }
            view_start.set(nvs);
            view_end.set(nve);
            refresh_view();
        }
    };

    // Toggle an inspector panel; selecting the open one closes the inspector.
    // The FFT engine only runs while its panel is showing.
    let toggle_panel = move |p: u8| {
        inspector.update(|c| *c = if *c == p { 0 } else { p });
    };
    {
        let alive = alive.clone();
        Effect::new(move |prev: Option<bool>| {
            let open = inspector.get() == 2;
            if prev.is_some() || open {
                if prev != Some(open) {
                    let (n, w, s) = (
                        fft_nbins.get_untracked(),
                        fft_window.get_untracked(),
                        fft_source.get_untracked(),
                    );
                    let alive = alive.clone();
                    spawn_local(async move {
                        if alive.load(Ordering::SeqCst) {
                            daq_set_fft(n, s, w, open).await;
                        }
                    });
                }
            }
            open
        });
    }
    let fft_panel_open = Signal::derive(move || inspector.get() == 2);
    let trig_panel_open = Signal::derive(move || inspector.get() == 3);

    view! {
        <div class="dq-root">
            // Control bar
            <div class="dq-bar" role="toolbar" aria-label="Acquisition controls">
                {move || if streaming.get() {
                    view!{
                        <button class="btn btn-sm btn-tinted tone-red dq-run" title="Stop streaming" on:click=stop_stream>
                            <Icon name="square" size=13 />"Stop"
                        </button>
                    }.into_any()
                } else {
                    view!{
                        <button class="btn btn-sm btn-primary dq-run" title="Start streaming" on:click=start_stream>
                            <Icon name="play" size=13 />"Run"
                        </button>
                    }.into_any()
                }}
                <label class="dq-ctl" title="Sample rate for the next run">
                    <span class="dq-ctl-label">"Rate"</span>
                    <select class="dropdown dropdown-sm" aria-label="Sample rate" on:change=move |ev| {
                        let v: u8 = event_target_value(&ev).parse().unwrap_or(3);
                        sample_rate_idx.set(v);
                    }>
                        {SAMPLE_RATE_LABELS.iter().enumerate().map(|(i,l)| {
                            view!{ <option value=i.to_string() selected=move || sample_rate_idx.get() as usize == i>{*l}</option> }
                        }).collect::<Vec<_>>()}
                    </select>
                </label>
                <label class="dq-ctl" title="Autofocus the view on the most recent data, or show the whole capture">
                    <span class="dq-ctl-label">"Focus"</span>
                    <select class="dropdown dropdown-sm" aria-label="Autofocus" on:change=move |ev| {
                        let v: i64 = event_target_value(&ev).parse().unwrap_or(-2);
                        autofocus.set(v);
                    }>
                        <option value="-2" selected=move || autofocus.get() == -2>"Full capture"</option>
                        <option value="10" selected=move || autofocus.get() == 10>"Last 10 s"</option>
                        <option value="30" selected=move || autofocus.get() == 30>"Last 30 s"</option>
                        <option value="60" selected=move || autofocus.get() == 60>"Last 1 min"</option>
                        <option value="300" selected=move || autofocus.get() == 300>"Last 5 min"</option>
                        <option value="-1" selected=move || autofocus.get() == -1>"Manual"</option>
                    </select>
                </label>
                <span class="dq-sep" aria-hidden="true"></span>
                // Series visibility
                <div class="dq-chips" role="group" aria-label="Visible traces">
                    <SeriesChip sig=show_i info=TRACK_I/>
                    <SeriesChip sig=show_v info=TRACK_V/>
                    <SeriesChip sig=show_p info=TRACK_P/>
                </div>
                <button type="button" class="dq-chip" class:on=move || tint_source.get()
                    aria-pressed=move || if tint_source.get() { "true" } else { "false" }
                    title="Tint the current trace by fusion source (fine, coarse, blend)"
                    on:click=move |_| tint_source.update(|t| *t = !*t)>
                    "Source tint"
                </button>
                <SegmentedControl
                    options=vec![(false, "Stacked"), (true, "Combined")]
                    value=combined
                    on_change=Callback::new(move |v: bool| combined.set(v))
                    small=true
                    aria_label="Trace layout"
                />
                <span class="dq-spacer"></span>
                <div class="dq-tools" role="group" aria-label="Panels">
                    <button type="button" class=move || tgl_class(perf_open.get())
                        aria-pressed=move || if perf_open.get() { "true" } else { "false" }
                        aria-label="Performance" title="Performance metrics overlay"
                        on:click=move |_| perf_open.update(|o| *o = !*o)>
                        <Icon name="gauge" size=15 /><span class="dq-tgl-text">"Perf"</span>
                    </button>
                    <button type="button" class=move || tgl_class(inspector.get() == 2)
                        aria-pressed=move || if inspector.get() == 2 { "true" } else { "false" }
                        aria-label="FFT" title="Continuous FFT spectrum panel"
                        on:click=move |_| toggle_panel(2)>
                        <Icon name="activity" size=15 /><span class="dq-tgl-text">"FFT"</span>
                    </button>
                    <button type="button" class=move || tgl_class(inspector.get() == 1)
                        aria-pressed=move || if inspector.get() == 1 { "true" } else { "false" }
                        aria-label="Settings" title="Acquisition and source settings"
                        on:click=move |_| toggle_panel(1)>
                        <Icon name="sliders-horizontal" size=15 /><span class="dq-tgl-text">"Settings"</span>
                    </button>
                    <button type="button" class=move || tgl_class(inspector.get() == 3)
                        aria-pressed=move || if inspector.get() == 3 { "true" } else { "false" }
                        aria-label="Triggers" title="Trigger and flag IO configuration"
                        on:click=move |_| toggle_panel(3)>
                        <Icon name="flag" size=15 /><span class="dq-tgl-text">"Triggers"</span>
                    </button>
                    <button type="button" class=move || tgl_class(cal_open.get())
                        aria-pressed=move || if cal_open.get() { "true" } else { "false" }
                        aria-label="Calibrate" title="SMU calibration wizard"
                        on:click=move |_| cal_open.set(true)>
                        <Icon name="ruler" size=15 /><span class="dq-tgl-text">"Calibrate"</span>
                    </button>
                </div>
            </div>

            // Live readouts + stream status
            <div class="dq-strip">
            {move || {
                let snap = snapshots.get();
                let st = snap.as_ref().and_then(|s| s.status);
                let en = snap.as_ref().and_then(|s| s.energy);
                // FINE ADC health badge: only shown when the firmware reports
                // extension v2 (adaq_ok_bits non-zero = firmware present) and
                // there is a problem. Three states:
                //   "FINE N/A"   — FINE ADAQ not initialised or 100% errors
                //   "FINE ERR X%" — errors detected but < 100% (partial)
                //   (hidden)      — FINE healthy / old firmware (adaq_ok_bits==0)
                let fine_badge = st.and_then(|s| {
                    let has_v2 = s.adaq_ok_bits != 0
                        || s.fine_err_pct > 0
                        || s.drop_fine > 0
                        || s.drop_coarse > 0
                        || s.fine_diag_sticky != 0;
                    if !has_v2 {
                        return None; // old firmware — don't show anything
                    }
                    let fine_ok = (s.adaq_ok_bits & 1) != 0;
                    // Decode diag_sticky bits into a human-readable cause.
                    // 0xFF = FINE ADAQ never responded to SPI (adaq_ok=false).
                    let cause = if !fine_ok || s.fine_diag_sticky == 0xFF {
                        "FINE ADAQ did not respond during init (adaq_ok=0). \
                         Check SPI3 bus, ADAQ reset line (GPIO2), and analog \
                         rail power sequence."
                    } else if s.fine_diag_sticky & (1 << 4) != 0 {
                        "FINE ADAQ: ERR_EXT_CLK_QUAL - external MCLK absent \
                         or out of spec. Check SiT8208 oscillator and \
                         CDCLVC1104 clock distribution (3V3 PG gate)."
                    } else if s.fine_diag_sticky & (1 << 6) != 0 {
                        "FINE ADAQ: ADC_ERROR - converter self-test fault. \
                         May indicate front-end power issue or damaged device."
                    } else if s.fine_diag_sticky & (1 << 2) != 0 {
                        "FINE ADAQ: FILT_NOT_SETTLED - filter settling fault. \
                         Check MCLK continuity and SPI config (MODE 3, \
                         ≤20 MHz)."
                    } else if s.fine_diag_sticky & (1 << 1) != 0 {
                        "FINE ADAQ: SPI_ERROR - communication fault on SPI3 \
                         bus. Check SCLK20/MOSI13/MISO21/CS12 routing."
                    } else if s.fine_diag_sticky & (1 << 3) != 0 {
                        "FINE ADAQ: FILT_SATURATED - input out of range. \
                         Fine trust window exceeded or shunt mux misconfigured."
                    } else {
                        "FINE ADAQ has status errors - fused current is \
                         COARSE-only."
                    };
                    if !fine_ok || s.fine_err_pct >= 90 {
                        Some(("FINE N/A", "red", cause))
                    } else if s.fine_err_pct > 0 {
                        Some(("FINE ERR", "orange", cause))
                    } else {
                        None
                    }
                });
                let val = |v: Option<String>| v.unwrap_or_else(|| NO_VALUE.to_string());
                view!{
                    <div class="dq-readouts" role="group" aria-label="Live readings">
                        <Tile label="Voltage" dot="voltage" value=val(en.map(|e| fmt_eng(e.last_v as f64, "V")))/>
                        <Tile label="Current" dot="current" value=val(en.map(|e| fmt_eng(e.last_i as f64, "A")))/>
                        <Tile label="Power" dot="power" value=val(en.map(|e| fmt_eng(e.last_p as f64, "W")))/>
                        <Tile label="Energy" dot="" value=val(en.map(|e| format!("{:.3} mWh", e.energy_mwh)))/>
                        <Tile label="Charge" dot="" value=val(en.map(|e| format!("{:.3} mAh", e.charge_mah)))/>
                        <Tile label="Range" dot="" value=val(st.map(|s| range_name(s.range)))/>
                        {fine_badge.map(|(label, tone, tip)| view!{
                            <span class=format!("badge tone-{tone} dq-health") title=tip>{label}</span>
                        })}
                        {st.filter(|s| (s.adaq_ok_bits & 1) != 0 && s.drop_fine > 500).map(|s| view!{
                            <span class="badge tone-orange dq-health"
                                title=format!("Sequence-pairing resync drops - FINE: {}, COARSE: {}. \
                                    High counts indicate FINE/COARSE ring overflow or ODR mismatch.",
                                    s.drop_fine, s.drop_coarse)
                            >"Drops"</span>
                        })}
                    </div>
                }
            }}
            <div class="dq-status" role="status">
                {move || tint_source.get().then(|| view!{
                    <span class="dq-legend"
                        title="The current trace is coloured by the autorange source: Fine = low-current precision path, Coarse = high-current path, Blend = transition.">
                        "Current source"
                        <span class="dq-legend-item"><span class="dq-sw sw-fine"></span>"Fine"</span>
                        <span class="dq-legend-item"><span class="dq-sw sw-coarse"></span>"Coarse"</span>
                        <span class="dq-legend-item"><span class="dq-sw sw-blend"></span>"Blend"</span>
                    </span>
                })}
                <span class="dq-hint">"Drag to select, scroll to zoom, drag the timeline to navigate"</span>
                <span class=move || if streaming.get() { "dot tone-green live" } else { "dot" }></span>
                <span>{move || if streaming.get() { "Streaming" } else { "Stopped" }}</span>
                <span class="dq-status-meta">
                    {move || format!("{:.1} s", total_samples.get() as f64 / rate_hz.get().max(1) as f64)}
                </span>
            </div>
            </div>

            // Main split: plot column + right inspector
            <div class="dq-main">
                // Plot column
                <div class="dq-plot-col">
                    <div class="dq-plot">
                        <canvas node_ref=gl_canvas class="dq-canvas" aria-hidden="true"></canvas>
                        <canvas node_ref=overlay class="dq-canvas dq-canvas-top"
                            role="img" aria-label="Current, voltage and power traces"
                            title="Drag to select, scroll to zoom, drag the timeline to navigate"
                            on:mousedown=on_mousedown
                            on:mousemove=on_mousemove
                            on:mouseup=on_mouseup
                            on:mouseleave=on_mouseleave
                            on:wheel=on_wheel
                        ></canvas>
                        {move || perf_open.get().then(|| {
                            let st = status.get();
                            let ing = st.as_ref().map(|s| s.ingest_sps).unwrap_or(0.0);
                            let tot = st.as_ref().map(|s| s.total_samples).unwrap_or(0);
                            let active = st.as_ref().map(|s| s.active).unwrap_or(false);
                            let overflow = st.as_ref().map(|s| s.overflow).unwrap_or(false);
                            let mem = st.as_ref().map(|s| s.mem_used_mb).unwrap_or(0.0);
                            let cap = st.as_ref().map(|s| s.max_samples).unwrap_or(0);
                            let raw_cap = st.as_ref().map(|s| s.raw_cap).unwrap_or(0);
                            let actual_rate = st.as_ref().map(|s| s.actual_rate_hz).unwrap_or(0.0);
                            let dropped = st.as_ref().map(|s| s.dropped_samples).unwrap_or(0);
                            let snap = snapshots.get();
                            let perf = snap.as_ref().and_then(|s| s.status);
                            let fifo_drops = perf.map(|s| s.fifo_drop_frames).unwrap_or(0);
                            let bps = perf.map(|s| s.bytes_per_sec).unwrap_or(0);
                            let f = fps.get();
                            let fm = fetch_ms.get();
                            // "Keeping up" = data not dropped and the backend serves
                            // views fast; render fps is informational.
                            let keepup = !active || (!overflow && fm < 60.0);
                            let fill = if cap > 0 { (tot as f64 / cap as f64 * 100.0).min(100.0) } else { 0.0 };
                            let raw_held = tot.min(raw_cap);
                            view!{
                                <div class="dq-perf" role="status" aria-label="Performance metrics">
                                    <div class="dq-perf-head">
                                        <strong>"Performance"</strong>
                                        <span class=if keepup { "badge tone-green" } else { "badge tone-red" }>
                                            {if keepup { "Keeping up" } else { "Behind" }}
                                        </span>
                                    </div>
                                    <PerfRow label="Ingest" value=format!("{:.2} MSa/s", ing / 1e6)/>
                                    <PerfRow label="Render" value=format!("{:.0} fps", f)/>
                                    <PerfRow label="View fetch" value=format!("{:.1} ms", fm)/>
                                    <PerfRow label="Samples" value=format!("{:.2} M", tot as f64 / 1e6)/>
                                    <PerfRow label="Store" value=format!("{:.0} MB", mem)/>
                                    <PerfRow label="Raw window" value=format!("{:.1} M", raw_held as f64 / 1e6)/>
                                    <PerfRow label="History cap" value=format!("{} M ({:.0}%)", cap / 1_000_000, fill)/>
                                    <PerfRow label="Actual rate" value={if actual_rate > 0.0 { format!("{:.3} kSPS", actual_rate / 1e3) } else { NO_VALUE.to_string() }}/>
                                    <PerfRow label="Dropped" value=format!("{}", dropped)/>
                                    <PerfRow label="USB drops" value=format!("{}", fifo_drops)/>
                                    <PerfRow label="Throughput" value=format!("{:.2} MB/s", bps as f64 / 1e6)/>
                                </div>
                            }
                        })}
                    </div>
                    // Selection integral strip
                    {move || {
                        integral.get().map(|r| {
                            view!{
                                <div class="dq-integral" role="status" aria-label="Selection summary">
                                    <span class="badge tone-blue">"Selection"</span>
                                    <Kv label="Duration" value=fmt_time(r.duration_s)/>
                                    <Kv label="Charge" value=format!("{:.4} mAh", r.charge_mah)/>
                                    <Kv label="Energy" value=format!("{:.4} mWh", r.energy_mwh)/>
                                    <Kv label="Avg current" value=fmt_eng(r.avg_i, "A")/>
                                    <Kv label="Avg power" value=fmt_eng(r.avg_p, "W")/>
                                    <span title="Projected consumption if this pattern ran for one hour">
                                        <Kv label="Per hour" value=format!("{:.2} mWh", r.projected_mwh_per_hour)/>
                                    </span>
                                </div>
                            }
                        })
                    }}
                </div>

                // Inspector: one panel at a time, all kept mounted so state survives.
                <aside class="inspector dq-inspector" aria-label="Inspector" prop:hidden=move || inspector.get() == 0>
                    <div class="inspector-header">
                        <span>{move || match inspector.get() {
                            1 => "Settings",
                            2 => "Spectrum",
                            3 => "Triggers and flags",
                            _ => "",
                        }}</span>
                        <button type="button" class="btn btn-plain btn-sm btn-icon" aria-label="Close inspector" title="Close inspector"
                            on:click=move |_| inspector.set(0)>
                            <Icon name="x" size=15 />
                        </button>
                    </div>
                    <div class="dq-panel" prop:hidden=move || inspector.get() != 1>
                        <SettingsPanel
                            sample_rate_idx=sample_rate_idx voltage_rate_idx=voltage_rate_idx
                            decimation=decimation hw_filter_idx=hw_filter_idx hw_decim_idx=hw_decim_idx sr_mode=sr_mode
                            status=status
                            range_lock_idx=range_lock_idx smooth_window=smooth_window filter_type=filter_type
                            raw_filter=raw_filter
                            vdut_mv=vdut_mv ilimit_ma=ilimit_ma source_enable=source_enable
                            snapshots=snapshots pd_warn=pd_warn
                            apply_source=Rc::new(apply_source.clone()) apply_range=Rc::new(apply_range.clone())
                            apply_rate=Rc::new(apply_rate.clone())
                        />
                    </div>
                    <div class="dq-panel" prop:hidden=move || inspector.get() != 2>
                        <FftPanel open=fft_panel_open snapshots=snapshots fft_nbins=fft_nbins fft_window=fft_window fft_source=fft_source apply=Rc::new(apply_fft.clone())/>
                    </div>
                    <div class="dq-panel" prop:hidden=move || inspector.get() != 3>
                        <TriggerPanel open=trig_panel_open/>
                    </div>
                </aside>
            </div>

            // SMU calibration wizard (modal; control plane via S3 BBP).
            <CalibrationWizard open=cal_open/>
        </div>
    }
}

fn range_name(r: u8) -> String {
    match r {
        0 => "HI 51Ω".into(),
        1 => "MID 2Ω".into(),
        2 => "LO 50mΩ".into(),
        _ => NO_VALUE.into(),
    }
}

fn ov2d(canvas: &HtmlCanvasElement) -> Option<CanvasRenderingContext2d> {
    canvas
        .get_context("2d")
        .ok()
        .flatten()
        .and_then(|o| o.dyn_into::<CanvasRenderingContext2d>().ok())
}

fn visible_tracks(i: bool, v: bool, p: bool) -> Vec<TrackInfo> {
    let mut t = Vec::new();
    if i {
        t.push(TRACK_I);
    }
    if v {
        t.push(TRACK_V);
    }
    if p {
        t.push(TRACK_P);
    }
    t
}

/// Vertical (y_top, y_bottom) pixel region per lane. In combined mode every
/// track shares the full trace region.
fn lane_regions(top: f64, bottom: f64, n: usize, combined: bool) -> Vec<(f64, f64)> {
    if n == 0 {
        return vec![];
    }
    if combined {
        return (0..n).map(|_| (top, bottom)).collect();
    }
    let nf = n as f64;
    let lane_h = ((bottom - top) - LANE_GAP * (nf - 1.0)) / nf;
    (0..n)
        .map(|i| {
            let y0 = top + i as f64 * (lane_h + LANE_GAP);
            (y0, y0 + lane_h)
        })
        .collect()
}

fn track_cols<'a>(
    vd: &'a DaqViewData,
    name: &str,
) -> (&'a Vec<f32>, &'a Vec<f32>, Option<&'a [u8]>) {
    match name {
        "Current" => (&vd.i_min, &vd.i_max, Some(vd.source.as_slice())),
        "Voltage" => (&vd.v_min, &vd.v_max, None),
        _ => (&vd.p_min, &vd.p_max, None),
    }
}

thread_local! {
    // Last known-good autorange per lane name, kept so that a view where
    // *every* visible column is a gap bucket (e.g. right after a dropout, at
    // high refresh rates) doesn't snap the axis to the (0.0, 1.0) default —
    // it holds the previous scale instead.
    static LAST_GOOD_RANGE: std::cell::RefCell<std::collections::HashMap<&'static str, (f32, f32)>> =
        std::cell::RefCell::new(std::collections::HashMap::new());
}

/// Compute the autoranged (lo, hi) for one lane's visible min/max envelope
/// columns, excluding gap-bucket columns (gap[k] == 1) and any non-finite
/// entries (gap fill values, or hi-res smoothing filters which aren't
/// NaN-aware) so dropouts and gap buckets don't collapse/pollute the Y
/// autoscale. If every column is a gap (or `gap` is absent/mismatched and no
/// finite samples remain), falls back to the previous good range for this
/// lane rather than the default (0.0, 1.0).
fn col_range_gapped(lane: &'static str, vmin: &[f32], vmax: &[f32], gap: Option<&[u8]>) -> (f32, f32) {
    let is_gap = |k: usize| gap.map(|g| g.get(k).copied().unwrap_or(0) == 1).unwrap_or(false);
    let mut lo = f32::INFINITY;
    let mut hi = f32::NEG_INFINITY;
    for k in 0..vmin.len() {
        if is_gap(k) {
            continue;
        }
        let a = vmin[k];
        let b = vmax.get(k).copied().unwrap_or(f32::NAN);
        if a.is_finite() {
            lo = lo.min(a);
        }
        if b.is_finite() {
            hi = hi.max(b);
        }
    }
    if !lo.is_finite() || !hi.is_finite() {
        // Every column was a gap (or non-finite) — hold the previous scale.
        return LAST_GOOD_RANGE
            .with(|m| m.borrow().get(lane).copied())
            .unwrap_or((0.0, 1.0));
    }
    let r = nice_range(lo, hi);
    LAST_GOOD_RANGE.with(|m| m.borrow_mut().insert(lane, r));
    r
}


// ── Display filtering (client-side, over the visible envelope) ───────────────
// Applied to the ~1800-column min/max envelope returned by the backend rather
// than the raw capture: cost is independent of capture length and off the live
// append path. The filter runs once on each column *midline* and the band
// half-width is filtered alongside (except high-pass, which keeps the true
// spread), so the envelope stays meaningful instead of collapsing the way
// independently filtering min and max did.

/// Centered box moving-average low-pass (window in columns), O(n) via prefix sum.
fn flt_moving_avg(x: &[f32], win: usize) -> Vec<f32> {
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
fn flt_ema(x: &[f32], win: usize) -> Vec<f32> {
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
fn flt_median(x: &[f32], win: usize) -> Vec<f32> {
    let n = x.len();
    if n == 0 || win <= 1 {
        return x.to_vec();
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

/// Dispatch one base filter. `kind`: 1=avg, 2=EMA, 3=median (else identity).
fn flt_apply(x: &[f32], win: usize, kind: u8) -> Vec<f32> {
    match kind {
        1 => flt_moving_avg(x, win),
        2 => flt_ema(x, win),
        3 => flt_median(x, win),
        _ => x.to_vec(),
    }
}

/// Window in columns for the selected `smooth` setting, expressed in **raw
/// samples** so the filter cutoff is a fixed real timescale and does not change
/// as you zoom (scope-like behaviour). `win_cols = smooth / samples_per_col`:
/// when zoomed in enough that the window spans several columns the smoothing is
/// visible; when zoomed out so far that the window is finer than one column it
/// collapses to 1 (no extra smoothing — the min/max decimation already
/// aggregates beyond the window, so anything still visible is real signal, not
/// sub-window noise the filter should remove).
fn flt_win_cols(span: u64, cols: usize, smooth: u32) -> usize {
    if cols <= 1 || smooth <= 1 {
        return 1;
    }
    let spc = (span / cols as u64).max(1);
    (((smooth as u64) + spc / 2) / spc).max(1) as usize
}

/// Filter a single min/max envelope in place. The midline is filtered with the
/// selected kind; the half-width is smoothed too for avg/EMA/median, or kept as
/// the true spread for high-pass (kind 4), which only removes DC from the line.
fn flt_band(lo: &mut [f32], hi: &mut [f32], win: usize, kind: u8) {
    let n = lo.len();
    if n == 0 || n != hi.len() || win <= 1 || kind == 0 {
        return;
    }
    let mut mid = vec![0.0f32; n];
    let mut half = vec![0.0f32; n];
    for k in 0..n {
        mid[k] = 0.5 * (lo[k] + hi[k]);
        half[k] = 0.5 * (hi[k] - lo[k]);
    }
    let midf = if kind == 4 {
        // High-pass = midline minus its moving average (AC ripple around zero).
        let lp = flt_moving_avg(&mid, win);
        mid.iter().zip(lp.iter()).map(|(a, b)| a - b).collect()
    } else {
        flt_apply(&mid, win, kind)
    };
    let halff = if kind == 4 {
        half // keep the true min/max spread around the AC line
    } else {
        flt_apply(&half, win, kind)
    };
    for k in 0..n {
        let h = halff[k].max(0.0);
        lo[k] = midf[k] - h;
        hi[k] = midf[k] + h;
    }
}

/// Return a display-filtered copy of `vd`. `kind`: 0=off, 1=avg, 2=EMA,
/// 3=median, 4=high-pass; `smooth` is the window in raw samples.
fn apply_display_filter(vd: DaqViewData, smooth: u32, kind: u8) -> DaqViewData {
    let mut out = vd;
    if kind == 0 || smooth <= 1 {
        return out;
    }
    let span = out.view_end.saturating_sub(out.view_start);
    let win = flt_win_cols(span, out.i_min.len(), smooth);
    if win <= 1 {
        return out;
    }
    flt_band(&mut out.i_min, &mut out.i_max, win, kind);
    flt_band(&mut out.v_min, &mut out.v_max, win, kind);
    flt_band(&mut out.p_min, &mut out.p_max, win, kind);
    out
}

#[allow(clippy::too_many_arguments)]
fn draw_overlay(
    canvas: &HtmlCanvasElement,
    dpr: f64,
    css_w: f64,
    css_h: f64,
    tracks: &[TrackInfo],
    regions: &[(f64, f64)],
    combined: bool,
    vd: Option<&DaqViewData>,
    overview: Option<&DaqViewData>,
    sel_start: Option<u64>,
    sel_end: Option<u64>,
    hover: Option<(f64, f64)>,
    _dragging: bool,
) {
    let Some(ctx) = ov2d(canvas) else { return };
    let pal = palette();
    ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();
    ctx.clear_rect(0.0, 0.0, css_w, css_h);

    let plot_x0 = LABEL_W;
    let plot_w = (css_w - LABEL_W).max(1.0);
    let trace_top = TOP_PAD;
    let trace_bottom = css_h - RULER_H - HEATMAP_H;
    let hm_top = css_h - HEATMAP_H;

    let Some(vd) = vd else {
        ctx.set_fill_style_str(&pal.axis);
        ctx.set_font(&pal.font(13));
        let _ = ctx.fill_text("Waiting for stream...", plot_x0 + 16.0, css_h / 2.0);
        return;
    };

    let total = vd.total_samples.max(1);
    let vs = vd.view_start;
    let ve = vd.view_end.max(vs + 1);
    let span = (ve - vs) as f64;
    let rate = if vd.actual_rate_hz > 0.0 {
        vd.actual_rate_hz
    } else {
        vd.sample_rate_hz.max(1) as f64
    };
    let cols = vd.i_min.len();
    let sample_to_x = |s: u64| plot_x0 + ((s.saturating_sub(vs)) as f64 / span) * plot_w;

    // Lane frames + labels. Series identity is a small dot; the name stays neutral.
    let dot = |x: f64, y: f64, color: &str| {
        ctx.set_fill_style_str(color);
        ctx.begin_path();
        let _ = ctx.arc(x, y, 3.5, 0.0, std::f64::consts::TAU);
        ctx.fill();
    };
    ctx.set_font(&pal.font(11));
    if combined {
        ctx.set_stroke_style_str(&pal.grid);
        ctx.set_line_width(1.0);
        ctx.stroke_rect(plot_x0, trace_top, plot_w, trace_bottom - trace_top);
        let mut ly = trace_top + 14.0;
        for t in tracks {
            dot(plot_x0 + 12.0, ly - 4.0, pal.series(t.name));
            ctx.set_fill_style_str(&pal.text);
            let _ = ctx.fill_text(t.name, plot_x0 + 21.0, ly);
            ly += 14.0;
        }
    } else {
        for (i, t) in tracks.iter().enumerate() {
            let (y_top, y_bottom) = regions[i];
            ctx.set_stroke_style_str(&pal.grid);
            ctx.set_line_width(1.0);
            ctx.stroke_rect(plot_x0, y_top, plot_w, y_bottom - y_top);
            ctx.begin_path();
            ctx.move_to(plot_x0, (y_top + y_bottom) / 2.0);
            ctx.line_to(plot_x0 + plot_w, (y_top + y_bottom) / 2.0);
            ctx.stroke();
            let (vmin, vmax, _) = track_cols(vd, t.name);
            let (lo, hi) = if vmin.is_empty() {
                (0.0, 1.0)
            } else {
                let gap_slice = if vd.gap.len() == vmin.len() {
                    Some(vd.gap.as_slice())
                } else {
                    None
                };
                col_range_gapped(t.name, vmin, vmax, gap_slice)
            };
            dot(12.0, y_top + 10.0, pal.series(t.name));
            ctx.set_fill_style_str(&pal.text);
            let _ = ctx.fill_text(t.name, 21.0, y_top + 14.0);
            ctx.set_fill_style_str(&pal.axis);
            let _ = ctx.fill_text(&fmt_eng(hi as f64, t.unit), 6.0, y_top + 28.0);
            let _ = ctx.fill_text(&fmt_eng(lo as f64, t.unit), 6.0, y_bottom - 4.0);
        }
    }

    // Selection highlight.
    if let (Some(s), Some(e)) = (sel_start, sel_end) {
        if e > s {
            let x0 = sample_to_x(s).max(plot_x0);
            let x1 = sample_to_x(e).min(plot_x0 + plot_w);
            ctx.set_fill_style_str(&pal.accent_tint);
            ctx.fill_rect(x0, trace_top, (x1 - x0).max(0.0), trace_bottom - trace_top);
            ctx.set_stroke_style_str(&pal.accent);
            ctx.set_line_width(1.0);
            ctx.begin_path();
            ctx.move_to(x0, trace_top);
            ctx.line_to(x0, trace_bottom);
            ctx.move_to(x1, trace_top);
            ctx.line_to(x1, trace_bottom);
            ctx.stroke();
        }
    }

    // Time ruler.
    ctx.set_font(&pal.font(10));
    for k in 0..=5 {
        let frac = k as f64 / 5.0;
        let x = plot_x0 + frac * plot_w;
        let t = (vs as f64 + frac * span) / rate;
        ctx.set_stroke_style_str(&pal.grid);
        ctx.begin_path();
        ctx.move_to(x, trace_top);
        ctx.line_to(x, trace_bottom);
        ctx.stroke();
        ctx.set_fill_style_str(&pal.axis);
        let _ = ctx.fill_text(&fmt_time(t), x + 2.0, trace_bottom + 14.0);
    }

    // Event markers (flags + triggers) \u2014 vertical lines at exact sample
    // positions, drawn at full fidelity regardless of decimation. Triggers are
    // solid and heavier, flags dashed; both carry a text tag in the timeline strip.
    let mut last_label_x = f64::NEG_INFINITY;
    for m in &vd.markers {
        if m.sample_index < vs || m.sample_index > ve {
            continue;
        }
        let x = sample_to_x(m.sample_index);
        let is_trig = m.kind == 1;
        ctx.set_stroke_style_str(&pal.marker);
        ctx.set_line_width(if is_trig { 1.6 } else { 1.0 });
        if !is_trig {
            let _ = ctx.set_line_dash(&js_sys::Array::of2(&4.0.into(), &3.0.into()));
        }
        ctx.begin_path();
        ctx.move_to(x, trace_top);
        ctx.line_to(x, trace_bottom);
        ctx.stroke();
        let _ = ctx.set_line_dash(&js_sys::Array::new());
        // Timeline tick + IO label.
        ctx.set_fill_style_str(&pal.marker);
        ctx.fill_rect(x - 1.0, trace_bottom, 2.0, RULER_H);
        ctx.set_font(&pal.font(10));
        // Dense markers: label only those with room, the lines still all draw.
        if x - last_label_x >= 44.0 {
            let _ = ctx.fill_text(
                &format!("{} {}", if is_trig { "Trig" } else { "Flag" }, m.channel),
                x + 3.0,
                trace_top + 10.0,
            );
            last_label_x = x;
        }
    }

    // Hover guide + per-track readout tooltip.
    if let Some((mx, my)) = hover {
        if mx >= plot_x0 && mx <= plot_x0 + plot_w && my >= trace_top && my <= trace_bottom && cols > 0
        {
            ctx.set_stroke_style_str(&pal.axis);
            ctx.set_line_width(1.0);
            ctx.begin_path();
            ctx.move_to(mx, trace_top);
            ctx.line_to(mx, trace_bottom);
            ctx.stroke();
            let frac = ((mx - plot_x0) / plot_w).clamp(0.0, 1.0);
            let col = ((frac * cols.saturating_sub(1) as f64).round() as usize).min(cols - 1);
            let t = (vs as f64 + frac * span) / rate;
            let val_of = |name: &str| -> f64 {
                let (vmin, vmax, _) = track_cols(vd, name);
                if col < vmin.len() {
                    ((vmin[col] + vmax[col]) / 2.0) as f64
                } else {
                    0.0
                }
            };
            // (text, series colour for the dot; None = plain line)
            let mut lines: Vec<(String, Option<&str>)> =
                vec![(format!("t = {}", fmt_time(t)), None)];
            if combined {
                for tr in tracks {
                    lines.push((
                        format!("{}: {}", tr.name, fmt_eng(val_of(tr.name), tr.unit)),
                        Some(pal.series(tr.name)),
                    ));
                }
            } else {
                let mut chosen = None;
                for (i, _t) in tracks.iter().enumerate() {
                    let (y0, y1) = regions[i];
                    if my >= y0 && my <= y1 {
                        chosen = Some(i);
                        break;
                    }
                }
                if let Some(i) = chosen {
                    let tr = tracks[i];
                    lines.push((
                        format!("{}: {}", tr.name, fmt_eng(val_of(tr.name), tr.unit)),
                        Some(pal.series(tr.name)),
                    ));
                }
            }
            let bw = 168.0;
            let lh = 16.0;
            let bh = lh * lines.len() as f64 + 8.0;
            let mut bx = mx + 12.0;
            if bx + bw > css_w {
                bx = mx - bw - 12.0;
            }
            let mut by = my + 12.0;
            if by + bh > trace_bottom {
                by = trace_bottom - bh;
            }
            if by < trace_top {
                by = trace_top;
            }
            ctx.set_fill_style_str(&pal.surface);
            ctx.fill_rect(bx, by, bw, bh);
            ctx.set_stroke_style_str(&pal.sep);
            ctx.set_line_width(1.0);
            ctx.stroke_rect(bx, by, bw, bh);
            ctx.set_font(&pal.font(11));
            for (k, (txt, color)) in lines.iter().enumerate() {
                let ly = by + 15.0 + k as f64 * lh;
                let mut tx = bx + 8.0;
                if let Some(c) = color {
                    dot(bx + 12.0, ly - 4.0, c);
                    tx = bx + 21.0;
                }
                ctx.set_fill_style_str(&pal.text);
                let _ = ctx.fill_text(txt, tx, ly);
            }
        }
    }

    // Full-capture dI/dt minimap + viewport indicator (click/drag to navigate).
    // Intensity is the accent colour's opacity, so it reads in both themes.
    ctx.set_fill_style_str(&pal.grid);
    ctx.fill_rect(plot_x0, hm_top, plot_w, HEATMAP_H);
    ctx.set_fill_style_str(&pal.axis);
    ctx.set_font(&pal.font(10));
    let _ = ctx.fill_text("dI/dt", 6.0, hm_top + 13.0);
    let _ = ctx.fill_text("(all)", 6.0, hm_top + 26.0);
    if let Some(ov) = overview {
        let n = ov.didt.len();
        if n > 0 {
            let max_d = ov.didt.iter().cloned().fold(0.0f32, f32::max).max(1e-9);
            let cw = plot_w / n as f64;
            ctx.set_fill_style_str(&pal.accent);
            for (i, &d) in ov.didt.iter().enumerate() {
                let tt = (d / max_d).clamp(0.0, 1.0) as f64;
                ctx.set_global_alpha(0.12 + 0.88 * tt);
                ctx.fill_rect(plot_x0 + i as f64 * cw, hm_top + 2.0, cw.max(1.0), HEATMAP_H - 4.0);
            }
            ctx.set_global_alpha(1.0);
        }
    }
    let vx0 = plot_x0 + (vs as f64 / total as f64) * plot_w;
    let vx1 = plot_x0 + (ve as f64 / total as f64) * plot_w;
    ctx.set_fill_style_str(&pal.accent_tint_strong);
    ctx.fill_rect(vx0, hm_top, (vx1 - vx0).max(2.0), HEATMAP_H);
    ctx.set_stroke_style_str(&pal.text);
    ctx.set_line_width(1.5);
    ctx.stroke_rect(vx0, hm_top, (vx1 - vx0).max(2.0), HEATMAP_H);

    if vd.overflow {
        ctx.set_fill_style_str(&pal.warn);
        ctx.set_font(&pal.font(11));
        let _ = ctx.fill_text("Acquisition truncated", plot_x0 + 8.0, trace_top + 12.0);
    }
}

// ---- Sub-components ---------------------------------------------------------

fn tgl_class(on: bool) -> &'static str {
    if on {
        "btn btn-sm btn-tinted dq-tgl"
    } else {
        "btn btn-sm btn-plain dq-tgl"
    }
}

/// Series visibility chip: channel-colour dot + name, toggles a trace.
#[component]
fn SeriesChip(sig: RwSignal<bool>, info: TrackInfo) -> impl IntoView {
    view! {
        <button type="button" class="dq-chip" class:on=move || sig.get()
            aria-pressed=move || if sig.get() { "true" } else { "false" }
            title=format!("Show {} trace", info.name.to_lowercase())
            on:click=move |_| sig.update(|v| *v = !*v)>
            <span class=format!("dq-dot s-{}", info.key)></span>
            {info.name}
        </button>
    }
}

/// Readout tile: label (with optional series dot) above a tabular value + unit.
#[component]
fn Tile(label: &'static str, dot: &'static str, value: String) -> impl IntoView {
    let (v, u) = split_val(value);
    view! {
        <div class="readout readout-sm dq-tile">
            <span class="readout-label">
                {(!dot.is_empty()).then(|| view!{ <span class=format!("dq-dot s-{dot}")></span> })}
                {label}
            </span>
            <span class="readout-value">
                {v}
                {(!u.is_empty()).then(|| view!{ <span class="unit">{u}</span> })}
            </span>
        </div>
    }
}

#[component]
fn Kv(label: &'static str, value: String) -> impl IntoView {
    view! {
        <span class="dq-kv">
            <span class="dq-kv-k">{label}</span>
            <span class="dq-kv-v">{value}</span>
        </span>
    }
}

#[component]
fn PerfRow(label: &'static str, value: String) -> impl IntoView {
    view! {
        <div class="dq-perf-row">
            <span>{label}</span>
            <span>{value}</span>
        </div>
    }
}

#[component]
fn FftPanel(
    open: Signal<bool>,
    snapshots: RwSignal<Option<DaqSnapshots>>,
    fft_nbins: RwSignal<u16>,
    fft_window: RwSignal<u8>,
    fft_source: RwSignal<u8>,
    apply: Rc<dyn Fn()>,
) -> impl IntoView {
    let canvas = NodeRef::<leptos::html::Canvas>::new();
    let theme = use_theme();
    // Redraw spectrum on snapshot / theme change, while the panel is showing.
    Effect::new(move |_| {
        let snap = snapshots.get();
        theme.resolved.track();
        if !open.get() {
            return;
        }
        let Some(c) = canvas.get() else { return };
        let canvas_el: HtmlCanvasElement = c.unchecked_into();
        let pal = palette();
        let dpr = web_sys::window().map(|w| w.device_pixel_ratio()).unwrap_or(1.0);
        let css_w = canvas_el.client_width() as f64;
        let css_h = canvas_el.client_height() as f64;
        if css_w < 2.0 || css_h < 2.0 {
            return;
        }
        canvas_el.set_width((css_w * dpr) as u32);
        canvas_el.set_height((css_h * dpr) as u32);
        let Some(ctx) = ov2d(&canvas_el) else { return };
        ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();
        ctx.set_fill_style_str(&pal.bg);
        ctx.fill_rect(0.0, 0.0, css_w, css_h);
        ctx.set_stroke_style_str(&pal.grid);
        ctx.set_line_width(1.0);
        for k in 1..4 {
            let y = css_h * k as f64 / 4.0;
            ctx.begin_path();
            ctx.move_to(0.0, y);
            ctx.line_to(css_w, y);
            ctx.stroke();
        }
        let bins = snap.and_then(|s| s.fft).map(|f| f.bins).unwrap_or_default();
        if bins.is_empty() {
            ctx.set_fill_style_str(&pal.axis);
            ctx.set_font(&pal.font(12));
            let _ = ctx.fill_text("No spectrum", 10.0, css_h / 2.0);
            return;
        }
        let max = bins.iter().cloned().fold(1e-9_f32, f32::max);
        ctx.set_stroke_style_str(if fft_source.get_untracked() == 1 { &pal.power } else { &pal.current });
        ctx.set_line_width(1.25);
        ctx.begin_path();
        let n = bins.len();
        for (i, &m) in bins.iter().enumerate() {
            let x = i as f64 / (n - 1).max(1) as f64 * css_w;
            let y = css_h - (m / max).clamp(0.0, 1.0) as f64 * (css_h - 10.0);
            if i == 0 { ctx.move_to(x, y); } else { ctx.line_to(x, y); }
        }
        ctx.stroke();
    });

    let apply2 = apply.clone();
    let apply3 = apply.clone();
    view! {
        <section class="inspector-section">
            <h4>"Spectrum"</h4>
            <canvas node_ref=canvas class="dq-fft-canvas" role="img" aria-label="FFT magnitude spectrum"></canvas>
        </section>
        <section class="inspector-section">
            <h4>"Analysis"</h4>
            <div class="dq-field">
                <span class="row-label">"Length"</span>
                <div class="seg seg-sm seg-block dq-seg-fit" role="radiogroup" aria-label="FFT length">
                    {[64u16,128,256,512,1024,2048,4096].iter().map(|n| {
                        let n=*n;
                        let a = apply.clone();
                        view!{
                            <button class:active=move || fft_nbins.get()==n
                                on:click=move |_| { fft_nbins.set(n); a(); }>{format!("{n}")}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="dq-field">
                <span class="row-label">"Window"</span>
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="FFT window">
                    {[("Rect",0u8),("Hann",1),("Blackman-Harris",2)].iter().map(|(l,w)| {
                        let w=*w;
                        let a = apply2.clone();
                        view!{
                            <button class:active=move || fft_window.get()==w
                                on:click=move |_| { fft_window.set(w); a(); }>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="dq-field">
                <span class="row-label">"Source"</span>
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="FFT source">
                    {[("Current",0u8),("Power",1)].iter().map(|(l,s)| {
                        let s=*s;
                        let a = apply3.clone();
                        view!{
                            <button class:active=move || fft_source.get()==s
                                on:click=move |_| { fft_source.set(s); a(); }>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
        </section>
    }
}

#[component]
#[allow(clippy::too_many_arguments)]
fn SettingsPanel(
    sample_rate_idx: RwSignal<u8>,
    voltage_rate_idx: RwSignal<u8>,
    decimation: RwSignal<u16>,
    hw_filter_idx: RwSignal<u8>,
    hw_decim_idx: RwSignal<u8>,
    sr_mode: RwSignal<bool>,
    status: RwSignal<Option<DaqStreamRuntimeStatus>>,
    range_lock_idx: RwSignal<u8>,
    smooth_window: RwSignal<u32>,
    filter_type: RwSignal<u8>,
    raw_filter: RwSignal<bool>,
    vdut_mv: RwSignal<u32>,
    ilimit_ma: RwSignal<u32>,
    source_enable: RwSignal<bool>,
    snapshots: RwSignal<Option<DaqSnapshots>>,
    pd_warn: RwSignal<String>,
    apply_source: Rc<dyn Fn()>,
    apply_range: Rc<dyn Fn()>,
    apply_rate: Rc<dyn Fn()>,
) -> impl IntoView {
    let apply_supply = apply_source.clone();
    // USB Decim must NOT go through apply_rate: that also re-issues the BBP
    // daq_cfg_set_enum(DAQ_K_SAMPLE_RATE_IDX, ...) call, which forces a real
    // hardware stop/reconfigure/run cycle on the P4 (daq_board_stop_fast +
    // run_fast) even though the rate index didn't change. Decimation only
    // needs CMD_SET_RATE (firmware only honors the decimation field there —
    // see CTRL_MSG_SET_RATE in daq_board.c).
    let apply_decim = move || {
        let (idx, vidx, dec) = (
            sample_rate_idx.get_untracked(),
            voltage_rate_idx.get_untracked(),
            decimation.get_untracked(),
        );
        spawn_local(async move {
            daq_set_rate(idx, vidx, dec).await;
            show_toast(&format!("USB decim: /{}", dec), "ok");
        });
    };
    let ar_minus = apply_decim.clone();
    let ar_input = apply_decim.clone();
    let ar_plus = apply_decim.clone();
    let range_labels = ["Auto", "HI µA", "MID mA", "LO A"];
    let filter_labels = [("Off", 0u8), ("Avg", 1), ("EMA", 2), ("Median", 3), ("HPF", 4)];
    // Hardware filter / decimation key constants (DAQ_K_FILTER=0x0106, DAQ_K_DECIMATION=0x0107)
    const HW_FILT_KEY: u16 = 0x0106;
    const HW_DECIM_KEY: u16 = 0x0107;
    // DAQ_K_SR_MODE = DAQ_KEY(GRP_ACQ=0x01, idx=0x09).
    const SR_MODE_KEY: u16 = 0x0109;
    let hw_filter_labels = [("Wideband", 0u8), ("Sinc5", 1), ("Sinc3", 2)];
    let hw_decim_labels  = [("x32", 0u8), ("x64", 1), ("x128", 2), ("x256", 3), ("x512", 4), ("x1024", 5)];
    view! {
        <section class="inspector-section">
            <h4>"Acquisition"</h4>
            <div class="dq-field">
                <span class="row-label">"Super resolution"
                    <span class="row-hint">"1 ksps current, 500 sps voltage, oversampled"</span>
                </span>
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="Super resolution">
                    {[("Off", false), ("On", true)].iter().map(|(l, on)| {
                        let on = *on;
                        view!{
                            <button class:active=move || sr_mode.get() == on
                                on:click=move |_| {
                                    sr_mode.set(on);
                                    spawn_local(async move {
                                        if daq_cfg_set_bool(SR_MODE_KEY, on).await {
                                            show_toast(
                                                if on { "Super Resolution on - ADC at max decimation + DSP low-pass" }
                                                else  { "Super Resolution off - rate/filter controls restored" },
                                                "ok");
                                        } else {
                                            show_toast("Super Resolution: send failed - BBP busy, retry", "err");
                                        }
                                    });
                                }>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="dq-field">
                <span class="row-label">"Current rate (fine/coarse)"</span>
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="Current sample rate">
                    {SAMPLE_RATE_SHORT.iter().enumerate().map(|(i, l)| {
                        let i = i as u8;
                        let ar = apply_rate.clone();
                        view!{
                            <button class:active=move || sample_rate_idx.get() == i
                                on:click=move |_| { sample_rate_idx.set(i); ar(); }>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="dq-field">
                <span class="row-label">"Voltage rate"</span>
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="Voltage sample rate">
                    {SAMPLE_RATE_SHORT.iter().enumerate().map(|(i, l)| {
                        let i = i as u8;
                        let ar = apply_rate.clone();
                        view!{
                            <button class:active=move || voltage_rate_idx.get() == i
                                on:click=move |_| { voltage_rate_idx.set(i); ar(); }>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="row">
                <span class="row-label">"USB decimation"<span class="row-hint">"keep every Nth sample"</span></span>
                <div class="dq-stepper">
                    <button type="button" class="btn btn-sm btn-icon" aria-label="Halve USB decimation" title="Halve"
                        on:click=move |_| { decimation.update(|d| *d = (*d / 2).max(1)); ar_minus(); }>
                        <Icon name="minus" size=14 />
                    </button>
                    <input class="dq-num" type="number" min="1" max="256" aria-label="USB decimation"
                        prop:value=move || decimation.get().to_string()
                        on:change=move |ev| { decimation.set(event_target_value(&ev).parse().unwrap_or(1).clamp(1, 256)); ar_input(); } />
                    <button type="button" class="btn btn-sm btn-icon" aria-label="Double USB decimation" title="Double"
                        on:click=move |_| { decimation.update(|d| *d = (*d * 2).min(256)); ar_plus(); }>
                        <Icon name="plus" size=14 />
                    </button>
                </div>
            </div>
            <div class="dq-field">
                <span class="row-label">"Hardware filter (ADC)"</span>
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="Hardware filter">
                    {hw_filter_labels.iter().map(|(l, t)| {
                        let t = *t;
                        let lbl = l.to_string();
                        view!{
                            <button class:active=move || hw_filter_idx.get() == t
                                on:click=move |_| {
                                    hw_filter_idx.set(t);
                                    let lbl2 = lbl.clone();
                                    spawn_local(async move {
                                        if daq_cfg_set_enum(HW_FILT_KEY, t).await {
                                            show_toast(&format!("HW filter: {} (restart stream to apply)", lbl2), "ok");
                                        } else {
                                            show_toast("HW filter: send failed - BBP busy, retry", "err");
                                        }
                                    });
                                }>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="dq-field">
                <span class="row-label">"Hardware decimation (ADC)"</span>
                <div class="seg seg-sm seg-block dq-seg-fit" role="radiogroup" aria-label="Hardware decimation">
                    {hw_decim_labels.iter().map(|(l, t)| {
                        let t = *t;
                        let lbl = l.to_string();
                        view!{
                            <button class:active=move || hw_decim_idx.get() == t
                                on:click=move |_| {
                                    hw_decim_idx.set(t);
                                    let lbl2 = lbl.clone();
                                    spawn_local(async move {
                                        if daq_cfg_set_enum(HW_DECIM_KEY, t).await {
                                            show_toast(&format!("HW decim: {} (restart stream to apply)", lbl2), "ok");
                                        } else {
                                            show_toast("HW decim: send failed - BBP busy, retry", "err");
                                        }
                                    });
                                }>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <dl class="kv dq-odr">
                <dt>"ODR, current (fine/coarse)"</dt>
                <dd>
                    {move || {
                        // sample_rate_hz comes from the device stream (WaveformRecord.sample_rate)
                        // which reports the actual ADAQ hardware ODR = fMOD / decimation_factor.
                        // e.g. Wideband + x1024: 8.192 MHz / 1024 = 8 kSPS
                        let s = status.get();
                        let hz = s.as_ref().map(|s| s.sample_rate_hz).unwrap_or(0);
                        if hz == 0 { NO_VALUE.to_string() } else if hz >= 1_000_000 { format!("{} MSPS", hz / 1_000_000) }
                        else if hz >= 1_000 { format!("{} kSPS", hz / 1_000) } else { format!("{} SPS", hz) }
                    }}
                </dd>
                <dt>"ODR, voltage (requested)"</dt>
                <dd>
                    {move || {
                        // voltage_sps_hz reflects the requested rate; actual voltage ODR
                        // is the same as current ODR since all ADAQ channels share one clock.
                        let s = status.get();
                        let hz = s.as_ref().map(|s| s.voltage_sps_hz).unwrap_or(0);
                        if hz == 0 { NO_VALUE.to_string() } else if hz >= 1_000_000 { format!("{} MSPS", hz / 1_000_000) }
                        else if hz >= 1_000 { format!("{} kSPS", hz / 1_000) } else { format!("{} SPS", hz) }
                    }}
                </dd>
            </dl>
            <p class="row-hint">
                "Actual ODR = fMOD / hardware decimation. Use Current rate to auto-select filter and decimation."
            </p>
            <div class="dq-field">
                <span class="row-label">"Range lock"</span>
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="Range lock">
                    {range_labels.iter().enumerate().map(|(i, l)| {
                        let i = i as u8;
                        let ar = apply_range.clone();
                        view!{
                            <button class:active=move || range_lock_idx.get() == i
                                on:click=move |_| { range_lock_idx.set(i); ar(); }>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
        </section>

        <section class="inspector-section">
            <h4>"Display filters"</h4>
            <div class="dq-field">
                <span class="row-label">"Type"</span>
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="Display filter type">
                    {filter_labels.iter().map(|(l, t)| {
                        let t = *t;
                        view!{
                            <button class:active=move || filter_type.get() == t
                                on:click=move |_| filter_type.set(t)>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="dq-field">
                <div class="dq-field-head">
                    <span class="row-label">{move || if filter_type.get() == 4 { "Cutoff window" } else { "Window" }}</span>
                    <span class="row-value">
                        {move || {
                            let w = smooth_window.get();
                            if filter_type.get() == 0 || w <= 1 { "Off".to_string() } else { format!("{w} smp") }
                        }}
                    </span>
                </div>
                <input type="range" min="1" max="512" step="1" aria-label="Filter window in samples"
                    prop:disabled=move || filter_type.get() == 0
                    prop:value=move || smooth_window.get().to_string()
                    on:input=move |ev| smooth_window.set(event_target_value(&ev).parse().unwrap_or(1).clamp(1, 512)) />
                <div class="seg seg-sm seg-block" role="radiogroup" aria-label="Filter window presets">
                    {[("Min", 4u32), ("8", 8), ("32", 32), ("128", 128), ("512", 512)].iter().map(|(l, w)| {
                        let w = *w;
                        view!{
                            <button class:active=move || smooth_window.get() == w
                                on:click=move |_| smooth_window.set(w)>{*l}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="row">
                <span class="row-label">"Hi-res"<span class="row-hint">"filter the raw signal, not the envelope"</span></span>
                <Switch
                    checked=raw_filter
                    on_change=Callback::new(move |v: bool| raw_filter.set(v))
                    aria_label="Hi-res filtering"
                    disabled=Signal::derive(move || filter_type.get() == 0)
                />
            </div>
        </section>

        <section class="inspector-section">
            <div class="dq-section-head">
                <h4>"DUT supply"</h4>
                {move || {
                    let snap = snapshots.get();
                    let st = snap.as_ref().and_then(|s| s.status);
                    let en = st.map(|s| s.source_enabled).unwrap_or(false);
                    let iout = snap.as_ref().and_then(|s| s.energy).map(|e| e.last_i as f64).unwrap_or(0.0);
                    let ilim = st.map(|s| s.ilimit_set as f64).unwrap_or(0.0);
                    let cc = ilim > 0.0 && iout.abs() >= 0.95 * ilim;
                    let (txt, tone, tip) = if !en {
                        ("Off", "gray", "Supply output is off")
                    } else if cc {
                        ("CC", "orange", "Constant current: the current limit is active")
                    } else {
                        ("CV", "green", "Constant voltage: regulating to the set voltage")
                    };
                    view!{ <span class=format!("badge tone-{tone}") title=tip>{txt}</span> }
                }}
            </div>
            <div class="row">
                <span class="row-label">"Output"
                    <span class="row-hint">{move || if source_enable.get() { "Powering the DUT" } else { "Disconnected from the DUT" }}</span>
                </span>
                <span class="dq-switch-state">
                    <span>{move || if source_enable.get() { "On" } else { "Off" }}</span>
                    <button type="button" role="switch" class="switch" class:on=move || source_enable.get()
                        aria-checked=move || if source_enable.get() { "true" } else { "false" }
                        aria-label="DUT supply output"
                        on:click=move |_| { source_enable.update(|e| *e = !*e); apply_source(); }></button>
                </span>
            </div>
            {move || (!pd_warn.get().is_empty()).then(|| view!{
                <Callout tone="red">{move || pd_warn.get()}</Callout>
            })}
            <div class="dq-field">
                <div class="dq-field-head">
                    <span class="row-label">"Output voltage"</span>
                    <span class="row-value">{move || format!("{:.3} V", vdut_mv.get() as f64 / 1000.0)}</span>
                </div>
                <input type="range" min="1800" max="19940" step="50" aria-label="DUT voltage in millivolts"
                    prop:value=move || vdut_mv.get().to_string()
                    on:input=move |ev| vdut_mv.set(event_target_value(&ev).parse().unwrap_or(3300).clamp(1800, 19940)) />
            </div>
            <div class="dq-field">
                <div class="dq-field-head">
                    <span class="row-label">"Current limit"</span>
                    <span class="row-value">{move || format!("{} mA", ilimit_ma.get())}</span>
                </div>
                <input type="range" min="100" max="2500" step="10" aria-label="DUT current limit in milliamps"
                    prop:value=move || ilimit_ma.get().to_string()
                    on:input=move |ev| ilimit_ma.set(event_target_value(&ev).parse().unwrap_or(500).clamp(100, 2500)) />
            </div>
            <button class="btn btn-primary btn-sm btn-block" on:click=move |_| apply_supply()>"Apply"</button>
        </section>

        <section class="inspector-section">
            <h4>"Measured"</h4>
            <div class="dq-io-grid">
                <div class="dq-io">
                    <div class="dq-io-title">"Output (DUT)"</div>
                    {move || {
                        let en = snapshots.get().and_then(|s| s.energy);
                        view!{
                            <dl class="kv">
                                <dt>"V"</dt><dd>{en.map(|e| fmt_eng(e.last_v as f64, "V")).unwrap_or_else(|| NO_VALUE.into())}</dd>
                                <dt>"I"</dt><dd>{en.map(|e| fmt_eng(e.last_i as f64, "A")).unwrap_or_else(|| NO_VALUE.into())}</dd>
                                <dt>"P"</dt><dd>{en.map(|e| fmt_eng(e.last_p as f64, "W")).unwrap_or_else(|| NO_VALUE.into())}</dd>
                            </dl>
                        }
                    }}
                </div>
                <div class="dq-io">
                    <div class="dq-io-title">"Input (rail)"</div>
                    {move || {
                        let st = snapshots.get().and_then(|s| s.status);
                        let vin = st.map(|s| s.in_voltage as f64).unwrap_or(0.0);
                        let iin = st.map(|s| s.in_current as f64).unwrap_or(0.0);
                        let pin = vin * iin;
                        view!{
                            <dl class="kv">
                                <dt>"V"</dt><dd>{if vin > 0.0 { fmt_eng(vin, "V") } else { NO_VALUE.into() }}</dd>
                                <dt>"I"</dt><dd>{if iin > 0.0 { fmt_eng(iin, "A") } else { NO_VALUE.into() }}</dd>
                                <dt>"P"</dt><dd>{if pin > 0.0 { fmt_eng(pin, "W") } else { NO_VALUE.into() }}</dd>
                            </dl>
                        }
                    }}
                </div>
            </div>
        </section>
    }
}

async fn slp(ms: u32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        if let Some(w) = web_sys::window() {
            w.set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms as i32)
                .ok();
        }
    });
    wasm_bindgen_futures::JsFuture::from(p).await.ok();
}
