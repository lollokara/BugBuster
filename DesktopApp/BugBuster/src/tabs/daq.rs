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

use crate::tabs::daq_gl::{GlRenderer, Lane};
use crate::tabs::daq_cal::CalibrationWizard;
use crate::tauri_bridge::*;

const LABEL_W: f64 = 70.0;
const RULER_H: f64 = 20.0;
const HEATMAP_H: f64 = 34.0;
const TOP_PAD: f64 = 10.0;
const LANE_GAP: f64 = 8.0;

const SAMPLE_RATE_LABELS: [&str; 5] = ["10 kSPS", "50 kSPS", "100 kSPS", "250 kSPS", "1 MSPS"];

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
    color: &'static str,
    color_gl: [f32; 3],
}

const TRACK_I: TrackInfo = TrackInfo {
    name: "Current",
    unit: "A",
    color: "#3b82f6",
    color_gl: [0.23, 0.51, 0.96],
};
const TRACK_V: TrackInfo = TrackInfo {
    name: "Voltage",
    unit: "V",
    color: "#10b981",
    color_gl: [0.06, 0.73, 0.51],
};
const TRACK_P: TrackInfo = TrackInfo {
    name: "Power",
    unit: "W",
    color: "#f59e0b",
    color_gl: [0.96, 0.62, 0.04],
};

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

    // ---- Selection / hover --------------------------------------------------
    let sel_anchor = RwSignal::new(Option::<u64>::None);
    let sel_start = RwSignal::new(Option::<u64>::None);
    let sel_end = RwSignal::new(Option::<u64>::None);
    let integral = RwSignal::new(Option::<DaqIntegral>::None);
    let hover = RwSignal::new(Option::<(f64, f64)>::None);
    let drag_mode = RwSignal::new(0u8); // 0 = none, 1 = select, 2 = minimap
    let drag_moved = RwSignal::new(false);

    // ---- Panels -------------------------------------------------------------
    let fft_open = RwSignal::new(false);
    let settings_open = RwSignal::new(true);
    let cal_open = RwSignal::new(false);

    // ---- Acquisition / source settings -------------------------------------
    let sample_rate_idx = RwSignal::new(3u8);
    let decimation = RwSignal::new(1u16);
    let range_lock_idx = RwSignal::new(0u8); // 0 = Auto, 1..3 = HI/MID/LO+1
    let vdut_mv = RwSignal::new(3300u32);
    let ilimit_ma = RwSignal::new(500u32);
    let source_enable = RwSignal::new(true);
    let fft_nbins = RwSignal::new(256u16);
    let fft_window = RwSignal::new(1u8);
    let fft_source = RwSignal::new(0u8);

    // ---- Canvas + renderer --------------------------------------------------
    let gl_canvas = NodeRef::<leptos::html::Canvas>::new();
    let overlay = NodeRef::<leptos::html::Canvas>::new();
    let renderer: Rc<RefCell<Option<GlRenderer>>> = Rc::new(RefCell::new(None));

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
        Rc::new(move || {
            let Some(gl_c) = gl_canvas.get() else { return };
            let Some(ov_c) = overlay.get() else { return };
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
            let Some(vd) = view_data.get_untracked() else {
                if let Some(rr) = renderer.borrow().as_ref() {
                    rr.render(pw as f32, ph as f32, x0, x1, &[]);
                }
                return;
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
                let (lo, hi) = col_range(vmin, vmax);
                let (y_top, y_bottom) = regions[i];
                let tint = t.name == "Current" && tint_cur;
                gl_lanes.push(Lane {
                    y_top: (y_top * dpr) as f32,
                    y_bottom: (y_bottom * dpr) as f32,
                    vmin,
                    vmax,
                    source: if tint { src } else { None },
                    lo,
                    hi,
                    color: t.color_gl,
                    tint,
                });
            }
            if let Some(rr) = renderer.borrow().as_ref() {
                rr.render(pw as f32, ph as f32, x0, x1, &gl_lanes);
            }
        })
    };

    // Overlay (2D) pass — cheap; re-runs on hover / selection too.
    let paint_overlay = Rc::new(move || {
        let Some(ov_c) = overlay.get() else { return };
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
        let vd = view_data.get_untracked();
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
    });

    let render_all = {
        let g = render_gl.clone();
        let o = paint_overlay.clone();
        Rc::new(move || {
            g();
            o();
        })
    };

    // Effect A: full repaint when data / tracks / layout change.
    {
        let render_all = render_all.clone();
        Effect::new(move |_| {
            view_data.track();
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
    let refresh_view = {
        let g = view_inflight.clone();
        Rc::new(move || {
            if g.get() {
                return;
            }
            let vs = view_start.get_untracked();
            let ve = view_end.get_untracked();
            if ve <= vs {
                return;
            }
            g.set(true);
            let g2 = g.clone();
            spawn_local(async move {
                if let Some(vd) = daq_get_view(vs, ve, 1800).await {
                    view_data.set(Some(vd));
                }
                g2.set(false);
            });
        })
    };
    let ov_inflight = Rc::new(Cell::new(false));
    let refresh_overview = {
        let g = ov_inflight.clone();
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
            spawn_local(async move {
                if let Some(vd) = daq_get_view(0, total, 1000).await {
                    overview_data.set(Some(vd));
                }
                g2.set(false);
            });
        })
    };

    // ---- Polling loop -------------------------------------------------------
    {
        let alive = alive.clone();
        let refresh_view = refresh_view.clone();
        let refresh_overview = refresh_overview.clone();
        spawn_local(async move {
            // Auto-connect to a real DAQ if present (mock connect handled by panel).
            if let Some(st) = daq_stream_status().await {
                if !st.connected && daq_check_usb().await {
                    daq_connect(false).await;
                }
            }
            let mut tick = 0u32;
            loop {
                if !alive.load(Ordering::SeqCst) {
                    break;
                }
                // Status + aggregate snapshots at ~5 Hz (cheaper than the view).
                if tick % 4 == 0 {
                    if let Some(st) = daq_stream_status().await {
                        total_samples.set(st.total_samples);
                        streaming.set(st.active);
                        if st.sample_rate_hz > 0 {
                            rate_hz.set(st.sample_rate_hz);
                        }
                        status.set(Some(st));
                    }
                    if let Some(snap) = daq_get_snapshots().await {
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
                tick = tick.wrapping_add(1);
                slp(45).await;
            }
        });
    }

    // Fetch integral whenever the selection changes.
    Effect::new(move |_| {
        let (s, e) = (sel_start.get(), sel_end.get());
        if let (Some(s), Some(e)) = (s, e) {
            if e > s {
                spawn_local(async move {
                    if let Some(r) = daq_get_integral(s, e).await {
                        integral.set(Some(r));
                    }
                });
                return;
            }
        }
        integral.set(None);
    });

    // ---- Control actions ----------------------------------------------------
    let start_stream = move |_| {
        let idx = sample_rate_idx.get_untracked();
        let dec = decimation.get_untracked();
        // Fresh capture — view the whole thing as it grows.
        autofocus.set(-2);
        sel_start.set(None);
        sel_end.set(None);
        sel_anchor.set(None);
        view_start.set(0);
        view_end.set(0);
        spawn_local(async move {
            daq_stream_start(idx, dec).await;
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
            daq_set_source(v, il, en).await;
        });
    };
    let apply_range = move || {
        let idx = range_lock_idx.get_untracked();
        let range = if idx == 0 { 0xFF } else { idx - 1 };
        spawn_local(async move {
            daq_set_range_lock(range).await;
        });
    };
    let apply_fft = move || {
        let (n, w, s, en) = (
            fft_nbins.get_untracked(),
            fft_window.get_untracked(),
            fft_source.get_untracked(),
            fft_open.get_untracked(),
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
    let on_mousedown = move |ev: leptos::ev::MouseEvent| {
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
        } else {
            // Plot: drag to select a segment.
            let s = x_to_sample(mx, css_w);
            sel_anchor.set(Some(s));
            sel_start.set(Some(s));
            sel_end.set(Some(s));
            drag_mode.set(1);
            drag_moved.set(false);
        }
    };
    let on_mousemove = move |ev: leptos::ev::MouseEvent| {
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
            2 => center_view_full(mx, css_w),
            _ => {}
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
    let on_wheel = move |ev: leptos::ev::WheelEvent| {
        ev.prevent_default();
        autofocus.set(-1);
        let total = total_samples.get_untracked();
        let vs = view_start.get_untracked();
        let ve = view_end.get_untracked();
        let span = (ve.saturating_sub(vs)).max(2) as f64;
        let factor = if ev.delta_y() > 0.0 { 1.25 } else { 1.0 / 1.25 };
        let new_span = (span * factor).clamp(64.0, total.max(64) as f64) as u64;
        let center = (vs + ve) / 2;
        let half = new_span / 2;
        let mut nvs = center.saturating_sub(half);
        let mut nve = nvs + new_span;
        if nve > total {
            nve = total;
            nvs = total.saturating_sub(new_span);
        }
        view_start.set(nvs);
        view_end.set(nve);
    };

    view! {
        <div class="daq-tab" style="display:flex;flex-direction:column;gap:8px;">
            // Toolbar
            <div class="daq-toolbar" style="display:flex;align-items:center;gap:10px;flex-wrap:wrap;padding:6px 8px;background:#0f172a;border-radius:8px;">
                {move || if streaming.get() {
                    view!{ <button class="btn btn-danger btn-sm" on:click=stop_stream>"■ Stop"</button> }.into_any()
                } else {
                    view!{ <button class="btn btn-primary btn-sm" on:click=start_stream>"▶ Start"</button> }.into_any()
                }}
                <label style="display:flex;align-items:center;gap:4px;font-size:12px;">"Rate"
                    <select class="select select-sm" on:change=move |ev| {
                        let v: u8 = event_target_value(&ev).parse().unwrap_or(3);
                        sample_rate_idx.set(v);
                    }>
                        {SAMPLE_RATE_LABELS.iter().enumerate().map(|(i,l)| {
                            view!{ <option value=i.to_string() selected=move || sample_rate_idx.get() as usize == i>{*l}</option> }
                        }).collect::<Vec<_>>()}
                    </select>
                </label>
                <label style="display:flex;align-items:center;gap:4px;font-size:12px;" title="Autofocus the view on the most recent data, or show the whole capture">"Autofocus"
                    <select class="select select-sm" on:change=move |ev| {
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
                <span style="width:1px;height:20px;background:#334155;"></span>
                // Track toggles
                <TrackToggle sig=show_i info=TRACK_I/>
                <TrackToggle sig=show_v info=TRACK_V/>
                <TrackToggle sig=show_p info=TRACK_P/>
                <label style="display:flex;align-items:center;gap:4px;font-size:12px;" title="Tint current by fusion source (FINE/COARSE/BLEND)">
                    <input type="checkbox" prop:checked=move || tint_source.get()
                        on:change=move |ev| tint_source.set(event_target_checked(&ev)) />
                    "Source tint"
                </label>
                <button class="btn btn-ghost btn-sm" title="Toggle stacked lanes vs. all signals overlaid in one view"
                    on:click=move |_| combined.update(|c| *c = !*c)>
                    {move || if combined.get() { "⊞ Stacked" } else { "⊟ Combined" }}
                </button>
                <span style="flex:1;"></span>
                <span style="font-size:11px;color:#64748b;">"Drag: select · Wheel: zoom · Drag timeline: navigate"</span>
                <button class="btn btn-ghost btn-sm" on:click=move |_| {
                    let open = !fft_open.get_untracked();
                    fft_open.set(open);
                    let (n,w,s) = (fft_nbins.get_untracked(), fft_window.get_untracked(), fft_source.get_untracked());
                    spawn_local(async move { daq_set_fft(n, s, w, open).await; });
                }>{move || if fft_open.get() { "FFT ◀" } else { "FFT ▶" }}</button>
                <button class="btn btn-ghost btn-sm" on:click=move |_| settings_open.update(|o| *o = !*o)>
                    {move || if settings_open.get() { "⚙ Hide" } else { "⚙ Settings" }}
                </button>
                <button class="btn btn-ghost btn-sm" on:click=move |_| cal_open.set(true)>
                    "🔧 Calibrate"
                </button>
            </div>

            // Live readouts
            {move || {
                let snap = snapshots.get();
                let st = snap.as_ref().and_then(|s| s.status);
                let en = snap.as_ref().and_then(|s| s.energy);
                view!{
                    <div class="daq-readouts" style="display:flex;gap:18px;flex-wrap:wrap;font-size:13px;padding:4px 8px;">
                        <Readout label="V" value=en.map(|e| fmt_eng(e.last_v as f64, "V")).unwrap_or("—".into()) color=TRACK_V.color/>
                        <Readout label="I" value=en.map(|e| fmt_eng(e.last_i as f64, "A")).unwrap_or("—".into()) color=TRACK_I.color/>
                        <Readout label="P" value=en.map(|e| fmt_eng(e.last_p as f64, "W")).unwrap_or("—".into()) color=TRACK_P.color/>
                        <Readout label="Energy" value=en.map(|e| format!("{:.3} mWh", e.energy_mwh)).unwrap_or("—".into()) color="#e2e8f0"/>
                        <Readout label="Charge" value=en.map(|e| format!("{:.3} mAh", e.charge_mah)).unwrap_or("—".into()) color="#e2e8f0"/>
                        <Readout label="Range" value=st.map(|s| range_name(s.range)).unwrap_or("—".into()) color="#94a3b8"/>
                    </div>
                }
            }}

            // Main split: plot + (optional) FFT panel + (optional) settings
            <div style="display:flex;flex:1;min-height:0;gap:8px;">
                // Plot column
                <div style="flex:1;min-width:0;display:flex;flex-direction:column;position:relative;">
                    <div style="flex:1;position:relative;min-height:200px;border:1px solid #1e293b;border-radius:8px;overflow:hidden;">
                        <canvas node_ref=gl_canvas style="position:absolute;inset:0;width:100%;height:100%;"></canvas>
                        <canvas node_ref=overlay
                            style="position:absolute;inset:0;width:100%;height:100%;cursor:crosshair;"
                            on:mousedown=on_mousedown
                            on:mousemove=on_mousemove
                            on:mouseup=on_mouseup
                            on:mouseleave=on_mouseleave
                            on:wheel=on_wheel
                        ></canvas>
                    </div>
                    // Selection integral panel
                    {move || {
                        integral.get().map(|r| {
                            view!{
                                <div class="daq-integral" style="margin-top:6px;padding:8px 12px;background:#0f172a;border-radius:8px;display:flex;gap:20px;flex-wrap:wrap;font-size:12px;">
                                    <strong style="color:#a855f7;">"Selection"</strong>
                                    <span>"Δt: "{fmt_time(r.duration_s)}</span>
                                    <span>"Charge: "{format!("{:.4} mAh", r.charge_mah)}</span>
                                    <span>"Energy: "{format!("{:.4} mWh", r.energy_mwh)}</span>
                                    <span>"Avg I: "{fmt_eng(r.avg_i, "A")}</span>
                                    <span>"Avg P: "{fmt_eng(r.avg_p, "W")}</span>
                                    <span style="color:#f59e0b;" title="Projected consumption if this pattern ran for one hour">
                                        "→ 1h: "{format!("{:.2} mWh", r.projected_mwh_per_hour)}
                                    </span>
                                </div>
                            }
                        })
                    }}
                </div>

                // FFT slide-in panel
                <div class="daq-fft-panel"
                    style=move || format!(
                        "width:{};min-width:0;overflow:hidden;transition:width 0.3s ease;background:#0f172a;border-radius:8px;",
                        if fft_open.get() { "320px" } else { "0px" })
                >
                    <div style="padding:10px;width:320px;">
                        <FftPanel snapshots=snapshots fft_nbins=fft_nbins fft_window=fft_window fft_source=fft_source apply=Rc::new(apply_fft.clone())/>
                    </div>
                </div>

                // Settings panel
                <Show when=move || settings_open.get()>
                    <div class="daq-settings" style="width:280px;overflow-y:auto;background:#0f172a;border-radius:8px;padding:12px;font-size:13px;">
                        <SettingsPanel
                            sample_rate_idx=sample_rate_idx decimation=decimation
                            range_lock_idx=range_lock_idx
                            vdut_mv=vdut_mv ilimit_ma=ilimit_ma source_enable=source_enable
                            apply_source=Rc::new(apply_source.clone()) apply_range=Rc::new(apply_range.clone())
                        />
                    </div>
                </Show>

                // SMU calibration wizard (modal; control plane via S3 BBP).
                <CalibrationWizard open=cal_open/>
            </div>
        </div>
    }
}

fn range_name(r: u8) -> String {
    match r {
        0 => "HI 51Ω".into(),
        1 => "MID 2Ω".into(),
        2 => "LO 50mΩ".into(),
        _ => "—".into(),
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

fn col_range(vmin: &[f32], vmax: &[f32]) -> (f32, f32) {
    let lo = vmin.iter().cloned().fold(f32::INFINITY, f32::min);
    let hi = vmax.iter().cloned().fold(f32::NEG_INFINITY, f32::max);
    nice_range(lo, hi)
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
    ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();
    ctx.clear_rect(0.0, 0.0, css_w, css_h);

    let plot_x0 = LABEL_W;
    let plot_w = (css_w - LABEL_W).max(1.0);
    let trace_top = TOP_PAD;
    let trace_bottom = css_h - RULER_H - HEATMAP_H;
    let hm_top = css_h - HEATMAP_H;

    let Some(vd) = vd else {
        ctx.set_fill_style_str("#64748b");
        ctx.set_font("13px sans-serif");
        let _ = ctx.fill_text("Waiting for stream…", plot_x0 + 16.0, css_h / 2.0);
        return;
    };

    let total = vd.total_samples.max(1);
    let vs = vd.view_start;
    let ve = vd.view_end.max(vs + 1);
    let span = (ve - vs) as f64;
    let rate = vd.sample_rate_hz.max(1) as f64;
    let cols = vd.i_min.len();
    let sample_to_x = |s: u64| plot_x0 + ((s.saturating_sub(vs)) as f64 / span) * plot_w;

    // Lane frames + labels.
    ctx.set_font("11px sans-serif");
    if combined {
        ctx.set_stroke_style_str("#1e293b");
        ctx.set_line_width(1.0);
        ctx.stroke_rect(plot_x0, trace_top, plot_w, trace_bottom - trace_top);
        let mut ly = trace_top + 14.0;
        for t in tracks {
            ctx.set_fill_style_str(t.color);
            let _ = ctx.fill_text(t.name, plot_x0 + 8.0, ly);
            ly += 14.0;
        }
    } else {
        for (i, t) in tracks.iter().enumerate() {
            let (y_top, y_bottom) = regions[i];
            ctx.set_stroke_style_str("#1e293b");
            ctx.set_line_width(1.0);
            ctx.stroke_rect(plot_x0, y_top, plot_w, y_bottom - y_top);
            ctx.set_stroke_style_str("#13203a");
            ctx.begin_path();
            ctx.move_to(plot_x0, (y_top + y_bottom) / 2.0);
            ctx.line_to(plot_x0 + plot_w, (y_top + y_bottom) / 2.0);
            ctx.stroke();
            let (vmin, vmax, _) = track_cols(vd, t.name);
            let (lo, hi) = if vmin.is_empty() {
                (0.0, 1.0)
            } else {
                col_range(vmin, vmax)
            };
            ctx.set_fill_style_str(t.color);
            let _ = ctx.fill_text(t.name, 6.0, y_top + 14.0);
            ctx.set_fill_style_str("#64748b");
            let _ = ctx.fill_text(&fmt_eng(hi as f64, t.unit), 6.0, y_top + 28.0);
            let _ = ctx.fill_text(&fmt_eng(lo as f64, t.unit), 6.0, y_bottom - 4.0);
        }
    }

    // Selection highlight.
    if let (Some(s), Some(e)) = (sel_start, sel_end) {
        if e > s {
            let x0 = sample_to_x(s).max(plot_x0);
            let x1 = sample_to_x(e).min(plot_x0 + plot_w);
            ctx.set_fill_style_str("rgba(168,85,247,0.18)");
            ctx.fill_rect(x0, trace_top, (x1 - x0).max(0.0), trace_bottom - trace_top);
            ctx.set_stroke_style_str("#a855f7");
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
    ctx.set_font("10px sans-serif");
    for k in 0..=5 {
        let frac = k as f64 / 5.0;
        let x = plot_x0 + frac * plot_w;
        let t = (vs as f64 + frac * span) / rate;
        ctx.set_stroke_style_str("#16233d");
        ctx.begin_path();
        ctx.move_to(x, trace_top);
        ctx.line_to(x, trace_bottom);
        ctx.stroke();
        ctx.set_fill_style_str("#94a3b8");
        let _ = ctx.fill_text(&fmt_time(t), x + 2.0, trace_bottom + 14.0);
    }

    // Hover guide + per-track readout tooltip.
    if let Some((mx, my)) = hover {
        if mx >= plot_x0 && mx <= plot_x0 + plot_w && my >= trace_top && my <= trace_bottom && cols > 0
        {
            ctx.set_stroke_style_str("#64748b");
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
            let mut lines: Vec<(String, &str)> =
                vec![(format!("t = {}", fmt_time(t)), "#cbd5e1")];
            if combined {
                for tr in tracks {
                    lines.push((format!("{}: {}", tr.name, fmt_eng(val_of(tr.name), tr.unit)), tr.color));
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
                    lines.push((format!("{}: {}", tr.name, fmt_eng(val_of(tr.name), tr.unit)), tr.color));
                }
            }
            let bw = 160.0;
            let lh = 15.0;
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
            ctx.set_fill_style_str("rgba(2,6,23,0.92)");
            ctx.fill_rect(bx, by, bw, bh);
            ctx.set_stroke_style_str("#334155");
            ctx.set_line_width(1.0);
            ctx.stroke_rect(bx, by, bw, bh);
            ctx.set_font("11px sans-serif");
            for (k, (txt, color)) in lines.iter().enumerate() {
                ctx.set_fill_style_str(color);
                let _ = ctx.fill_text(txt, bx + 8.0, by + 14.0 + k as f64 * lh);
            }
        }
    }

    // Full-capture dI/dt minimap + viewport indicator (click/drag to navigate).
    ctx.set_fill_style_str("#0b1220");
    ctx.fill_rect(plot_x0, hm_top, plot_w, HEATMAP_H);
    ctx.set_fill_style_str("#64748b");
    ctx.set_font("10px sans-serif");
    let _ = ctx.fill_text("dI/dt", 6.0, hm_top + 13.0);
    let _ = ctx.fill_text("(all)", 6.0, hm_top + 26.0);
    if let Some(ov) = overview {
        let n = ov.didt.len();
        if n > 0 {
            let max_d = ov.didt.iter().cloned().fold(0.0f32, f32::max).max(1e-9);
            let cw = plot_w / n as f64;
            for (i, &d) in ov.didt.iter().enumerate() {
                let tt = (d / max_d).clamp(0.0, 1.0) as f64;
                let r = (40.0 + 215.0 * tt) as u8;
                let g = (70.0 * (1.0 - tt)) as u8;
                let b = (170.0 * (1.0 - tt)) as u8;
                ctx.set_fill_style_str(&format!("rgb({r},{g},{b})"));
                ctx.fill_rect(plot_x0 + i as f64 * cw, hm_top + 2.0, cw.max(1.0), HEATMAP_H - 4.0);
            }
        }
    }
    let vx0 = plot_x0 + (vs as f64 / total as f64) * plot_w;
    let vx1 = plot_x0 + (ve as f64 / total as f64) * plot_w;
    ctx.set_fill_style_str("rgba(168,85,247,0.22)");
    ctx.fill_rect(vx0, hm_top, (vx1 - vx0).max(2.0), HEATMAP_H);
    ctx.set_stroke_style_str("#a855f7");
    ctx.set_line_width(1.5);
    ctx.stroke_rect(vx0, hm_top, (vx1 - vx0).max(2.0), HEATMAP_H);

    if vd.overflow {
        ctx.set_fill_style_str("#f59e0b");
        ctx.set_font("11px sans-serif");
        let _ = ctx.fill_text("ACQUISITION TRUNCATED", plot_x0 + 8.0, trace_top + 12.0);
    }
}

// ---- Sub-components ---------------------------------------------------------

#[component]
fn TrackToggle(sig: RwSignal<bool>, info: TrackInfo) -> impl IntoView {
    view! {
        <label style=move || format!(
            "display:flex;align-items:center;gap:4px;font-size:12px;color:{};opacity:{};cursor:pointer;",
            info.color, if sig.get() { "1" } else { "0.45" })
        >
            <input type="checkbox" prop:checked=move || sig.get()
                on:change=move |ev| sig.set(event_target_checked(&ev)) />
            {info.name}
        </label>
    }
}

#[component]
fn Readout(label: &'static str, value: String, color: &'static str) -> impl IntoView {
    view! {
        <span style="display:flex;align-items:baseline;gap:5px;">
            <span style="color:#64748b;font-size:11px;">{label}</span>
            <strong style=format!("color:{};font-variant-numeric:tabular-nums;", color)>{value}</strong>
        </span>
    }
}

#[component]
fn FftPanel(
    snapshots: RwSignal<Option<DaqSnapshots>>,
    fft_nbins: RwSignal<u16>,
    fft_window: RwSignal<u8>,
    fft_source: RwSignal<u8>,
    apply: Rc<dyn Fn()>,
) -> impl IntoView {
    let canvas = NodeRef::<leptos::html::Canvas>::new();
    // Redraw spectrum on snapshot change.
    Effect::new(move |_| {
        let snap = snapshots.get();
        let Some(c) = canvas.get() else { return };
        let canvas_el: HtmlCanvasElement = c.unchecked_into();
        let dpr = web_sys::window().map(|w| w.device_pixel_ratio()).unwrap_or(1.0);
        let css_w = canvas_el.client_width() as f64;
        let css_h = 220.0_f64;
        canvas_el.set_width((css_w * dpr) as u32);
        canvas_el.set_height((css_h * dpr) as u32);
        let Some(ctx) = ov2d(&canvas_el) else { return };
        ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();
        ctx.set_fill_style_str("#0b1220");
        ctx.fill_rect(0.0, 0.0, css_w, css_h);
        let bins = snap.and_then(|s| s.fft).map(|f| f.bins).unwrap_or_default();
        if bins.is_empty() {
            ctx.set_fill_style_str("#64748b");
            let _ = ctx.fill_text("No spectrum", 10.0, css_h / 2.0);
            return;
        }
        let max = bins.iter().cloned().fold(1e-9_f32, f32::max);
        ctx.set_stroke_style_str("#22d3ee");
        ctx.set_line_width(1.0);
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
        <div>
            <h3 style="margin:0 0 8px;font-size:14px;">"Spectrum (FFT)"</h3>
            <canvas node_ref=canvas style="width:100%;height:220px;border-radius:6px;"></canvas>
            <div style="display:flex;flex-direction:column;gap:8px;margin-top:10px;">
                <label style="display:flex;justify-content:space-between;align-items:center;font-size:12px;">
                    "Length"
                    <select class="select select-sm" on:change=move |ev| {
                        fft_nbins.set(event_target_value(&ev).parse().unwrap_or(256));
                        apply();
                    }>
                        {[64u16,128,256,512,1024,2048,4096].iter().map(|n| {
                            let n=*n;
                            view!{ <option value=n.to_string() selected=move || fft_nbins.get()==n>{format!("{n}")}</option> }
                        }).collect::<Vec<_>>()}
                    </select>
                </label>
                <label style="display:flex;justify-content:space-between;align-items:center;font-size:12px;">
                    "Window"
                    <select class="select select-sm" on:change=move |ev| {
                        fft_window.set(event_target_value(&ev).parse().unwrap_or(1));
                        apply2();
                    }>
                        <option value="0" selected=move || fft_window.get()==0>"Rectangular"</option>
                        <option value="1" selected=move || fft_window.get()==1>"Hann"</option>
                        <option value="2" selected=move || fft_window.get()==2>"Blackman-Harris"</option>
                    </select>
                </label>
                <label style="display:flex;justify-content:space-between;align-items:center;font-size:12px;">
                    "Source"
                    <select class="select select-sm" on:change=move |ev| {
                        fft_source.set(event_target_value(&ev).parse().unwrap_or(0));
                        apply3();
                    }>
                        <option value="0" selected=move || fft_source.get()==0>"Current"</option>
                        <option value="1" selected=move || fft_source.get()==1>"Power"</option>
                    </select>
                </label>
            </div>
        </div>
    }
}

#[component]
#[allow(clippy::too_many_arguments)]
fn SettingsPanel(
    sample_rate_idx: RwSignal<u8>,
    decimation: RwSignal<u16>,
    range_lock_idx: RwSignal<u8>,
    vdut_mv: RwSignal<u32>,
    ilimit_ma: RwSignal<u32>,
    source_enable: RwSignal<bool>,
    apply_source: Rc<dyn Fn()>,
    apply_range: Rc<dyn Fn()>,
) -> impl IntoView {
    let apply_source2 = apply_source.clone();
    let apply_source3 = apply_source.clone();
    view! {
        <div style="display:flex;flex-direction:column;gap:14px;">
            <section>
                <h3 style="margin:0 0 8px;font-size:14px;">"Acquisition"</h3>
                <label style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;">
                    "Sample rate"
                    <select class="select select-sm" on:change=move |ev| sample_rate_idx.set(event_target_value(&ev).parse().unwrap_or(3))>
                        {SAMPLE_RATE_LABELS.iter().enumerate().map(|(i,l)| {
                            view!{ <option value=i.to_string() selected=move || sample_rate_idx.get() as usize==i>{*l}</option> }
                        }).collect::<Vec<_>>()}
                    </select>
                </label>
                <label style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;">
                    "Decimation"
                    <input type="number" min="1" max="256" class="input input-sm" style="width:80px;"
                        prop:value=move || decimation.get().to_string()
                        on:change=move |ev| decimation.set(event_target_value(&ev).parse().unwrap_or(1).clamp(1,256)) />
                </label>
                <label style="display:flex;justify-content:space-between;align-items:center;">
                    "Range lock"
                    <select class="select select-sm" on:change=move |ev| { range_lock_idx.set(event_target_value(&ev).parse().unwrap_or(0)); apply_range(); }>
                        <option value="0" selected=move || range_lock_idx.get()==0>"Auto"</option>
                        <option value="1" selected=move || range_lock_idx.get()==1>"HI 51Ω (µA)"</option>
                        <option value="2" selected=move || range_lock_idx.get()==2>"MID 2Ω (mA)"</option>
                        <option value="3" selected=move || range_lock_idx.get()==3>"LO 50mΩ (A)"</option>
                    </select>
                </label>
            </section>

            <section>
                <h3 style="margin:0 0 8px;font-size:14px;">"Source (SMU)"</h3>
                <label style="display:flex;align-items:center;gap:6px;margin-bottom:6px;">
                    <input type="checkbox" prop:checked=move || source_enable.get()
                        on:change=move |ev| { source_enable.set(event_target_checked(&ev)); apply_source(); } />
                    "Output enabled"
                </label>
                <label style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;">
                    "V_DUT (mV)"
                    <input type="number" min="1800" max="20000" step="100" class="input input-sm" style="width:90px;"
                        prop:value=move || vdut_mv.get().to_string()
                        on:change=move |ev| { vdut_mv.set(event_target_value(&ev).parse().unwrap_or(3300).clamp(1800,20000)); apply_source2(); } />
                </label>
                <label style="display:flex;justify-content:space-between;align-items:center;">
                    "I limit (mA)"
                    <input type="number" min="100" max="2500" step="10" class="input input-sm" style="width:90px;"
                        prop:value=move || ilimit_ma.get().to_string()
                        on:change=move |ev| { ilimit_ma.set(event_target_value(&ev).parse().unwrap_or(500).clamp(100,2500)); apply_source3(); } />
                </label>
            </section>
        </div>
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
