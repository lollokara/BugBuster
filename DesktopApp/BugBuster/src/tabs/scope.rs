use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

const COLORS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];
const COLORS_ALPHA: [&str; 4] = ["rgba(59,130,246,0.2)", "rgba(16,185,129,0.2)", "rgba(245,158,11,0.2)", "rgba(168,85,247,0.2)"];
const MAX_POINTS: usize = 8000;

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

    // Scope data: 4 channels of points
    let scope_data: [RwSignal<Vec<ScopePoint>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    // Accumulate data from device state polling
    Effect::new(move || {
        if !running.get() { return; }
        let ds = state.get();
        let now = js_sys::Date::now();
        let ch_en = channels_en.get();
        for i in 0..4 {
            if ch_en[i] && i < ds.channels.len() {
                scope_data[i].update(|data| {
                    data.push(ScopePoint { time_ms: now, value: ds.channels[i].adc_value });
                    // Trim old data
                    let cutoff = now - window_sec.get_untracked() * 1000.0;
                    data.retain(|p| p.time_ms > cutoff);
                    if data.len() > MAX_POINTS {
                        let excess = data.len() - MAX_POINTS;
                        data.drain(0..excess);
                    }
                });
            }
        }
    });

    // Canvas render loop
    let render = move || {
        let Some(canvas) = canvas_ref.get() else { return };
        let canvas: HtmlCanvasElement = canvas.into();
        let dpr = web_sys::window().unwrap().device_pixel_ratio();
        let rect = canvas.get_bounding_client_rect();
        let w = rect.width();
        let h = rect.height();
        canvas.set_width((w * dpr) as u32);
        canvas.set_height((h * dpr) as u32);

        let ctx: CanvasRenderingContext2d = canvas
            .get_context("2d").unwrap().unwrap()
            .dyn_into().unwrap();
        ctx.scale(dpr, dpr).unwrap();

        // Clear
        ctx.set_fill_style_str("#111827");
        ctx.fill_rect(0.0, 0.0, w, h);

        let now = js_sys::Date::now();
        let win_ms = window_sec.get() * 1000.0;
        let t_start = now - win_ms;
        let ch_en = channels_en.get();

        // Determine Y range
        let yr = y_range.get();
        let (y_min, y_max) = if yr == "auto" {
            let mut mn = f64::MAX;
            let mut mx = f64::MIN;
            for i in 0..4 {
                if ch_en[i] {
                    let data = scope_data[i].get();
                    for p in &data {
                        let v = p.value as f64;
                        if v < mn { mn = v; }
                        if v > mx { mx = v; }
                    }
                }
            }
            if mn == f64::MAX { (0.0, 12.0) }
            else {
                let margin = (mx - mn).max(0.1) * 0.1;
                (mn - margin, mx + margin)
            }
        } else if yr == "0-12" { (0.0, 12.0) }
        else if yr == "pm12" { (-12.0, 12.0) }
        else if yr == "0-625m" { (0.0, 0.625) }
        else if yr == "0-25m" { (0.0, 25.0) }
        else { (0.0, 12.0) };

        let y_span = (y_max - y_min).max(0.001);
        let margin_l = 60.0;
        let margin_r = 10.0;
        let margin_t = 10.0;
        let margin_b = 25.0;
        let plot_w = w - margin_l - margin_r;
        let plot_h = h - margin_t - margin_b;

        // Grid
        ctx.set_stroke_style_str("rgba(148,163,184,0.1)");
        ctx.set_line_width(1.0);
        let y_steps = 5;
        for j in 0..=y_steps {
            let y = margin_t + (j as f64 / y_steps as f64) * plot_h;
            ctx.begin_path();
            ctx.move_to(margin_l, y);
            ctx.line_to(w - margin_r, y);
            ctx.stroke();

            let val = y_max - (j as f64 / y_steps as f64) * y_span;
            ctx.set_fill_style_str("#64748b");
            ctx.set_font("10px monospace");
            ctx.set_text_align("right");
            let _ = ctx.fill_text(&format!("{:.2}", val), margin_l - 5.0, y + 3.0);
        }

        // X axis labels
        let x_steps = 6;
        for j in 0..=x_steps {
            let frac = j as f64 / x_steps as f64;
            let x = margin_l + frac * plot_w;
            let secs_ago = (1.0 - frac) * window_sec.get();
            ctx.set_text_align("center");
            let _ = ctx.fill_text(&format!("-{:.0}s", secs_ago), x, h - 5.0);
        }

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
                let x = margin_l + ((p.time_ms - t_start) / win_ms) * plot_w;
                let y = margin_t + ((y_max - p.value as f64) / y_span) * plot_h;
                if !started { ctx.move_to(x, y); started = true; }
                else { ctx.line_to(x, y); }
            }
            ctx.stroke();
        }

        // Current values overlay
        ctx.set_font("12px monospace");
        ctx.set_text_align("right");
        let mut y_offset = 20.0;
        for ch in 0..4 {
            if !ch_en[ch] { continue; }
            let data = scope_data[ch].get();
            if let Some(last) = data.last() {
                ctx.set_fill_style_str(COLORS[ch]);
                let _ = ctx.fill_text(
                    &format!("CH {}: {:.4}", CH_NAMES[ch], last.value),
                    w - 15.0, y_offset
                );
                y_offset += 16.0;
            }
        }
    };

    // Animation frame loop
    let render_clone = render.clone();
    spawn_local(async move {
        loop {
            gloo_timers_sleep(33).await; // ~30fps
            render_clone();
        }
    });

    view! {
        <div class="tab-content scope-tab">
            <div class="scope-toolbar">
                // Channel toggles
                {(0..4).map(|ch| {
                    view! {
                        <label class="scope-ch-toggle" style=format!("--ch-color: {}", COLORS[ch])>
                            <input type="checkbox"
                                prop:checked=move || channels_en.get()[ch]
                                on:change=move |e| {
                                    let checked: bool = event_target_checked(&e);
                                    set_channels_en.update(|arr| arr[ch] = checked);
                                }
                            />
                            <span>{format!("CH {}", CH_NAMES[ch])}</span>
                        </label>
                    }
                }).collect::<Vec<_>>()}

                <span class="toolbar-sep">"|"</span>

                <select class="dropdown dropdown-sm"
                    on:change=move |e| set_window_sec.set(event_target_value(&e).parse().unwrap_or(30.0))
                >
                    <option value="10">"10s"</option>
                    <option value="30" selected>"30s"</option>
                    <option value="60">"60s"</option>
                    <option value="120">"2min"</option>
                    <option value="300">"5min"</option>
                </select>

                <select class="dropdown dropdown-sm"
                    on:change=move |e| set_y_range.set(event_target_value(&e))
                >
                    <option value="auto" selected>"Y: Auto"</option>
                    <option value="0-12">"0-12V"</option>
                    <option value="pm12">"±12V"</option>
                    <option value="0-625m">"0-625mV"</option>
                    <option value="0-25m">"0-25mA"</option>
                </select>

                <span class="toolbar-sep">"|"</span>

                <button class="btn btn-sm" class:btn-primary=move || !running.get()
                    class:btn-danger=move || running.get()
                    on:click=move |_| set_running.update(|r| *r = !*r)
                >
                    {move || if running.get() { "Pause" } else { "Run" }}
                </button>

                <button class="btn btn-sm" on:click=move |_| {
                    for ch in 0..4 { scope_data[ch].set(Vec::new()); }
                }>"Clear"</button>

                <span class="toolbar-sep">"|"</span>

                <button class="btn btn-sm" class:btn-primary=move || !recording.get()
                    class:btn-danger=move || recording.get()
                    on:click=move |_| set_recording.update(|r| *r = !*r)
                >
                    {move || if recording.get() { "Stop CSV" } else { "Record CSV" }}
                </button>
            </div>

            <canvas node_ref=canvas_ref class="scope-canvas"></canvas>
        </div>
    }
}

fn event_target_checked(ev: &leptos::ev::Event) -> bool {
    use wasm_bindgen::JsCast;
    ev.target().unwrap().unchecked_into::<web_sys::HtmlInputElement>().checked()
}

// Simple async sleep for render loop
async fn gloo_timers_sleep(ms: u32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window().unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms as i32)
            .unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(promise).await.unwrap();
}
