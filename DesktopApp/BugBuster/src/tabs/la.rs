use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::components::icons::Icon;
use crate::components::ui::{EmptyState, Switch};
use crate::theme::{css_var, use_theme};
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;
use web_sys::{CanvasRenderingContext2d, HtmlCanvasElement, MouseEvent};

use std::cell::{Cell, RefCell};

// Channel colours come from the design tokens so the canvas follows the theme.
const CH_VARS: [&str; 4] = ["--ch-a", "--ch-b", "--ch-c", "--ch-d"];
const TOOLBAR_HEIGHT: f64 = 24.0; // Top padding above the first track
const TRACK_HEIGHT: f64 = 62.0;
// u64::MAX cannot cross serde_wasm_bindgen (it panics in the bridge); 2^53-1 is the JS-safe maximum.
const JS_MAX_SAFE: u64 = (1u64 << 53) - 1;
const LABEL_WIDTH: f64 = 0.0; // Channel names live in the DOM column beside the canvas
const RULER_HEIGHT: f64 = 22.0;
const MINIMAP_HEIGHT: f64 = 28.0;
const SIGNAL_MARGIN: f64 = 8.0;

thread_local! {
    static LAST_CANVAS_SIZE: Cell<(u32, u32, u32)> = const { Cell::new((0, 0, 0)) };
}

fn format_time(seconds: f64) -> String {
    if seconds.abs() < 1e-6 {
        return format!("{:.1}ns", seconds * 1e9);
    }
    if seconds.abs() < 1e-3 {
        return format!("{:.1}µs", seconds * 1e6);
    }
    if seconds.abs() < 1.0 {
        return format!("{:.2}ms", seconds * 1e3);
    }
    format!("{:.3}s", seconds)
}

/// Reformat an annotation text string according to the chosen display format.
/// Only "data" and "address" ann_types are reformatted; control/info stay as-is.
fn reformat_ann(text: &str, fmt: &str, ann_type: &str) -> String {
    if fmt == "hex" {
        return text.to_string();
    }
    if ann_type != "data" && ann_type != "address" {
        return text.to_string();
    }
    // Parse numeric value from "0xNN" or "'c'" encoding
    let (val, suffix): (Option<u64>, String) =
        if let Some(rest) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
            let mut parts = rest.splitn(2, ' ');
            let hex_part = parts.next().unwrap_or("");
            let suf = parts.next().map(|s| format!(" {}", s)).unwrap_or_default();
            (u64::from_str_radix(hex_part, 16).ok(), suf)
        } else if text.starts_with('\'') && text.ends_with('\'') && text.len() == 3 {
            (Some(text.chars().nth(1).unwrap() as u64), String::new())
        } else {
            return text.to_string();
        };
    match val {
        None => text.to_string(),
        Some(v) => match fmt {
            "dec" => format!("{}{}", v, suffix),
            "ascii" => {
                let b = v as u8;
                if (0x20..=0x7E).contains(&b) {
                    format!("'{}'", b as char)
                } else {
                    format!("\\x{:02X}", v)
                }
            }
            "bin" => format!("{:08b}{}", v, suffix),
            _ => text.to_string(),
        },
    }
}

fn stream_status_badge(status: &LaStreamRuntimeStatus) -> &'static str {
    if status.last_error.is_some() {
        "ERROR"
    } else if matches!(status.stop_reason.as_deref(), Some(reason) if reason != "none" && reason != "host_stop")
    {
        "DEGRADED"
    } else if status.active {
        "LIVE"
    } else if status.total_bytes > 0 || status.chunk_count > 0 {
        "STOPPED"
    } else {
        "IDLE"
    }
}

async fn sleep_ms(ms: i32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window()
            .unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms)
            .unwrap();
    });
    let _ = wasm_bindgen_futures::JsFuture::from(promise).await;
}

/// Logic level of `ch` at sample `cs`, from the transitions in the current view.
fn level_at(d: &LaViewData, ch: usize, cs: u64) -> Option<u8> {
    let trans = d.channel_transitions.get(ch)?;
    let mut val = 0u8;
    for &(s, v) in trans.iter() {
        if s <= cs {
            val = v;
        } else {
            break;
        }
    }
    Some(val)
}

fn ch_chip(
    i: u8,
    active: impl Fn() -> bool + Copy + Send + Sync + 'static,
    pick: impl Fn() + Copy + Send + Sync + 'static,
) -> impl IntoView {
    view! {
        <button type="button" class="la-chip"
            class:active=active
            aria-pressed=move || if active() { "true" } else { "false" }
            style=format!("--ch-color: var({})", CH_VARS[i as usize])
            on:click=move |_| pick()
        >{i.to_string()}</button>
    }
}

fn ch_picker(
    label: &'static str,
    active: impl Fn(u8) -> bool + Copy + Send + Sync + 'static,
    pick: impl Fn(u8) + Copy + Send + Sync + 'static,
) -> impl IntoView {
    view! {
        <div class="la-field">
            <span class="la-field-label">{label}</span>
            <div class="la-chips" role="group" aria-label=label>
                {(0..4u8).map(|i| ch_chip(i, move || active(i), move || pick(i))).collect::<Vec<_>>()}
            </div>
        </div>
    }
}

/// Channel picker with an extra "Off" chip (UART RX, SPI CS).
fn ch_picker_off(
    label: &'static str,
    active: impl Fn(u8) -> bool + Copy + Send + Sync + 'static,
    pick: impl Fn(u8) + Copy + Send + Sync + 'static,
    off_active: impl Fn() -> bool + Copy + Send + Sync + 'static,
    set_off: impl Fn() + Copy + Send + Sync + 'static,
) -> impl IntoView {
    view! {
        <div class="la-field">
            <span class="la-field-label">{label}</span>
            <div class="la-chips" role="group" aria-label=label>
                <button type="button" class="la-chip la-chip-off"
                    class:active=off_active
                    aria-pressed=move || if off_active() { "true" } else { "false" }
                    title="Not connected"
                    on:click=move |_| set_off()
                >"Off"</button>
                {(0..4u8).map(|i| ch_chip(i, move || active(i), move || pick(i))).collect::<Vec<_>>()}
            </div>
        </div>
    }
}

#[component]
pub fn LaTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let _ = state;
    // Render epoch — each LaTab mount claims a new epoch so stale render
    // loops from prior mounts exit promptly (Bug Issue 6, now mirrored for LA).
    let render_epoch: RwSignal<u64> = RwSignal::new(0u64);
    let my_epoch = render_epoch.get_untracked().wrapping_add(1);
    render_epoch.set(my_epoch);
    on_cleanup(move || {
        render_epoch.set(render_epoch.get_untracked().wrapping_add(1));
    });

    // Reset shared LAST_CANVAS_SIZE on fresh mount so the DPR/size pass
    // recomputes from scratch (prevents zoomed-top-left render from stale state).
    LAST_CANVAS_SIZE.with(|c| c.set((0, 0, 0)));

    // Capture state
    let (capture_info, set_capture_info) = signal(Option::<LaCaptureInfo>::None);
    let (view_data, set_view_data) = signal(Option::<LaViewData>::None);

    // View window (sample indices)
    let (view_start, set_view_start) = signal(0u64);
    let (view_end, set_view_end) = signal(10000u64);

    // Decoder annotations (fetched from backend)
    let (annotations, set_annotations) = signal(Vec::<serde_json::Value>::new());

    // Config
    let (channels, set_channels) = signal("4".to_string());
    let (rate, set_rate) = signal("1000000".to_string());
    let (depth, set_depth) = signal("100000".to_string());
    let (trig_type, set_trig_type) = signal("0".to_string());
    let (trig_ch, set_trig_ch) = signal("0".to_string());

    // Mode / capture options
    let (rle_enabled, set_rle_enabled) = signal(false);
    let (stream_mode, set_stream_mode) = signal(false);
    let (streaming, set_streaming) = signal(false);
    let (stream_runtime, set_stream_runtime) = signal(LaStreamRuntimeStatus::default());

    // HAT / Signal Conditioning state (fetched on mount, hidden if no HAT)
    let (hat_detected, set_hat_detected) = signal(false);
    let (hat_rails, set_hat_rails) = signal(Vec::<HatRailStatus>::new());
    let (ls_oe, set_ls_oe) = signal(false);
    let (ls_dir, set_ls_dir) = signal(false); // false = B→A (RP2040 listens)
    let (la_route, set_la_route_la) = signal(0u8); // 0 = Low-Speed, 1 = High-Speed

    // Decoder form state
    let (add_dec_type, set_add_dec_type) = signal("uart".to_string());
    let (add_dec_ch_a, set_add_dec_ch_a) = signal(0u8); // UART TX / I2C SDA / SPI MOSI
    let (add_dec_ch_b, set_add_dec_ch_b) = signal(1u8); // UART RX (0xFF=off) / I2C SCL / SPI MISO
    let (add_dec_uart_rx_off, set_add_dec_uart_rx_off) = signal(true); // UART RX disabled by default
    let (add_dec_ch_c, set_add_dec_ch_c) = signal(2u8); // SPI CLK
    let (add_dec_ch_d, set_add_dec_ch_d) = signal(3u8); // SPI CS
    let (add_dec_baud, set_add_dec_baud) = signal("115200".to_string());
    let (add_dec_spi_mode, set_add_dec_spi_mode) = signal(0u8); // CPOL<<1 | CPHA
    let (add_dec_spi_cs_off, set_add_dec_spi_cs_off) = signal(false);
    let (ann_fmt, set_ann_fmt) = signal("hex".to_string());
    let (next_dec_id, set_next_dec_id) = signal(0u32);
    // Active decoders: (id, type, label, ch_a, ch_b, ch_c, ch_d, extra_param)
    let (decoders, set_decoders) =
        signal(Vec::<(u32, String, String, u8, u8, u8, u8, String)>::new());

    // Cursor
    let (cursor_sample, set_cursor_sample) = signal(Option::<u64>::None);

    // Editable channel names (display only)
    let ch_names: [RwSignal<String>; 4] =
        std::array::from_fn(|i| RwSignal::new(format!("CH{}", i)));
    // Per-channel (top, track height, band height) in canvas CSS px, written by the renderer
    let (ch_layout, set_ch_layout) = signal(Vec::<(f64, f64, f64)>::new());
    // Repaint triggers: canvas resize / theme flip
    let (size_tick, set_size_tick) = signal(0u32);
    let (theme_tick, set_theme_tick) = signal(0u32);

    // Auto-downgrade sample rate if it exceeds bandwidth limits in Stream mode
    leptos::prelude::Effect::new(move |_| {
        if stream_mode.get() {
            let ch_count: u8 = channels.get().parse().unwrap_or(4);
            let max_rate = if ch_count <= 1 {
                5000000
            } else if ch_count == 2 {
                2000000
            } else {
                1000000
            };

            let current_rate: u32 = rate.get_untracked().parse().unwrap_or(1000000);
            if current_rate > max_rate {
                set_rate.set(max_rate.to_string());
            }
        }
    });

    // Drag state
    let (dragging, set_dragging) = signal(false);
    let (drag_start_x, set_drag_start_x) = signal(0.0f64);
    let (drag_start_vs, set_drag_start_vs) = signal(0u64);
    let (drag_start_ve, set_drag_start_ve) = signal(0u64);

    // Selection range (sample indices) — click sets anchor, shift+click sets range
    let (sel_anchor, set_sel_anchor) = signal(Option::<u64>::None);
    let (sel_start, set_sel_start) = signal(Option::<u64>::None);
    let (sel_end, set_sel_end) = signal(Option::<u64>::None);

    // Per-channel Y offsets (recomputed when annotations change, shared with mouse handler)
    let (ch_y_offsets, set_ch_y_offsets) = signal(Vec::<f64>::new());

    // Hover state for signal measurement overlay
    let (hover_ch, set_hover_ch) = signal(Option::<usize>::None);
    let (hover_x, set_hover_x) = signal(0.0f64);
    let (_hover_y, set_hover_y) = signal(0.0f64);

    // Canvas ref
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    // Alive flag — flips false on tab unmount so background tasks stop safely.
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    // The canvas has no resize event: poll its size and the resolved theme, then bump a tick.
    let theme = use_theme();
    {
        let alive = alive.clone();
        spawn_local(async move {
            let mut last_theme = "";
            let mut last_size = (0u32, 0u32);
            loop {
                if !alive.load(std::sync::atomic::Ordering::Relaxed)
                    || render_epoch.get_untracked() != my_epoch
                {
                    break;
                }
                let cur_theme = theme.resolved.get_untracked();
                if cur_theme != last_theme {
                    last_theme = cur_theme;
                    set_theme_tick.update(|t| *t = t.wrapping_add(1));
                }
                if let Some(c) = canvas_ref.get_untracked() {
                    let sz = (c.client_width().max(0) as u32, c.client_height().max(0) as u32);
                    if sz != last_size && sz.0 > 0 {
                        last_size = sz;
                        set_size_tick.update(|t| *t = t.wrapping_add(1));
                    }
                }
                sleep_ms(150).await;
            }
        });
    }

    // Listen for "la-done" event (capture complete notification from RP2040)
    {
        let set_ci2 = set_capture_info;
        let alive = alive.clone();
        spawn_local(async move {
            let alive_inner = alive.clone();
            let closure = Closure::new(move |_event: JsValue| {
                if !alive_inner.load(std::sync::atomic::Ordering::Relaxed) {
                    return;
                }
                show_toast("LA Capture complete!", "ok");
                // Fetch capture info and update view
                let alive_deep = alive_inner.clone();
                spawn_local(async move {
                    if let Some(info) = la_get_capture_info().await {
                        if alive_deep.load(std::sync::atomic::Ordering::Relaxed) {
                            set_view_start.set(0);
                            set_view_end.set(info.total_samples);
                            set_ci2.set(Some(info));
                        }
                    }
                });
            });
            listen("la-done", &closure).await;
            closure.forget();
        });
    }

    // Fetch view data when viewport or capture changes (streaming updates capture_info)
    let set_vd = set_view_data;
    let canvas_ref_fetch = canvas_ref;
    let alive_view = alive.clone();
    Effect::new(move |_| {
        let vs = view_start.get();
        let ve = view_end.get();
        let _ci = capture_info.get(); // re-fetch when new data arrives during streaming
        let _sz = size_tick.get(); // re-fetch with the right pixel budget after a resize
        let max_p = canvas_ref_fetch
            .get()
            .and_then(|el| {
                let w = el.client_width() as usize;
                if w == 0 { None } else { Some(w) }
            });
        let alive = alive_view.clone();
        spawn_local(async move {
            if let Some(data) = la_get_view(vs, ve, max_p).await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_vd.set(Some(data));
                }
            }
        });
    });

    // Fetch HAT capabilities and rail state once on mount
    let alive_hat = alive.clone();
    Effect::new(move |_| {
        let alive = alive_hat.clone();
        spawn_local(async move {
            if hat_get_caps().await.is_some() {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_hat_detected.set(true);
                }
            }
            if let Some(rails) = hat_get_rail_status().await {
                if alive.load(std::sync::atomic::Ordering::Relaxed) {
                    set_hat_rails.set(rails);
                }
            }
        });
    });

    // Canvas render effect
    Effect::new(move |_| {
        let data = view_data.get();
        let cursor = cursor_sample.get();
        let fmt = ann_fmt.get(); // track format at top level so changes always redraw
        let _ = (size_tick.get(), theme_tick.get());
                                 // Stale-mount guard (Fix 1): a newer LaTab has taken over — skip render.
        if render_epoch.get_untracked() != my_epoch {
            return;
        }
        let Some(canvas_el) = canvas_ref.get() else {
            return;
        };
        let canvas: HtmlCanvasElement = canvas_el;
        let Some(ctx) = canvas.get_context("2d").ok().flatten() else {
            return;
        };
        let ctx: CanvasRenderingContext2d = ctx.unchecked_into();

        let dpr = web_sys::window().unwrap().device_pixel_ratio();
        let w = canvas.client_width() as f64;
        let h = canvas.client_height() as f64;
        let size_key = (w as u32, h as u32, (dpr * 100.0) as u32);
        LAST_CANVAS_SIZE.with(|cell| {
            if cell.get() != size_key {
                canvas.set_width((w * dpr) as u32);
                canvas.set_height((h * dpr) as u32);
                ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();
                cell.set(size_key);
            }
        });

        // Theme palette, read at draw time so a theme flip repaints correctly.
        let ch_cols: Vec<String> = CH_VARS.iter().map(|v| css_var(v)).collect();
        let c_bg = css_var("--surface-plot");
        let c_panel = css_var("--surface-2");
        let c_surface = css_var("--surface-1");
        let c_grid = css_var("--grid-line");
        let c_sep = css_var("--sep");
        let c_frame = css_var("--sep-strong");
        let c_label = css_var("--label-2");
        let c_dim = css_var("--label-3");
        let c_text = css_var("--label-1");
        let c_cursor = css_var("--series-marker");
        let c_trigger = css_var("--c-orange");
        let c_accent = css_var("--accent");
        let c_sel = css_var("--c-purple");
        let a_green = css_var("--c-green");
        let a_green_t = css_var("--c-green-text");
        let a_orange = css_var("--c-orange");
        let a_orange_t = css_var("--c-orange-text");
        let a_red = css_var("--c-red");
        let a_red_t = css_var("--c-red-text");
        let a_teal = css_var("--c-teal");
        let a_teal_t = css_var("--c-teal-text");
        let a_blue = css_var("--c-blue");
        let a_blue_t = css_var("--c-blue-text");
        let f_mono = {
            let f = css_var("--font-mono");
            if f.is_empty() { "monospace".to_string() } else { f }
        };

        // Background
        ctx.set_fill_style_str(&c_bg);
        ctx.fill_rect(0.0, 0.0, w, h);

        let Some(ref data) = data else {
            // No data: the DOM empty state explains what to do.
            set_ch_layout.set(Vec::new());
            return;
        };

        let num_ch = data.channels as usize;
        let vs = data.view_start as f64;
        let ve = data.view_end as f64;
        let span = ve - vs;
        if span <= 0.0 {
            return;
        }

        let plot_w = w - LABEL_WIDTH;
        let plot_h = h - RULER_HEIGHT - MINIMAP_HEIGHT;

        // ── Dynamic track heights: fill available space, expand for annotations ──
        let anns = annotations.get();
        let ann_row_h = 20.0_f64;
        let ann_gap = 3.0_f64;

        // Count max annotation row per channel
        let mut ch_ann_rows = vec![0usize; num_ch];
        for ann in &anns {
            let ch = ann.get("channel").and_then(|v| v.as_u64()).unwrap_or(0) as usize;
            let row = ann.get("row").and_then(|v| v.as_u64()).unwrap_or(0) as usize + 1;
            if ch < num_ch {
                ch_ann_rows[ch] = ch_ann_rows[ch].max(row);
            }
        }

        // Compute annotation overhead per channel
        let total_ann_extra: f64 = (0..num_ch)
            .map(|ch| {
                if ch_ann_rows[ch] > 0 {
                    ann_gap + ann_row_h * ch_ann_rows[ch] as f64
                } else {
                    0.0
                }
            })
            .sum();

        // Dynamic track height: fill available signal area, min TRACK_HEIGHT
        let available = plot_h - TOOLBAR_HEIGHT - total_ann_extra;
        let track_h = (available / num_ch as f64).max(TRACK_HEIGHT).min(150.0);

        // Cumulative Y offsets per channel
        let mut ch_y = vec![TOOLBAR_HEIGHT; num_ch];
        let mut ch_h = vec![track_h; num_ch];
        {
            let mut y = TOOLBAR_HEIGHT;
            for ch in 0..num_ch {
                ch_y[ch] = y;
                let ann_extra = if ch_ann_rows[ch] > 0 {
                    ann_gap + ann_row_h * ch_ann_rows[ch] as f64
                } else {
                    0.0
                };
                ch_h[ch] = track_h + ann_extra;
                y += ch_h[ch];
            }
        }
        // Share offsets with mouse handler (untracked write — no reactive loop)
        set_ch_y_offsets.set(ch_y.clone());
        set_ch_layout.set((0..num_ch).map(|c| (ch_y[c], track_h, ch_h[c])).collect());

        // Grid lines (batched into single path)
        ctx.set_stroke_style_str(&c_grid);
        ctx.set_line_width(1.0);
        let time_per_px = span / plot_w;
        let grid_step = 10.0f64.powf((time_per_px * 100.0).log10().ceil()); // ~100px spacing
        let first_grid = (vs / grid_step).ceil() * grid_step;
        let mut g = first_grid;
        ctx.begin_path();
        while g < ve {
            let x = LABEL_WIDTH + ((g - vs) / span) * plot_w;
            ctx.move_to(x, TOOLBAR_HEIGHT);
            ctx.line_to(x, plot_h);
            g += grid_step;
        }
        ctx.stroke();

        // Channel separators (batched into single path)
        ctx.set_stroke_style_str(&c_sep);
        ctx.begin_path();
        for ch in 0..num_ch {
            let sep_y = ch_y[ch] + ch_h[ch];
            ctx.move_to(LABEL_WIDTH, sep_y);
            ctx.line_to(w, sep_y);
        }
        ctx.stroke();

        // Draw waveforms
        for ch in 0..num_ch {
            if ch >= data.channel_transitions.len() {
                continue;
            }
            let transitions = &data.channel_transitions[ch];
            let color = ch_cols[ch % 4].as_str();
            let y_top = ch_y[ch] + SIGNAL_MARGIN;
            let y_bot = ch_y[ch] + track_h - SIGNAL_MARGIN;
            let y_high = y_top;
            let y_low = y_bot;

            // Waveform rendering — step function or density mode for zoomed-out signals
            if transitions.is_empty() {
                ctx.set_stroke_style_str(color);
                ctx.set_line_width(1.5);
                ctx.begin_path();
                ctx.move_to(LABEL_WIDTH, y_low);
                ctx.line_to(w, y_low);
                ctx.stroke();
            } else {
                let init_val = if transitions[0].0 <= data.view_start {
                    transitions[0].1
                } else {
                    1 - transitions[0].1
                };

                // Density mode: use high-performance per-pixel fill for decimated/high-freq signals
                let dense = data.decimated
                    || (if transitions.len() < 3 {
                        false
                    } else {
                        let mut min_gap = f64::MAX;
                        for i in 1..transitions.len().min(100) {
                            // check sample of transitions
                            let px1 = (transitions[i - 1].0 as f64 - vs) / span * plot_w;
                            let px2 = (transitions[i].0 as f64 - vs) / span * plot_w;
                            let gap = (px2 - px1).abs();
                            if gap < min_gap {
                                min_gap = gap;
                            }
                            if min_gap < 2.0 {
                                break;
                            }
                        }
                        min_gap < 2.0
                    });
                if dense {
                    let pixels = plot_w as usize;
                    // bit0=has_low, bit1=has_high
                    let mut px_flags = vec![0u8; pixels + 2];
                    let mut cur = init_val;
                    let mut prev_px: i64 = -1;

                    for &(sample, val) in transitions {
                        let px = ((sample as f64 - vs) / span * plot_w) as i64;

                        // Fill pixels between previous and current transition
                        let fill_s = (prev_px + 1).max(0) as usize;
                        let fill_e = (px - 1).min(pixels as i64);
                        if fill_e >= 0 && fill_s <= fill_e as usize {
                            let bit: u8 = if cur == 1 { 0b10 } else { 0b01 };
                            for f in fill_s..=(fill_e as usize).min(pixels) {
                                px_flags[f] |= bit;
                            }
                        }
                        // At transition pixel, mark both incoming and outgoing states
                        if px >= 0 && px <= pixels as i64 {
                            let pu = px as usize;
                            px_flags[pu] |= if cur == 1 { 0b10 } else { 0b01 };
                            px_flags[pu] |= if val == 1 { 0b10 } else { 0b01 };
                        }
                        cur = val;
                        prev_px = px;
                        if px > pixels as i64 {
                            break;
                        }
                    }
                    // Fill remaining pixels with final state
                    let fill_s = (prev_px + 1).max(0) as usize;
                    if fill_s <= pixels {
                        let bit: u8 = if cur == 1 { 0b10 } else { 0b01 };
                        for f in fill_s..=pixels {
                            px_flags[f] |= bit;
                        }
                    }

                    // Gap fill pass:
                    // 1. Any pixel with 0 flags inherits from left neighbor (float rounding gaps)
                    // 2. Single-pixel non-dense (01 or 10) between dense (11) neighbors
                    //    gets promoted to dense — prevents flickering thin lines between bytes
                    for px in 1..=pixels {
                        if px_flags[px] == 0 && px_flags[px - 1] != 0 {
                            px_flags[px] = px_flags[px - 1];
                        }
                    }
                    for px in 1..pixels {
                        let f = px_flags[px];
                        if f != 0 && f != 0b11 {
                            let left = px_flags[px - 1];
                            let right = px_flags[px + 1];
                            if left == 0b11 && right == 0b11 {
                                px_flags[px] = 0b11;
                            }
                        }
                    }

                    // Render with 3-pass run-length batching
                    ctx.set_fill_style_str(color);
                    let max_px = pixels.min(plot_w as usize);

                    // Pass 1: dense transition bars (0b11) — reduced alpha
                    ctx.set_global_alpha(0.7);
                    {
                        let mut run_start: Option<usize> = None;
                        for px in 0..=max_px {
                            if px_flags[px] == 0b11 {
                                run_start.get_or_insert(px);
                            } else if let Some(s) = run_start.take() {
                                ctx.fill_rect(
                                    LABEL_WIDTH + s as f64,
                                    y_high,
                                    (px - s) as f64,
                                    y_low - y_high,
                                );
                            }
                        }
                        if let Some(s) = run_start {
                            ctx.fill_rect(
                                LABEL_WIDTH + s as f64,
                                y_high,
                                (max_px + 1 - s) as f64,
                                y_low - y_high,
                            );
                        }
                    }

                    // Pass 2: high-only lines (0b10)
                    ctx.set_global_alpha(1.0);
                    {
                        let mut run_start: Option<usize> = None;
                        for px in 0..=max_px {
                            if px_flags[px] == 0b10 {
                                run_start.get_or_insert(px);
                            } else if let Some(s) = run_start.take() {
                                ctx.fill_rect(LABEL_WIDTH + s as f64, y_high, (px - s) as f64, 2.0);
                            }
                        }
                        if let Some(s) = run_start {
                            ctx.fill_rect(
                                LABEL_WIDTH + s as f64,
                                y_high,
                                (max_px + 1 - s) as f64,
                                2.0,
                            );
                        }
                    }

                    // Pass 3: low-only lines (0b01)
                    {
                        let mut run_start: Option<usize> = None;
                        for px in 0..=max_px {
                            if px_flags[px] == 0b01 {
                                run_start.get_or_insert(px);
                            } else if let Some(s) = run_start.take() {
                                ctx.fill_rect(
                                    LABEL_WIDTH + s as f64,
                                    y_low - 1.0,
                                    (px - s) as f64,
                                    2.0,
                                );
                            }
                        }
                        if let Some(s) = run_start {
                            ctx.fill_rect(
                                LABEL_WIDTH + s as f64,
                                y_low - 1.0,
                                (max_px + 1 - s) as f64,
                                2.0,
                            );
                        }
                    }
                } else {
                    // Step-function path with sub-pixel protection
                    ctx.set_stroke_style_str(color);
                    ctx.set_line_width(1.5);
                    ctx.begin_path();

                    let y_init = if init_val == 1 { y_high } else { y_low };
                    ctx.move_to(LABEL_WIDTH, y_init);

                    let mut current_val = init_val;
                    let mut drawn_val = init_val;
                    let mut last_drawn_x: f64 = LABEL_WIDTH;

                    for &(sample, val) in transitions {
                        let x = (LABEL_WIDTH + ((sample as f64 - vs) / span) * plot_w)
                            .max(LABEL_WIDTH)
                            .min(w);
                        if val != current_val {
                            if x >= last_drawn_x + 0.5 {
                                // Extend horizontal at drawn level
                                ctx.line_to(x, if drawn_val == 1 { y_high } else { y_low });
                                // Squish any skipped transitions at this x
                                if drawn_val != current_val {
                                    ctx.line_to(x, if current_val == 1 { y_high } else { y_low });
                                }
                                // Draw current transition
                                ctx.line_to(x, if val == 1 { y_high } else { y_low });
                                drawn_val = val;
                                last_drawn_x = x;
                            }
                            current_val = val;
                        }
                    }
                    // Extend to right edge, squishing any pending transitions
                    ctx.line_to(w, if drawn_val == 1 { y_high } else { y_low });
                    if drawn_val != current_val {
                        ctx.line_to(w, if current_val == 1 { y_high } else { y_low });
                    }
                    ctx.stroke();
                }
            }

            // Fill under signal (subtle)
            ctx.set_global_alpha(0.05);
            ctx.set_fill_style_str(color);
            ctx.fill_rect(LABEL_WIDTH, y_top, plot_w, y_bot - y_top);
            ctx.set_global_alpha(1.0);
        }

        // Time ruler with tick marks and consistent unit labels
        let ruler_y = plot_h;
        ctx.set_fill_style_str(&c_panel);
        ctx.fill_rect(0.0, ruler_y, w, RULER_HEIGHT);
        ctx.set_stroke_style_str(&c_frame);
        ctx.begin_path();
        ctx.move_to(0.0, ruler_y);
        ctx.line_to(w, ruler_y);
        ctx.stroke();

        let sample_rate = data.sample_rate_hz as f64;
        let span_sec = if sample_rate > 0.0 {
            span / sample_rate
        } else {
            1.0
        };
        let (unit_suffix, unit_div) = if span_sec < 2e-6 {
            ("ns", 1e9)
        } else if span_sec < 2e-3 {
            ("µs", 1e6)
        } else if span_sec < 2.0 {
            ("ms", 1e3)
        } else {
            ("s", 1.0)
        };

        // Major tick marks + labels
        ctx.set_stroke_style_str(&c_dim);
        ctx.set_line_width(1.0);
        ctx.set_fill_style_str(&c_label);
        ctx.set_font(&format!("{}px {}", 10, f_mono));
        ctx.set_text_align("center");
        let mut g = first_grid;
        while g < ve {
            let x = LABEL_WIDTH + ((g - vs) / span) * plot_w;
            // Major tick
            ctx.begin_path();
            ctx.move_to(x, ruler_y);
            ctx.line_to(x, ruler_y + 4.0);
            ctx.stroke();
            // Label
            let t = if sample_rate > 0.0 {
                g / sample_rate
            } else {
                0.0
            };
            let label = format!("{:.1}{}", t * unit_div, unit_suffix);
            // Keep the first label inside the canvas now that there is no left gutter
            ctx.set_text_align(if x < 24.0 { "left" } else { "center" });
            ctx.fill_text(&label, if x < 24.0 { x + 3.0 } else { x }, ruler_y + 15.0)
                .ok();
            // Minor ticks (5 subdivisions)
            ctx.set_stroke_style_str(&c_frame);
            let minor_step = grid_step / 5.0;
            for m in 1..5 {
                let mx = LABEL_WIDTH + ((g + minor_step * m as f64 - vs) / span) * plot_w;
                if mx > LABEL_WIDTH && mx < w {
                    ctx.begin_path();
                    ctx.move_to(mx, ruler_y);
                    ctx.line_to(mx, ruler_y + 3.0);
                    ctx.stroke();
                }
            }
            ctx.set_stroke_style_str(&c_dim);
            g += grid_step;
        }
        // Cursor
        if let Some(cs) = cursor {
            if cs >= data.view_start && cs <= data.view_end {
                let x = LABEL_WIDTH + ((cs as f64 - vs) / span) * plot_w;
                ctx.set_stroke_style_str(&c_cursor);
                ctx.set_line_width(1.0);
                ctx.begin_path();
                ctx.move_to(x, TOOLBAR_HEIGHT);
                ctx.line_to(x, plot_h);
                ctx.stroke();

                // Cursor time label
                let t = if sample_rate > 0.0 {
                    cs as f64 / sample_rate
                } else {
                    0.0
                };
                let t_label = format_time(t);
                ctx.set_font(&format!("{}px {}", 10, f_mono));
                ctx.set_fill_style_str(&c_panel);
                ctx.set_text_align("center");
                let tw = t_label.len() as f64 * 6.2 + 8.0;
                ctx.fill_rect(x - tw / 2.0, ruler_y + 3.0, tw, RULER_HEIGHT - 4.0);
                ctx.set_fill_style_str(&c_cursor);
                ctx.fill_text(&t_label, x, ruler_y + 15.0).ok();
            }
        }

        // Trigger marker
        if let Some(ts) = data.trigger_sample {
            if ts >= data.view_start && ts <= data.view_end {
                let x = LABEL_WIDTH + ((ts as f64 - vs) / span) * plot_w;
                ctx.set_stroke_style_str(&c_trigger);
                ctx.set_line_width(1.0);
                ctx.set_line_dash(&JsValue::from(js_sys::Array::of2(
                    &JsValue::from(4.0),
                    &JsValue::from(4.0),
                )))
                .ok();
                ctx.begin_path();
                ctx.move_to(x, TOOLBAR_HEIGHT);
                ctx.line_to(x, plot_h);
                ctx.stroke();
                ctx.set_line_dash(&JsValue::from(js_sys::Array::new())).ok();
            }
        }

        // Hover measurement overlay — show period, frequency, duty cycle, width
        if let Some(hch) = hover_ch.get() {
            if hch < data.channel_transitions.len() {
                let transitions = &data.channel_transitions[hch];
                let color = ch_cols[hch % 4].as_str();
                let mx = hover_x.get();

                if mx > LABEL_WIDTH && transitions.len() >= 3 {
                    let frac = (mx - LABEL_WIDTH) / plot_w;
                    let mouse_sample = vs + span * frac;
                    let sample_rate = data.sample_rate_hz as f64;

                    // Collect ALL edges (both rising and falling) with their types
                    let mut edges: Vec<(u64, bool)> = Vec::new(); // (sample, is_rising)
                    for i in 1..transitions.len() {
                        let prev_val = transitions[i - 1].1;
                        let cur_val = transitions[i].1;
                        if prev_val != cur_val {
                            edges.push((transitions[i].0, cur_val == 1));
                        }
                    }

                    // Find the edge nearest to the mouse cursor
                    if edges.len() >= 2 {
                        let mut nearest_idx = 0;
                        let mut nearest_dist = f64::MAX;
                        for (i, &(s, _)) in edges.iter().enumerate() {
                            let dist = (s as f64 - mouse_sample).abs();
                            if dist < nearest_dist {
                                nearest_dist = dist;
                                nearest_idx = i;
                            }
                        }

                        // Find next edge of the SAME direction to measure a full period
                        let (nearest_sample, nearest_is_rising) = edges[nearest_idx];
                        let mut period_end = None;
                        for i in (nearest_idx + 1)..edges.len() {
                            if edges[i].1 == nearest_is_rising {
                                period_end = Some(i);
                                break;
                            }
                        }

                        if let Some(pe_idx) = period_end {
                            let e1 = nearest_sample;
                            let e2 = edges[pe_idx].0;
                            let period_samples = e2 - e1;

                            if period_samples > 0 && sample_rate > 0.0 {
                                let period_sec = period_samples as f64 / sample_rate;
                                let freq = 1.0 / period_sec;

                                // Find the opposite edge between e1 and e2 for duty cycle
                                let mut high_time = 0u64;
                                if nearest_is_rising {
                                    // Rising→Rising: high time = time from e1 to first falling edge
                                    for i in (nearest_idx + 1)..pe_idx {
                                        if !edges[i].1 {
                                            // falling edge
                                            high_time = edges[i].0 - e1;
                                            break;
                                        }
                                    }
                                } else {
                                    // Falling→Falling: high time = time from first rising to e2
                                    for i in (nearest_idx + 1)..pe_idx {
                                        if edges[i].1 {
                                            // rising edge
                                            high_time = e2 - edges[i].0;
                                            break;
                                        }
                                    }
                                }

                                let duty = high_time as f64 / period_samples as f64 * 100.0;
                                let width_sec = high_time as f64 / sample_rate;

                                // Draw period arrow
                                let x1 = LABEL_WIDTH + ((e1 as f64 - vs) / span) * plot_w;
                                let x2 = LABEL_WIDTH + ((e2 as f64 - vs) / span) * plot_w;
                                let y_track_top = ch_y
                                    .get(hch)
                                    .copied()
                                    .unwrap_or(TOOLBAR_HEIGHT + hch as f64 * track_h)
                                    + SIGNAL_MARGIN;
                                let arrow_y = y_track_top - 2.0;

                                ctx.set_stroke_style_str(color);
                                ctx.set_line_width(1.0);
                                ctx.begin_path();
                                // Arrow line
                                ctx.move_to(x1, arrow_y);
                                ctx.line_to(x2, arrow_y);
                                // Arrowheads
                                ctx.move_to(x1, arrow_y);
                                ctx.line_to(x1 + 5.0, arrow_y - 3.0);
                                ctx.move_to(x1, arrow_y);
                                ctx.line_to(x1 + 5.0, arrow_y + 3.0);
                                ctx.move_to(x2, arrow_y);
                                ctx.line_to(x2 - 5.0, arrow_y - 3.0);
                                ctx.move_to(x2, arrow_y);
                                ctx.line_to(x2 - 5.0, arrow_y + 3.0);
                                // Vertical markers
                                let y_bot = ch_y
                                    .get(hch)
                                    .copied()
                                    .unwrap_or(TOOLBAR_HEIGHT + hch as f64 * track_h)
                                    + track_h
                                    - SIGNAL_MARGIN;
                                ctx.move_to(x1, y_track_top);
                                ctx.line_to(x1, y_bot);
                                ctx.move_to(x2, y_track_top);
                                ctx.line_to(x2, y_bot);
                                ctx.stroke();

                                // Measurement label
                                let box_x = (x1 + x2) / 2.0;
                                let box_y = arrow_y - 6.0;

                                let freq_str = if freq >= 1e6 {
                                    format!("{:.2} MHz", freq / 1e6)
                                } else if freq >= 1e3 {
                                    format!("{:.2} kHz", freq / 1e3)
                                } else {
                                    format!("{:.1} Hz", freq)
                                };

                                let label = format!(
                                    "P: {}  F: {}  W: {}  D: {:.1}%",
                                    format_time(period_sec),
                                    freq_str,
                                    format_time(width_sec),
                                    duty
                                );

                                ctx.set_font(&format!("{}px {}", 10, f_mono));
                                let text_width = label.len() as f64 * 6.1;
                                let bx = box_x - text_width / 2.0 - 4.0;
                                let by = box_y - 12.0;
                                ctx.set_global_alpha(0.94);
                                ctx.set_fill_style_str(&c_surface);
                                ctx.fill_rect(bx, by, text_width + 8.0, 14.0);
                                ctx.set_global_alpha(1.0);
                                ctx.set_stroke_style_str(color);
                                ctx.set_line_width(0.5);
                                ctx.stroke_rect(bx, by, text_width + 8.0, 14.0);
                                ctx.set_fill_style_str(&c_text);
                                ctx.set_text_align("center");
                                ctx.fill_text(&label, box_x, box_y - 1.0).ok();
                            }
                        }
                    }
                }
            }
        }

        // Overview bar — density heatmap + viewport indicator + time range
        {
            let mm_y = h - MINIMAP_HEIGHT;
            let mm_w = w - LABEL_WIDTH;
            let mm_heat_top = mm_y + 10.0; // below time labels
            let mm_heat_h = MINIMAP_HEIGHT - 12.0;

            // Background
            ctx.set_fill_style_str(&c_panel);
            ctx.fill_rect(0.0, mm_y, w, MINIMAP_HEIGHT);
            ctx.set_stroke_style_str(&c_frame);
            ctx.begin_path();
            ctx.move_to(0.0, mm_y);
            ctx.line_to(w, mm_y);
            ctx.stroke();

            let total = data.total_samples as f64;
            if total > 0.0 {
                // Time range labels
                let total_sec = if sample_rate > 0.0 {
                    total / sample_rate
                } else {
                    0.0
                };
                ctx.set_fill_style_str(&c_dim);
                ctx.set_font(&format!("{}px {}", 9, f_mono));
                ctx.set_text_align("left");
                ctx.fill_text("0s", LABEL_WIDTH + 3.0, mm_y + 9.0).ok();
                ctx.set_text_align("right");
                ctx.fill_text(&format_time(total_sec), w - 3.0, mm_y + 9.0)
                    .ok();

                // Density heatmap
                let density = &data.density;
                if !density.is_empty() {
                    let max_d = *density.iter().max().unwrap_or(&1) as f64;
                    let buckets = density.len() as f64;
                    for (i, &count) in density.iter().enumerate() {
                        if count == 0 {
                            continue;
                        }
                        let intensity = (count as f64 / max_d).sqrt(); // sqrt for better contrast
                        let bx = LABEL_WIDTH + (i as f64 / buckets) * mm_w;
                        let bw = (mm_w / buckets).max(1.0);
                        // Accent colour; stronger = more transitions
                        ctx.set_global_alpha(0.12 + 0.88 * intensity);
                        ctx.set_fill_style_str(&c_accent);
                        ctx.fill_rect(bx, mm_heat_top, bw + 0.5, mm_heat_h);
                    }
                }

                ctx.set_global_alpha(1.0);
                // Viewport indicator box
                let vp_x1 = LABEL_WIDTH + (data.view_start as f64 / total) * mm_w;
                let vp_x2 =
                    LABEL_WIDTH + (data.view_end.min(data.total_samples) as f64 / total) * mm_w;
                let vp_w = (vp_x2 - vp_x1).max(3.0);
                ctx.set_global_alpha(0.14);
                ctx.set_fill_style_str(&c_sel);
                ctx.fill_rect(vp_x1, mm_heat_top, vp_w, mm_heat_h);
                ctx.set_global_alpha(1.0);
                ctx.set_stroke_style_str(&c_sel);
                ctx.set_line_width(1.5);
                ctx.stroke_rect(vp_x1, mm_heat_top, vp_w, mm_heat_h);
                // Edge grips
                ctx.set_line_width(2.0);
                ctx.set_stroke_style_str(&c_sel);
                for xg in [vp_x1, vp_x1 + vp_w] {
                    ctx.begin_path();
                    ctx.move_to(xg, mm_heat_top + 2.0);
                    ctx.line_to(xg, mm_heat_top + mm_heat_h - 2.0);
                    ctx.stroke();
                }
            }

        }

        // Draw selection highlight
        let ss_sel = sel_start.get();
        let se_sel = sel_end.get();
        if let (Some(s_sel), Some(e_sel)) = (ss_sel, se_sel) {
            if e_sel > data.view_start && s_sel < data.view_end {
                let x1_sel =
                    LABEL_WIDTH + ((s_sel.max(data.view_start) as f64 - vs) / span) * plot_w;
                let x2_sel = LABEL_WIDTH + ((e_sel.min(data.view_end) as f64 - vs) / span) * plot_w;
                ctx.set_fill_style_str(&c_sel);
                ctx.set_global_alpha(0.12);
                ctx.fill_rect(
                    x1_sel,
                    TOOLBAR_HEIGHT,
                    x2_sel - x1_sel,
                    plot_h - TOOLBAR_HEIGHT,
                );
                ctx.set_global_alpha(1.0);
                // Selection edges
                ctx.set_stroke_style_str(&c_sel);
                ctx.set_line_width(1.0);
                for xedge in [x1_sel, x2_sel] {
                    ctx.begin_path();
                    ctx.move_to(xedge, TOOLBAR_HEIGHT);
                    ctx.line_to(xedge, plot_h);
                    ctx.stroke();
                }
            }
        }

        // Draw decoder annotations (anns already computed above for ch_y)
        for ann in &anns {
            let Some(ss) = ann.get("startSample").and_then(|v| v.as_u64()) else {
                continue;
            };
            let Some(es) = ann.get("endSample").and_then(|v| v.as_u64()) else {
                continue;
            };
            let raw_text = ann.get("text").and_then(|v| v.as_str()).unwrap_or("?");
            let ann_type = ann
                .get("annType")
                .and_then(|v| v.as_str())
                .unwrap_or("data");
            let text_owned = reformat_ann(raw_text, &fmt, ann_type);
            let text = text_owned.as_str();
            let (color, tcolor) = match ann_type {
                "address" => (a_green.as_str(), a_green_t.as_str()),
                "control" => (a_orange.as_str(), a_orange_t.as_str()),
                "error" => (a_red.as_str(), a_red_t.as_str()),
                "info" => (a_teal.as_str(), a_teal_t.as_str()),
                _ => (a_blue.as_str(), a_blue_t.as_str()), // data (default)
            };
            let row = ann.get("row").and_then(|v| v.as_u64()).unwrap_or(0) as usize;
            let ch = ann.get("channel").and_then(|v| v.as_u64()).unwrap_or(0) as usize;

            if es < data.view_start || ss > data.view_end {
                continue;
            }

            let x1 = LABEL_WIDTH + ((ss as f64 - vs) / span) * plot_w;
            let x2 = LABEL_WIDTH + ((es as f64 - vs) / span) * plot_w;
            let ann_h = ann_row_h - 2.0;
            // Position annotation below the signal area, in the expanded annotation strip
            let track_y = ch_y
                .get(ch)
                .copied()
                .unwrap_or(TOOLBAR_HEIGHT + ch as f64 * track_h);
            let ann_y = track_y + track_h + ann_gap + ann_row_h * row as f64;

            // Annotation box
            ctx.set_fill_style_str(color);
            ctx.set_global_alpha(0.15);
            ctx.fill_rect(x1, ann_y, (x2 - x1).max(2.0), ann_h);
            ctx.set_global_alpha(1.0);

            // Border
            ctx.set_stroke_style_str(color);
            ctx.set_line_width(1.0);
            ctx.stroke_rect(x1, ann_y, (x2 - x1).max(2.0), ann_h);

            // Text (if box wide enough)
            // Only label boxes wide enough for the text, so narrow frames do not overprint
            if x2 - x1 > text.chars().count() as f64 * 6.6 + 6.0 {
                ctx.set_fill_style_str(tcolor);
                ctx.set_font(&format!("{}px {}", 11, f_mono));
                ctx.set_text_align("center");
                ctx.set_text_baseline("middle");
                let tx = (x1 + x2) / 2.0;
                ctx.fill_text(text, tx, ann_y + ann_h / 2.0).ok();
                ctx.set_text_baseline("alphabetic");
            }
        }
    });

    // Mouse handlers for zoom/pan/cursor
    // rAF scroll throttle: coalesce rapid wheel events into one signal update per frame
    thread_local! {
        static PENDING_VIEWPORT: RefCell<Option<(u64, u64)>> = const { RefCell::new(None) };
        static RAF_SCHEDULED: Cell<bool> = const { Cell::new(false) };
    }

    let on_wheel = move |e: web_sys::WheelEvent| {
        e.prevent_default();
        let vs = view_start.get_untracked();
        let ve = view_end.get_untracked();
        let span = (ve - vs) as f64;

        // Gentler zoom: 5% per scroll tick
        let factor = if e.delta_y() < 0.0 { 0.95 } else { 1.0 / 0.95 };

        // Zoom centered on mouse position
        let Some(canvas_el) = canvas_ref.get() else {
            return;
        };
        let canvas: HtmlCanvasElement = canvas_el;
        let rect = canvas.get_bounding_client_rect();
        let mouse_x = e.client_x() as f64 - rect.left();
        let w = canvas.client_width() as f64;
        let plot_w = w - LABEL_WIDTH;

        // Fraction of viewport where mouse is (0.0 = left edge, 1.0 = right edge)
        let frac = ((mouse_x - LABEL_WIDTH) / plot_w).clamp(0.0, 1.0);

        let total = capture_info
            .get_untracked()
            .map(|i| i.total_samples)
            .unwrap_or(u64::MAX);
        let margin = (total as f64 * 0.05).max(100.0) as u64; // 5% margin
        let limit = total.saturating_add(margin);
        let new_span = (span * factor).max(10.0).min(limit as f64);
        // Keep the sample under the mouse cursor fixed
        let mouse_sample = vs as f64 + span * frac;
        let mut new_start = (mouse_sample - new_span * frac).max(0.0) as u64;
        let mut new_end = new_start + new_span as u64;
        // Clamp to capture bounds with margin
        if new_end > limit {
            new_end = limit;
            new_start = limit.saturating_sub(new_span as u64);
        }

        // Write the computed viewport to the pending slot
        PENDING_VIEWPORT.with(|p| *p.borrow_mut() = Some((new_start, new_end)));

        // Schedule a rAF callback if one isn't already pending
        if !RAF_SCHEDULED.with(|s| s.get()) {
            RAF_SCHEDULED.with(|s| s.set(true));
            let cb = Closure::once(move || {
                RAF_SCHEDULED.with(|s| s.set(false));
                if let Some((s, e)) = PENDING_VIEWPORT.with(|p| p.borrow_mut().take()) {
                    set_view_start.set(s);
                    set_view_end.set(e);
                }
            });
            let _ = web_sys::window()
                .unwrap()
                .request_animation_frame(cb.as_ref().unchecked_ref());
            cb.forget();
        }
    };

    let on_mousedown = move |e: MouseEvent| {
        let Some(canvas_el) = canvas_ref.get() else {
            return;
        };
        let canvas: HtmlCanvasElement = canvas_el;
        let rect = canvas.get_bounding_client_rect();
        let x = e.client_x() as f64 - rect.left();
        let y = e.client_y() as f64 - rect.top();
        let h = canvas.client_height() as f64;
        if x < LABEL_WIDTH {
            return;
        }

        // Check if click is in the overview/minimap bar
        let mm_y = h - MINIMAP_HEIGHT;
        if y >= mm_y {
            // Click in minimap → jump viewport to that position
            let total = capture_info
                .get_untracked()
                .map(|i| i.total_samples)
                .unwrap_or(1);
            let mm_w = canvas.client_width() as f64 - LABEL_WIDTH;
            let frac = ((x - LABEL_WIDTH) / mm_w).clamp(0.0, 1.0);
            let click_sample = (frac * total as f64) as u64;
            let vs = view_start.get_untracked();
            let ve = view_end.get_untracked();
            let half_span = (ve - vs) / 2;
            let new_start = click_sample.saturating_sub(half_span);
            let new_end = new_start + (ve - vs);
            set_view_start.set(new_start.min(total.saturating_sub(ve - vs)));
            set_view_end.set(new_end.min(total));
            return;
        }

        set_dragging.set(true);
        set_drag_start_x.set(x);
        set_drag_start_vs.set(view_start.get_untracked());
        set_drag_start_ve.set(view_end.get_untracked());
    };

    let on_mousemove = move |e: MouseEvent| {
        let Some(canvas_el) = canvas_ref.get() else {
            return;
        };
        let canvas: HtmlCanvasElement = canvas_el;
        let rect = canvas.get_bounding_client_rect();
        let x = e.client_x() as f64 - rect.left();
        let y = e.client_y() as f64 - rect.top();
        let h = canvas.client_height() as f64;
        let plot_h = h - RULER_HEIGHT - MINIMAP_HEIGHT;

        // Track hover position for measurement overlay
        set_hover_x.set(x);
        set_hover_y.set(y);
        if x > LABEL_WIDTH && y > TOOLBAR_HEIGHT && y < plot_h {
            // Find channel whose Y range contains the cursor
            let offsets = ch_y_offsets.get_untracked();
            let ch_idx = if offsets.is_empty() {
                ((y - TOOLBAR_HEIGHT) / TRACK_HEIGHT) as usize
            } else {
                // Last channel whose start ≤ y (within signal area only)
                offsets
                    .iter()
                    .enumerate()
                    .rev()
                    .find(|(_, &cy)| y >= cy && y < cy + TRACK_HEIGHT)
                    .map(|(i, _)| i)
                    .unwrap_or_else(|| offsets.len().saturating_sub(1))
            };
            set_hover_ch.set(Some(ch_idx));
        } else {
            set_hover_ch.set(None);
        }

        // Drag handling
        if !dragging.get_untracked() {
            return;
        }

        let w = canvas.client_width() as f64;
        let plot_w = w - LABEL_WIDTH;

        let dx_px = x - drag_start_x.get_untracked();
        let vs0 = drag_start_vs.get_untracked();
        let ve0 = drag_start_ve.get_untracked();
        let span = (ve0 - vs0) as f64;

        let total = capture_info
            .get_untracked()
            .map(|i| i.total_samples)
            .unwrap_or(u64::MAX);
        let margin = (total as f64 * 0.05).max(100.0) as u64;
        let limit = total.saturating_add(margin);
        let d_samples = (dx_px / plot_w * span) as i64;
        let mut new_start = (vs0 as i64 - d_samples).max(0) as u64;
        let mut new_end = new_start + span as u64;
        // Clamp to capture bounds with margin
        if new_end > limit {
            new_end = limit;
            new_start = limit.saturating_sub(span as u64);
        }

        set_view_start.set(new_start);
        set_view_end.set(new_end);
    };

    let on_mouseup = move |e: MouseEvent| {
        let was_dragging = dragging.get_untracked();
        set_dragging.set(false);

        // If it was a click (no drag), place cursor
        if was_dragging {
            let dx = {
                let Some(canvas_el) = canvas_ref.get() else {
                    return;
                };
                let canvas: HtmlCanvasElement = canvas_el;
                let rect = canvas.get_bounding_client_rect();
                let x = e.client_x() as f64 - rect.left();
                (x - drag_start_x.get_untracked()).abs()
            };
            if dx > 3.0 {
                return;
            } // Was a real drag, not a click
        }

        // Place cursor / selection
        let Some(canvas_el) = canvas_ref.get() else {
            return;
        };
        let canvas: HtmlCanvasElement = canvas_el;
        let rect = canvas.get_bounding_client_rect();
        let x = e.client_x() as f64 - rect.left();
        let w = canvas.client_width() as f64;
        let plot_w = w - LABEL_WIDTH;
        if x < LABEL_WIDTH {
            return;
        }
        let frac = (x - LABEL_WIDTH) / plot_w;
        let vs = view_start.get_untracked();
        let ve = view_end.get_untracked();
        let sample = vs + ((ve - vs) as f64 * frac) as u64;
        set_cursor_sample.set(Some(sample));

        if e.shift_key() {
            // Shift+click: extend selection from anchor to here
            if let Some(anchor) = sel_anchor.get_untracked() {
                let s = anchor.min(sample);
                let e_val = anchor.max(sample);
                set_sel_start.set(Some(s));
                set_sel_end.set(Some(e_val));
                // Copy decoded data in selection to clipboard
                let anns = annotations.get_untracked();
                let decs = decoders.get_untracked();
                let fmt = ann_fmt.get_untracked();
                if !anns.is_empty() {
                    // Build channel→name map from decoders
                    let mut ch_names: std::collections::HashMap<u8, String> =
                        std::collections::HashMap::new();
                    for (_, dtype, _, ch_a, ch_b, _, _, extra) in &decs {
                        match dtype.as_str() {
                            "uart" => {
                                ch_names.insert(*ch_a, "TX".into());
                                // Only register RX if it's enabled (extra = "baud,rxch")
                                let has_rx = extra
                                    .split_once(',')
                                    .map(|x| x.1)
                                    .and_then(|s| s.parse::<u8>().ok())
                                    .is_some();
                                if has_rx {
                                    ch_names.insert(*ch_b, "RX".into());
                                }
                            }
                            "spi" => {
                                ch_names.insert(*ch_a, "MOSI".into());
                                if *ch_b != *ch_a {
                                    ch_names.insert(*ch_b, "MISO".into());
                                }
                            }
                            "i2c" => {
                                ch_names.insert(*ch_a, "SDA".into());
                                ch_names.insert(*ch_b, "SCL".into());
                            }
                            _ => {}
                        }
                    }
                    let multi_ch = ch_names.len() > 1;
                    // Collect annotations that overlap the selection, grouped by channel
                    let mut parts: Vec<(u8, String)> = Vec::new();
                    for ann in &anns {
                        let Some(ss_a) = ann.get("startSample").and_then(|v| v.as_u64()) else {
                            continue;
                        };
                        let Some(es_a) = ann.get("endSample").and_then(|v| v.as_u64()) else {
                            continue;
                        };
                        if es_a < s || ss_a > e_val {
                            continue;
                        }
                        let ann_type = ann
                            .get("annType")
                            .and_then(|v| v.as_str())
                            .unwrap_or("data");
                        if ann_type == "control" || ann_type == "info" {
                            continue;
                        }
                        let raw_text = ann.get("text").and_then(|v| v.as_str()).unwrap_or("?");
                        let ch = ann.get("channel").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                        let text = reformat_ann(raw_text, &fmt, ann_type);
                        parts.push((ch, text));
                    }
                    if !parts.is_empty() {
                        // Build clipboard text
                        let clip_text = if multi_ch {
                            // Group by channel name
                            let mut lines: Vec<String> = Vec::new();
                            let mut last_ch: Option<u8> = None;
                            for (ch, text) in &parts {
                                if last_ch != Some(*ch) {
                                    let name = ch_names.get(ch).map(|s| s.as_str()).unwrap_or("?");
                                    lines.push(format!("[{}] {}", name, text));
                                    last_ch = Some(*ch);
                                } else {
                                    lines.last_mut().unwrap().push(' ');
                                    lines.last_mut().unwrap().push_str(text);
                                }
                            }
                            lines.join("\n")
                        } else {
                            parts
                                .iter()
                                .map(|(_, t)| t.as_str())
                                .collect::<Vec<_>>()
                                .join(" ")
                        };
                        // Copy to clipboard
                        if let Some(win) = web_sys::window() {
                            let nav = win.navigator();
                            let clip = nav.clipboard();
                            let _ = clip.write_text(&clip_text);
                            show_toast(
                                &format!(
                                    "Copied: {}",
                                    if clip_text.len() > 40 {
                                        &clip_text[..40]
                                    } else {
                                        &clip_text
                                    }
                                ),
                                "ok",
                            );
                        }
                    }
                }
            }
        } else {
            // Normal click: set anchor, clear selection
            set_sel_anchor.set(Some(sample));
            set_sel_start.set(None);
            set_sel_end.set(None);
        }
    };

    let on_mouseleave = move |_: MouseEvent| {
        set_dragging.set(false);
    };

    // Keyboard handler: Backspace deletes selected range
    {
        use wasm_bindgen::closure::Closure;
        use wasm_bindgen::JsCast;
        let closure =
            Closure::<dyn Fn(web_sys::KeyboardEvent)>::new(move |e: web_sys::KeyboardEvent| {
                // Ignore if user is typing in an input
                if let Some(tag) = e
                    .target()
                    .and_then(|t| t.dyn_into::<web_sys::Element>().ok())
                    .map(|el| el.tag_name())
                {
                    if tag == "INPUT" || tag == "TEXTAREA" {
                        return;
                    }
                }
                if e.key() == "Backspace" || e.key() == "Delete" {
                    let ss = sel_start.get_untracked();
                    let se = sel_end.get_untracked();
                    if let (Some(s), Some(e_val)) = (ss, se) {
                        e.prevent_default();
                        spawn_local(async move {
                            if let Some(info) = la_delete_range(s, e_val).await {
                                let total = info.total_samples;
                                set_capture_info.set(Some(info));
                                // Clear selection and stale annotations
                                set_sel_anchor.set(None);
                                set_sel_start.set(None);
                                set_sel_end.set(None);
                                set_annotations.set(vec![]);
                                // Adjust viewport
                                let vs = view_start.get_untracked();
                                let ve = view_end.get_untracked();
                                let new_end = ve.min(total);
                                set_view_start.set(vs.min(new_end.saturating_sub(1)));
                                set_view_end.set(new_end);
                                show_toast(&format!("Deleted {} samples", e_val - s + 1), "ok");
                            }
                        });
                    }
                }
                // Escape clears selection
                if e.key() == "Escape" {
                    set_sel_anchor.set(None);
                    set_sel_start.set(None);
                    set_sel_end.set(None);
                }
            });
        let doc = web_sys::window().unwrap().document().unwrap();
        doc.add_event_listener_with_callback("keydown", closure.as_ref().unchecked_ref())
            .unwrap();
        closure.forget();
    }

    let ch_count = move || channels.get().parse::<u8>().unwrap_or(4);
    let max_rate = move || -> u32 {
        if stream_mode.get() {
            let ch = ch_count();
            if ch <= 1 {
                5_000_000
            } else if ch == 2 {
                2_000_000
            } else {
                1_000_000
            }
        } else {
            125_000_000
        }
    };
    let rate_too_fast = move |hz: u32| hz > max_rate();
    let ch_on = move |i: usize| (i as u8) < ch_count();
    let no_data = move || view_data.with(|d| d.is_none());
    let baud_ok = move || add_dec_baud.with(|b| b.trim().parse::<u32>().is_ok_and(|v| v > 0));
    let ch_row_style = move |i: usize| -> String {
        ch_layout.with(|l| match l.get(i) {
            Some(&(y, th, h)) => format!("top:{y}px;height:{h}px;--track-h:{th}px"),
            None => {
                if view_data.with(|d| d.is_some()) {
                    "display:none".to_string()
                } else {
                    format!(
                        "top:{}px;height:{}px;--track-h:{}px",
                        TOOLBAR_HEIGHT + i as f64 * TRACK_HEIGHT,
                        TRACK_HEIGHT,
                        TRACK_HEIGHT
                    )
                }
            }
        })
    };
    let ch_level = move |i: usize| -> Option<u8> {
        let cs = cursor_sample.get()?;
        view_data.with(|d| d.as_ref().and_then(|d| level_at(d, i, cs)))
    };
    let rail0_mv = move || {
        hat_rails.with(|r| {
            r.iter()
                .find(|r| r.rail_id == 0)
                .map(|r| r.voltage_mv)
                .unwrap_or(0)
        })
    };
    let rail0_on = move || {
        hat_rails.with(|r| {
            r.iter()
                .find(|r| r.rail_id == 0)
                .map(|r| r.enabled)
                .unwrap_or(false)
        })
    };
    let insp_open = RwSignal::new(
        web_sys::window()
            .and_then(|w| w.inner_width().ok())
            .and_then(|v| v.as_f64())
            .map(|w| w >= 1100.0)
            .unwrap_or(true),
    );

    view! {
        <div class="la-root">
            // ============ TOP CONTROL BAR ============
            <div class="la-bar" role="toolbar" aria-label="Logic analyzer controls">
                <div class="la-group">
                    <button type="button" class="btn btn-sm btn-primary la-arm"
                        title="Arm the analyzer and start a capture"
                        on:click={
                        let set_ci5 = set_capture_info;
                        move |_| {
                            let ch: u8 = channels.get_untracked().parse().unwrap_or(4);
                            let r: u32 = rate.get_untracked().parse().unwrap_or(1000000);
                            let d: u32 = depth.get_untracked().parse().unwrap_or(100000);
                            let tt: u8 = trig_type.get_untracked().parse().unwrap_or(0);
                            let tc: u8 = trig_ch.get_untracked().parse().unwrap_or(0);
                            let rle_en = rle_enabled.get_untracked();
                            let is_stream = stream_mode.get_untracked();

                            if is_stream {
                                // Stream mode: try gapless USB streaming first, fall back to cycle-based
                                set_streaming.set(false);
                                set_stream_runtime.set(LaStreamRuntimeStatus::default());
                                spawn_local(async move {
                                    // Defensive stop only if backend says a stream is active —
                                    // avoids firing a full BBP STOP + 500ms settle round-trip
                                    // when there's nothing to stop (Pass 4).
                                    if la_stream_usb_active().await {
                                        let _ = la_stream_usb_stop().await;
                                        // Small delay between defensive stop and start — the
                                        // backend's own 500ms settle inside la_stream_usb_stop
                                        // already covers firmware rearm; this is extra margin.
                                        let p = js_sys::Promise::new(&mut |resolve, _| {
                                            web_sys::window().unwrap()
                                                .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 150).unwrap();
                                        });
                                        let _ = wasm_bindgen_futures::JsFuture::from(p).await;
                                    }
                                    set_streaming.set(true);

                                    web_sys::console::log_1(&format!("[STREAM] Starting gapless USB: ch={} rate={} depth={}", ch, r, d).into());

                                    // Try gapless USB streaming (bypasses ESP32 for data path)
                                    let usb_ok = la_stream_usb_start(ch, r, d, rle_en, tt, tc).await;

                                    if usb_ok {
                                        // Gapless mode active — wait a bit for background task to start
                                        web_sys::console::log_1(&"[STREAM] Gapless USB streaming active".into());
                                        show_toast("USB stream started", "ok");

                                        // Give background task time to open port and start streaming
                                        let p = js_sys::Promise::new(&mut |resolve, _| {
                                            web_sys::window().unwrap()
                                                .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 2000).unwrap();
                                        });
                                        let _ = wasm_bindgen_futures::JsFuture::from(p).await;

                                        let mut first_capture = true;
                                        let mut inactive_count = 0u32;
                                        while streaming.get_untracked() {
                                            if render_epoch.get_untracked() != my_epoch { break; }
                                            let p = js_sys::Promise::new(&mut |resolve, _| {
                                                web_sys::window().unwrap()
                                                    .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 200).unwrap();
                                            });
                                            let _ = wasm_bindgen_futures::JsFuture::from(p).await;

                                            let stream_status = la_stream_usb_status().await.unwrap_or_default();
                                            set_stream_runtime.set(stream_status.clone());
                                            if let Some(err) = stream_status.last_error.clone() {
                                                web_sys::console::error_1(&format!("[STREAM] {}", err).into());
                                                show_toast(&format!("USB stream failed: {}", err), "err");
                                                break;
                                            }

                                            // Check if backend is still streaming (allow a few misses while the
                                            // background task tears down cleanly after an explicit stop).
                                            if !stream_status.active && !la_stream_usb_active().await {
                                                inactive_count += 1;
                                                if inactive_count >= 3 {
                                                    web_sys::console::log_1(&"[STREAM] Backend stream stopped".into());
                                                    break;
                                                }
                                            } else {
                                                inactive_count = 0;
                                            }

                                            // Get current store info for UI update
                                            if let Some(info) = la_get_capture_info().await {
                                                if info.total_samples > 0 {
                                                    if first_capture {
                                                        set_view_start.set(0);
                                                        set_view_end.set(info.total_samples);
                                                        first_capture = false;
                                                    }
                                                    set_ci5.set(Some(info));
                                                }
                                            }
                                        }

                                        // Loop exited — the Stop / Clear handlers have already
                                        // called la_stream_usb_stop() (or will, on reentrance
                                        // the backend short-circuits). Only fetch info/status
                                        // here so we don't emit a redundant BBP STOP round-trip
                                        // (Pass 4: collapse redundant stops).
                                        if let Some(info) = la_get_capture_info().await {
                                            set_ci5.set(Some(info));
                                        }
                                        if let Some(status) = la_stream_usb_status().await {
                                            set_stream_runtime.set(status);
                                        }
                                    } else {
                                        // Fallback: cycle-based streaming via la_stream_cycle
                                        web_sys::console::log_1(&"[STREAM] USB gapless unavailable, falling back to cycle mode".into());
                                        let mut first_capture = true;
                                        let mut cycle = 0u32;
                                        while streaming.get_untracked() {
                                            if render_epoch.get_untracked() != my_epoch { break; }
                                            cycle += 1;
                                            let t0 = js_sys::Date::now();
                                            let info = la_stream_cycle(ch, r, d, rle_en, tt, tc).await;
                                            let dt = js_sys::Date::now() - t0;
                                            if let Some(ref i) = info {
                                                web_sys::console::log_1(&format!("[STREAM] Cycle {}: {} samples ({:.0}ms)", cycle, i.total_samples, dt).into());
                                                if first_capture {
                                                    set_view_start.set(0);
                                                    set_view_end.set(i.total_samples);
                                                    first_capture = false;
                                                }
                                                set_ci5.set(Some(i.clone()));
                                            } else {
                                                web_sys::console::log_1(&format!("[STREAM] Cycle {} FAILED ({:.0}ms)", cycle, dt).into());
                                                break;
                                            }
                                        }
                                    }

                                    set_streaming.set(false);
                                    show_toast("Stream stopped", "ok");
                                });
                                return;
                            }

                            spawn_local(async move {
                                // Stop any previous capture
                                la_invoke_stop().await;
                                // Small delay for RP2040 cleanup
                                let promise = js_sys::Promise::new(&mut |resolve, _| {
                                    web_sys::window().unwrap().set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 200).unwrap();
                                });
                                wasm_bindgen_futures::JsFuture::from(promise).await.ok();

                                la_invoke_configure(ch, r, d, rle_en).await;
                                la_invoke_set_trigger(tt, tc).await;
                                la_invoke_arm().await;
                                show_toast(if tt == 0 { "Capturing..." } else { "Armed — waiting for trigger..." }, "ok");

                                // Auto-poll for capture done — simple delay + status loop
                                let mut captured = false;
                                for _ in 0..300 {  // Up to 30 seconds
                                    // Sleep 100ms between polls
                                    let p = js_sys::Promise::new(&mut |resolve, _| {
                                        web_sys::window().unwrap()
                                            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 100).unwrap();
                                    });
                                    let _ = wasm_bindgen_futures::JsFuture::from(p).await;

                                    // Query RP2040 LA status via ESP32
                                    #[derive(serde::Deserialize)]
                                    #[allow(dead_code)]
                                    struct StRsp {
                                        state: Option<u8>,
                                        #[serde(default)]
                                        channels: u8,
                                        #[serde(default, rename = "samplesCaptured")]
                                        samples_captured: u32,
                                        #[serde(default, rename = "totalSamples")]
                                        total_samples: u32,
                                        #[serde(default, rename = "actualRateHz")]
                                        actual_rate_hz: u32,
                                    }
                                    let result = try_invoke("la_get_status", JsValue::NULL).await;
                                    if let Some(st) = result.and_then(|r| serde_wasm_bindgen::from_value::<StRsp>(r).ok()) {
                                        let state = st.state.unwrap_or(255);
                                        if state == 3 {
                                            // DONE — auto-read data
                                            show_toast("Triggered! Reading data...", "ok");
                                            #[derive(serde::Serialize)]
                                            struct RdArgs {
                                                channels: u8,
                                                #[serde(rename = "sampleRateHz")]
                                                sample_rate_hz: u32,
                                                #[serde(rename = "totalSamples")]
                                                total_samples: u32,
                                            }
                                            let args = serde_wasm_bindgen::to_value(&RdArgs {
                                                channels: st.channels.max(ch),
                                                sample_rate_hz: if st.actual_rate_hz > 0 { st.actual_rate_hz } else { r },
                                                total_samples: if st.samples_captured > 0 { st.samples_captured } else { d },
                                            }).unwrap();
                                            let read_result = try_invoke("la_read_uart_chunks", args).await;
                                            if let Some(info) = read_result.and_then(|r| serde_wasm_bindgen::from_value::<LaCaptureInfo>(r).ok()) {
                                                set_view_start.set(0);
                                                set_view_end.set(info.total_samples);
                                                set_ci5.set(Some(info));
                                                show_toast("Capture complete!", "ok");
                                                captured = true;
                                            } else {
                                                show_toast("Data read failed — try Read button", "err");
                                            }
                                            break;
                                        }
                                        // Still armed/capturing — keep polling
                                    }
                                }
                                if !captured {
                                    show_toast("Capture timeout — use Read button manually", "err");
                                }
                            });
                        }
                    }
                    >
                        {move || if streaming.get() {
                            view! { <span class="dot tone-green live"></span> }.into_any()
                        } else {
                            view! { <Icon name="play" size=13 /> }.into_any()
                        }}
                        {move || if streaming.get() { "Stream" } else { "Arm" }}
                    </button>
                    <button type="button" class="btn btn-sm"
                        title="Force a trigger now"
                        on:click=move |_| { spawn_local(async { la_invoke_force().await; show_toast("Triggered", "ok"); }); }
                    ><Icon name="zap" size=13 />"Force"</button>
                    <button type="button" class="btn btn-sm btn-tinted tone-red"
                        title="Stop the capture or stream"
                        on:click=move |_| {
                            // Always reset streaming flag, even if the invoke rejects
                            // (Bug 7) — guarantees the next Arm isn't gated by a stale bit.
                            spawn_local(async move {
                                let _ = la_stream_usb_stop().await;
                                la_invoke_stop().await;
                                set_streaming.set(false);
                                show_toast("Stopped", "ok");
                            });
                        }
                    ><Icon name="square" size=13 />"Stop"</button>
                </div>

                <span class="la-divider"></span>

                <div class="la-group">
                    <label class="la-tb-field">
                        <span>"Rate"</span>
                        <select class="la-select" aria-label="Sample rate" title="Sample rate"
                            on:change=move |e| set_rate.set(event_target_value(&e))
                        >
                            {[("100 kS/s", 100_000u32), ("500 kS/s", 500_000), ("1 MS/s", 1_000_000),
                              ("2 MS/s", 2_000_000), ("5 MS/s", 5_000_000), ("10 MS/s", 10_000_000),
                              ("25 MS/s", 25_000_000), ("50 MS/s", 50_000_000), ("125 MS/s", 125_000_000)]
                                .into_iter().map(|(label, hz)| {
                                    let v = hz.to_string();
                                    let v2 = v.clone();
                                    view! {
                                        <option value=v selected=move || rate.get() == v2
                                            disabled=move || rate_too_fast(hz)>{label}</option>
                                    }
                                }).collect::<Vec<_>>()}
                        </select>
                    </label>
                    <label class="la-tb-field">
                        <span>"Depth"</span>
                        <select class="la-select" aria-label="Memory depth" title="Memory depth (samples)"
                            on:change=move |e| set_depth.set(event_target_value(&e))
                        >
                            {[("10K", "10000"), ("50K", "50000"), ("100K", "100000"), ("500K", "500000")]
                                .into_iter().map(|(label, val)| {
                                    view! {
                                        <option value=val selected=move || depth.get() == val>{label}</option>
                                    }
                                }).collect::<Vec<_>>()}
                        </select>
                    </label>
                </div>

                <span class="la-divider"></span>

                <div class="la-group">
                    <label class="la-tb-field">
                        <span>"Trigger"</span>
                        <select class="la-select" aria-label="Trigger type" title="Trigger condition"
                            on:change=move |e| set_trig_type.set(event_target_value(&e))
                        >
                            {[("None", "0"), ("Rising edge", "1"), ("Falling edge", "2"), ("High level", "4"), ("Low level", "5")]
                                .into_iter().map(|(label, val)| {
                                    view! {
                                        <option value=val selected=move || trig_type.get() == val>{label}</option>
                                    }
                                }).collect::<Vec<_>>()}
                        </select>
                    </label>
                    <select class="la-select la-select-ch" aria-label="Trigger channel"
                        title=move || if trig_type.get() == "0" { "Choose a trigger condition first" } else { "Trigger channel" }
                        disabled=move || trig_type.get() == "0"
                        on:change=move |e| set_trig_ch.set(event_target_value(&e))
                    >
                        {(0..4u8).map(|i| {
                            let v = i.to_string();
                            let v2 = v.clone();
                            view! {
                                <option value=v selected=move || trig_ch.get() == v2>{format!("CH{}", i)}</option>
                            }
                        }).collect::<Vec<_>>()}
                    </select>
                </div>

                <span class="la-divider"></span>

                <div class="la-group">
                    <div class="seg seg-sm" role="radiogroup" aria-label="Capture mode">
                        <button type="button" role="radio"
                            class:active=move || !stream_mode.get()
                            aria-checked=move || if !stream_mode.get() { "true" } else { "false" }
                            title="Capture a fixed memory depth then stop"
                            on:click=move |_| set_stream_mode.set(false)
                        >"Memory"</button>
                        <button type="button" role="radio"
                            class:active=move || stream_mode.get()
                            aria-checked=move || if stream_mode.get() { "true" } else { "false" }
                            title="Continuous live capture (limited sample rate)"
                            on:click=move |_| {
                                set_stream_mode.set(true);
                                set_rle_enabled.set(true);
                                show_toast("RLE enabled by default on Stream mode", "ok");
                                // Limit rate to stream-safe values
                                let r = rate.get_untracked();
                                let r_hz: u32 = r.parse().unwrap_or(1000000);
                                if r_hz > 2000000 { set_rate.set("1000000".to_string()); }
                            }
                        >"Stream"</button>
                    </div>
                    <button type="button" class="btn btn-sm"
                        class:btn-tinted=move || rle_enabled.get()
                        aria-pressed=move || if rle_enabled.get() { "true" } else { "false" }
                        disabled=move || stream_mode.get()
                        title=move || if stream_mode.get() { "RLE is forced in Stream mode" } else { "Run-Length Encoding — compresses captures, more depth for slow signals" }
                        on:click=move |_| {
                            if !stream_mode.get() {
                                set_rle_enabled.update(|v| *v = !*v);
                            }
                        }
                    >"RLE"</button>
                </div>

                <div class="la-group la-bar-end">
                    <div class="la-group la-zoom" role="group" aria-label="Zoom">
                        <button type="button" class="btn btn-sm btn-icon" aria-label="Zoom in" title="Zoom in"
                            on:click=move |_| {
                                let vs = view_start.get_untracked();
                                let ve = view_end.get_untracked();
                                let c = vs + (ve - vs) / 2;
                                let ns = ((ve - vs) as f64 * 0.5) as u64;
                                set_view_start.set(c.saturating_sub(ns / 2));
                                set_view_end.set(c.saturating_sub(ns / 2) + ns.max(10));
                            }
                        ><Icon name="zoom-in" size=15 /></button>
                        <button type="button" class="btn btn-sm btn-icon" aria-label="Zoom out" title="Zoom out"
                            on:click=move |_| {
                                let vs = view_start.get_untracked();
                                let ve = view_end.get_untracked();
                                let c = vs + (ve - vs) / 2;
                                let ns = ((ve - vs) as f64 * 2.0) as u64;
                                set_view_start.set(c.saturating_sub(ns / 2));
                                set_view_end.set(c.saturating_sub(ns / 2) + ns);
                            }
                        ><Icon name="zoom-out" size=15 /></button>
                        <button type="button" class="btn btn-sm" title="Fit the whole capture in view"
                            on:click=move |_| {
                                spawn_local(async move {
                                    if let Some(info) = la_get_capture_info().await {
                                        set_view_start.set(0);
                                        set_view_end.set(info.total_samples);
                                    }
                                });
                            }
                        ><Icon name="maximize-2" size=13 />"Fit"</button>
                    </div>
                    <button type="button" class="btn btn-sm btn-icon btn-plain la-insp-toggle"
                        class:la-toggle-on=move || insp_open.get()
                        aria-label="Toggle inspector" title="Toggle inspector"
                        aria-pressed=move || if insp_open.get() { "true" } else { "false" }
                        on:click=move |_| insp_open.update(|v| *v = !*v)
                    ><Icon name="panel-right" size=16 /></button>
                </div>
            </div>

            // ============ CHANNELS + WAVEFORM + INSPECTOR ============
            <div class="la-main">
                <div class="la-chcol" role="group" aria-label="Channels">
                    {(0..4usize).map(|i| {
                        let name_sig = ch_names[i];
                        view! {
                            <div class="la-ch"
                                class:la-ch-off=move || !ch_on(i)
                                style=move || format!("{};--ch-color:var({})", ch_row_style(i), CH_VARS[i])>
                                <div class="la-ch-in">
                                    <input type="checkbox"
                                        aria-label=format!("Enable CH{}", i)
                                        title=move || {
                                            if ch_on(i) { format!("CH{} enabled — click to disable it and the channels after it", i) }
                                            else { format!("CH{} disabled — click to enable CH0–CH{}", i, i) }
                                        }
                                        prop:checked=move || ch_on(i)
                                        on:change=move |_| {
                                            let i = i as u8;
                                            let ch_count: u8 = channels.get_untracked().parse().unwrap_or(4);
                                            // Click on a disabled channel → enable up to that channel
                                            // Click on the last enabled → reduce count
                                            let new_count = if i < ch_count {
                                                // Clicking an enabled channel: disable from this one onwards (min 1)
                                                i.max(1)
                                            } else {
                                                // Clicking a disabled channel: enable up to and including it
                                                i + 1
                                            };
                                            set_channels.set(new_count.to_string());
                                        }
                                    />
                                    <span class="la-ch-sw" aria-hidden="true"></span>
                                    <input type="text" class="la-ch-name"
                                        aria-label=format!("Name of CH{}", i)
                                        maxlength="16"
                                        prop:value=move || name_sig.get()
                                        on:input=move |e| name_sig.set(event_target_value(&e))
                                    />
                                    <span class="la-ch-lvl num"
                                        class:hi=move || ch_level(i) == Some(1)
                                        title="Level at cursor">
                                        {move || ch_level(i).map(|v| v.to_string()).unwrap_or_default()}
                                    </span>
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()}
                    <div class="la-ch-foot la-ch-foot-ruler">"Time"</div>
                    <div class="la-ch-foot la-ch-foot-map">"Overview"</div>
                </div>

                <div class="la-plot">
                    <canvas node_ref=canvas_ref class="la-canvas"
                        class:la-grabbing=move || dragging.get()
                        aria-label="Logic analyzer waveform"
                        on:wheel=on_wheel
                        on:mousedown=on_mousedown
                        on:mousemove=on_mousemove
                        on:mouseup=on_mouseup
                        on:mouseleave=on_mouseleave
                    />
                    <div class="la-empty" class:la-hidden=move || !no_data()>
                        <EmptyState icon="binary" title="No capture yet"
                            message="Pick a rate and trigger, then press Arm. Or load a generated UART and SPI waveform to try the decoders.">
                            <button type="button" class="btn btn-sm btn-tinted"
                                on:click={
                        let set_ci = set_capture_info;
                        move |_| {
                            spawn_local(async move {
                                let sample_rate: u32 = 1_000_000; // 1MHz
                                let num_samples: u32 = 50_000;
                                let channels: u8 = 4;

                                // Per-sample nibble: bit0=CH0, bit1=CH1, bit2=CH2, bit3=CH3
                                // Default all idle: UART=HIGH(1), SPI MOSI=HIGH(1), CLK=LOW(0), CS=HIGH(1)
                                let mut ps = vec![0b1011u8; num_samples as usize]; // CH0,CH1,CH3=1; CH2=0

                                // ── Helpers ──────────────────────────────────────────
                                fn set_ch(ps: &mut [u8], start: usize, count: usize, ch: u8, val: bool) {
                                    let mask = 1u8 << ch;
                                    for s in start..(start + count).min(ps.len()) {
                                        if val { ps[s] |= mask; } else { ps[s] &= !mask; }
                                    }
                                }

                                // UART: 8N1, idle HIGH, LSB first. Returns end sample.
                                fn uart_byte(ps: &mut [u8], start: usize, byte: u8, spb: usize) -> usize {
                                    let mut s = start;
                                    set_ch(ps, s, spb, 0, false); s += spb; // start LOW
                                    for bit in 0..8u8 {
                                        set_ch(ps, s, spb, 0, (byte >> bit) & 1 == 1); s += spb;
                                    }
                                    s += spb; // stop bit (already HIGH)
                                    s
                                }

                                // SPI mode 0: CLK idle LOW, sample on rising edge.
                                // CS on CH3 (active low), CLK on CH2, MOSI/MISO on CH1.
                                // Returns end sample.
                                fn spi_byte(ps: &mut [u8], start: usize, byte: u8, half: usize) -> usize {
                                    let mut s = start;
                                    for bit in (0..8u8).rev() { // MSB first
                                        let val = (byte >> bit) & 1 == 1;
                                        // CLK low: set MOSI
                                        set_ch(ps, s, half, 2, false); // CLK low
                                        set_ch(ps, s, half, 1, val);   // MOSI
                                        s += half;
                                        // CLK high: data stable (sample here)
                                        set_ch(ps, s, half, 2, true);  // CLK high
                                        set_ch(ps, s, half, 1, val);   // MOSI stable
                                        s += half;
                                    }
                                    s
                                }

                                // ── CH0: UART "Hello!\r\n" then "0x55 0xAA" ──────
                                let spb = 104usize; // ~9600 baud @ 1MHz
                                let mut pos = 500usize;
                                for &b in b"Hello!\r\n" {
                                    pos = uart_byte(&mut ps, pos, b, spb);
                                    pos += spb / 4; // small inter-byte gap
                                }
                                pos += spb * 20; // longer inter-message gap
                                for &b in &[0x55u8, 0xAAu8, 0xFFu8, 0x00u8] {
                                    pos = uart_byte(&mut ps, pos, b, spb);
                                    pos += spb / 4;
                                }

                                // ── CH1/CH2/CH3: SPI 0xDE 0xAD 0xBE 0xEF ──────
                                let half = 5usize; // 100kHz SPI @ 1MHz (10 samples/bit)
                                let mut spi_pos = 25_000usize;
                                // Run bytes first to find end position, then set CS low for full range
                                let cs1_start = spi_pos - 10;
                                for &b in &[0xDEu8, 0xADu8, 0xBEu8, 0xEFu8, 0xCAu8, 0xFEu8] {
                                    spi_pos = spi_byte(&mut ps, spi_pos, b, half);
                                    spi_pos += half; // inter-byte gap
                                }
                                let cs1_end = spi_pos + half * 2;
                                set_ch(&mut ps, cs1_start, cs1_end - cs1_start, 3, false); // CS low during burst
                                set_ch(&mut ps, spi_pos, half * 2, 3, true); // CS high after burst

                                // Second SPI burst: ASCII "SPI" in bytes
                                spi_pos += half * 10;
                                let cs2_start = spi_pos - 5;
                                for &b in b"SPI" {
                                    spi_pos = spi_byte(&mut ps, spi_pos, b, half);
                                    spi_pos += half;
                                }
                                let cs2_end = spi_pos + half * 2;
                                set_ch(&mut ps, cs2_start, cs2_end - cs2_start, 3, false);
                                set_ch(&mut ps, spi_pos, half * 2, 3, true);

                                // Pack nibbles: 2 samples per byte (4 bits each, lower nibble first)
                                let raw: Vec<u8> = ps.chunks(2).map(|c| {
                                    let lo = c[0] & 0x0F;
                                    let hi = if c.len() > 1 { c[1] & 0x0F } else { 0 };
                                    lo | (hi << 4)
                                }).collect();

                                // Load into backend store
                                #[derive(serde::Serialize)]
                                struct Args {
                                    #[serde(rename = "rawData")]
                                    raw_data: Vec<u8>,
                                    channels: u8,
                                    #[serde(rename = "sampleRateHz")]
                                    sample_rate_hz: u32,
                                }
                                let args = serde_wasm_bindgen::to_value(&Args {
                                    raw_data: raw, channels, sample_rate_hz: sample_rate
                                }).unwrap();
                                let result = try_invoke("la_load_raw", args).await;

                                // Debug: log what we got back
                                web_sys::console::log_1(&format!("la_load_raw result: {:?}", result).into());

                                let total: u64 = result.and_then(|r| serde_wasm_bindgen::from_value(r).ok()).unwrap_or(0);

                                if total > 0 {
                                    set_view_start.set(0);
                                    set_view_end.set(total);
                                    set_ci.set(Some(LaCaptureInfo {
                                        channels,
                                        sample_rate_hz: sample_rate,
                                        total_samples: total,
                                        duration_sec: total as f64 / sample_rate as f64,
                                        trigger_sample: None,
                                    }));
                                    show_toast(&format!("Loaded {} test samples", total), "ok");
                                }
                            });
                        }
                    }
                            ><Icon name="wand-sparkles" size=13 />"Load sample waveform"</button>
                        </EmptyState>
                    </div>
                </div>

                <aside class="inspector la-inspector" class:la-hidden=move || !insp_open.get() aria-label="Logic analyzer inspector">
                    <div class="inspector-header">
                        <span>"Inspector"</span>
                        <button type="button" class="btn btn-sm btn-icon btn-plain"
                            aria-label="Close inspector" title="Close inspector"
                            on:click=move |_| insp_open.set(false)
                        ><Icon name="x" size=14 /></button>
                    </div>

                    // ---- Decoders ----
                    <section class="inspector-section" aria-label="Protocol decoders">
                        <div class="la-sec-head">
                            <h4>"Protocol decoders"</h4>
                            <button type="button" class="btn btn-sm btn-tinted"
                                title="Decode the full capture with every decoder below"
                                on:click={
                                let set_ann = set_annotations;
                                move |_| {
                                    let decs = decoders.get_untracked();
                                    // Always decode full capture, not just the visible viewport
                                    let dec_start: u64 = 0;
                                    let dec_end: u64 = capture_info.get_untracked()
                                        .map(|i| i.total_samples)
                                        .unwrap_or(JS_MAX_SAFE);
                                    spawn_local(async move {
                                        let mut all_anns: Vec<serde_json::Value> = Vec::new();
                                        for (_, dtype, _, ch_a, ch_b, ch_c, ch_d, extra) in &decs {
                                            let anns = match dtype.as_str() {
                                                "uart" => {
                                                    // extra format: "baud,rxch" or "baud,"
                                                    let mut parts = extra.splitn(2, ',');
                                                    let baud: u32 = parts.next().unwrap_or("115200").parse().unwrap_or(115200);
                                                    let rx_ch: Option<u8> = parts.next().and_then(|s| s.parse().ok());
                                                    la_decode_uart(*ch_a, rx_ch, baud, dec_start, dec_end).await
                                                }
                                                "i2c" => la_decode_i2c(*ch_a, *ch_b, dec_start, dec_end).await,
                                                "spi" => {
                                                    let mode: u8 = extra.parse().unwrap_or(0);
                                                    la_decode_spi(*ch_a, *ch_b, *ch_c, *ch_d, mode >> 1, mode & 1, dec_start, dec_end).await
                                                }
                                                _ => vec![],
                                            };
                                            all_anns.extend(anns);
                                        }
                                        let n = all_anns.len();
                                        set_ann.set(all_anns);
                                        show_toast(&format!("Decoded {} annotations", n), "ok");
                                    });
                                }
                            }
                            ><Icon name="play" size=12 />"Run All"</button>
                        </div>

                        {move || {
                            let decs = decoders.get();
                            if decs.is_empty() {
                                view! { <p class="la-note">"No decoders yet. Add one below."</p> }.into_any()
                            } else {
                                view! {
                                    <ul class="la-decs">
                                        {decs.iter().map(|(id, _, label, _, _, _, _, _)| {
                                            let id2 = *id;
                                            view! {
                                                <li class="la-dec">
                                                    <span class="la-dec-label">{label.clone()}</span>
                                                    <button type="button" class="btn btn-sm btn-icon btn-plain"
                                                        aria-label="Remove decoder" title="Remove decoder"
                                                        on:click=move |_| {
                                                            set_decoders.update(|v| v.retain(|(id, _, _, _, _, _, _, _)| *id != id2));
                                                        }
                                                    ><Icon name="x" size=13 /></button>
                                                </li>
                                            }
                                        }).collect::<Vec<_>>()}
                                    </ul>
                                }.into_any()
                            }
                        }}

                        <div class="la-add">
                            <div class="la-field">
                                <span class="la-field-label">"Add"</span>
                                <div class="seg seg-sm" role="radiogroup" aria-label="Decoder type">
                                    {["uart", "i2c", "spi"].iter().map(|t| {
                                        let t_str = t.to_string();
                                        let t_str2 = t.to_string();
                                        let ts = t.to_string();
                                        view! {
                                            <button type="button" role="radio"
                                                class:active=move || add_dec_type.get() == t_str
                                                aria-checked=move || if add_dec_type.get() == t_str2 { "true" } else { "false" }
                                                on:click=move |_| set_add_dec_type.set(ts.clone())
                                            >{t.to_uppercase()}</button>
                                        }
                                    }).collect::<Vec<_>>()}
                                </div>
                            </div>

                            {move || {
                                let t = add_dec_type.get();
                                if t == "uart" {
                                    view! {
                                        {ch_picker("TX",
                                            move |i| add_dec_ch_a.get() == i,
                                            move |i| set_add_dec_ch_a.set(i))}
                                        {ch_picker_off("RX",
                                            move |i| !add_dec_uart_rx_off.get() && add_dec_ch_b.get() == i,
                                            move |i| { set_add_dec_uart_rx_off.set(false); set_add_dec_ch_b.set(i); },
                                            move || add_dec_uart_rx_off.get(),
                                            move || set_add_dec_uart_rx_off.set(true))}
                                        <div class="la-field">
                                            <span class="la-field-label">"Baud"</span>
                                            <input type="text" class="la-baud num" inputmode="numeric"
                                                aria-label="UART baud rate"
                                                aria-invalid=move || if baud_ok() { "false" } else { "true" }
                                                prop:value=move || add_dec_baud.get()
                                                on:change=move |e| {
                                                    let input: web_sys::HtmlInputElement = e.target().unwrap().unchecked_into();
                                                    set_add_dec_baud.set(input.value());
                                                }
                                            />
                                        </div>
                                    }.into_any()
                                } else if t == "spi" {
                                    view! {
                                        {ch_picker("MOSI",
                                            move |i| add_dec_ch_a.get() == i,
                                            move |i| set_add_dec_ch_a.set(i))}
                                        {ch_picker("MISO",
                                            move |i| add_dec_ch_b.get() == i,
                                            move |i| set_add_dec_ch_b.set(i))}
                                        {ch_picker("CLK",
                                            move |i| add_dec_ch_c.get() == i,
                                            move |i| set_add_dec_ch_c.set(i))}
                                        {ch_picker_off("CS",
                                            move |i| !add_dec_spi_cs_off.get() && add_dec_ch_d.get() == i,
                                            move |i| { set_add_dec_spi_cs_off.set(false); set_add_dec_ch_d.set(i); },
                                            move || add_dec_spi_cs_off.get(),
                                            move || set_add_dec_spi_cs_off.set(true))}
                                        <div class="la-field">
                                            <span class="la-field-label">"Mode"</span>
                                            <div class="seg seg-sm" role="radiogroup" aria-label="SPI mode">
                                                {[("0", "CPOL0/CPHA0"), ("1", "CPOL0/CPHA1"), ("2", "CPOL1/CPHA0"), ("3", "CPOL1/CPHA1")].iter().map(|(m, tip)| {
                                                    let mv: u8 = m.parse().unwrap();
                                                    view! {
                                                        <button type="button" role="radio"
                                                            class:active=move || add_dec_spi_mode.get() == mv
                                                            aria-checked=move || if add_dec_spi_mode.get() == mv { "true" } else { "false" }
                                                            title=tip.to_string()
                                                            on:click=move |_| set_add_dec_spi_mode.set(mv)
                                                        >{format!("M{}", mv)}</button>
                                                    }
                                                }).collect::<Vec<_>>()}
                                            </div>
                                        </div>
                                    }.into_any()
                                } else {
                                    view! {
                                        {ch_picker("SDA",
                                            move |i| add_dec_ch_a.get() == i,
                                            move |i| set_add_dec_ch_a.set(i))}
                                        {ch_picker("SCL",
                                            move |i| add_dec_ch_b.get() == i,
                                            move |i| set_add_dec_ch_b.set(i))}
                                    }.into_any()
                                }
                            }}

                            <button type="button" class="btn btn-sm btn-primary la-add-btn"
                                on:click=move |_| {
                                let dtype = add_dec_type.get_untracked();
                                let ch_a = add_dec_ch_a.get_untracked();
                                let ch_b = add_dec_ch_b.get_untracked();
                                let ch_c = add_dec_ch_c.get_untracked();
                                let ch_d = add_dec_ch_d.get_untracked();
                                let baud = add_dec_baud.get_untracked();
                                let spi_mode = add_dec_spi_mode.get_untracked();
                                let rx_off = add_dec_uart_rx_off.get_untracked();
                                let cs_off = add_dec_spi_cs_off.get_untracked();
                                let (label, extra) = match dtype.as_str() {
                                    "uart" => {
                                        let rx_part = if rx_off { String::new() } else { format!(" RX=CH{}", ch_b) };
                                        let extra = if rx_off { format!("{},", baud) } else { format!("{},{}", baud, ch_b) };
                                        (format!("UART TX=CH{}{} {}bd", ch_a, rx_part, baud), extra)
                                    }
                                    "i2c"  => (format!("I2C SDA=CH{} SCL=CH{}", ch_a, ch_b), "0".into()),
                                    "spi"  => {
                                        let cs_str = if cs_off { "–".to_string() } else { format!("CH{}", ch_d) };
                                        (format!("SPI MOSI=CH{} MISO=CH{} CLK=CH{} CS={} M{}", ch_a, ch_b, ch_c, cs_str, spi_mode), spi_mode.to_string())
                                    },
                                    _      => return,
                                };
                                let id = next_dec_id.get_untracked();
                                set_next_dec_id.set(id + 1);
                                // For SPI with no CS, store 0xFF as sentinel for cs_channel
                                let effective_ch_d = if dtype == "spi" && cs_off { 0xFF } else { ch_d };
                                set_decoders.update(|v| v.push((id, dtype, label, ch_a, ch_b, ch_c, effective_ch_d, extra)));
                            }
                            ><Icon name="plus" size=13 />"Add decoder"</button>
                        </div>

                        <div class="la-field">
                            <span class="la-field-label">"Format"</span>
                            <div class="seg seg-sm" role="radiogroup" aria-label="Annotation format">
                                {[("Hex", "hex"), ("Dec", "dec"), ("ASCII", "ascii"), ("Bin", "bin")].iter().map(|(lbl, val)| {
                                    let v1 = val.to_string();
                                    let v2 = val.to_string();
                                    let v3 = val.to_string();
                                    let l = lbl.to_string();
                                    view! {
                                        <button type="button" role="radio"
                                            class:active=move || ann_fmt.get() == v1
                                            aria-checked=move || if ann_fmt.get() == v2 { "true" } else { "false" }
                                            on:click=move |_| set_ann_fmt.set(v3.clone())
                                        >{l}</button>
                                    }
                                }).collect::<Vec<_>>()}
                            </div>
                        </div>
                    </section>

                    // ---- Measurements ----
                    <section class="inspector-section" aria-label="Measurements">
                        <h4>"Measurements"</h4>
                        <dl class="kv la-kv">
                            <dt>"Capture"</dt>
                            <dd>{move || capture_info.get().map(|i| format!("{} ch · {}", i.channels, format_time(1.0 / i.sample_rate_hz.max(1) as f64))).unwrap_or_else(|| "—".to_string())}</dd>
                            <dt>"Samples"</dt>
                            <dd>{move || capture_info.get().map(|i| format!("{}", i.total_samples)).unwrap_or_else(|| "—".to_string())}</dd>
                            <dt>"Duration"</dt>
                            <dd>{move || capture_info.get().map(|i| format_time(i.duration_sec)).unwrap_or_else(|| "—".to_string())}</dd>
                            <dt>"Cursor"</dt>
                            <dd>{move || match (cursor_sample.get(), capture_info.get()) {
                                (Some(cs), Some(i)) if i.sample_rate_hz > 0 => format_time(cs as f64 / i.sample_rate_hz as f64),
                                _ => "—".to_string(),
                            }}</dd>
                            <dt>"Selection"</dt>
                            <dd>{move || match (sel_start.get(), sel_end.get(), capture_info.get()) {
                                (Some(s), Some(e), Some(i)) if i.sample_rate_hz > 0 => {
                                    format_time((e - s) as f64 / i.sample_rate_hz as f64)
                                }
                                _ => "—".to_string(),
                            }}</dd>
                            <dt>"Selected"</dt>
                            <dd>{move || match (sel_start.get(), sel_end.get()) {
                                (Some(s), Some(e)) => format!("{} samples", e - s + 1),
                                _ => "—".to_string(),
                            }}</dd>
                        </dl>
                        <p class="la-note">
                            "Hover a signal for period, frequency and duty. Click to place the cursor. "
                            <kbd>"Shift"</kbd>"+click selects a range and copies its decoded data. "
                            <kbd>"Backspace"</kbd>" deletes the selection."
                        </p>
                    </section>

                    // ---- Annotations ----
                    <section class="inspector-section" aria-label="Annotations">
                        <div class="la-sec-head">
                            <h4>"Annotations"</h4>
                            <span class="badge num">{move || annotations.with(|a| a.len())}</span>
                        </div>
                        {move || {
                            let anns = annotations.get();
                            if anns.is_empty() {
                                return view! { <p class="la-note">"Run a decoder to list decoded frames here."</p> }.into_any();
                            }
                            let fmt = ann_fmt.get();
                            let sr = capture_info.with(|i| i.as_ref().map(|i| i.sample_rate_hz as f64).unwrap_or(0.0));
                            let total = anns.len();
                            let rows = anns.iter().take(300).map(|ann| {
                                let ss = ann.get("startSample").and_then(|v| v.as_u64()).unwrap_or(0);
                                let raw = ann.get("text").and_then(|v| v.as_str()).unwrap_or("?");
                                let at = ann.get("annType").and_then(|v| v.as_str()).unwrap_or("data").to_string();
                                let ch = ann.get("channel").and_then(|v| v.as_u64()).unwrap_or(0);
                                let text = reformat_ann(raw, &fmt, &at);
                                let t = if sr > 0.0 { format_time(ss as f64 / sr) } else { format!("{}", ss) };
                                let cls = format!("la-ann-row la-ann-{}", at);
                                view! {
                                    <button type="button" class=cls title="Place the cursor at this frame"
                                        on:click=move |_| set_cursor_sample.set(Some(ss))>
                                        <span class="la-ann-time num">{t}</span>
                                        <span class="la-ann-ch num">{format!("CH{}", ch)}</span>
                                        <span class="la-ann-text num">{text}</span>
                                    </button>
                                }
                            }).collect::<Vec<_>>();
                            view! {
                                <div class="la-anns" role="list">
                                    {rows}
                                    {(total > 300).then(|| view! { <p class="la-note">{format!("+{} more not listed", total - 300)}</p> })}
                                </div>
                            }.into_any()
                        }}
                    </section>

                    // ---- Capture data / files ----
                    <section class="inspector-section" aria-label="Capture data">
                        <h4>"Capture data"</h4>
                        <div class="la-btn-grid">
                            <button type="button" class="btn btn-sm"
                                title="Read the finished capture back from the device over UART"
                                on:click={
                        let set_ci4 = set_capture_info;
                        move |_| {
                            let ch: u8 = channels.get_untracked().parse().unwrap_or(4);
                            let r: u32 = rate.get_untracked().parse().unwrap_or(1000000);
                            let d: u32 = depth.get_untracked().parse().unwrap_or(100000);
                            spawn_local(async move {
                                show_toast("Reading capture data via UART...", "ok");
                                #[derive(serde::Serialize)]
                                #[serde(rename_all = "camelCase")]
                                struct Args { channels: u8, sample_rate_hz: u32, total_samples: u32 }
                                let args = serde_wasm_bindgen::to_value(&Args {
                                    channels: ch, sample_rate_hz: r, total_samples: d,
                                }).unwrap();
                                let result = try_invoke("la_read_uart_chunks", args).await;
                                match result.and_then(|r| serde_wasm_bindgen::from_value::<LaCaptureInfo>(r).ok()) {
                                    Some(info) => {
                                        set_view_start.set(0);
                                        set_view_end.set(info.total_samples);
                                        set_ci4.set(Some(info));
                                        show_toast("Capture data loaded!", "ok");
                                    }
                                    None => {
                                        show_toast("Read failed", "err");
                                    }
                                }
                            });
                        }
                    }
                            ><Icon name="download" size=13 />"Read"</button>
                            <button type="button" class="btn btn-sm"
                                title="Load a generated UART + SPI waveform"
                                on:click={
                        let set_ci = set_capture_info;
                        move |_| {
                            spawn_local(async move {
                                let sample_rate: u32 = 1_000_000; // 1MHz
                                let num_samples: u32 = 50_000;
                                let channels: u8 = 4;

                                // Per-sample nibble: bit0=CH0, bit1=CH1, bit2=CH2, bit3=CH3
                                // Default all idle: UART=HIGH(1), SPI MOSI=HIGH(1), CLK=LOW(0), CS=HIGH(1)
                                let mut ps = vec![0b1011u8; num_samples as usize]; // CH0,CH1,CH3=1; CH2=0

                                // ── Helpers ──────────────────────────────────────────
                                fn set_ch(ps: &mut [u8], start: usize, count: usize, ch: u8, val: bool) {
                                    let mask = 1u8 << ch;
                                    for s in start..(start + count).min(ps.len()) {
                                        if val { ps[s] |= mask; } else { ps[s] &= !mask; }
                                    }
                                }

                                // UART: 8N1, idle HIGH, LSB first. Returns end sample.
                                fn uart_byte(ps: &mut [u8], start: usize, byte: u8, spb: usize) -> usize {
                                    let mut s = start;
                                    set_ch(ps, s, spb, 0, false); s += spb; // start LOW
                                    for bit in 0..8u8 {
                                        set_ch(ps, s, spb, 0, (byte >> bit) & 1 == 1); s += spb;
                                    }
                                    s += spb; // stop bit (already HIGH)
                                    s
                                }

                                // SPI mode 0: CLK idle LOW, sample on rising edge.
                                // CS on CH3 (active low), CLK on CH2, MOSI/MISO on CH1.
                                // Returns end sample.
                                fn spi_byte(ps: &mut [u8], start: usize, byte: u8, half: usize) -> usize {
                                    let mut s = start;
                                    for bit in (0..8u8).rev() { // MSB first
                                        let val = (byte >> bit) & 1 == 1;
                                        // CLK low: set MOSI
                                        set_ch(ps, s, half, 2, false); // CLK low
                                        set_ch(ps, s, half, 1, val);   // MOSI
                                        s += half;
                                        // CLK high: data stable (sample here)
                                        set_ch(ps, s, half, 2, true);  // CLK high
                                        set_ch(ps, s, half, 1, val);   // MOSI stable
                                        s += half;
                                    }
                                    s
                                }

                                // ── CH0: UART "Hello!\r\n" then "0x55 0xAA" ──────
                                let spb = 104usize; // ~9600 baud @ 1MHz
                                let mut pos = 500usize;
                                for &b in b"Hello!\r\n" {
                                    pos = uart_byte(&mut ps, pos, b, spb);
                                    pos += spb / 4; // small inter-byte gap
                                }
                                pos += spb * 20; // longer inter-message gap
                                for &b in &[0x55u8, 0xAAu8, 0xFFu8, 0x00u8] {
                                    pos = uart_byte(&mut ps, pos, b, spb);
                                    pos += spb / 4;
                                }

                                // ── CH1/CH2/CH3: SPI 0xDE 0xAD 0xBE 0xEF ──────
                                let half = 5usize; // 100kHz SPI @ 1MHz (10 samples/bit)
                                let mut spi_pos = 25_000usize;
                                // Run bytes first to find end position, then set CS low for full range
                                let cs1_start = spi_pos - 10;
                                for &b in &[0xDEu8, 0xADu8, 0xBEu8, 0xEFu8, 0xCAu8, 0xFEu8] {
                                    spi_pos = spi_byte(&mut ps, spi_pos, b, half);
                                    spi_pos += half; // inter-byte gap
                                }
                                let cs1_end = spi_pos + half * 2;
                                set_ch(&mut ps, cs1_start, cs1_end - cs1_start, 3, false); // CS low during burst
                                set_ch(&mut ps, spi_pos, half * 2, 3, true); // CS high after burst

                                // Second SPI burst: ASCII "SPI" in bytes
                                spi_pos += half * 10;
                                let cs2_start = spi_pos - 5;
                                for &b in b"SPI" {
                                    spi_pos = spi_byte(&mut ps, spi_pos, b, half);
                                    spi_pos += half;
                                }
                                let cs2_end = spi_pos + half * 2;
                                set_ch(&mut ps, cs2_start, cs2_end - cs2_start, 3, false);
                                set_ch(&mut ps, spi_pos, half * 2, 3, true);

                                // Pack nibbles: 2 samples per byte (4 bits each, lower nibble first)
                                let raw: Vec<u8> = ps.chunks(2).map(|c| {
                                    let lo = c[0] & 0x0F;
                                    let hi = if c.len() > 1 { c[1] & 0x0F } else { 0 };
                                    lo | (hi << 4)
                                }).collect();

                                // Load into backend store
                                #[derive(serde::Serialize)]
                                struct Args {
                                    #[serde(rename = "rawData")]
                                    raw_data: Vec<u8>,
                                    channels: u8,
                                    #[serde(rename = "sampleRateHz")]
                                    sample_rate_hz: u32,
                                }
                                let args = serde_wasm_bindgen::to_value(&Args {
                                    raw_data: raw, channels, sample_rate_hz: sample_rate
                                }).unwrap();
                                let result = try_invoke("la_load_raw", args).await;

                                // Debug: log what we got back
                                web_sys::console::log_1(&format!("la_load_raw result: {:?}", result).into());

                                let total: u64 = result.and_then(|r| serde_wasm_bindgen::from_value(r).ok()).unwrap_or(0);

                                if total > 0 {
                                    set_view_start.set(0);
                                    set_view_end.set(total);
                                    set_ci.set(Some(LaCaptureInfo {
                                        channels,
                                        sample_rate_hz: sample_rate,
                                        total_samples: total,
                                        duration_sec: total as f64 / sample_rate as f64,
                                        trigger_sample: None,
                                    }));
                                    show_toast(&format!("Loaded {} test samples", total), "ok");
                                }
                            });
                        }
                    }
                            ><Icon name="wand-sparkles" size=13 />"Test Data"</button>
                            <button type="button" class="btn btn-sm"
                                title="Import a capture from a JSON file"
                                on:click={
                                let set_ci3 = set_capture_info;
                                move |_| {
                                    spawn_local(async move {
                                        #[derive(serde::Serialize)]
                                        struct Filter { name: String, extensions: Vec<String> }
                                        #[derive(serde::Serialize)]
                                        struct Args { title: String, filters: Vec<Filter> }
                                        let args = serde_wasm_bindgen::to_value(&Args {
                                            title: "Import JSON".into(),
                                            filters: vec![Filter { name: "JSON files".into(), extensions: vec!["json".into()] }],
                                        }).unwrap();
                                        let result = try_invoke("pick_config_open_file", args).await;
                                        if let Some(path) = result.and_then(|r| serde_wasm_bindgen::from_value::<Option<String>>(r).ok().flatten()) {
                                            if !path.is_empty() {
                                                if let Some(info) = la_import_json_file(&path).await {
                                                    set_view_start.set(0);
                                                    set_view_end.set(info.total_samples);
                                                    set_ci3.set(Some(info));
                                                    show_toast("Imported capture", "ok");
                                                }
                                            }
                                        }
                                    });
                                }
                            }
                            ><Icon name="folder-open" size=13 />"Import"</button>
                            <button type="button" class="btn btn-sm btn-tinted tone-red"
                                title="Discard the current capture"
                                on:click=move |_| {
                        // Stop any active streaming
                        set_streaming.set(false);
                        spawn_local(async move {
                            let _ = la_stream_usb_stop().await;
                        });
                        set_capture_info.set(None);
                        set_view_data.set(None);
                        set_annotations.set(vec![]);
                        set_sel_anchor.set(None);
                        set_sel_start.set(None);
                        set_sel_end.set(None);
                        set_cursor_sample.set(None);
                        set_view_start.set(0);
                        set_view_end.set(0);
                        // Clear backend store
                        spawn_local(async {
                            let _ = la_delete_range(0, JS_MAX_SAFE).await;
                        });
                        show_toast("Capture cleared", "ok");
                    }
                            ><Icon name="trash-2" size=13 />"Clear"</button>
                            <button type="button" class="btn btn-sm"
                                title="Export the capture as VCD"
                                on:click=move |_| {
                                spawn_local(async {
                                    #[derive(serde::Serialize)]
                                    struct Filter { name: String, extensions: Vec<String> }
                                    #[derive(serde::Serialize)]
                                    struct Args { title: String, filters: Vec<Filter> }
                                    let args = serde_wasm_bindgen::to_value(&Args {
                                        title: "Export VCD".into(),
                                        filters: vec![Filter { name: "VCD files".into(), extensions: vec!["vcd".into()] }],
                                    }).unwrap();
                                    let result = try_invoke("pick_save_file", args).await;
                                    if let Some(path) = result.and_then(|r| serde_wasm_bindgen::from_value::<Option<String>>(r).ok().flatten()) {
                                        if !path.is_empty() {
                                            la_export_vcd(&path).await;
                                            show_toast("Exported VCD", "ok");
                                        }
                                    }
                                });
                            }
                            ><Icon name="file-down" size=13 />"Export VCD"</button>
                            <button type="button" class="btn btn-sm"
                                title="Export the capture as JSON"
                                on:click=move |_| {
                                spawn_local(async {
                                    #[derive(serde::Serialize)]
                                    struct Filter { name: String, extensions: Vec<String> }
                                    #[derive(serde::Serialize)]
                                    struct Args { title: String, filters: Vec<Filter> }
                                    let args = serde_wasm_bindgen::to_value(&Args {
                                        title: "Export JSON".into(),
                                        filters: vec![Filter { name: "JSON files".into(), extensions: vec!["json".into()] }],
                                    }).unwrap();
                                    let result = try_invoke("pick_save_file", args).await;
                                    if let Some(path) = result.and_then(|r| serde_wasm_bindgen::from_value::<Option<String>>(r).ok().flatten()) {
                                        if !path.is_empty() {
                                            la_export_json(&path).await;
                                            show_toast("Exported JSON", "ok");
                                        }
                                    }
                                });
                            }
                            ><Icon name="file-down" size=13 />"Export JSON"</button>
                        </div>
                    </section>

                    // ---- Signal conditioning (HAT): route, VLOGIC, OE, DIR ----
                    {move || {
                        if !hat_detected.get() {
                            return ().into_any();
                        }
                        view! {
                            <section class="inspector-section" aria-label="Signal conditioning">
                                <h4>"Signal conditioning"</h4>
                                <div class="la-field">
                                    <span class="la-field-label">"Route"</span>
                                    <div class="seg seg-sm" role="radiogroup" aria-label="Capture route">
                                        <button type="button" role="radio"
                                            class:active=move || la_route.get() == 0
                                            aria-checked=move || if la_route.get() == 0 { "true" } else { "false" }
                                            title="Low-Speed path via EXP_EXT (up to 4ch @ 1 MHz)"
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    if let Some(r) = hat_la_set_route(0).await {
                                                        set_la_route_la.set(r);
                                                        show_toast("Route → Low-Speed", "ok");
                                                    }
                                                });
                                            }
                                        >"Low-speed"</button>
                                        <button type="button" role="radio"
                                            class:active=move || la_route.get() == 1
                                            aria-checked=move || if la_route.get() == 1 { "true" } else { "false" }
                                            title="High-Speed path via Conn1 (up to 3ch) — DIR auto-locked B\u{2192}A"
                                            on:click=move |_| {
                                                spawn_local(async move {
                                                    if let Some(r) = hat_la_set_route(1).await {
                                                        set_la_route_la.set(r);
                                                        // Immediately force DIR = B→A so RP2040 listens
                                                        let cur_oe = ls_oe.get_untracked();
                                                        if let Some(s) = hat_set_level_shift(cur_oe, false).await {
                                                            set_ls_oe.set(s.oe);
                                                            set_ls_dir.set(s.dir);
                                                        }
                                                        show_toast("Route \u{2192} High-Speed, DIR locked B\u{2192}A", "ok");
                                                    }
                                                });
                                            }
                                        >"High-speed"</button>
                                    </div>
                                </div>
                                <div class="la-field">
                                    <span class="la-field-label">"VLOGIC"</span>
                                    <div class="seg seg-sm" role="radiogroup" aria-label="VLOGIC voltage">
                                        {[1800u16, 2500u16, 3300u16, 5000u16].into_iter().map(|mv| {
                                            let label = format!("{:.1} V", mv as f32 / 1000.0);
                                            let label_click = label.clone();
                                            view! {
                                                <button type="button" role="radio"
                                                    class:active=move || rail0_mv() == mv
                                                    aria-checked=move || if rail0_mv() == mv { "true" } else { "false" }
                                                    title={format!("Set VLOGIC (3V3_ADJ) to {}", label)}
                                                    on:click={
                                                        let lc = label_click.clone();
                                                        move |_| {
                                                            let lc2 = lc.clone();
                                                            spawn_local(async move {
                                                                if let Some(rails) = hat_set_rail_voltage(0, mv).await {
                                                                    set_hat_rails.set(rails);
                                                                    show_toast(&format!("VLOGIC \u{2192} {}", lc2), "ok");
                                                                }
                                                            });
                                                        }
                                                    }
                                                >{label.clone()}</button>
                                            }
                                        }).collect::<Vec<_>>()}
                                    </div>
                                </div>
                                <div class="row">
                                    <span class="row-label" title="Enable / disable 3V3_ADJ (VLOGIC) rail">"VLOGIC rail"</span>
                                    <Switch
                                        checked=Signal::derive(move || rail0_on())
                                        aria_label="VLOGIC rail enable"
                                        on_change=Callback::new(move |_| {
                                            spawn_local(async move {
                                                let cur = hat_rails.get_untracked().iter().find(|r| r.rail_id == 0).map(|r| r.enabled).unwrap_or(false);
                                                if let Some(rails) = hat_set_rail_enable(0, !cur).await {
                                                    set_hat_rails.set(rails);
                                                    show_toast(if !cur { "VLOGIC enabled" } else { "VLOGIC disabled" }, "ok");
                                                }
                                            });
                                        })
                                    />
                                </div>
                                <div class="row">
                                    <span class="row-label" title="Level-shifter Output Enable — requires VLOGIC">"Output enable"</span>
                                    <Switch
                                        checked=Signal::derive(move || ls_oe.get())
                                        aria_label="Level-shifter output enable"
                                        on_change=Callback::new(move |_| {
                                            spawn_local(async move {
                                                let next_oe = !ls_oe.get_untracked();
                                                let cur_dir = ls_dir.get_untracked();
                                                if let Some(s) = hat_set_level_shift(next_oe, cur_dir).await {
                                                    set_ls_oe.set(s.oe);
                                                    set_ls_dir.set(s.dir);
                                                    show_toast(if s.oe { "OE active" } else { "OE tri-state" }, "ok");
                                                }
                                            });
                                        })
                                    />
                                </div>
                                <div class="la-field">
                                    <span class="la-field-label">"Direction"</span>
                                    <div class="seg seg-sm" role="radiogroup" aria-label="Level-shifter direction"
                                        title=move || {
                                            if la_route.get() == 1 {
                                                "DIR locked B\u{2192}A — High-Speed (RP2040 = input)".to_string()
                                            } else if ls_dir.get() {
                                                "DIR: A\u{2192}B (RP2040 drives) — click to switch B\u{2192}A".to_string()
                                            } else {
                                                "DIR: B\u{2192}A (RP2040 listens) — click to switch A\u{2192}B".to_string()
                                            }
                                        }>
                                        <button type="button" role="radio"
                                            class:active=move || !ls_dir.get()
                                            aria-checked=move || if !ls_dir.get() { "true" } else { "false" }
                                            disabled=move || la_route.get() == 1
                                            on:click=move |_| {
                                                if la_route.get_untracked() == 1 || !ls_dir.get_untracked() { return; }
                                                spawn_local(async move {
                                                    let cur_oe  = ls_oe.get_untracked();
                                                    if let Some(s) = hat_set_level_shift(cur_oe, false).await {
                                                        set_ls_oe.set(s.oe);
                                                        set_ls_dir.set(s.dir);
                                                    }
                                                });
                                            }
                                        >"B\u{2192}A"</button>
                                        <button type="button" role="radio"
                                            class:active=move || ls_dir.get()
                                            aria-checked=move || if ls_dir.get() { "true" } else { "false" }
                                            disabled=move || la_route.get() == 1
                                            on:click=move |_| {
                                                if la_route.get_untracked() == 1 || ls_dir.get_untracked() { return; }
                                                spawn_local(async move {
                                                    let cur_oe  = ls_oe.get_untracked();
                                                    if let Some(s) = hat_set_level_shift(cur_oe, true).await {
                                                        set_ls_oe.set(s.oe);
                                                        set_ls_dir.set(s.dir);
                                                    }
                                                });
                                            }
                                        >"A\u{2192}B"</button>
                                    </div>
                                </div>
                            </section>
                        }.into_any()
                    }}
                </aside>
            </div>

            // ============ STATUS STRIP ============
            {move || {
                let info = capture_info.get();
                let cursor = cursor_sample.get();
                let vd = view_data.get();
                let status = stream_runtime.get();
                let badge = stream_status_badge(&status);
                let tone = match badge {
                    "LIVE" => "green",
                    "DEGRADED" => "orange",
                    "ERROR" => "red",
                    "STOPPED" => "blue",
                    _ => "gray",
                };
                view! {
                    <div class="la-status" role="status">
                        <span class="la-st" title="Live vendor-bulk runtime status">
                            <span class=format!("badge tone-{}", tone)>{badge}</span>
                            <span class="la-st-text">{summarize_la_stream_status(&status)}</span>
                        </span>
                        <span class="la-st">
                            {if let Some(ref i) = info {
                                format!("{}ch @ {} | {} samples | {}", i.channels, format_time(1.0 / i.sample_rate_hz as f64), i.total_samples, format_time(i.duration_sec))
                            } else {
                                "No capture".to_string()
                            }}
                        </span>
                        <span class=if cursor.is_some() { "la-st la-st-cursor is-set" } else { "la-st la-st-cursor" }>
                            {if let Some(cs) = cursor {
                                let sr = info.as_ref().map(|i| i.sample_rate_hz as f64).unwrap_or(1.0);
                                let t = if sr > 0.0 { cs as f64 / sr } else { 0.0 };
                                let ch_vals: String = if let Some(ref d) = vd {
                                    (0..d.channels as usize).map(|ch| {
                                        format!(" {}:{}", ch_names[ch.min(3)].get_untracked(), level_at(d, ch, cs).unwrap_or(0))
                                    }).collect()
                                } else { String::new() };
                                format!("Cursor: {}{}", format_time(t), ch_vals)
                            } else {
                                "Click the waveform to place a cursor".to_string()
                            }}
                        </span>
                    </div>
                }
            }}
        </div>
    }
}

#[cfg(test)]
mod tests {
    use super::stream_status_badge;
    use crate::tauri_bridge::{summarize_la_stream_status, LaStreamRuntimeStatus};

    #[test]
    fn stream_badge_prefers_error() {
        let status = LaStreamRuntimeStatus {
            last_error: Some("bad packet".to_string()),
            ..Default::default()
        };
        assert_eq!(stream_status_badge(&status), "ERROR");
    }

    #[test]
    fn stream_badge_marks_degraded_stop() {
        let status = LaStreamRuntimeStatus {
            stop_reason: Some("dma_overrun".to_string()),
            ..Default::default()
        };
        assert_eq!(stream_status_badge(&status), "DEGRADED");
        assert!(summarize_la_stream_status(&status).contains("dma_overrun"));
    }

    #[test]
    fn stream_badge_marks_live_state() {
        let status = LaStreamRuntimeStatus {
            active: true,
            total_bytes: 1024,
            chunk_count: 8,
            ..Default::default()
        };
        assert_eq!(stream_status_badge(&status), "LIVE");
    }
}
