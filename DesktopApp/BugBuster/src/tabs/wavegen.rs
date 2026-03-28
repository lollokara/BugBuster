use leptos::prelude::*;
use serde::Serialize;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

#[component]
pub fn WavegenTab() -> impl IntoView {
    let (channel, set_channel) = signal(0u8);
    let (waveform, set_waveform) = signal("sine".to_string());
    let (freq_hz, set_freq_hz) = signal(1.0f64);
    let (amplitude, set_amplitude) = signal(5.0f64);
    let (offset, set_offset) = signal(0.0f64);
    let (running, set_running) = signal(false);

    let preview_ref = NodeRef::<leptos::html::Canvas>::new();

    // Draw waveform preview
    Effect::new(move || {
        let wf = waveform.get();
        let amp = amplitude.get();
        let off = offset.get();
        let Some(canvas) = preview_ref.get() else { return };
        let canvas: HtmlCanvasElement = canvas.into();
        let w = 400.0;
        let h = 150.0;
        canvas.set_width(400);
        canvas.set_height(150);

        let ctx: CanvasRenderingContext2d = canvas
            .get_context("2d").unwrap().unwrap()
            .dyn_into().unwrap();

        ctx.set_fill_style_str("#111827");
        ctx.fill_rect(0.0, 0.0, w, h);

        // Grid
        ctx.set_stroke_style_str("rgba(148,163,184,0.15)");
        ctx.set_line_width(1.0);
        ctx.begin_path();
        ctx.move_to(0.0, h / 2.0);
        ctx.line_to(w, h / 2.0);
        ctx.stroke();

        // Waveform
        ctx.set_stroke_style_str("#3b82f6");
        ctx.set_line_width(2.0);
        ctx.begin_path();

        let y_range = amp * 2.2;
        for px in 0..400 {
            let t = px as f64 / 400.0; // 0..1 = one period
            let v = match wf.as_str() {
                "sine" => off + amp * (t * std::f64::consts::TAU).sin(),
                "square" => off + if t < 0.5 { amp } else { -amp },
                "triangle" => off + amp * (1.0 - 4.0 * (t - 0.25).abs().fract().min(0.5)),
                "sawtooth" => off + amp * (2.0 * t - 1.0),
                _ => 0.0,
            };
            let y = h / 2.0 - (v / y_range) * (h - 20.0);
            if px == 0 { ctx.move_to(px as f64, y); }
            else { ctx.line_to(px as f64, y); }
        }
        ctx.stroke();
    });

    let toggle = move |_: leptos::ev::MouseEvent| {
        let new_state = !running.get_untracked();
        set_running.set(new_state);
        if new_state {
            #[derive(Serialize)]
            struct Args { channel: u8, waveform: String, freq_hz: f64, amplitude: f64, offset: f64 }
            let args = serde_wasm_bindgen::to_value(&Args {
                channel: channel.get_untracked(),
                waveform: waveform.get_untracked(),
                freq_hz: freq_hz.get_untracked(),
                amplitude: amplitude.get_untracked(),
                offset: offset.get_untracked(),
            }).unwrap();
            invoke_void("start_wavegen", args);
        } else {
            invoke_void("stop_wavegen", wasm_bindgen::JsValue::NULL);
        }
    };

    view! {
        <div class="tab-content">
            <div class="wavegen-layout">
                <div class="card wavegen-controls">
                    <div class="card-header"><span>"Waveform Generator"</span></div>
                    <div class="card-body">
                        <div class="config-row">
                            <label>"Channel"</label>
                            <select class="dropdown"
                                on:change=move |e| {
                                    set_channel.set(event_target_value(&e).parse().unwrap_or(0));
                                }
                            >
                                {(0..4).map(|i| {
                                    view! { <option value=i.to_string()>{format!("CH {}", CH_NAMES[i])}</option> }
                                }).collect::<Vec<_>>()}
                            </select>
                        </div>
                        <div class="config-row">
                            <label>"Waveform"</label>
                            <select class="dropdown"
                                on:change=move |e| set_waveform.set(event_target_value(&e))
                            >
                                <option value="sine">"Sine"</option>
                                <option value="square">"Square"</option>
                                <option value="triangle">"Triangle"</option>
                                <option value="sawtooth">"Sawtooth"</option>
                            </select>
                        </div>
                        <div class="config-row">
                            <label>"Frequency (Hz)"</label>
                            <input type="number" class="number-input"
                                min="0.1" max="1000" step="0.1"
                                prop:value=move || format!("{:.1}", freq_hz.get())
                                on:input=move |e| {
                                    if let Ok(v) = event_target_value(&e).parse() { set_freq_hz.set(v); }
                                }
                            />
                        </div>
                        <div class="config-row">
                            <label>"Amplitude (V)"</label>
                            <input type="number" class="number-input"
                                min="0" max="12" step="0.1"
                                prop:value=move || format!("{:.1}", amplitude.get())
                                on:input=move |e| {
                                    if let Ok(v) = event_target_value(&e).parse() { set_amplitude.set(v); }
                                }
                            />
                        </div>
                        <div class="config-row">
                            <label>"Offset (V)"</label>
                            <input type="number" class="number-input"
                                min="-12" max="12" step="0.1"
                                prop:value=move || format!("{:.1}", offset.get())
                                on:input=move |e| {
                                    if let Ok(v) = event_target_value(&e).parse() { set_offset.set(v); }
                                }
                            />
                        </div>
                        <button class="btn" class:btn-primary=move || !running.get()
                            class:btn-danger=move || running.get()
                            on:click=toggle
                            style="width: 100%; margin-top: 12px;"
                        >
                            {move || if running.get() { "Stop" } else { "Start" }}
                        </button>
                    </div>
                </div>

                <div class="card wavegen-preview">
                    <div class="card-header"><span>"Preview (1 period)"</span></div>
                    <div class="card-body">
                        <canvas node_ref=preview_ref class="wavegen-canvas" width="400" height="150"></canvas>
                    </div>
                </div>
            </div>
        </div>
    }
}
