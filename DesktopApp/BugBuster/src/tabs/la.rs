use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::tauri_bridge::*;
// Dropdown no longer used — replaced with pill/toggle buttons
// use crate::components::controls::Dropdown;
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d, MouseEvent};

const CH_COLORS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];
const TOOLBAR_HEIGHT: f64 = 44.0; // Space reserved for toolbar overlay
const TRACK_HEIGHT: f64 = 62.0;
const LABEL_WIDTH: f64 = 50.0;
const RULER_HEIGHT: f64 = 22.0;
const MINIMAP_HEIGHT: f64 = 28.0;
const SIGNAL_MARGIN: f64 = 8.0;

fn format_time(seconds: f64) -> String {
    if seconds.abs() < 1e-6 { return format!("{:.1}ns", seconds * 1e9); }
    if seconds.abs() < 1e-3 { return format!("{:.1}µs", seconds * 1e6); }
    if seconds.abs() < 1.0  { return format!("{:.2}ms", seconds * 1e3); }
    format!("{:.3}s", seconds)
}

/// Reformat an annotation text string according to the chosen display format.
/// Only "data" and "address" ann_types are reformatted; control/info stay as-is.
fn reformat_ann(text: &str, fmt: &str, ann_type: &str) -> String {
    if fmt == "hex" { return text.to_string(); }
    if ann_type != "data" && ann_type != "address" { return text.to_string(); }
    // Parse numeric value from "0xNN" or "'c'" encoding
    let (val, suffix): (Option<u64>, String) = if let Some(rest) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
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
            "dec"   => format!("{}{}", v, suffix),
            "ascii" => {
                let b = v as u8;
                if b >= 0x20 && b <= 0x7E { format!("'{}'", b as char) } else { format!("\\x{:02X}", v) }
            },
            "bin"   => format!("{:08b}{}", v, suffix),
            _       => text.to_string(),
        },
    }
}

#[component]
pub fn LaTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let _ = state;
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

    // Decoder panel state
    let (decoder_panel_open, set_decoder_panel_open) = signal(false);
    let (add_dec_type, set_add_dec_type) = signal("uart".to_string());
    let (add_dec_ch_a, set_add_dec_ch_a) = signal(0u8);  // UART TX / I2C SDA / SPI MOSI
    let (add_dec_ch_b, set_add_dec_ch_b) = signal(1u8);  // UART RX (0xFF=off) / I2C SCL / SPI MISO
    let (add_dec_uart_rx_off, set_add_dec_uart_rx_off) = signal(true); // UART RX disabled by default
    let (add_dec_ch_c, set_add_dec_ch_c) = signal(2u8);  // SPI CLK
    let (add_dec_ch_d, set_add_dec_ch_d) = signal(3u8);  // SPI CS
    let (add_dec_baud, set_add_dec_baud) = signal("115200".to_string());
    let (add_dec_spi_mode, set_add_dec_spi_mode) = signal(0u8); // CPOL<<1 | CPHA
    let (add_dec_spi_cs_off, set_add_dec_spi_cs_off) = signal(false);
    let (ann_fmt, set_ann_fmt) = signal("hex".to_string());
    let (next_dec_id, set_next_dec_id) = signal(0u32);
    // Active decoders: (id, type, label, ch_a, ch_b, ch_c, ch_d, extra_param)
    let (decoders, set_decoders) = signal(Vec::<(u32, String, String, u8, u8, u8, u8, String)>::new());

    // Cursor
    let (cursor_sample, set_cursor_sample) = signal(Option::<u64>::None);

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

    // Fetch view data when viewport or capture changes (streaming updates capture_info)
    let set_vd = set_view_data;
    Effect::new(move |_| {
        let vs = view_start.get();
        let ve = view_end.get();
        let _ci = capture_info.get(); // re-fetch when new data arrives during streaming
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
        let fmt = ann_fmt.get(); // track format at top level so changes always redraw
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

        // ── Dynamic track heights: fill available space, expand for annotations ──
        let anns = annotations.get();
        let ann_row_h = 20.0_f64;
        let ann_gap = 3.0_f64;

        // Count max annotation row per channel
        let mut ch_ann_rows = vec![0usize; num_ch];
        for ann in &anns {
            let ch = ann.get("channel").and_then(|v| v.as_u64()).unwrap_or(0) as usize;
            let row = ann.get("row").and_then(|v| v.as_u64()).unwrap_or(0) as usize + 1;
            if ch < num_ch { ch_ann_rows[ch] = ch_ann_rows[ch].max(row); }
        }

        // Compute annotation overhead per channel
        let total_ann_extra: f64 = (0..num_ch).map(|ch| {
            if ch_ann_rows[ch] > 0 { ann_gap + ann_row_h * ch_ann_rows[ch] as f64 } else { 0.0 }
        }).sum();

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
                let ann_extra = if ch_ann_rows[ch] > 0 { ann_gap + ann_row_h * ch_ann_rows[ch] as f64 } else { 0.0 };
                ch_h[ch] = track_h + ann_extra;
                y += ch_h[ch];
            }
        }
        // Share offsets with mouse handler (untracked write — no reactive loop)
        set_ch_y_offsets.set(ch_y.clone());

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

        // Channel separators (after signal + annotation area)
        for ch in 0..num_ch {
            let sep_y = ch_y[ch] + ch_h[ch];
            ctx.set_stroke_style_str("rgba(59, 130, 246, 0.15)");
            ctx.begin_path();
            ctx.move_to(LABEL_WIDTH, sep_y);
            ctx.line_to(w, sep_y);
            ctx.stroke();
        }

        // Draw waveforms
        for ch in 0..num_ch {
            if ch >= data.channel_transitions.len() { continue; }
            let transitions = &data.channel_transitions[ch];
            let color = CH_COLORS[ch % CH_COLORS.len()];
            let y_top = ch_y[ch] + SIGNAL_MARGIN;
            let y_bot = ch_y[ch] + track_h - SIGNAL_MARGIN;
            let y_high = y_top;
            let y_low = y_bot;

            // Channel label
            ctx.set_fill_style_str(color);
            ctx.set_font("11px 'JetBrains Mono', monospace");
            ctx.set_text_align("right");
            ctx.fill_text(&format!("CH{}", ch), LABEL_WIDTH - 6.0, y_top + (y_bot - y_top) / 2.0 + 4.0).ok();

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

                // Density mode: use per-pixel fill when any transitions are < 3px apart
                let dense = if transitions.len() < 3 { false } else {
                    let mut min_gap = f64::MAX;
                    for i in 1..transitions.len() {
                        let px1 = (transitions[i-1].0 as f64 - vs) / span * plot_w;
                        let px2 = (transitions[i].0 as f64 - vs) / span * plot_w;
                        let gap = (px2 - px1).abs();
                        if gap < min_gap { min_gap = gap; }
                        if min_gap < 3.0 { break; } // early exit
                    }
                    min_gap < 3.0
                };
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
                        if px > pixels as i64 { break; }
                    }
                    // Fill remaining pixels with final state
                    let fill_s = (prev_px + 1).max(0) as usize;
                    if fill_s <= pixels {
                        let bit: u8 = if cur == 1 { 0b10 } else { 0b01 };
                        for f in fill_s..=pixels { px_flags[f] |= bit; }
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

                    // Render per pixel
                    ctx.set_fill_style_str(color);
                    for px in 0..=pixels.min(plot_w as usize) {
                        let x = LABEL_WIDTH + px as f64;
                        match px_flags[px] {
                            0b11 => { // Both high and low — dense transition bar
                                ctx.set_global_alpha(0.7);
                                ctx.fill_rect(x, y_high, 1.0, y_low - y_high);
                                ctx.set_global_alpha(1.0);
                            }
                            0b10 => { ctx.fill_rect(x, y_high, 1.0, 2.0); }
                            0b01 => { ctx.fill_rect(x, y_low - 1.0, 1.0, 2.0); }
                            _ => {}
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
                            .max(LABEL_WIDTH).min(w);
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
        ctx.set_fill_style_str("#111a2e");
        ctx.fill_rect(0.0, ruler_y, w, RULER_HEIGHT);
        ctx.set_stroke_style_str("rgba(59, 130, 246, 0.3)");
        ctx.begin_path();
        ctx.move_to(0.0, ruler_y);
        ctx.line_to(w, ruler_y);
        ctx.stroke();

        let sample_rate = data.sample_rate_hz as f64;
        let span_sec = if sample_rate > 0.0 { span / sample_rate } else { 1.0 };
        let (unit_suffix, unit_div) = if span_sec < 2e-6 { ("ns", 1e9) }
            else if span_sec < 2e-3 { ("µs", 1e6) }
            else if span_sec < 2.0 { ("ms", 1e3) }
            else { ("s", 1.0) };

        // Major tick marks + labels
        ctx.set_stroke_style_str("rgba(139, 157, 195, 0.5)");
        ctx.set_line_width(1.0);
        ctx.set_fill_style_str("#8b9dc3");
        ctx.set_font("9px 'JetBrains Mono', monospace");
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
            let t = if sample_rate > 0.0 { g / sample_rate } else { 0.0 };
            let label = format!("{:.1}{}", t * unit_div, unit_suffix);
            ctx.fill_text(&label, x, ruler_y + 15.0).ok();
            // Minor ticks (5 subdivisions)
            ctx.set_stroke_style_str("rgba(139, 157, 195, 0.2)");
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
            ctx.set_stroke_style_str("rgba(139, 157, 195, 0.5)");
            g += grid_step;
        }
        // Unit label in left margin
        ctx.set_fill_style_str("#5a6d8a");
        ctx.set_font("8px 'JetBrains Mono', monospace");
        ctx.set_text_align("right");
        ctx.fill_text(unit_suffix, LABEL_WIDTH - 4.0, ruler_y + 14.0).ok();

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
                ctx.fill_text(&format_time(t), x, ruler_y + 14.0).ok();
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
                            if dist < nearest_dist { nearest_dist = dist; nearest_idx = i; }
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
                                        if !edges[i].1 { // falling edge
                                            high_time = edges[i].0 - e1;
                                            break;
                                        }
                                    }
                                } else {
                                    // Falling→Falling: high time = time from first rising to e2
                                    for i in (nearest_idx + 1)..pe_idx {
                                        if edges[i].1 { // rising edge
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
                                let y_track_top = ch_y.get(hch).copied().unwrap_or(TOOLBAR_HEIGHT + hch as f64 * track_h) + SIGNAL_MARGIN;
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
                                let y_bot = ch_y.get(hch).copied().unwrap_or(TOOLBAR_HEIGHT + hch as f64 * track_h) + track_h - SIGNAL_MARGIN;
                                ctx.move_to(x1, y_track_top);
                                ctx.line_to(x1, y_bot);
                                ctx.move_to(x2, y_track_top);
                                ctx.line_to(x2, y_bot);
                                ctx.stroke();

                                // Measurement label
                                let box_x = (x1 + x2) / 2.0;
                                let box_y = arrow_y - 6.0;

                                let freq_str = if freq >= 1e6 { format!("{:.2} MHz", freq / 1e6) }
                                    else if freq >= 1e3 { format!("{:.2} kHz", freq / 1e3) }
                                    else { format!("{:.1} Hz", freq) };

                                let label = format!("P: {}  F: {}  W: {}  D: {:.1}%",
                                    format_time(period_sec), freq_str, format_time(width_sec), duty);

                                ctx.set_font("9px 'JetBrains Mono', monospace");
                                let text_width = label.len() as f64 * 5.5;
                                let bx = box_x - text_width / 2.0 - 4.0;
                                let by = box_y - 12.0;
                                ctx.set_fill_style_str("rgba(6, 10, 20, 0.92)");
                                ctx.fill_rect(bx, by, text_width + 8.0, 14.0);
                                ctx.set_stroke_style_str(color);
                                ctx.set_line_width(0.5);
                                ctx.stroke_rect(bx, by, text_width + 8.0, 14.0);
                                ctx.set_fill_style_str(color);
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
            let mm_heat_top = mm_y + 10.0;    // below time labels
            let mm_heat_h = MINIMAP_HEIGHT - 12.0;

            // Background
            ctx.set_fill_style_str("#080e1a");
            ctx.fill_rect(0.0, mm_y, w, MINIMAP_HEIGHT);
            ctx.set_stroke_style_str("rgba(59, 130, 246, 0.25)");
            ctx.begin_path();
            ctx.move_to(0.0, mm_y);
            ctx.line_to(w, mm_y);
            ctx.stroke();

            let total = data.total_samples as f64;
            if total > 0.0 {
                // Time range labels
                let total_sec = if sample_rate > 0.0 { total / sample_rate } else { 0.0 };
                ctx.set_fill_style_str("#5a6d8a");
                ctx.set_font("8px 'JetBrains Mono', monospace");
                ctx.set_text_align("left");
                ctx.fill_text("0s", LABEL_WIDTH + 3.0, mm_y + 9.0).ok();
                ctx.set_text_align("right");
                ctx.fill_text(&format_time(total_sec), w - 3.0, mm_y + 9.0).ok();

                // Density heatmap
                let density = &data.density;
                if !density.is_empty() {
                    let max_d = *density.iter().max().unwrap_or(&1) as f64;
                    let buckets = density.len() as f64;
                    for (i, &count) in density.iter().enumerate() {
                        if count == 0 { continue; }
                        let intensity = (count as f64 / max_d).sqrt(); // sqrt for better contrast
                        let bx = LABEL_WIDTH + (i as f64 / buckets) * mm_w;
                        let bw = (mm_w / buckets).max(1.0);
                        // Blue glow: brighter = more transitions
                        let r = (20.0 + 39.0 * intensity) as u8;
                        let g_c = (40.0 + 90.0 * intensity) as u8;
                        let b = (80.0 + 166.0 * intensity) as u8;
                        ctx.set_fill_style_str(&format!("rgb({},{},{})", r, g_c, b));
                        ctx.fill_rect(bx, mm_heat_top, bw + 0.5, mm_heat_h);
                    }
                }

                // Viewport indicator box
                let vp_x1 = LABEL_WIDTH + (data.view_start as f64 / total) * mm_w;
                let vp_x2 = LABEL_WIDTH + (data.view_end.min(data.total_samples) as f64 / total) * mm_w;
                let vp_w = (vp_x2 - vp_x1).max(3.0);
                ctx.set_fill_style_str("rgba(168, 85, 247, 0.12)");
                ctx.fill_rect(vp_x1, mm_heat_top, vp_w, mm_heat_h);
                ctx.set_stroke_style_str("#a855f7");
                ctx.set_line_width(1.5);
                ctx.stroke_rect(vp_x1, mm_heat_top, vp_w, mm_heat_h);
                // Edge grips
                ctx.set_line_width(2.0);
                ctx.set_stroke_style_str("#c084fc");
                for xg in [vp_x1, vp_x1 + vp_w] {
                    ctx.begin_path();
                    ctx.move_to(xg, mm_heat_top + 2.0);
                    ctx.line_to(xg, mm_heat_top + mm_heat_h - 2.0);
                    ctx.stroke();
                }
            }

            // Label
            ctx.set_fill_style_str("#5a6d8a");
            ctx.set_font("7px 'JetBrains Mono', monospace");
            ctx.set_text_align("right");
            ctx.fill_text("NAV", LABEL_WIDTH - 4.0, mm_y + 9.0).ok();
        }

        // Draw selection highlight
        let ss_sel = sel_start.get();
        let se_sel = sel_end.get();
        if let (Some(s_sel), Some(e_sel)) = (ss_sel, se_sel) {
            if e_sel > data.view_start && s_sel < data.view_end {
                let x1_sel = LABEL_WIDTH + ((s_sel.max(data.view_start) as f64 - vs) / span) * plot_w;
                let x2_sel = LABEL_WIDTH + ((e_sel.min(data.view_end) as f64 - vs) / span) * plot_w;
                ctx.set_fill_style_str("#a855f7");
                ctx.set_global_alpha(0.12);
                ctx.fill_rect(x1_sel, TOOLBAR_HEIGHT, x2_sel - x1_sel, plot_h - TOOLBAR_HEIGHT);
                ctx.set_global_alpha(1.0);
                // Selection edges
                ctx.set_stroke_style_str("#a855f780");
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
            let Some(ss) = ann.get("startSample").and_then(|v| v.as_u64()) else { continue };
            let Some(es) = ann.get("endSample").and_then(|v| v.as_u64()) else { continue };
            let raw_text = ann.get("text").and_then(|v| v.as_str()).unwrap_or("?");
            let ann_type = ann.get("annType").and_then(|v| v.as_str()).unwrap_or("data");
            let text_owned = reformat_ann(raw_text, &fmt, ann_type);
            let text = text_owned.as_str();
            let color = match ann_type {
                "address" => "#10b981",
                "control" => "#f59e0b",
                "error"   => "#ef4444",
                "info"    => "#06b6d4",
                _         => "#3b82f6", // data (default)
            };
            let row = ann.get("row").and_then(|v| v.as_u64()).unwrap_or(0) as usize;
            let ch = ann.get("channel").and_then(|v| v.as_u64()).unwrap_or(0) as usize;

            if es < data.view_start || ss > data.view_end { continue; }

            let x1 = LABEL_WIDTH + ((ss as f64 - vs) / span) * plot_w;
            let x2 = LABEL_WIDTH + ((es as f64 - vs) / span) * plot_w;
            let ann_h = ann_row_h - 2.0;
            // Position annotation below the signal area, in the expanded annotation strip
            let track_y = ch_y.get(ch).copied().unwrap_or(TOOLBAR_HEIGHT + ch as f64 * track_h);
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
            if x2 - x1 > 20.0 {
                ctx.set_fill_style_str(color);
                ctx.set_font("11px 'JetBrains Mono', monospace");
                ctx.set_text_align("center");
                ctx.set_text_baseline("middle");
                let tx = (x1 + x2) / 2.0;
                ctx.fill_text(text, tx, ann_y + ann_h / 2.0).ok();
                ctx.set_text_baseline("alphabetic");
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

        let total = capture_info.get_untracked().map(|i| i.total_samples).unwrap_or(u64::MAX);
        let margin = (total as f64 * 0.05).max(100.0) as u64; // 5% margin
        let limit = total.saturating_add(margin);
        let new_span = (span * factor).max(10.0).min(limit as f64);
        // Keep the sample under the mouse cursor fixed
        let mouse_sample = vs as f64 + span * frac;
        let mut new_start = (mouse_sample - new_span * frac).max(0.0) as u64;
        let mut new_end = new_start + new_span as u64;
        // Clamp to capture bounds with margin
        if new_end > limit { new_end = limit; new_start = limit.saturating_sub(new_span as u64); }

        set_view_start.set(new_start);
        set_view_end.set(new_end);
    };

    let on_mousedown = move |e: MouseEvent| {
        let Some(canvas_el) = canvas_ref.get() else { return; };
        let canvas: HtmlCanvasElement = canvas_el.into();
        let rect = canvas.get_bounding_client_rect();
        let x = e.client_x() as f64 - rect.left();
        let y = e.client_y() as f64 - rect.top();
        let h = canvas.client_height() as f64;
        if x < LABEL_WIDTH { return; }

        // Check if click is in the overview/minimap bar
        let mm_y = h - MINIMAP_HEIGHT;
        if y >= mm_y {
            // Click in minimap → jump viewport to that position
            let total = capture_info.get_untracked().map(|i| i.total_samples).unwrap_or(1);
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
            // Find channel whose Y range contains the cursor
            let offsets = ch_y_offsets.get_untracked();
            let ch_idx = if offsets.is_empty() {
                ((y - TOOLBAR_HEIGHT) / TRACK_HEIGHT) as usize
            } else {
                // Last channel whose start ≤ y (within signal area only)
                offsets.iter().enumerate().rev()
                    .find(|(_, &cy)| y >= cy && y < cy + TRACK_HEIGHT)
                    .map(|(i, _)| i)
                    .unwrap_or_else(|| offsets.len().saturating_sub(1))
            };
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

        let total = capture_info.get_untracked().map(|i| i.total_samples).unwrap_or(u64::MAX);
        let margin = (total as f64 * 0.05).max(100.0) as u64;
        let limit = total.saturating_add(margin);
        let d_samples = (dx_px / plot_w * span) as i64;
        let mut new_start = (vs0 as i64 - d_samples).max(0) as u64;
        let mut new_end = new_start + span as u64;
        // Clamp to capture bounds with margin
        if new_end > limit { new_end = limit; new_start = limit.saturating_sub(span as u64); }

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

        // Place cursor / selection
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
                    let mut ch_names: std::collections::HashMap<u8, String> = std::collections::HashMap::new();
                    for (_, dtype, _, ch_a, ch_b, _, _, extra) in &decs {
                        match dtype.as_str() {
                            "uart" => {
                                ch_names.insert(*ch_a, "TX".into());
                                // Only register RX if it's enabled (extra = "baud,rxch")
                                let has_rx = extra.splitn(2, ',').nth(1).and_then(|s| s.parse::<u8>().ok()).is_some();
                                if has_rx { ch_names.insert(*ch_b, "RX".into()); }
                            }
                            "spi"  => {
                                ch_names.insert(*ch_a, "MOSI".into());
                                if *ch_b != *ch_a { ch_names.insert(*ch_b, "MISO".into()); }
                            }
                            "i2c"  => { ch_names.insert(*ch_a, "SDA".into()); ch_names.insert(*ch_b, "SCL".into()); }
                            _ => {}
                        }
                    }
                    let multi_ch = ch_names.len() > 1;
                    // Collect annotations that overlap the selection, grouped by channel
                    let mut parts: Vec<(u8, String)> = Vec::new();
                    for ann in &anns {
                        let Some(ss_a) = ann.get("startSample").and_then(|v| v.as_u64()) else { continue };
                        let Some(es_a) = ann.get("endSample").and_then(|v| v.as_u64()) else { continue };
                        if es_a < s || ss_a > e_val { continue; }
                        let ann_type = ann.get("annType").and_then(|v| v.as_str()).unwrap_or("data");
                        if ann_type == "control" || ann_type == "info" { continue; }
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
                            parts.iter().map(|(_, t)| t.as_str()).collect::<Vec<_>>().join(" ")
                        };
                        // Copy to clipboard
                        if let Some(win) = web_sys::window() {
                            let nav = win.navigator();
                            let clip = nav.clipboard();
                            let _ = clip.write_text(&clip_text);
                            show_toast(&format!("Copied: {}", if clip_text.len() > 40 { &clip_text[..40] } else { &clip_text }), "ok");
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
        let closure = Closure::<dyn Fn(web_sys::KeyboardEvent)>::new(move |e: web_sys::KeyboardEvent| {
            // Ignore if user is typing in an input
            if let Some(tag) = e.target().and_then(|t| t.dyn_into::<web_sys::Element>().ok()).map(|el| el.tag_name()) {
                if tag == "INPUT" || tag == "TEXTAREA" { return; }
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
        doc.add_event_listener_with_callback("keydown", closure.as_ref().unchecked_ref()).unwrap();
        closure.forget();
    }

    view! {
        <div class="tab-content" style="display: flex; flex-direction: column; height: calc(100vh - 100px); gap: 0; overflow: hidden; margin: -20px; padding: 0">

            // Toolbar with labeled sections
            <div style="display: flex; align-items: flex-end; gap: 4px; padding: 6px 8px; border-bottom: 1px solid var(--border, #1e293b); flex-wrap: wrap; flex-shrink: 0">

                // Channels section — toggle channels on/off
                <div style="display: flex; flex-direction: column; gap: 2px">
                    <span style="font-size: 8px; color: var(--text-muted, #5a6d8a); text-transform: uppercase; letter-spacing: 0.5px">"Channels"</span>
                    <div style="display: flex; gap: 3px">
                        {(0..4u8).map(|i| {
                            let color = CH_COLORS[i as usize];
                            view! {
                                <button
                                    style=move || {
                                        let ch_count: u8 = channels.get().parse().unwrap_or(4);
                                        let enabled = i < ch_count;
                                        if enabled {
                                            format!("font-size: 9px; font-weight: 700; padding: 2px 8px; border-radius: 4px; cursor: pointer; font-family: 'JetBrains Mono', monospace; \
                                                background: {}30; color: {}; border: 1.5px solid {}", color, color, color)
                                        } else {
                                            "font-size: 9px; font-weight: 700; padding: 2px 8px; border-radius: 4px; cursor: pointer; font-family: 'JetBrains Mono', monospace; \
                                                background: transparent; color: #333; border: 1.5px solid #222".into()
                                        }
                                    }
                                    on:click=move |_| {
                                        let ch_count: u8 = channels.get_untracked().parse().unwrap_or(4);
                                        // Click on a disabled channel → enable up to that channel
                                        // Click on the last enabled → reduce count
                                        let new_count = if i < ch_count {
                                            // Clicking an enabled channel: disable from this one onwards (min 1)
                                            (i as u8).max(1)
                                        } else {
                                            // Clicking a disabled channel: enable up to and including it
                                            i + 1
                                        };
                                        set_channels.set(new_count.to_string());
                                    }
                                    title=move || {
                                        let ch_count: u8 = channels.get().parse().unwrap_or(4);
                                        if i < ch_count { format!("CH{} enabled — click to disable", i) }
                                        else { format!("CH{} disabled — click to enable", i) }
                                    }
                                >{format!("CH{}", i)}</button>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>

                <div style="width: 1px; height: 32px; background: var(--border, #1e293b); margin: 0 4px; align-self: flex-end"></div>

                // Sample Rate section
                <div style="display: flex; flex-direction: column; gap: 2px">
                    <span style="font-size: 8px; color: var(--text-muted, #5a6d8a); text-transform: uppercase; letter-spacing: 0.5px">"Sample Rate"</span>
                    <div style="display: flex; gap: 2px">
                        {move || {
                            // Stream mode limits rates (USB FS ~800KB/s → ~1.6M samples/s for 4ch)
                            let is_stream = stream_mode.get();
                            let pairs: &[(&str, &str)] = if is_stream {
                                &[("100k","100000"), ("500k","500000"), ("1M","1000000"), ("2M","2000000")]
                            } else {
                                &[("100k","100000"), ("500k","500000"), ("1M","1000000"),
                                  ("5M","5000000"), ("10M","10000000"), ("25M","25000000"),
                                  ("50M","50000000"), ("100M","125000000")]
                            };
                            pairs.iter().map(|(label, val)| {
                                let v = val.to_string();
                                let l = label.to_string();
                                view! {
                                    <button
                                        style=move || {
                                            let active = rate.get() == v;
                                            format!("font-size: 9px; padding: 2px 7px; border-radius: 10px; cursor: pointer; font-family: 'JetBrains Mono', monospace; transition: all 0.15s; {}",
                                                if active { "background: #3b82f6; color: #fff; border: 1px solid #3b82f6" }
                                                else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                                        }
                                        on:click={ let v2 = val.to_string(); move |_| set_rate.set(v2.clone()) }
                                    >{l.clone()}</button>
                                }
                            }).collect::<Vec<_>>()
                        }}
                    </div>
                </div>

                <div style="width: 1px; height: 32px; background: var(--border, #1e293b); margin: 0 4px; align-self: flex-end"></div>

                // Memory Depth section
                <div style="display: flex; flex-direction: column; gap: 2px">
                    <span style="font-size: 8px; color: var(--text-muted, #5a6d8a); text-transform: uppercase; letter-spacing: 0.5px">"Memory Depth"</span>
                    <div style="display: flex; gap: 2px">
                        {["10K", "50K", "100K", "500K"].iter().zip(
                            ["10000", "50000", "100000", "500000"].iter()
                        ).map(|(label, val)| {
                            let v = val.to_string();
                            let l = label.to_string();
                            view! {
                                <button
                                    style=move || {
                                        let active = depth.get() == v;
                                        format!("font-size: 9px; padding: 2px 7px; border-radius: 10px; cursor: pointer; font-family: 'JetBrains Mono', monospace; transition: all 0.15s; {}",
                                            if active { "background: #10b981; color: #fff; border: 1px solid #10b981" }
                                            else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                                    }
                                    on:click={ let v2 = val.to_string(); move |_| set_depth.set(v2.clone()) }
                                >{l.clone()}</button>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>

                <div style="width: 1px; height: 32px; background: var(--border, #1e293b); margin: 0 4px; align-self: flex-end"></div>

                // Trigger section
                <div style="display: flex; flex-direction: column; gap: 2px">
                    <span style="font-size: 8px; color: var(--text-muted, #5a6d8a); text-transform: uppercase; letter-spacing: 0.5px">"Polarity"</span>
                    <div style="display: flex; gap: 2px; align-items: center">
                        {[("0", "—", "None"), ("1", "↑", "Rising"), ("2", "↓", "Falling"), ("4", "▔", "High"), ("5", "▁", "Low")].iter().map(|(val, icon, tip)| {
                            let v = val.to_string();
                            let ic = icon.to_string();
                            let tt = tip.to_string();
                            view! {
                                <button
                                    style=move || {
                                        let active = trig_type.get() == v;
                                        format!("font-size: 12px; padding: 1px 5px; border-radius: 4px; cursor: pointer; min-width: 22px; transition: all 0.15s; {}",
                                            if active { "background: #f59e0b30; color: #f59e0b; border: 1px solid #f59e0b" }
                                            else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                                    }
                                    title=tt.clone()
                                    on:click={ let v2 = val.to_string(); move |_| set_trig_type.set(v2.clone()) }
                                >{ic.clone()}</button>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>

                // Trigger Channel
                <div style="display: flex; flex-direction: column; gap: 2px">
                    <span style="font-size: 8px; color: var(--text-muted, #5a6d8a); text-transform: uppercase; letter-spacing: 0.5px">"Trigger Ch"</span>
                    <div style="display: flex; gap: 2px">
                        {(0..4u8).map(|i| {
                            let v = i.to_string();
                            let color = CH_COLORS[i as usize];
                            view! {
                                <button
                                    style=move || {
                                        let active = trig_ch.get() == v;
                                        format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; font-family: 'JetBrains Mono', monospace; {}",
                                            if active { format!("background: {}30; color: {}; border: 1px solid {}", color, color, color) }
                                            else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)".into() })
                                    }
                                    on:click={ let v2 = i.to_string(); move |_| set_trig_ch.set(v2.clone()) }
                                >{format!("{}", i)}</button>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>

                <div style="width: 1px; height: 32px; background: var(--border, #1e293b); margin: 0 4px; align-self: flex-end"></div>

                // Mode section (Memory/Stream + RLE)
                <div style="display: flex; flex-direction: column; gap: 2px">
                    <span style="font-size: 8px; color: var(--text-muted, #5a6d8a); text-transform: uppercase; letter-spacing: 0.5px">"Capture Mode"</span>
                    <div style="display: flex; gap: 2px">
                        <button
                            style=move || format!("font-size: 9px; padding: 2px 8px; border-radius: 10px; cursor: pointer; font-family: 'JetBrains Mono', monospace; transition: all 0.15s; {}",
                                if !stream_mode.get() { "background: #8b5cf6; color: #fff; border: 1px solid #8b5cf6" }
                                else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                            on:click=move |_| set_stream_mode.set(false)
                            title="Capture a fixed memory depth then stop"
                        >"Memory"</button>
                        <button
                            style=move || format!("font-size: 9px; padding: 2px 8px; border-radius: 10px; cursor: pointer; font-family: 'JetBrains Mono', monospace; transition: all 0.15s; {}",
                                if stream_mode.get() { "background: #06b6d4; color: #fff; border: 1px solid #06b6d4" }
                                else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                            on:click=move |_| {
                                set_stream_mode.set(true);
                                // Limit rate to stream-safe values
                                let r = rate.get_untracked();
                                let r_hz: u32 = r.parse().unwrap_or(1000000);
                                if r_hz > 2000000 { set_rate.set("1000000".to_string()); }
                            }
                            title="Continuous live capture (limited sample rate)"
                        >"Stream"</button>
                        <button
                            style=move || format!("font-size: 9px; padding: 2px 8px; border-radius: 10px; cursor: pointer; font-family: 'JetBrains Mono', monospace; transition: all 0.15s; {}",
                                if rle_enabled.get() { "background: #06b6d430; color: #06b6d4; border: 1px solid #06b6d4" }
                                else { "background: transparent; color: #06b6d480; border: 1px solid #06b6d430" })
                            on:click=move |_| set_rle_enabled.update(|v| *v = !*v)
                            title="Run-Length Encoding — compresses captures, more depth for slow signals"
                        >"RLE"</button>
                    </div>
                </div>

                <div style="width: 1px; height: 32px; background: var(--border, #1e293b); margin: 0 4px; align-self: flex-end"></div>

                // Decoders toggle button
                <div style="display: flex; flex-direction: column; gap: 2px">
                    <span style="font-size: 8px; color: var(--text-muted, #5a6d8a); text-transform: uppercase; letter-spacing: 0.5px">"Decoders"</span>
                    <button
                        style=move || format!("font-size: 9px; padding: 2px 10px; border-radius: 10px; cursor: pointer; font-family: 'JetBrains Mono', monospace; transition: all 0.15s; {}",
                            if decoder_panel_open.get() { "background: #a855f7; color: #fff; border: 1px solid #a855f7" }
                            else { "background: transparent; color: #a855f780; border: 1px solid #a855f740" })
                        on:click=move |_| set_decoder_panel_open.update(|v| *v = !*v)
                        title="Configure protocol decoders (UART, I2C, SPI)"
                    >{move || if decoder_panel_open.get() { "▲ Hide" } else { "▼ Show" }}</button>
                </div>

                <div style="width: 1px; height: 32px; background: var(--border, #1e293b); margin: 0 4px; align-self: flex-end"></div>

                // Control buttons
                <button style=move || format!("font-size: 10px; padding: 3px 12px; border-radius: 4px; cursor: pointer; {}",
                    if streaming.get() { "background: #10b98140; color: #10b981; border: 1px solid #10b98180; animation: pulse 1s infinite" }
                    else { "background: #10b98125; color: #10b981; border: 1px solid #10b98150" })
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
                                set_streaming.set(true);
                                spawn_local(async move {
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
                                            let p = js_sys::Promise::new(&mut |resolve, _| {
                                                web_sys::window().unwrap()
                                                    .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 200).unwrap();
                                            });
                                            let _ = wasm_bindgen_futures::JsFuture::from(p).await;

                                            // Check if backend is still streaming (allow a few misses)
                                            if !la_stream_usb_active().await {
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

                                        // Stop gapless stream
                                        if let Some(info) = la_stream_usb_stop().await {
                                            set_ci5.set(Some(info));
                                        }
                                    } else {
                                        // Fallback: cycle-based streaming via la_stream_cycle
                                        web_sys::console::log_1(&"[STREAM] USB gapless unavailable, falling back to cycle mode".into());
                                        let mut first_capture = true;
                                        let mut cycle = 0u32;
                                        while streaming.get_untracked() {
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
                                    let result = invoke("la_get_status", JsValue::NULL).await;
                                    if let Ok(st) = serde_wasm_bindgen::from_value::<StRsp>(result) {
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
                                            let read_result = invoke("la_read_uart_chunks", args).await;
                                            if let Ok(info) = serde_wasm_bindgen::from_value::<LaCaptureInfo>(read_result) {
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
                >{move || if streaming.get() { "● Stream" } else { "Arm" }}</button>
                <button style="font-size: 10px; padding: 3px 12px; background: #f59e0b25; color: #f59e0b; border: 1px solid #f59e0b50; border-radius: 4px; cursor: pointer"
                    on:click=move |_| { spawn_local(async { la_invoke_force().await; show_toast("Triggered", "ok"); }); }
                >"Force"</button>
                <button style="font-size: 10px; padding: 3px 12px; background: #ef444425; color: #ef4444; border: 1px solid #ef444450; border-radius: 4px; cursor: pointer"
                    on:click=move |_| {
                        set_streaming.set(false);
                        spawn_local(async {
                            // Stop USB gapless stream if active
                            let _ = la_stream_usb_stop().await;
                            // Also send BBP stop (for cycle-based / ESP32 path)
                            la_invoke_stop().await;
                            show_toast("Stopped", "ok");
                        });
                    }
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

                // Clear button
                <button style="font-size: 10px; padding: 3px 12px; background: #64748b15; color: #64748b; border: 1px solid #64748b40; border-radius: 4px; cursor: pointer"
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
                            let _ = la_delete_range(0, u64::MAX).await;
                        });
                        show_toast("Capture cleared", "ok");
                    }
                >"Clear"</button>

                // Test data button — loads decodable protocol waveforms
                // CH0: UART TX 9600 baud  → decoder: UART TX=CH0 Baud=9600
                // CH1: SPI MOSI           → decoder: SPI MOSI=CH1 MISO=CH1 CLK=CH2 CS=CH3 Mode=M0
                // CH2: SPI CLK
                // CH3: SPI CS (active low)
                <button style="font-size: 10px; padding: 3px 12px; background: #8b5cf625; color: #8b5cf6; border: 1px solid #8b5cf650; border-radius: 4px; cursor: pointer"
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

            // Decoder panel (toggleable)
            {move || if decoder_panel_open.get() { view! {
                <div style="display: flex; flex-direction: column; gap: 6px; padding: 8px; border-bottom: 1px solid var(--border, #1e293b); background: #070d1a; flex-shrink: 0">
                    // Header row
                    <div style="display: flex; align-items: center; gap: 8px">
                        <span style="font-size: 10px; font-weight: 700; color: #a855f7; font-family: 'JetBrains Mono', monospace; text-transform: uppercase; letter-spacing: 0.5px">"Protocol Decoders"</span>
                        <div style="flex: 1"></div>
                        // Format selector
                        {[("Hex","hex"), ("Dec","dec"), ("ASCII","ascii"), ("Bin","bin")].iter().map(|(lbl, val)| {
                            let v1 = val.to_string();
                            let v2 = val.to_string();
                            let l = lbl.to_string();
                            view! {
                                <button
                                    style=move || {
                                        let active = ann_fmt.get() == v1;
                                        format!("font-size: 8px; padding: 1px 5px; border-radius: 3px; cursor: pointer; font-family: 'JetBrains Mono', monospace; {}",
                                            if active { "background: #a855f730; color: #a855f7; border: 1px solid #a855f7" }
                                            else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                                    }
                                    on:click={let v = v2.clone(); move |_| set_ann_fmt.set(v.clone())}
                                >{l}</button>
                            }
                        }).collect::<Vec<_>>()}
                        <button
                            style="font-size: 9px; padding: 2px 10px; background: #a855f720; color: #a855f7; border: 1px solid #a855f750; border-radius: 3px; cursor: pointer; font-family: 'JetBrains Mono', monospace"
                            on:click={
                                let set_ann = set_annotations;
                                move |_| {
                                    let decs = decoders.get_untracked();
                                    // Always decode full capture, not just the visible viewport
                                    let dec_start: u64 = 0;
                                    let dec_end: u64 = capture_info.get_untracked()
                                        .map(|i| i.total_samples)
                                        .unwrap_or(u64::MAX);
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
                        >"▶ Run All"</button>
                    </div>

                    // Active decoders list
                    {move || {
                        let decs = decoders.get();
                        if decs.is_empty() {
                            view! { <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">"No decoders — add one below"</span> }.into_any()
                        } else {
                            view! {
                                <div style="display: flex; flex-direction: column; gap: 3px">
                                    {decs.iter().map(|(id, _, label, _, _, _, _, _)| {
                                        let id2 = *id;
                                        view! {
                                            <div style="display: flex; align-items: center; gap: 6px; padding: 3px 8px; background: #0c1222; border: 1px solid #1e293b; border-radius: 4px">
                                                <span style="font-size: 9px; color: #c4d4f0; font-family: 'JetBrains Mono', monospace">{label.clone()}</span>
                                                <div style="flex: 1"></div>
                                                <button
                                                    style="font-size: 9px; padding: 0px 6px; background: #ef444420; color: #ef4444; border: 1px solid #ef444440; border-radius: 2px; cursor: pointer"
                                                    on:click=move |_| {
                                                        set_decoders.update(|v| v.retain(|(id, _, _, _, _, _, _, _)| *id != id2));
                                                    }
                                                >"✕"</button>
                                            </div>
                                        }
                                    }).collect::<Vec<_>>()}
                                </div>
                            }.into_any()
                        }
                    }}

                    // Add decoder form
                    <div style="display: flex; align-items: center; gap: 6px; flex-wrap: wrap">
                        // Type selector
                        <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">"Add:"</span>
                        {["uart", "i2c", "spi"].iter().map(|t| {
                            let t_str = t.to_string();
                            view! {
                                <button
                                    style=move || format!("font-size: 9px; padding: 1px 8px; border-radius: 10px; cursor: pointer; font-family: 'JetBrains Mono', monospace; text-transform: uppercase; {}",
                                        if add_dec_type.get() == t_str { "background: #a855f7; color: #fff; border: 1px solid #a855f7" }
                                        else { "background: transparent; color: #a855f780; border: 1px solid #a855f740" })
                                    on:click={ let ts = t.to_string(); move |_| set_add_dec_type.set(ts.clone()) }
                                >{t.to_uppercase()}</button>
                            }
                        }).collect::<Vec<_>>()}

                        // Channel selectors (adapt to type)
                        {move || {
                            let t = add_dec_type.get();
                            if t == "uart" {
                                // UART: TX selector + optional RX with Off button
                                view! {
                                    <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">"TX"</span>
                                    <div style="display: flex; gap: 2px">
                                        {(0..4u8).map(|i| view! {
                                            <button
                                                style=move || {
                                                    let active = add_dec_ch_a.get() == i;
                                                    let col = CH_COLORS[i as usize];
                                                    if active { format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: {}30; color: {}; border: 1px solid {}", col, col, col) }
                                                    else { "font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)".into() }
                                                }
                                                on:click=move |_| set_add_dec_ch_a.set(i)
                                            >{i.to_string()}</button>
                                        }).collect::<Vec<_>>()}
                                    </div>
                                    <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">"RX"</span>
                                    <div style="display: flex; gap: 2px">
                                        <button
                                            style=move || format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; {}",
                                                if add_dec_uart_rx_off.get() { "background: #64748b40; color: #94a3b8; border: 1px solid #64748b" }
                                                else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                                            on:click=move |_| set_add_dec_uart_rx_off.set(true)
                                        >"–"</button>
                                        {(0..4u8).map(|i| view! {
                                            <button
                                                style=move || {
                                                    let active = !add_dec_uart_rx_off.get() && add_dec_ch_b.get() == i;
                                                    let col = CH_COLORS[i as usize];
                                                    if active { format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: {}30; color: {}; border: 1px solid {}", col, col, col) }
                                                    else { "font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)".into() }
                                                }
                                                on:click=move |_| { set_add_dec_uart_rx_off.set(false); set_add_dec_ch_b.set(i); }
                                            >{i.to_string()}</button>
                                        }).collect::<Vec<_>>()}
                                    </div>
                                }.into_any()
                            } else if t == "spi" {
                                // SPI: MOSI/MISO/CLK with generic buttons; CS with optional "–"
                                let ch_labels: Vec<(&str, ReadSignal<u8>, WriteSignal<u8>)> = vec![
                                    ("MOSI", add_dec_ch_a, set_add_dec_ch_a),
                                    ("MISO", add_dec_ch_b, set_add_dec_ch_b),
                                    ("CLK",  add_dec_ch_c, set_add_dec_ch_c),
                                ];
                                view! {
                                    {ch_labels.into_iter().map(|(lbl, sig, set_sig)| view! {
                                        <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">{lbl}</span>
                                        <div style="display: flex; gap: 2px">
                                            {(0..4u8).map(|i| view! {
                                                <button
                                                    style=move || {
                                                        let active = sig.get() == i;
                                                        let col = CH_COLORS[i as usize];
                                                        if active { format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: {}30; color: {}; border: 1px solid {}", col, col, col) }
                                                        else { "font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)".into() }
                                                    }
                                                    on:click=move |_| set_sig.set(i)
                                                >{i.to_string()}</button>
                                            }).collect::<Vec<_>>()}
                                        </div>
                                    }).collect::<Vec<_>>()}
                                    // CS row with "–" (no CS) option
                                    <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">"CS"</span>
                                    <div style="display: flex; gap: 2px">
                                        <button
                                            style=move || {
                                                let active = add_dec_spi_cs_off.get();
                                                format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; {}",
                                                    if active { "background: #64748b30; color: #94a3b8; border: 1px solid #64748b" }
                                                    else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                                            }
                                            on:click=move |_| set_add_dec_spi_cs_off.set(true)
                                        >"–"</button>
                                        {(0..4u8).map(|i| view! {
                                            <button
                                                style=move || {
                                                    let active = !add_dec_spi_cs_off.get() && add_dec_ch_d.get() == i;
                                                    let col = CH_COLORS[i as usize];
                                                    if active { format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: {}30; color: {}; border: 1px solid {}", col, col, col) }
                                                    else { "font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)".into() }
                                                }
                                                on:click=move |_| { set_add_dec_spi_cs_off.set(false); set_add_dec_ch_d.set(i); }
                                            >{i.to_string()}</button>
                                        }).collect::<Vec<_>>()}
                                    </div>
                                }.into_any()
                            } else {
                                let ch_labels: Vec<(&str, ReadSignal<u8>, WriteSignal<u8>)> = match t.as_str() {
                                    "i2c" => vec![("SDA", add_dec_ch_a, set_add_dec_ch_a), ("SCL", add_dec_ch_b, set_add_dec_ch_b)],
                                    _     => vec![],
                                };
                                ch_labels.into_iter().map(|(lbl, sig, set_sig)| {
                                    view! {
                                        <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">{lbl}</span>
                                        <div style="display: flex; gap: 2px">
                                            {(0..4u8).map(|i| view! {
                                                <button
                                                    style=move || {
                                                        let active = sig.get() == i;
                                                        let col = CH_COLORS[i as usize];
                                                        if active { format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: {}30; color: {}; border: 1px solid {}", col, col, col) }
                                                        else { "font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)".into() }
                                                    }
                                                    on:click=move |_| set_sig.set(i)
                                                >{i.to_string()}</button>
                                            }).collect::<Vec<_>>()}
                                        </div>
                                    }
                                }).collect::<Vec<_>>().into_any()
                            }
                        }}

                        // UART: baud rate input
                        {move || {
                            if add_dec_type.get() == "uart" { view! {
                                <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">"Baud"</span>
                                <input
                                    type="text"
                                    prop:value=move || add_dec_baud.get()
                                    style="background: #0c1222; border: 1px solid #1e293b; color: #e2e8f0; padding: 1px 6px; border-radius: 3px; width: 72px; font-family: 'JetBrains Mono', monospace; font-size: 9px"
                                    on:change=move |e| {
                                        let input: web_sys::HtmlInputElement = e.target().unwrap().unchecked_into();
                                        set_add_dec_baud.set(input.value());
                                    }
                                />
                            }.into_any() } else { view! { <span></span> }.into_any() }
                        }}

                        // SPI: mode selector
                        {move || {
                            if add_dec_type.get() == "spi" { view! {
                                <span style="font-size: 9px; color: var(--text-muted); font-family: 'JetBrains Mono', monospace">"Mode"</span>
                                {[("0","CPOL0/CPHA0"), ("1","CPOL0/CPHA1"), ("2","CPOL1/CPHA0"), ("3","CPOL1/CPHA1")].iter().map(|(m, tip)| {
                                    let mv: u8 = m.parse().unwrap();
                                    view! {
                                        <button
                                            style=move || {
                                                let active = add_dec_spi_mode.get() == mv;
                                                format!("font-size: 9px; padding: 1px 5px; border-radius: 3px; cursor: pointer; {}",
                                                    if active { "background: #06b6d430; color: #06b6d4; border: 1px solid #06b6d4" }
                                                    else { "background: transparent; color: var(--text-dim); border: 1px solid var(--border, #333)" })
                                            }
                                            title=tip.to_string()
                                            on:click=move |_| set_add_dec_spi_mode.set(mv)
                                        >{format!("M{}", mv)}</button>
                                    }
                                }).collect::<Vec<_>>()}
                            }.into_any() } else { view! { <span></span> }.into_any() }
                        }}

                        // Add button
                        <button
                            style="font-size: 9px; padding: 2px 10px; background: #10b98120; color: #10b981; border: 1px solid #10b98150; border-radius: 3px; cursor: pointer; font-family: 'JetBrains Mono', monospace"
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
                        >"+ Add"</button>
                    </div>
                </div>
            }.into_any() } else { view! { <div></div> }.into_any() }}

            // Canvas waveform area
            <canvas
                node_ref=canvas_ref
                style=move || format!("flex: 1; width: 100%; min-height: 0; cursor: {}; border-radius: 4px",
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
                let vd = view_data.get();
                view! {
                    <div style="display: flex; align-items: center; gap: 8px; padding: 4px 8px; font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; border-top: 1px solid var(--border, #1e293b); flex-shrink: 0">
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
