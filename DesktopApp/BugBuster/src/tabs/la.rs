use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::tauri_bridge::*;
use crate::components::controls::Dropdown;
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d, MouseEvent};

const CH_COLORS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];
const TOOLBAR_HEIGHT: f64 = 44.0; // Space reserved for toolbar overlay
const TRACK_HEIGHT: f64 = 56.0;
const LABEL_WIDTH: f64 = 50.0;
const RULER_HEIGHT: f64 = 24.0;
const MINIMAP_HEIGHT: f64 = 20.0;
const SIGNAL_MARGIN: f64 = 8.0;

fn format_time(seconds: f64) -> String {
    if seconds.abs() < 1e-6 { return format!("{:.1}ns", seconds * 1e9); }
    if seconds.abs() < 1e-3 { return format!("{:.1}µs", seconds * 1e6); }
    if seconds.abs() < 1.0  { return format!("{:.2}ms", seconds * 1e3); }
    format!("{:.3}s", seconds)
}

#[component]
pub fn LaTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // Capture state
    let (capture_info, set_capture_info) = signal(Option::<LaCaptureInfo>::None);
    let (view_data, set_view_data) = signal(Option::<LaViewData>::None);

    // View window (sample indices)
    let (view_start, set_view_start) = signal(0u64);
    let (view_end, set_view_end) = signal(10000u64);

    // Decoder annotations (fetched from backend)
    let (annotations, set_annotations) = signal(Vec::<serde_json::Value>::new());
    let (active_decoders, set_active_decoders) = signal(Vec::<String>::new()); // ["uart", "i2c", "spi"]

    // Config
    let (channels, set_channels) = signal("4".to_string());
    let (rate, set_rate) = signal("1000000".to_string());
    let (depth, set_depth) = signal("100000".to_string());
    let (trig_type, set_trig_type) = signal("0".to_string());
    let (trig_ch, set_trig_ch) = signal("0".to_string());

    // Cursor
    let (cursor_sample, set_cursor_sample) = signal(Option::<u64>::None);

    // Drag state
    let (dragging, set_dragging) = signal(false);
    let (drag_start_x, set_drag_start_x) = signal(0.0f64);
    let (drag_start_vs, set_drag_start_vs) = signal(0u64);
    let (drag_start_ve, set_drag_start_ve) = signal(0u64);

    // Hover state for signal measurement overlay
    let (hover_ch, set_hover_ch) = signal(Option::<usize>::None);
    let (hover_x, set_hover_x) = signal(0.0f64);
    let (hover_y, set_hover_y) = signal(0.0f64);

    // Canvas ref
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    // Listen for "la-done" event (capture complete notification from RP2040)
    {
        let set_ci2 = set_capture_info;
        spawn_local(async move {
            let closure = Closure::new(move |_event: JsValue| {
                show_toast("LA Capture complete!", "ok");
                // Fetch capture info and update view
                spawn_local(async move {
                    if let Some(info) = la_get_capture_info().await {
                        set_view_start.set(0);
                        set_view_end.set(info.total_samples);
                        set_ci2.set(Some(info));
                    }
                });
            });
            listen("la-done", &closure).await;
            closure.forget();
        });
    }

    // Fetch view data when viewport changes
    let set_vd = set_view_data;
    Effect::new(move |_| {
        let vs = view_start.get();
        let ve = view_end.get();
        spawn_local(async move {
            if let Some(data) = la_get_view(vs, ve).await {
                set_vd.set(Some(data));
            }
        });
    });

    // Canvas render effect
    Effect::new(move |_| {
        let data = view_data.get();
        let cursor = cursor_sample.get();
        let Some(canvas_el) = canvas_ref.get() else { return; };
        let canvas: HtmlCanvasElement = canvas_el.into();
        let Some(ctx) = canvas.get_context("2d").ok().flatten() else { return; };
        let ctx: CanvasRenderingContext2d = ctx.unchecked_into();

        let dpr = web_sys::window().unwrap().device_pixel_ratio();
        let w = canvas.client_width() as f64;
        let h = canvas.client_height() as f64;
        canvas.set_width((w * dpr) as u32);
        canvas.set_height((h * dpr) as u32);
        ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();

        // Background
        ctx.set_fill_style_str("#060a14");
        ctx.fill_rect(0.0, 0.0, w, h);

        let Some(ref data) = data else {
            // No data — draw placeholder
            ctx.set_fill_style_str("#5a6d8a");
            ctx.set_font("14px 'JetBrains Mono', monospace");
            ctx.set_text_align("center");
            ctx.fill_text("No capture data — configure and capture to see waveforms", w / 2.0, h / 2.0).ok();
            return;
        };

        let num_ch = data.channels as usize;
        let vs = data.view_start as f64;
        let ve = data.view_end as f64;
        let span = ve - vs;
        if span <= 0.0 { return; }

        let plot_w = w - LABEL_WIDTH;
        let plot_h = h - RULER_HEIGHT - MINIMAP_HEIGHT;

        // Grid lines
        ctx.set_stroke_style_str("rgba(59, 130, 246, 0.08)");
        ctx.set_line_width(1.0);
        let time_per_px = span / plot_w;
        let grid_step = 10.0f64.powf((time_per_px * 100.0).log10().ceil()); // ~100px spacing
        let first_grid = (vs / grid_step).ceil() * grid_step;
        let mut g = first_grid;
        while g < ve {
            let x = LABEL_WIDTH + ((g - vs) / span) * plot_w;
            ctx.begin_path();
            ctx.move_to(x, TOOLBAR_HEIGHT);
            ctx.line_to(x, plot_h);
            ctx.stroke();
            g += grid_step;
        }

        // Channel separators
        for ch in 0..num_ch {
            let y = TOOLBAR_HEIGHT + ch as f64 * TRACK_HEIGHT;
            ctx.set_stroke_style_str("rgba(59, 130, 246, 0.15)");
            ctx.begin_path();
            ctx.move_to(LABEL_WIDTH, y + TRACK_HEIGHT);
            ctx.line_to(w, y + TRACK_HEIGHT);
            ctx.stroke();
        }

        // Draw waveforms
        for ch in 0..num_ch {
            if ch >= data.channel_transitions.len() { continue; }
            let transitions = &data.channel_transitions[ch];
            let color = CH_COLORS[ch % CH_COLORS.len()];
            let y_top = TOOLBAR_HEIGHT + ch as f64 * TRACK_HEIGHT + SIGNAL_MARGIN;
            let y_bot = TOOLBAR_HEIGHT + (ch + 1) as f64 * TRACK_HEIGHT - SIGNAL_MARGIN;
            let y_high = y_top;
            let y_low = y_bot;

            // Channel label
            ctx.set_fill_style_str(color);
            ctx.set_font("11px 'JetBrains Mono', monospace");
            ctx.set_text_align("right");
            ctx.fill_text(&format!("CH{}", ch), LABEL_WIDTH - 6.0, y_top + (y_bot - y_top) / 2.0 + 4.0).ok();

            // Waveform path — step function (no fill, no close)
            ctx.set_stroke_style_str(color);
            ctx.set_line_width(1.5);
            ctx.begin_path();

            if transitions.is_empty() {
                // No transitions — flat line at low
                ctx.move_to(LABEL_WIDTH, y_low);
                ctx.line_to(w, y_low);
            } else {
                // Determine initial value (value before first visible transition)
                let init_val = if transitions[0].0 <= data.view_start {
                    transitions[0].1  // First transition is at or before view start
                } else {
                    // First transition is after view start — initial state is opposite
                    1 - transitions[0].1
                };

                let y_init = if init_val == 1 { y_high } else { y_low };
                ctx.move_to(LABEL_WIDTH, y_init);

                let mut current_val = init_val;

                for &(sample, val) in transitions {
                    let x = LABEL_WIDTH + ((sample as f64 - vs) / span) * plot_w;
                    // Clamp x to visible area
                    let x = x.max(LABEL_WIDTH).min(w);

                    if val != current_val {
                        // Horizontal line to the transition point at current level
                        let y_cur = if current_val == 1 { y_high } else { y_low };
                        ctx.line_to(x, y_cur);
                        // Vertical transition to new level
                        let y_new = if val == 1 { y_high } else { y_low };
                        ctx.line_to(x, y_new);
                        current_val = val;
                    }
                }

                // Extend to right edge at current level
                let y_final = if current_val == 1 { y_high } else { y_low };
                ctx.line_to(w, y_final);
            }

            ctx.stroke();  // Stroke only, never fill the path

            // Fill under signal (subtle)
            ctx.set_global_alpha(0.05);
            ctx.set_fill_style_str(color);
            ctx.fill_rect(LABEL_WIDTH, y_top, plot_w, y_bot - y_top);
            ctx.set_global_alpha(1.0);
        }

        // Time ruler
        let ruler_y = plot_h;
        ctx.set_fill_style_str("#0c1222");
        ctx.fill_rect(0.0, ruler_y, w, RULER_HEIGHT);
        ctx.set_stroke_style_str("rgba(59, 130, 246, 0.3)");
        ctx.begin_path();
        ctx.move_to(0.0, ruler_y);
        ctx.line_to(w, ruler_y);
        ctx.stroke();

        // Time labels
        ctx.set_fill_style_str("#8b9dc3");
        ctx.set_font("10px 'JetBrains Mono', monospace");
        ctx.set_text_align("center");
        let sample_rate = data.sample_rate_hz as f64;
        let mut g = first_grid;
        while g < ve {
            let x = LABEL_WIDTH + ((g - vs) / span) * plot_w;
            let t = if sample_rate > 0.0 { g / sample_rate } else { 0.0 };
            ctx.fill_text(&format_time(t), x, ruler_y + 16.0).ok();
            g += grid_step;
        }

        // Cursor
        if let Some(cs) = cursor {
            if cs >= data.view_start && cs <= data.view_end {
                let x = LABEL_WIDTH + ((cs as f64 - vs) / span) * plot_w;
                ctx.set_stroke_style_str("#ef4444");
                ctx.set_line_width(1.0);
                ctx.begin_path();
                ctx.move_to(x, TOOLBAR_HEIGHT);
                ctx.line_to(x, plot_h);
                ctx.stroke();

                // Cursor time label
                let t = if sample_rate > 0.0 { cs as f64 / sample_rate } else { 0.0 };
                ctx.set_fill_style_str("#ef4444");
                ctx.set_font("10px 'JetBrains Mono', monospace");
                ctx.fill_text(&format_time(t), x, ruler_y + 16.0).ok();
            }
        }

        // Trigger marker
        if let Some(ts) = data.trigger_sample {
            if ts >= data.view_start && ts <= data.view_end {
                let x = LABEL_WIDTH + ((ts as f64 - vs) / span) * plot_w;
                ctx.set_stroke_style_str("#f59e0b");
                ctx.set_line_width(1.0);
                ctx.set_line_dash(&JsValue::from(js_sys::Array::of2(&JsValue::from(4.0), &JsValue::from(4.0)))).ok();
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
                let color = CH_COLORS[hch % CH_COLORS.len()];

                // Find the sample under the mouse
                let mx = hover_x.get();
                if mx > LABEL_WIDTH {
                    let frac = (mx - LABEL_WIDTH) / plot_w;
                    let mouse_sample = vs + span * frac;

                    // Find two consecutive rising edges around the mouse to measure period
                    let mut rising_edges: Vec<u64> = Vec::new();
                    for i in 1..transitions.len() {
                        if transitions[i - 1].1 == 0 && transitions[i].1 == 1 {
                            rising_edges.push(transitions[i].0);
                        }
                    }

                    // Find the nearest period around mouse position
                    if rising_edges.len() >= 2 {
                        // Find the rising edge just before the mouse
                        let mut edge_idx = 0;
                        for (i, &e) in rising_edges.iter().enumerate() {
                            if e as f64 <= mouse_sample { edge_idx = i; }
                        }

                        if edge_idx + 1 < rising_edges.len() {
                            let e1 = rising_edges[edge_idx];
                            let e2 = rising_edges[edge_idx + 1];
                            let period_samples = e2 - e1;
                            let sample_rate = data.sample_rate_hz as f64;

                            if period_samples > 0 && sample_rate > 0.0 {
                                let period_sec = period_samples as f64 / sample_rate;
                                let freq = 1.0 / period_sec;

                                // Find high time within this period for duty cycle
                                let mut high_samples = 0u64;
                                for s in e1..e2 {
                                    // Check value at this sample
                                    let mut val = 0u8;
                                    for &(ts, tv) in transitions.iter() {
                                        if ts <= s { val = tv; } else { break; }
                                    }
                                    if val == 1 { high_samples += 1; }
                                }
                                let duty = high_samples as f64 / period_samples as f64 * 100.0;
                                let width_sec = high_samples as f64 / sample_rate;

                                // Draw period arrow between the two edges
                                let x1 = LABEL_WIDTH + ((e1 as f64 - vs) / span) * plot_w;
                                let x2 = LABEL_WIDTH + ((e2 as f64 - vs) / span) * plot_w;
                                let y_track_top = TOOLBAR_HEIGHT + hch as f64 * TRACK_HEIGHT + SIGNAL_MARGIN;
                                let arrow_y = y_track_top - 2.0;

                                // Arrow line
                                ctx.set_stroke_style_str(color);
                                ctx.set_line_width(1.0);
                                ctx.begin_path();
                                ctx.move_to(x1, arrow_y);
                                ctx.line_to(x2, arrow_y);
                                // Left arrowhead
                                ctx.move_to(x1, arrow_y);
                                ctx.line_to(x1 + 5.0, arrow_y - 3.0);
                                ctx.move_to(x1, arrow_y);
                                ctx.line_to(x1 + 5.0, arrow_y + 3.0);
                                // Right arrowhead
                                ctx.move_to(x2, arrow_y);
                                ctx.line_to(x2 - 5.0, arrow_y - 3.0);
                                ctx.move_to(x2, arrow_y);
                                ctx.line_to(x2 - 5.0, arrow_y + 3.0);
                                // Vertical markers at edges
                                let y_bot = TOOLBAR_HEIGHT + (hch + 1) as f64 * TRACK_HEIGHT - SIGNAL_MARGIN;
                                ctx.move_to(x1, y_track_top);
                                ctx.line_to(x1, y_bot);
                                ctx.move_to(x2, y_track_top);
                                ctx.line_to(x2, y_bot);
                                ctx.stroke();

                                // Measurement text box
                                let box_x = (x1 + x2) / 2.0;
                                let box_y = arrow_y - 6.0;

                                let freq_str = if freq >= 1e6 { format!("{:.2} MHz", freq / 1e6) }
                                    else if freq >= 1e3 { format!("{:.2} kHz", freq / 1e3) }
                                    else { format!("{:.1} Hz", freq) };
                                let period_str = format_time(period_sec);
                                let width_str = format_time(width_sec);
                                let duty_str = format!("{:.1}%", duty);

                                let label = format!("P: {}  F: {}  W: {}  D: {}", period_str, freq_str, width_str, duty_str);

                                // Background box
                                ctx.set_font("9px 'JetBrains Mono', monospace");
                                let text_width = label.len() as f64 * 5.5;  // Approximate monospace width
                                let bx = box_x - text_width / 2.0 - 4.0;
                                let by = box_y - 12.0;
                                ctx.set_fill_style_str("rgba(6, 10, 20, 0.9)");
                                ctx.fill_rect(bx, by, text_width + 8.0, 14.0);
                                ctx.set_stroke_style_str(color);
                                ctx.set_line_width(0.5);
                                ctx.stroke_rect(bx, by, text_width + 8.0, 14.0);

                                // Text
                                ctx.set_fill_style_str(color);
                                ctx.set_text_align("center");
                                ctx.fill_text(&label, box_x, box_y - 1.0).ok();
                            }
                        }
                    }
                }
            }
        }

        // Minimap — full capture overview with viewport indicator
        {
            let mm_y = h - MINIMAP_HEIGHT;
            let mm_w = w - LABEL_WIDTH;

            // Background
            ctx.set_fill_style_str("#0c1222");
            ctx.fill_rect(LABEL_WIDTH, mm_y, mm_w, MINIMAP_HEIGHT);
            ctx.set_stroke_style_str("rgba(59, 130, 246, 0.2)");
            ctx.begin_path();
            ctx.move_to(LABEL_WIDTH, mm_y);
            ctx.line_to(w, mm_y);
            ctx.stroke();

            let total = data.total_samples as f64;
            if total > 0.0 {
                // Draw simplified waveform for CH0 in minimap
                if !data.channel_transitions.is_empty() {
                    ctx.set_stroke_style_str("rgba(59, 130, 246, 0.4)");
                    ctx.set_line_width(1.0);
                    ctx.begin_path();
                    let mm_top = mm_y + 3.0;
                    let mm_bot = mm_y + MINIMAP_HEIGHT - 3.0;
                    // Use ALL transitions (not just visible) for full overview
                    // Since we only have visible range transitions, approximate
                    ctx.move_to(LABEL_WIDTH, mm_bot);
                    ctx.line_to(w, mm_bot);
                    ctx.stroke();
                }

                // Viewport indicator
                let vp_x1 = LABEL_WIDTH + (data.view_start as f64 / total) * mm_w;
                let vp_x2 = LABEL_WIDTH + (data.view_end as f64 / total) * mm_w;
                ctx.set_fill_style_str("rgba(59, 130, 246, 0.15)");
                ctx.fill_rect(vp_x1, mm_y, (vp_x2 - vp_x1).max(2.0), MINIMAP_HEIGHT);
                ctx.set_stroke_style_str("#3b82f6");
                ctx.set_line_width(1.0);
                ctx.stroke_rect(vp_x1, mm_y, (vp_x2 - vp_x1).max(2.0), MINIMAP_HEIGHT);
            }

            // Label
            ctx.set_fill_style_str("#5a6d8a");
            ctx.set_font("8px 'JetBrains Mono', monospace");
            ctx.set_text_align("right");
            ctx.fill_text("MAP", LABEL_WIDTH - 4.0, mm_y + 13.0).ok();
        }

        // Draw decoder annotations
        let anns = annotations.get();
        for ann in &anns {
            let Some(ss) = ann.get("startSample").and_then(|v| v.as_u64()) else { continue };
            let Some(es) = ann.get("endSample").and_then(|v| v.as_u64()) else { continue };
            let text = ann.get("text").and_then(|v| v.as_str()).unwrap_or("?");
            let color = ann.get("color").and_then(|v| v.as_str()).unwrap_or("#3b82f6");
            let row = ann.get("row").and_then(|v| v.as_u64()).unwrap_or(0) as usize;

            if es < data.view_start || ss > data.view_end { continue; }

            let x1 = LABEL_WIDTH + ((ss as f64 - vs) / span) * plot_w;
            let x2 = LABEL_WIDTH + ((es as f64 - vs) / span) * plot_w;
            let ann_h = 18.0;
            let ann_y = plot_h - RULER_HEIGHT - ann_h * (1 + row) as f64 - 2.0;

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
            if x2 - x1 > 20.0 {
                ctx.set_fill_style_str(color);
                ctx.set_font("9px 'JetBrains Mono', monospace");
                ctx.set_text_align("center");
                let tx = (x1 + x2) / 2.0;
                ctx.fill_text(text, tx, ann_y + 13.0).ok();
            }
        }
    });

    // Mouse handlers for zoom/pan/cursor
    let on_wheel = move |e: web_sys::WheelEvent| {
        e.prevent_default();
        let vs = view_start.get_untracked();
        let ve = view_end.get_untracked();
        let span = (ve - vs) as f64;

        // Gentler zoom: 5% per scroll tick
        let factor = if e.delta_y() < 0.0 { 0.95 } else { 1.0 / 0.95 };

        // Zoom centered on mouse position
        let Some(canvas_el) = canvas_ref.get() else { return; };
        let canvas: HtmlCanvasElement = canvas_el.into();
        let rect = canvas.get_bounding_client_rect();
        let mouse_x = e.client_x() as f64 - rect.left();
        let w = canvas.client_width() as f64;
        let plot_w = w - LABEL_WIDTH;

        // Fraction of viewport where mouse is (0.0 = left edge, 1.0 = right edge)
        let frac = ((mouse_x - LABEL_WIDTH) / plot_w).clamp(0.0, 1.0);

        let new_span = (span * factor).max(10.0);
        // Keep the sample under the mouse cursor fixed
        let mouse_sample = vs as f64 + span * frac;
        let new_start = (mouse_sample - new_span * frac).max(0.0) as u64;
        let new_end = new_start + new_span as u64;

        set_view_start.set(new_start);
        set_view_end.set(new_end);
    };

    let on_mousedown = move |e: MouseEvent| {
        let Some(canvas_el) = canvas_ref.get() else { return; };
        let canvas: HtmlCanvasElement = canvas_el.into();
        let rect = canvas.get_bounding_client_rect();
        let x = e.client_x() as f64 - rect.left();
        if x < LABEL_WIDTH { return; }

        set_dragging.set(true);
        set_drag_start_x.set(x);
        set_drag_start_vs.set(view_start.get_untracked());
        set_drag_start_ve.set(view_end.get_untracked());
    };

    let on_mousemove = move |e: MouseEvent| {
        let Some(canvas_el) = canvas_ref.get() else { return; };
        let canvas: HtmlCanvasElement = canvas_el.into();
        let rect = canvas.get_bounding_client_rect();
        let x = e.client_x() as f64 - rect.left();
        let y = e.client_y() as f64 - rect.top();
        let h = canvas.client_height() as f64;
        let plot_h = h - RULER_HEIGHT - MINIMAP_HEIGHT;

        // Track hover position for measurement overlay
        set_hover_x.set(x);
        set_hover_y.set(y);
        if x > LABEL_WIDTH && y > TOOLBAR_HEIGHT && y < plot_h {
            let ch_idx = ((y - TOOLBAR_HEIGHT) / TRACK_HEIGHT) as usize;
            set_hover_ch.set(Some(ch_idx));
        } else {
            set_hover_ch.set(None);
        }

        // Drag handling
        if !dragging.get_untracked() { return; }

        let w = canvas.client_width() as f64;
        let plot_w = w - LABEL_WIDTH;

        let dx_px = x - drag_start_x.get_untracked();
        let vs0 = drag_start_vs.get_untracked();
        let ve0 = drag_start_ve.get_untracked();
        let span = (ve0 - vs0) as f64;

        let d_samples = (dx_px / plot_w * span) as i64;
        let new_start = (vs0 as i64 - d_samples).max(0) as u64;
        let new_end = new_start + span as u64;

        set_view_start.set(new_start);
        set_view_end.set(new_end);
    };

    let on_mouseup = move |e: MouseEvent| {
        let was_dragging = dragging.get_untracked();
        set_dragging.set(false);

        // If it was a click (no drag), place cursor
        if was_dragging {
            let dx = {
                let Some(canvas_el) = canvas_ref.get() else { return; };
                let canvas: HtmlCanvasElement = canvas_el.into();
                let rect = canvas.get_bounding_client_rect();
                let x = e.client_x() as f64 - rect.left();
                (x - drag_start_x.get_untracked()).abs()
            };
            if dx > 3.0 { return; } // Was a real drag, not a click
        }

        // Place cursor
        let Some(canvas_el) = canvas_ref.get() else { return; };
        let canvas: HtmlCanvasElement = canvas_el.into();
        let rect = canvas.get_bounding_client_rect();
        let x = e.client_x() as f64 - rect.left();
        let w = canvas.client_width() as f64;
        let plot_w = w - LABEL_WIDTH;
        if x < LABEL_WIDTH { return; }
        let frac = (x - LABEL_WIDTH) / plot_w;
        let vs = view_start.get_untracked();
        let ve = view_end.get_untracked();
        let sample = vs + ((ve - vs) as f64 * frac) as u64;
        set_cursor_sample.set(Some(sample));
    };

    let on_mouseleave = move |_: MouseEvent| {
        set_dragging.set(false);
    };

    let ch_options = vec![
        ("1".into(), "1 Channel".into()),
        ("2".into(), "2 Channels".into()),
        ("4".into(), "4 Channels".into()),
    ];
    let rate_options = vec![
        ("100000".into(), "100 kHz".into()),
        ("500000".into(), "500 kHz".into()),
        ("1000000".into(), "1 MHz".into()),
        ("5000000".into(), "5 MHz".into()),
        ("10000000".into(), "10 MHz".into()),
        ("25000000".into(), "25 MHz".into()),
    ];
    let depth_options = vec![
        ("10000".into(), "10K".into()),
        ("50000".into(), "50K".into()),
        ("100000".into(), "100K".into()),
        ("500000".into(), "500K".into()),
    ];
    let trigger_options = vec![
        ("0".into(), "None".into()),
        ("1".into(), "Rising".into()),
        ("2".into(), "Falling".into()),
        ("3".into(), "Any Edge".into()),
        ("4".into(), "High".into()),
        ("5".into(), "Low".into()),
    ];
    let trig_ch_options = vec![
        ("0".into(), "CH0".into()),
        ("1".into(), "CH1".into()),
        ("2".into(), "CH2".into()),
        ("3".into(), "CH3".into()),
    ];

    view! {
        <div class="tab-content" style="display: flex; flex-direction: column; height: 100%; gap: 0">
            <div class="tab-desc">
                "Logic Analyzer — Capture and analyze digital signals from the HAT expansion board."
            </div>

            // Toolbar
            <div style="display: flex; align-items: center; gap: 8px; padding: 8px 0; border-bottom: 1px solid var(--border, #1e293b); flex-wrap: wrap">
                <Dropdown value=Signal::derive(move || channels.get()) on_change=Callback::new(move |v: String| set_channels.set(v)) options=ch_options.clone() />
                <Dropdown value=Signal::derive(move || rate.get()) on_change=Callback::new(move |v: String| set_rate.set(v)) options=rate_options.clone() />
                <Dropdown value=Signal::derive(move || depth.get()) on_change=Callback::new(move |v: String| set_depth.set(v)) options=depth_options.clone() />
                <span style="color: var(--text-dim); font-size: 10px; margin: 0 4px">"|"</span>
                <span style="font-size: 10px; color: var(--text-dim)">"Trigger:"</span>
                <Dropdown value=Signal::derive(move || trig_type.get()) on_change=Callback::new(move |v: String| set_trig_type.set(v)) options=trigger_options.clone() />
                <Dropdown value=Signal::derive(move || trig_ch.get()) on_change=Callback::new(move |v: String| set_trig_ch.set(v)) options=trig_ch_options.clone() />
                <span style="color: var(--text-dim); font-size: 10px; margin: 0 4px">"|"</span>

                // Control buttons
                <button style="font-size: 10px; padding: 3px 12px; background: #10b98125; color: #10b981; border: 1px solid #10b98150; border-radius: 4px; cursor: pointer"
                    on:click={
                        let set_ci5 = set_capture_info;
                        move |_| {
                            let ch: u8 = channels.get_untracked().parse().unwrap_or(4);
                            let r: u32 = rate.get_untracked().parse().unwrap_or(1000000);
                            let d: u32 = depth.get_untracked().parse().unwrap_or(100000);
                            let tt: u8 = trig_type.get_untracked().parse().unwrap_or(0);
                            let tc: u8 = trig_ch.get_untracked().parse().unwrap_or(0);
                            spawn_local(async move {
                                // Stop any previous capture
                                la_invoke_stop().await;
                                // Small delay for RP2040 cleanup
                                let promise = js_sys::Promise::new(&mut |resolve, _| {
                                    web_sys::window().unwrap().set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 200).unwrap();
                                });
                                wasm_bindgen_futures::JsFuture::from(promise).await.ok();

                                la_invoke_configure(ch, r, d).await;
                                la_invoke_set_trigger(tt, tc).await;
                                la_invoke_arm().await;
                                show_toast(if tt == 0 { "Capturing..." } else { "Armed — waiting for trigger..." }, "ok");

                                // Auto-poll for capture done
                                for _ in 0..200 {  // Up to 20 seconds
                                    let promise = js_sys::Promise::new(&mut |resolve, _| {
                                        web_sys::window().unwrap().set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 100).unwrap();
                                    });
                                    wasm_bindgen_futures::JsFuture::from(promise).await.ok();

                                    if let Some(info) = la_get_capture_info().await {
                                        // la_get_capture_info only returns data if store exists
                                        // We need to check LA status instead
                                    }

                                    // Check status via BBP
                                    #[derive(serde::Deserialize)]
                                    #[serde(rename_all = "camelCase")]
                                    struct St { state: u8, channels: u8, samples_captured: u32, total_samples: u32, actual_rate_hz: u32 }
                                    let result = invoke("la_get_status", JsValue::NULL).await;
                                    if let Ok(st) = serde_wasm_bindgen::from_value::<St>(result) {
                                        if st.state == 3 {
                                            // DONE! Auto-read data
                                            show_toast("Capture done! Reading...", "ok");
                                            #[derive(serde::Serialize)]
                                            #[serde(rename_all = "camelCase")]
                                            struct ReadArgs { channels: u8, sample_rate_hz: u32, total_samples: u32 }
                                            let args = serde_wasm_bindgen::to_value(&ReadArgs {
                                                channels: st.channels, sample_rate_hz: st.actual_rate_hz, total_samples: st.samples_captured,
                                            }).unwrap();
                                            let read_result = invoke("la_read_uart_chunks", args).await;
                                            if let Ok(info) = serde_wasm_bindgen::from_value::<LaCaptureInfo>(read_result) {
                                                set_view_start.set(0);
                                                set_view_end.set(info.total_samples);
                                                set_ci5.set(Some(info));
                                                show_toast("Capture complete!", "ok");
                                            } else {
                                                show_toast("Read failed", "err");
                                            }
                                            break;
                                        }
                                    }
                                }
                            });
                        }
                    }
                >"Arm"</button>
                <button style="font-size: 10px; padding: 3px 12px; background: #f59e0b25; color: #f59e0b; border: 1px solid #f59e0b50; border-radius: 4px; cursor: pointer"
                    on:click=move |_| { spawn_local(async { la_invoke_force().await; show_toast("Triggered", "ok"); }); }
                >"Force"</button>
                <button style="font-size: 10px; padding: 3px 12px; background: #ef444425; color: #ef4444; border: 1px solid #ef444450; border-radius: 4px; cursor: pointer"
                    on:click=move |_| { spawn_local(async { la_invoke_stop().await; show_toast("Stopped", "ok"); }); }
                >"Stop"</button>

                // Read capture — reads data from RP2040 via UART chunks
                <button style="font-size: 10px; padding: 3px 12px; background: #3b82f625; color: #3b82f6; border: 1px solid #3b82f650; border-radius: 4px; cursor: pointer"
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
                                let result = invoke("la_read_uart_chunks", args).await;
                                match serde_wasm_bindgen::from_value::<LaCaptureInfo>(result) {
                                    Ok(info) => {
                                        set_view_start.set(0);
                                        set_view_end.set(info.total_samples);
                                        set_ci4.set(Some(info));
                                        show_toast("Capture data loaded!", "ok");
                                    }
                                    Err(e) => {
                                        show_toast(&format!("Read failed: {:?}", e), "err");
                                    }
                                }
                            });
                        }
                    }
                >"Read"</button>

                // Test data button — loads synthetic waveforms to verify renderer
                <button style="font-size: 10px; padding: 3px 12px; background: #8b5cf625; color: #8b5cf6; border: 1px solid #8b5cf650; border-radius: 4px; cursor: pointer"
                    on:click={
                        let set_ci = set_capture_info;
                        move |_| {
                            spawn_local(async move {
                                // Generate test data: 4 channels, various patterns
                                // CH0: 1kHz square wave, CH1: 500Hz, CH2: 2kHz, CH3: slow toggle
                                let sample_rate: u32 = 100000; // 100kHz
                                let num_samples: u32 = 10000;
                                let channels: u8 = 4;
                                let samples_per_word = 32 / channels as u32;
                                let total_bytes = (num_samples + samples_per_word - 1) / samples_per_word * 4;
                                let mut raw = vec![0u8; total_bytes as usize];

                                // Pack samples into bytes (4 bits per sample for 4 channels)
                                let mut byte_idx = 0usize;
                                let mut bit_pos = 0u8;
                                for s in 0..num_samples {
                                    let ch0 = if (s / 50) % 2 == 0 { 1u8 } else { 0 };  // 1kHz @ 100kHz
                                    let ch1 = if (s / 100) % 2 == 0 { 1u8 } else { 0 }; // 500Hz
                                    let ch2 = if (s / 25) % 2 == 0 { 1u8 } else { 0 };  // 2kHz
                                    let ch3 = if (s / 500) % 2 == 0 { 1u8 } else { 0 }; // 100Hz
                                    let nibble = ch0 | (ch1 << 1) | (ch2 << 2) | (ch3 << 3);
                                    if byte_idx < raw.len() {
                                        raw[byte_idx] |= nibble << bit_pos;
                                    }
                                    bit_pos += 4;
                                    if bit_pos >= 8 {
                                        bit_pos = 0;
                                        byte_idx += 1;
                                    }
                                }

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
                                let result = invoke("la_load_raw", args).await;

                                // Debug: log what we got back
                                web_sys::console::log_1(&format!("la_load_raw result: {:?}", result).into());

                                let total: u64 = serde_wasm_bindgen::from_value(result).unwrap_or(0);

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
                >"Test Data"</button>

                // Zoom buttons
                <div style="margin-left: auto; display: flex; gap: 4px; align-items: center">
                    <button style="font-size: 10px; padding: 2px 8px; background: #3b82f615; color: #3b82f6; border: 1px solid #3b82f640; border-radius: 4px; cursor: pointer"
                        on:click=move |_| {
                            let vs = view_start.get_untracked();
                            let ve = view_end.get_untracked();
                            let c = vs + (ve - vs) / 2;
                            let ns = ((ve - vs) as f64 * 0.5) as u64;
                            set_view_start.set(c.saturating_sub(ns / 2));
                            set_view_end.set(c.saturating_sub(ns / 2) + ns.max(10));
                        }
                    >"Zoom +"</button>
                    <button style="font-size: 10px; padding: 2px 8px; background: #3b82f615; color: #3b82f6; border: 1px solid #3b82f640; border-radius: 4px; cursor: pointer"
                        on:click=move |_| {
                            let vs = view_start.get_untracked();
                            let ve = view_end.get_untracked();
                            let c = vs + (ve - vs) / 2;
                            let ns = ((ve - vs) as f64 * 2.0) as u64;
                            set_view_start.set(c.saturating_sub(ns / 2));
                            set_view_end.set(c.saturating_sub(ns / 2) + ns);
                        }
                    >"Zoom -"</button>
                    <button style="font-size: 10px; padding: 2px 8px; background: #3b82f615; color: #3b82f6; border: 1px solid #3b82f640; border-radius: 4px; cursor: pointer"
                        on:click=move |_| {
                            spawn_local(async move {
                                if let Some(info) = la_get_capture_info().await {
                                    set_view_start.set(0);
                                    set_view_end.set(info.total_samples);
                                }
                            });
                        }
                    >"Fit"</button>
                </div>
            </div>

            // Canvas waveform area
            <canvas
                node_ref=canvas_ref
                style=move || format!("flex: 1; width: 100%; min-height: 200px; cursor: {}; border-radius: 4px",
                    if dragging.get() { "grabbing" } else { "crosshair" })
                on:wheel=on_wheel
                on:mousedown=on_mousedown
                on:mousemove=on_mousemove
                on:mouseup=on_mouseup
                on:mouseleave=on_mouseleave
            />

            // Status bar with cursor readout + export buttons
            {move || {
                let info = capture_info.get();
                let cursor = cursor_sample.get();
                let vs = view_start.get();
                let ve = view_end.get();
                let vd = view_data.get();
                view! {
                    <div style="display: flex; align-items: center; gap: 8px; padding: 4px 8px; font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; border-top: 1px solid var(--border, #1e293b)">
                        // Capture info
                        {if let Some(ref i) = info {
                            view! {
                                <span>{format!("{}ch @ {} | {} samples | {}", i.channels, format_time(1.0 / i.sample_rate_hz as f64), i.total_samples, format_time(i.duration_sec))}</span>
                            }.into_any()
                        } else {
                            view! { <span>"No capture"</span> }.into_any()
                        }}
                        <span style="color: var(--text-muted)">"|"</span>

                        // Cursor readout with per-channel values
                        {if let Some(cs) = cursor {
                            let sr = info.as_ref().map(|i| i.sample_rate_hz as f64).unwrap_or(1.0);
                            let t = if sr > 0.0 { cs as f64 / sr } else { 0.0 };
                            // Get channel values at cursor from view data
                            let ch_vals: String = if let Some(ref d) = vd {
                                (0..d.channels).map(|ch| {
                                    // Find value at cursor sample from transitions
                                    let trans = &d.channel_transitions[ch as usize];
                                    let mut val = 0u8;
                                    for &(s, v) in trans.iter() {
                                        if s <= cs { val = v; } else { break; }
                                    }
                                    format!(" CH{}:{}", ch, val)
                                }).collect()
                            } else { String::new() };
                            view! {
                                <span style="color: #ef4444">{format!("Cursor: {}{}", format_time(t), ch_vals)}</span>
                            }.into_any()
                        } else {
                            view! { <span style="color: var(--text-muted)">"Click waveform to place cursor"</span> }.into_any()
                        }}

                        // Spacer
                        <div style="flex: 1"></div>

                        // Export buttons
                        <button style="font-size: 9px; padding: 1px 8px; background: #06b6d415; color: #06b6d4; border: 1px solid #06b6d440; border-radius: 3px; cursor: pointer"
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
                                    let result = invoke("pick_save_file", args).await;
                                    if let Some(path) = serde_wasm_bindgen::from_value::<Option<String>>(result).ok().flatten() {
                                        if !path.is_empty() {
                                            la_export_vcd(&path).await;
                                            show_toast("Exported VCD", "ok");
                                        }
                                    }
                                });
                            }
                        >"Export VCD"</button>
                        <button style="font-size: 9px; padding: 1px 8px; background: #06b6d415; color: #06b6d4; border: 1px solid #06b6d440; border-radius: 3px; cursor: pointer"
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
                                    let result = invoke("pick_save_file", args).await;
                                    if let Some(path) = serde_wasm_bindgen::from_value::<Option<String>>(result).ok().flatten() {
                                        if !path.is_empty() {
                                            la_export_json(&path).await;
                                            show_toast("Exported JSON", "ok");
                                        }
                                    }
                                });
                            }
                        >"Export JSON"</button>
                        <button style="font-size: 9px; padding: 1px 8px; background: #a855f715; color: #a855f7; border: 1px solid #a855f740; border-radius: 3px; cursor: pointer"
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
                                        let result = invoke("pick_config_open_file", args).await;
                                        if let Some(path) = serde_wasm_bindgen::from_value::<Option<String>>(result).ok().flatten() {
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
                        >"Import"</button>
                    </div>
                }
            }}
        </div>
    }
}
