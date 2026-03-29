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

/// Convert 24-bit raw ADC code to voltage (0-12V unipolar range)
fn raw_to_voltage(raw: u32, range: u8) -> f32 {
    let code = raw as f32;
    let full_scale = 16777216.0f32; // 2^24
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

    let scope_data: [RwSignal<Vec<ScopePoint>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    // Track sample count for rate display
    let sample_counter = RwSignal::new(0u32);
    let last_rate_check = RwSignal::new(js_sys::Date::now());

    // Listen for ADC stream events
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            // Parse the Tauri event wrapper
            #[derive(Deserialize)]
            struct TauriEvt { payload: Vec<u8> }
            let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvt>(event) else { return };

            if !running.get_untracked() { return; }

            let Some((mask, samples)) = parse_adc_event(&evt.payload) else { return };
            let now = js_sys::Date::now();
            let ch_en = channels_en.get_untracked();

            // Get ADC ranges from device state for conversion
            let ds = state.get_untracked();
            let ranges: [u8; 4] = std::array::from_fn(|i| {
                if i < ds.channels.len() { ds.channels[i].adc_range } else { 0 }
            });

            // Determine time step between samples (spread evenly across batch)
            let batch_dt = if samples.len() > 1 { 2.0 / samples.len() as f64 } else { 0.0 };

            for (si, raw) in samples.iter().enumerate() {
                let t = now - (samples.len() - 1 - si) as f64 * batch_dt;
                for ch in 0..4 {
                    if ch_en[ch] && (mask & (1 << ch)) != 0 {
                        let v = raw_to_voltage(raw[ch], ranges[ch]);
                        scope_data[ch].update(|data| {
                            data.push(ScopePoint { time_ms: t, value: v });
                        });
                    }
                }
            }

            // Update sample counter
            sample_counter.update(|c| *c += samples.len() as u32);

            // Trim old data (keep 1.5x window + cap at MAX_POINTS)
            let win = window_sec.get_untracked();
            let cutoff = now - win * 1000.0 * 1.5;
            for ch in 0..4 {
                scope_data[ch].update(|data| {
                    data.retain(|p| p.time_ms > cutoff);
                    if data.len() > MAX_POINTS { data.drain(0..data.len() - MAX_POINTS); }
                });
            }

            // Binary recording: forward raw payload to BBSC writer
            if recording.get_untracked() {
                let payload_clone = evt.payload.clone();
                let args = serde_wasm_bindgen::to_value(
                    &serde_json::json!({ "rawPayload": payload_clone })
                ).unwrap();
                spawn_local(async move {
                    let _ = invoke("append_recording_data", args).await;
                });
            }
        });
        listen("adc-stream", &closure).await;
        closure.forget();
    });

    // Rate counter: update every second
    spawn_local(async move {
        loop {
            sleep_ms(1000).await;
            let count = sample_counter.get_untracked();
            sample_counter.set(0);
            set_sample_rate.set(count);
        }
    });

    // Start/stop ADC stream when running changes
    Effect::new(move || {
        let is_running = running.get();
        let ch_en = channels_en.get();
        if is_running {
            let mask: u8 = (0..4).fold(0u8, |m, i| if ch_en[i] { m | (1 << i) } else { m });
            if mask == 0 { return; }
            // Start ADC stream with divider=1 (full rate)
            #[derive(serde::Serialize)]
            #[serde(rename_all = "camelCase")]
            struct StartArgs { channel_mask: u8, divider: u8 }
            let args = serde_wasm_bindgen::to_value(&StartArgs { channel_mask: mask, divider: 1 }).unwrap();
            spawn_local(async move {
                let _ = invoke("start_adc_stream", args).await;
            });
        } else {
            spawn_local(async move {
                let _ = invoke("stop_adc_stream", wasm_bindgen::JsValue::NULL).await;
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
                <canvas node_ref=canvas_ref class="scope-canvas"></canvas>
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
