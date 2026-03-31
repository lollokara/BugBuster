use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Deserialize;
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

const COLORS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];
const MAX_POINTS: usize = 100_000;  // 100k points per channel for high-rate data

#[derive(Clone)]
struct ScopePoint {
    time_ms: f64,
    value: f32,
}

/// Convert 24-bit raw ADC code to a value based on channel function and ADC range.
/// For current-input modes (function 4, 5, 11, 12) the ADC code maps to 0-25 mA.
/// For current-output modes (function 2, 10) the ADC reads compliance voltage, so
/// we fall through to the normal voltage conversion.
/// All other modes use the standard voltage ADC ranges.
fn raw_to_value(raw: u32, range: u8, function: u8) -> f32 {
    let code = raw as f32;
    let full_scale = 16777216.0f32; // 2^24

    // Current-input modes: ADC code represents 0-25 mA
    match function {
        4 | 5 | 11 | 12 => return code / full_scale * 25.0,
        _ => {}
    }

    // Voltage (and current-output compliance voltage) conversion
    match range {
        0 => code / full_scale * 12.0,                          // 0..12V
        1 => (code / full_scale * 24.0) - 12.0,                // -12..12V
        2 => (code / full_scale * 0.625) - 0.3125,             // -312.5..312.5mV
        3 => (code / full_scale * 0.3125) - 0.3125,            // -312.5..0mV
        4 => code / full_scale * 0.3125,                        // 0..312.5mV
        5 => code / full_scale * 0.625,                          // 0..625mV
        6 => (code / full_scale * 0.208) - 0.104,              // -104..104mV
        7 => (code / full_scale * 5.0) - 2.5,                  // -2.5..2.5V
        _ => code / full_scale * 12.0,
    }
}

/// Parse binary ADC stream event payload
fn parse_adc_event(data: &[u8]) -> Option<(u8, Vec<[u32; 4]>)> {
    if data.len() < 7 { return None; }
    let mask = data[0];
    // timestamp at [1..5] — not used for now (we use JS Date.now)
    let count = u16::from_le_bytes([data[5], data[6]]) as usize;
    let num_ch = (0..4).filter(|b| mask & (1 << b) != 0).count();
    if num_ch == 0 { return None; }

    let expected_data_len = 7 + count * num_ch * 3;
    if data.len() < expected_data_len { return None; }

    let mut samples = Vec::with_capacity(count);
    let mut pos = 7;
    for _ in 0..count {
        let mut raw = [0u32; 4];
        for ch in 0..4 {
            if mask & (1 << ch) != 0 {
                if pos + 3 > data.len() { break; }
                raw[ch] = data[pos] as u32
                    | ((data[pos + 1] as u32) << 8)
                    | ((data[pos + 2] as u32) << 16);
                pos += 3;
            }
        }
        samples.push(raw);
    }
    Some((mask, samples))
}

#[component]
pub fn ScopeTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (running, set_running) = signal(false);
    let (channels_en, set_channels_en) = signal([true, false, false, false]);
    let (window_sec, set_window_sec) = signal(30.0f64);
    let (y_range, set_y_range) = signal("auto".to_string());
    let (recording, set_recording) = signal(false);
    let (csv_path, set_csv_path) = signal(String::new());
    let (sample_rate, set_sample_rate) = signal(0u32);  // Actual samples/sec counter
    let (cursor_x, set_cursor_x) = signal(Option::<f64>::None);  // Cursor X position (None = hidden)

    let scope_data: [RwSignal<Vec<ScopePoint>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    // Track sample count for rate display
    let sample_counter = RwSignal::new(0u32);
    let last_rate_check = RwSignal::new(js_sys::Date::now());

    // Display listener: "adc-stream" events (throttled to ~30 Hz by backend)
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            #[derive(Deserialize)]
            struct TauriEvt { payload: Vec<u8> }
            let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvt>(event) else { return };
            if !running.get_untracked() { return; }

            let Some((mask, samples)) = parse_adc_event(&evt.payload) else { return };
            let now = js_sys::Date::now();
            let ch_en = channels_en.get_untracked();
            let ds = state.get_untracked();
            let ranges: [u8; 4] = std::array::from_fn(|i| {
                if i < ds.channels.len() { ds.channels[i].adc_range } else { 0 }
            });
            let functions: [u8; 4] = std::array::from_fn(|i| {
                if i < ds.channels.len() { ds.channels[i].function } else { 0 }
            });

            // Push last sample of batch to display
            if let Some(raw) = samples.last() {
                for ch in 0..4 {
                    if !ch_en[ch] || (mask & (1 << ch)) == 0 { continue; }
                    let v = raw_to_value(raw[ch], ranges[ch], functions[ch]);
                    scope_data[ch].update(|data| {
                        data.push(ScopePoint { time_ms: now, value: v });
                    });
                }
            }

            // Count samples for rate display
            sample_counter.update(|c| *c += samples.len() as u32);

            // Trim old data
            let win = window_sec.get_untracked();
            let cutoff = now - win * 1000.0 * 1.5;
            for ch in 0..4 {
                scope_data[ch].update(|data| {
                    data.retain(|p| p.time_ms > cutoff);
                    if data.len() > MAX_POINTS { data.drain(0..data.len() - MAX_POINTS); }
                });
            }
        });
        listen("adc-stream", &closure).await;
        closure.forget();
    });

    // Recording is handled directly in the Tauri backend (connection_manager.rs)
    // — no frontend involvement needed for full-rate recording.

    // Rate counter + auto-restart: update every second
    let zero_count = std::cell::Cell::new(0u32);
    let restart_count = std::cell::Cell::new(0u32);
    const MAX_RESTARTS: u32 = 5;
    spawn_local(async move {
        loop {
            sleep_ms(1000).await;
            let count = sample_counter.get_untracked();
            sample_counter.set(0);
            set_sample_rate.set(count);

            // If scope is running but no samples for 3 seconds, restart stream
            if running.get_untracked() && count == 0 {
                let z = zero_count.get() + 1;
                zero_count.set(z);
                if z >= 3 {
                    zero_count.set(0);
                    let restarts = restart_count.get() + 1;
                    restart_count.set(restarts);
                    if restarts <= MAX_RESTARTS {
                        log(&format!("Scope: 0 SPS for 3s, restarting stream ({}/{})...", restarts, MAX_RESTARTS));
                        // Toggle running to trigger the Effect restart
                        set_running.set(false);
                        sleep_ms(100).await;
                        set_running.set(true);
                    } else if restarts == MAX_RESTARTS + 1 {
                        log("Scope: max restart attempts reached, stopping auto-restart.");
                    }
                }
            } else {
                zero_count.set(0);
                restart_count.set(0);
            }
        }
    });

    // Start/stop ADC stream when running or channel selection changes.
    // Always stop-then-start to handle ERR_STREAM_ACTIVE.
    Effect::new(move || {
        let is_running = running.get();
        let ch_en = channels_en.get();
        if is_running {
            let mask: u8 = (0..4).fold(0u8, |m, i| if ch_en[i] { m | (1 << i) } else { m });
            if mask == 0 {
                log("Scope: no channels enabled");
                set_running.set(false);
                return;
            }
            #[derive(serde::Serialize)]
            #[serde(rename_all = "camelCase")]
            struct StartArgs { channel_mask: u8, divider: u8 }
            let args = serde_wasm_bindgen::to_value(&StartArgs { channel_mask: mask, divider: 1 }).unwrap();
            spawn_local(async move {
                // Stop any existing stream first (ignore errors)
                let _ = invoke("stop_adc_stream", wasm_bindgen::JsValue::NULL).await;
                // Delay to let firmware clear the stream state (200ms for HTTP transport)
                sleep_ms(200).await;
                // Start fresh
                let result = invoke("start_adc_stream", args).await;
                log(&format!("Scope: stream started mask=0x{:02X}, result={:?}", mask, result));
            });
        } else {
            spawn_local(async move {
                let _ = invoke("stop_adc_stream", wasm_bindgen::JsValue::NULL).await;
                log("Scope: stream stopped");
            });
        }
    });

    // Render loop (~30 FPS)
    spawn_local(async move {
        loop {
            sleep_ms(33).await;

            let Some(canvas) = canvas_ref.get() else { continue };
            let canvas: HtmlCanvasElement = canvas.into();
            let Some(ctx) = canvas.get_context("2d").ok().flatten() else { continue };
            let ctx: CanvasRenderingContext2d = ctx.unchecked_into();

            let dpr = web_sys::window().unwrap().device_pixel_ratio();
            let rect = canvas.get_bounding_client_rect();
            let w = rect.width();
            let h = rect.height();
            let cw = (w * dpr) as u32;
            let ch = (h * dpr) as u32;
            if canvas.width() != cw { canvas.set_width(cw); }
            if canvas.height() != ch { canvas.set_height(ch); }
            ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();

            // Clear
            ctx.set_fill_style_str("#0f1729");
            ctx.fill_rect(0.0, 0.0, w, h);

            let ch_en = channels_en.get_untracked();
            let now = js_sys::Date::now();
            let win_ms = window_sec.get_untracked() * 1000.0;
            let t_start = now - win_ms;
            let yr = y_range.get_untracked();

            // Y range
            let (y_min, y_max) = if yr == "auto" {
                let mut mn = f64::MAX;
                let mut mx = f64::MIN;
                for i in 0..4 {
                    if !ch_en[i] { continue; }
                    scope_data[i].with_untracked(|data| {
                        for p in data.iter().rev().take(5000) {
                            if p.time_ms < t_start { break; }
                            let v = p.value as f64;
                            if v < mn { mn = v; }
                            if v > mx { mx = v; }
                        }
                    });
                }
                if mn == f64::MAX { (0.0, 1.0) }
                else {
                    let margin = (mx - mn).max(0.01) * 0.15;
                    (mn - margin, mx + margin)
                }
            } else {
                match yr.as_str() {
                    "0-12" => (0.0, 12.0), "pm12" => (-12.0, 12.0),
                    "0-625m" => (0.0, 0.625), "0-25m" => (0.0, 25.0),
                    _ => (0.0, 12.0),
                }
            };

            // Plot area
            let ml = 65.0; let mr = 15.0; let mt = 15.0; let mb = 30.0;
            let pw = w - ml - mr;
            let ph = h - mt - mb;
            let y_span = y_max - y_min;

            // Grid
            ctx.set_font("10px 'JetBrains Mono', monospace");
            ctx.set_text_align("right");
            for j in 0..=6 {
                let frac = j as f64 / 6.0;
                let y = mt + frac * ph;
                let val = y_max - frac * y_span;
                ctx.set_stroke_style_str(if j == 3 { "rgba(148,163,184,0.15)" } else { "rgba(148,163,184,0.07)" });
                ctx.begin_path();
                ctx.move_to(ml, y);
                ctx.line_to(w - mr, y);
                ctx.stroke();
                ctx.set_fill_style_str("rgba(148,163,184,0.5)");
                let _ = ctx.fill_text(&format!("{:.2}", val), ml - 8.0, y + 4.0);
            }
            ctx.set_text_align("center");
            for j in 0..=8 {
                let frac = j as f64 / 8.0;
                let x = ml + frac * pw;
                ctx.set_stroke_style_str("rgba(148,163,184,0.05)");
                ctx.begin_path();
                ctx.move_to(x, mt);
                ctx.line_to(x, h - mb);
                ctx.stroke();
                let secs_ago = (1.0 - frac) * window_sec.get_untracked();
                ctx.set_fill_style_str("rgba(148,163,184,0.4)");
                let _ = ctx.fill_text(&format!("-{:.0}s", secs_ago), x, h - mb + 16.0);
            }
            ctx.set_stroke_style_str("rgba(148,163,184,0.2)");
            ctx.stroke_rect(ml, mt, pw, ph);

            // Plot channels with decimation for performance
            for c in 0..4 {
                if !ch_en[c] { continue; }
                scope_data[c].with_untracked(|data| {
                    if data.len() < 2 { return; }

                    ctx.set_stroke_style_str(COLORS[c]);
                    ctx.set_line_width(1.2);
                    ctx.begin_path();
                    let mut started = false;

                    // Decimation: if we have more points than pixels, skip
                    let visible: usize = data.iter().filter(|p| p.time_ms >= t_start).count();
                    let skip = if visible > (pw as usize * 2) { visible / (pw as usize * 2) } else { 1 };
                    let mut idx = 0;

                    for p in data.iter() {
                        if p.time_ms < t_start { continue; }
                        idx += 1;
                        if skip > 1 && idx % skip != 0 { continue; }

                        let x = ml + ((p.time_ms - t_start) / win_ms) * pw;
                        let y = mt + ((y_max - p.value as f64) / y_span) * ph;
                        let y = y.clamp(mt, mt + ph);
                        if !started { ctx.move_to(x, y); started = true; }
                        else { ctx.line_to(x, y); }
                    }
                    ctx.stroke();
                });
            }

            // Cursor rendering
            if let Some(cx) = cursor_x.get_untracked() {
                // Draw vertical cursor line
                ctx.set_stroke_style_str("rgba(248,250,252,0.6)");
                ctx.set_line_width(1.0);
                ctx.begin_path();
                ctx.move_to(cx, mt);
                ctx.line_to(cx, mt + ph);
                ctx.stroke();

                // Compute time at cursor position
                let cursor_frac = (cx - ml) / pw;
                let cursor_time = t_start + cursor_frac * win_ms;
                let secs_ago = (now - cursor_time) / 1000.0;

                // Build tooltip with time and interpolated values for each enabled channel
                let mut tooltip_lines: Vec<String> = Vec::new();
                tooltip_lines.push(format!("t: -{:.2}s", secs_ago));

                for c in 0..4 {
                    if !ch_en[c] { continue; }
                    scope_data[c].with_untracked(|data| {
                        // Find the two points bracketing cursor_time and interpolate
                        let mut interp_val: Option<f32> = None;
                        for i in 1..data.len() {
                            if data[i - 1].time_ms <= cursor_time && data[i].time_ms >= cursor_time {
                                let dt_seg = data[i].time_ms - data[i - 1].time_ms;
                                if dt_seg > 0.0 {
                                    let t_frac = (cursor_time - data[i - 1].time_ms) / dt_seg;
                                    interp_val = Some(
                                        data[i - 1].value + (data[i].value - data[i - 1].value) * t_frac as f32
                                    );
                                } else {
                                    interp_val = Some(data[i].value);
                                }
                                break;
                            }
                        }
                        if let Some(v) = interp_val {
                            tooltip_lines.push(format!("CH {}: {:.4}", CH_NAMES[c], v));

                            // Draw a dot at the intersection
                            let y = mt + ((y_max - v as f64) / y_span) * ph;
                            let y = y.clamp(mt, mt + ph);
                            ctx.set_fill_style_str(COLORS[c]);
                            ctx.begin_path();
                            let _ = ctx.arc(cx, y, 4.0, 0.0, std::f64::consts::TAU);
                            ctx.fill();
                        }
                    });
                }

                // Draw tooltip background
                let tt_x = if cx + 160.0 > w - mr { cx - 165.0 } else { cx + 10.0 };
                let tt_y = mt + 10.0;
                let line_h = 16.0;
                let tt_h = tooltip_lines.len() as f64 * line_h + 8.0;
                let tt_w = 155.0;

                ctx.set_fill_style_str("rgba(15,23,42,0.92)");
                ctx.fill_rect(tt_x, tt_y, tt_w, tt_h);
                ctx.set_stroke_style_str("rgba(59,130,246,0.3)");
                ctx.stroke_rect(tt_x, tt_y, tt_w, tt_h);

                ctx.set_font("11px 'JetBrains Mono', monospace");
                ctx.set_text_align("left");
                for (li, line) in tooltip_lines.iter().enumerate() {
                    let color = if li == 0 {
                        "rgba(148,163,184,0.8)"
                    } else {
                        COLORS[{
                            // Map tooltip line index back to channel index
                            let mut ch_idx = 0;
                            let mut count = 0;
                            for c in 0..4 {
                                if ch_en[c] {
                                    count += 1;
                                    if count == li { ch_idx = c; break; }
                                }
                            }
                            ch_idx
                        }]
                    };
                    ctx.set_fill_style_str(color);
                    let _ = ctx.fill_text(line, tt_x + 6.0, tt_y + 14.0 + li as f64 * line_h);
                }
            }

            // Legend with current values
            ctx.set_font("12px 'JetBrains Mono', monospace");
            ctx.set_text_align("left");
            let mut ly = mt + 16.0;
            for c in 0..4 {
                if !ch_en[c] { continue; }
                scope_data[c].with_untracked(|data| {
                    if let Some(last) = data.last() {
                        ctx.set_fill_style_str("rgba(15,23,42,0.85)");
                        ctx.fill_rect(ml + 8.0, ly - 12.0, 160.0, 18.0);
                        ctx.set_fill_style_str(COLORS[c]);
                        ctx.fill_rect(ml + 8.0, ly - 12.0, 3.0, 18.0);
                        let _ = ctx.fill_text(
                            &format!(" CH {} : {:.4}", CH_NAMES[c], last.value),
                            ml + 14.0, ly
                        );
                        ly += 22.0;
                    }
                });
            }

            // Sample rate indicator
            let rate = sample_rate.get_untracked();
            if running.get_untracked() && rate > 0 {
                ctx.set_fill_style_str("#10b981");
                ctx.begin_path();
                let _ = ctx.arc(w - mr - 8.0, mt + 8.0, 4.0, 0.0, std::f64::consts::TAU);
                ctx.fill();
                ctx.set_font("9px 'JetBrains Mono', monospace");
                ctx.set_text_align("right");
                ctx.set_fill_style_str("rgba(148,163,184,0.6)");
                let _ = ctx.fill_text(&format!("{} SPS", rate), w - mr - 18.0, mt + 12.0);
            }
        }
    });

    view! {
        <div class="tab-content scope-tab">
            <div class="tab-desc">"Real-time oscilloscope. Polls ADC scope buckets (min/max/avg per 10ms window) and plots waveforms on canvas. Enable channels, set time window and Y-range."</div>
            <div class="scope-toolbar">
                {(0..4).map(|ch| {
                    view! {
                        <label class="scope-ch-toggle" style=format!("--ch-color: {}", COLORS[ch])>
                            <input type="checkbox"
                                prop:checked=move || channels_en.get()[ch]
                                on:change=move |e| {
                                    let checked: bool = e.target().unwrap().unchecked_into::<web_sys::HtmlInputElement>().checked();
                                    set_channels_en.update(|arr| arr[ch] = checked);
                                }
                            />
                            <span class="scope-ch-label" style=format!("color: {}", COLORS[ch])>{format!("CH {}", CH_NAMES[ch])}</span>
                        </label>
                    }
                }).collect::<Vec<_>>()}

                <div class="toolbar-divider"></div>

                <select class="dropdown dropdown-sm"
                    on:change=move |e| set_window_sec.set(event_target_value(&e).parse().unwrap_or(30.0))
                >
                    <option value="1">"1s"</option>
                    <option value="5">"5s"</option>
                    <option value="10">"10s"</option>
                    <option value="30" selected>"30s"</option>
                    <option value="60">"1min"</option>
                    <option value="120">"2min"</option>
                    <option value="300">"5min"</option>
                </select>

                <select class="dropdown dropdown-sm"
                    on:change=move |e| set_y_range.set(event_target_value(&e))
                >
                    <option value="auto" selected>"Y: Auto"</option>
                    <option value="0-12">"0–12V"</option>
                    <option value="pm12">"±12V"</option>
                    <option value="0-625m">"0–625mV"</option>
                    <option value="0-25m">"0–25mA"</option>
                </select>

                <div class="toolbar-divider"></div>

                <button class="scope-btn" class:scope-btn-active=move || running.get()
                    on:click=move |_| {
                        let new = !running.get_untracked();
                        set_running.set(new);
                        if !new {
                            // Stopped: clear sample rate
                            set_sample_rate.set(0);
                        }
                    }
                >
                    <span class="scope-btn-dot" class:running=move || running.get()></span>
                    {move || if running.get() {
                        format!("Stop ({}SPS)", sample_rate.get())
                    } else {
                        "Start".to_string()
                    }}
                </button>

                <button class="scope-btn" on:click=move |_| {
                    for ch in 0..4 { scope_data[ch].set(Vec::new()); }
                }>"Clear"</button>

                <div class="toolbar-divider"></div>

                // BBSC binary recording
                <button class="scope-btn" class:scope-btn-csv=move || recording.get()
                    on:click=move |_| {
                        if recording.get_untracked() {
                            // Stop recording
                            spawn_local(async move {
                                let result = invoke("stop_recording", wasm_bindgen::JsValue::NULL).await;
                                if let Ok(count) = serde_wasm_bindgen::from_value::<u64>(result) {
                                    log(&format!("Recording stopped: {} samples", count));
                                }
                            });
                            set_recording.set(false);
                        } else {
                            // Pick file and start BBSC recording
                            let ch_en = channels_en.get_untracked();
                            let mask: u8 = (0..4).fold(0u8, |m, i| if ch_en[i] { m | (1 << i) } else { m });
                            let ds = state.get_untracked();
                            let ranges: Vec<u8> = (0..4).map(|i| {
                                if i < ds.channels.len() { ds.channels[i].adc_range } else { 0 }
                            }).collect();
                            let rate = sample_rate.get_untracked();

                            spawn_local(async move {
                                let result = invoke("pick_save_file", wasm_bindgen::JsValue::NULL).await;
                                if let Ok(opt) = serde_wasm_bindgen::from_value::<Option<String>>(result) {
                                    if let Some(path) = opt {
                                        if !path.is_empty() {
                                            let bbsc_path = if path.ends_with(".csv") {
                                                path.replace(".csv", ".bbsc")
                                            } else if !path.ends_with(".bbsc") {
                                                format!("{}.bbsc", path)
                                            } else { path };

                                            let args = serde_wasm_bindgen::to_value(
                                                &serde_json::json!({
                                                    "path": bbsc_path,
                                                    "channelMask": mask,
                                                    "adcRanges": ranges,
                                                    "sampleRate": rate.max(20)
                                                })
                                            ).unwrap();
                                            let _ = invoke("start_recording", args).await;
                                            set_csv_path.set(bbsc_path);
                                            set_recording.set(true);
                                        }
                                    }
                                }
                            });
                        }
                    }
                >
                    {move || if recording.get() { "Stop Recording" } else { "Record" }}
                </button>

                // Export BBSC → CSV
                <button class="scope-btn"
                    on:click=move |_| {
                        spawn_local(async move {
                            // Pick BBSC file to open
                            let result = invoke("pick_save_file", wasm_bindgen::JsValue::NULL).await;
                            if let Ok(Some(export_csv)) = serde_wasm_bindgen::from_value::<Option<String>>(result) {
                                if export_csv.is_empty() { return; }
                                let last_rec = csv_path.get_untracked();
                                let bbsc = if !last_rec.is_empty() && last_rec.ends_with(".bbsc") {
                                    last_rec
                                } else {
                                    log("No BBSC file to export. Record first.");
                                    return;
                                };
                                let csv = if export_csv.ends_with(".bbsc") {
                                    export_csv.replace(".bbsc", ".csv")
                                } else { export_csv };

                                let args = serde_wasm_bindgen::to_value(
                                    &serde_json::json!({ "bbscPath": bbsc, "csvPath": csv })
                                ).unwrap();
                                let result = invoke("export_bbsc_to_csv", args).await;
                                if let Ok(count) = serde_wasm_bindgen::from_value::<u64>(result) {
                                    log(&format!("Exported {} samples to CSV", count));
                                }
                            }
                        });
                    }
                >"Export CSV"</button>

                {move || {
                    let path = csv_path.get();
                    if !path.is_empty() && recording.get() {
                        Some(view! { <span class="csv-path">"→ "{path}</span> })
                    } else { None }
                }}
            </div>

            <div class="scope-canvas-wrap">
                <canvas node_ref=canvas_ref class="scope-canvas"
                    on:click=move |e: web_sys::MouseEvent| {
                        // Place or move cursor at click X position (relative to canvas)
                        let target = e.target().unwrap();
                        let canvas: HtmlCanvasElement = target.unchecked_into();
                        let rect = canvas.get_bounding_client_rect();
                        let x = e.client_x() as f64 - rect.left();
                        set_cursor_x.set(Some(x));
                    }
                    on:contextmenu=move |e: web_sys::MouseEvent| {
                        e.prevent_default();
                        set_cursor_x.set(None);
                    }
                    on:keydown=move |e: web_sys::KeyboardEvent| {
                        if e.key() == "Escape" {
                            set_cursor_x.set(None);
                        }
                    }
                    tabindex="0"
                ></canvas>
            </div>
        </div>
    }
}

async fn sleep_ms(ms: u32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window().unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms as i32)
            .unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(promise).await.unwrap();
}
