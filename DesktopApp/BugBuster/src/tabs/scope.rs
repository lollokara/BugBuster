use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

const COLORS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];
const MAX_POINTS: usize = 10000;

#[derive(Clone)]
struct ScopePoint {
    time_ms: f64,
    value: f32,
}

#[component]
pub fn ScopeTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (running, set_running) = signal(false);
    let (channels_en, set_channels_en) = signal([true, false, false, false]);
    let (window_sec, set_window_sec) = signal(30.0f64);
    let (y_range, set_y_range) = signal("auto".to_string());
    let (recording, set_recording) = signal(false);
    let (csv_path, set_csv_path) = signal(String::new());

    let scope_data: [RwSignal<Vec<ScopePoint>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    // Accumulate data and append to CSV if recording
    Effect::new(move || {
        if !running.get() { return; }
        let ds = state.get();
        let now = js_sys::Date::now();
        let ch_en = channels_en.get();
        for i in 0..4 {
            if ch_en[i] && i < ds.channels.len() {
                scope_data[i].update(|data| {
                    data.push(ScopePoint { time_ms: now, value: ds.channels[i].adc_value });
                    let cutoff = now - window_sec.get_untracked() * 1000.0 * 1.5;
                    data.retain(|p| p.time_ms > cutoff);
                    if data.len() > MAX_POINTS { data.drain(0..data.len() - MAX_POINTS); }
                });
            }
        }

        // Append CSV row if recording
        if recording.get_untracked() {
            let mut ch_values = Vec::with_capacity(4);
            for i in 0..4 {
                if i < ds.channels.len() {
                    ch_values.push(ds.channels[i].adc_value);
                } else {
                    ch_values.push(0.0);
                }
            }
            let args = serde_wasm_bindgen::to_value(
                &serde_json::json!({ "timestampMs": now, "chValues": ch_values })
            ).unwrap();
            spawn_local(async move {
                let _ = invoke("append_csv_data", args).await;
            });
        }
    });

    // Render loop
    spawn_local(async move {
        loop {
            sleep_ms(33).await;

            let Some(canvas) = canvas_ref.get() else { continue };
            let canvas: HtmlCanvasElement = canvas.into();
            let dpr = web_sys::window().unwrap().device_pixel_ratio();
            let rect = canvas.get_bounding_client_rect();
            let w = rect.width();
            let h = rect.height();
            if w < 10.0 || h < 10.0 { continue; }
            canvas.set_width((w * dpr) as u32);
            canvas.set_height((h * dpr) as u32);

            let ctx: CanvasRenderingContext2d = canvas
                .get_context("2d").unwrap().unwrap()
                .dyn_into().unwrap();
            ctx.scale(dpr, dpr).unwrap();

            // Background
            ctx.set_fill_style_str("#0f1729");
            ctx.fill_rect(0.0, 0.0, w, h);

            let now = js_sys::Date::now();
            let win_ms = window_sec.get() * 1000.0;
            let t_start = now - win_ms;
            let ch_en = channels_en.get();

            // Y range
            let yr = y_range.get();
            let (y_min, y_max) = if yr == "auto" {
                let mut mn = f64::MAX;
                let mut mx = f64::MIN;
                for i in 0..4 {
                    if ch_en[i] {
                        let data = scope_data[i].get();
                        for p in &data {
                            if p.time_ms >= t_start {
                                let v = p.value as f64;
                                if v < mn { mn = v; }
                                if v > mx { mx = v; }
                            }
                        }
                    }
                }
                if mn == f64::MAX { (0.0, 1.0) }
                else {
                    let margin = (mx - mn).max(0.01) * 0.15;
                    (mn - margin, mx + margin)
                }
            } else if yr == "0-12" { (0.0, 12.0) }
            else if yr == "pm12" { (-12.0, 12.0) }
            else if yr == "0-625m" { (0.0, 0.625) }
            else if yr == "0-25m" { (0.0, 25.0) }
            else { (0.0, 12.0) };

            let y_span = (y_max - y_min).max(0.001);
            let ml = 65.0; let mr = 15.0; let mt = 15.0; let mb = 30.0;
            let pw = w - ml - mr;
            let ph = h - mt - mb;

            // Grid lines
            ctx.set_line_width(1.0);
            let y_steps = 6;
            for j in 0..=y_steps {
                let frac = j as f64 / y_steps as f64;
                let y = mt + frac * ph;
                let val = y_max - frac * y_span;

                ctx.set_stroke_style_str(if j == y_steps / 2 { "rgba(148,163,184,0.15)" } else { "rgba(148,163,184,0.07)" });
                ctx.begin_path();
                ctx.move_to(ml, y);
                ctx.line_to(w - mr, y);
                ctx.stroke();

                ctx.set_fill_style_str("#64748b");
                ctx.set_font("11px monospace");
                ctx.set_text_align("right");
                let _ = ctx.fill_text(&format!("{:.2}", val), ml - 8.0, y + 4.0);
            }

            let x_steps = 8;
            for j in 0..=x_steps {
                let frac = j as f64 / x_steps as f64;
                let x = ml + frac * pw;

                ctx.set_stroke_style_str("rgba(148,163,184,0.05)");
                ctx.begin_path();
                ctx.move_to(x, mt);
                ctx.line_to(x, h - mb);
                ctx.stroke();

                let secs_ago = (1.0 - frac) * window_sec.get();
                ctx.set_fill_style_str("#64748b");
                ctx.set_text_align("center");
                ctx.set_font("10px monospace");
                let _ = ctx.fill_text(&format!("-{:.0}s", secs_ago), x, h - mb + 16.0);
            }

            // Plot border
            ctx.set_stroke_style_str("rgba(148,163,184,0.2)");
            ctx.set_line_width(1.0);
            ctx.stroke_rect(ml, mt, pw, ph);

            // Plot channels
            for ch in 0..4 {
                if !ch_en[ch] { continue; }
                let data = scope_data[ch].get();
                if data.len() < 2 { continue; }

                ctx.set_stroke_style_str(COLORS[ch]);
                ctx.set_line_width(1.5);
                ctx.begin_path();
                let mut started = false;
                for p in &data {
                    if p.time_ms < t_start { continue; }
                    let x = ml + ((p.time_ms - t_start) / win_ms) * pw;
                    let y = mt + ((y_max - p.value as f64) / y_span) * ph;
                    let y = y.clamp(mt, mt + ph);
                    if !started { ctx.move_to(x, y); started = true; }
                    else { ctx.line_to(x, y); }
                }
                ctx.stroke();
            }

            // Current values legend
            ctx.set_font("12px 'Inter', monospace");
            ctx.set_text_align("left");
            let mut ly = mt + 16.0;
            for ch in 0..4 {
                if !ch_en[ch] { continue; }
                let data = scope_data[ch].get();
                if let Some(last) = data.last() {
                    // Background pill
                    ctx.set_fill_style_str("rgba(15,23,42,0.8)");
                    ctx.fill_rect(ml + 8.0, ly - 12.0, 140.0, 18.0);
                    ctx.set_fill_style_str(COLORS[ch]);
                    ctx.fill_rect(ml + 8.0, ly - 12.0, 3.0, 18.0);
                    let _ = ctx.fill_text(
                        &format!(" CH {} : {:.4}", CH_NAMES[ch], last.value),
                        ml + 14.0, ly
                    );
                    ly += 22.0;
                }
            }

            // Status indicator
            if running.get() {
                ctx.set_fill_style_str("#10b981");
                ctx.begin_path();
                ctx.arc(w - mr - 8.0, mt + 8.0, 4.0, 0.0, std::f64::consts::TAU).unwrap();
                ctx.fill();
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
                    on:click=move |_| set_running.update(|r| *r = !*r)
                >
                    <span class="scope-btn-dot" class:running=move || running.get()></span>
                    {move || if running.get() { "Recording" } else { "Start" }}
                </button>

                <button class="scope-btn" on:click=move |_| {
                    for ch in 0..4 { scope_data[ch].set(Vec::new()); }
                }>"Clear"</button>

                <div class="toolbar-divider"></div>

                <button class="scope-btn" class:scope-btn-csv=move || recording.get()
                    on:click=move |_| {
                        if recording.get_untracked() {
                            // Stop recording: close the CSV file on backend
                            spawn_local(async move {
                                let _ = invoke("stop_csv_recording", wasm_bindgen::JsValue::NULL).await;
                            });
                            set_recording.set(false);
                            set_csv_path.set(String::new());
                        } else {
                            // Ask user for save location via our backend command
                            spawn_local(async move {
                                let result = invoke("pick_save_file", wasm_bindgen::JsValue::NULL).await;
                                log(&format!("pick_save_file result: {:?}", result));
                                // Result is Option<String> serialized
                                if let Ok(opt) = serde_wasm_bindgen::from_value::<Option<String>>(result) {
                                    if let Some(path) = opt {
                                        if !path.is_empty() {
                                            // Open CSV file and write header on backend
                                            let args = serde_wasm_bindgen::to_value(
                                                &serde_json::json!({ "path": path })
                                            ).unwrap();
                                            let start_result = invoke("start_csv_recording", args).await;
                                            log(&format!("start_csv_recording result: {:?}", start_result));
                                            set_csv_path.set(path);
                                            set_recording.set(true);
                                        }
                                    }
                                }
                            });
                        }
                    }
                >
                    {move || if recording.get() { "⬛ Stop Recording" } else { "⏺ Record CSV" }}
                </button>
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
