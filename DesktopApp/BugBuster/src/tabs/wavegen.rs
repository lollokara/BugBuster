use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

#[component]
pub fn WavegenTab() -> impl IntoView {
    let (channel, set_channel) = signal(0u8);
    let (waveform, set_waveform) = signal("sine".to_string());
    let (mode, set_mode) = signal("voltage".to_string()); // "voltage" or "current"
    let (freq_hz, set_freq_hz) = signal(1.0f64);
    let (amplitude, set_amplitude) = signal(5.0f64);
    let (offset, set_offset) = signal(0.0f64);
    let (running, set_running) = signal(false);

    let (edit_freq, set_edit_freq) = signal("1.0".to_string());
    let (edit_amp, set_edit_amp) = signal("5.0".to_string());
    let (edit_off, set_edit_off) = signal("0.0".to_string());

    let preview_ref = NodeRef::<leptos::html::Canvas>::new();

    let unit = move || if mode.get() == "current" { "mA" } else { "V" };
    let max_amp = move || if mode.get() == "current" { 25.0 } else { 12.0 };

    // Draw preview
    Effect::new(move || {
        let wf = waveform.get();
        let amp = amplitude.get();
        let off = offset.get();
        let Some(canvas) = preview_ref.get() else { return };
        let canvas: HtmlCanvasElement = canvas.into();
        let dpr = web_sys::window().unwrap().device_pixel_ratio();
        let rect = canvas.get_bounding_client_rect();
        let w = rect.width();
        let h = rect.height();
        if w < 10.0 { return; }
        canvas.set_width((w * dpr) as u32);
        canvas.set_height((h * dpr) as u32);

        let ctx: CanvasRenderingContext2d = canvas
            .get_context("2d").unwrap().unwrap()
            .dyn_into().unwrap();
        ctx.scale(dpr, dpr).unwrap();

        ctx.set_fill_style_str("#0f1729");
        ctx.fill_rect(0.0, 0.0, w, h);

        // Zero line
        ctx.set_stroke_style_str("rgba(148,163,184,0.12)");
        ctx.set_line_width(1.0);
        ctx.begin_path();
        ctx.move_to(0.0, h / 2.0);
        ctx.line_to(w, h / 2.0);
        ctx.stroke();

        let y_range = (amp.abs() + off.abs()) * 1.3;
        let y_range = y_range.max(0.1);

        let color = if mode.get() == "current" { "#a855f7" } else { "#3b82f6" };
        ctx.set_stroke_style_str(color);
        ctx.set_line_width(2.0);
        ctx.begin_path();

        let steps = w as usize;
        for px in 0..steps {
            let t = px as f64 / steps as f64;
            let v = match wf.as_str() {
                "sine" => off + amp * (t * std::f64::consts::TAU).sin(),
                "square" => off + if t < 0.5 { amp } else { -amp },
                "triangle" => {
                    let v = if t < 0.25 { t * 4.0 }
                        else if t < 0.75 { 2.0 - t * 4.0 }
                        else { t * 4.0 - 4.0 };
                    off + amp * v
                }
                "sawtooth" => off + amp * (2.0 * t - 1.0),
                _ => 0.0,
            };
            let y = h / 2.0 - (v / y_range) * (h / 2.0 - 10.0);
            if px == 0 { ctx.move_to(px as f64, y); }
            else { ctx.line_to(px as f64, y); }
        }
        ctx.stroke();

        ctx.set_fill_style_str("#64748b");
        ctx.set_font("10px monospace");
        ctx.set_text_align("left");
        let u = if mode.get() == "current" { "mA" } else { "V" };
        let _ = ctx.fill_text(&format!("+{:.1}{}", amp + off, u), 4.0, 14.0);
        let _ = ctx.fill_text(&format!("{:.1}{}", -amp + off, u), 4.0, h - 4.0);
    });

    let commit_freq = move || {
        if let Ok(v) = edit_freq.get_untracked().parse::<f64>() { set_freq_hz.set(v.clamp(0.01, 1000.0)); }
        set_edit_freq.set(format!("{:.1}", freq_hz.get_untracked()));
    };
    let commit_amp = move || {
        if let Ok(v) = edit_amp.get_untracked().parse::<f64>() { set_amplitude.set(v.clamp(0.0, max_amp())); }
        set_edit_amp.set(format!("{:.1}", amplitude.get_untracked()));
    };
    let commit_off = move || {
        if let Ok(v) = edit_off.get_untracked().parse::<f64>() { set_offset.set(v.clamp(-max_amp(), max_amp())); }
        set_edit_off.set(format!("{:.1}", offset.get_untracked()));
    };

    let toggle = move |_: leptos::ev::MouseEvent| {
        let new_state = !running.get_untracked();
        set_running.set(new_state);
        if new_state {
            #[derive(Serialize)]
            #[serde(rename_all = "camelCase")]
            struct Args { channel: u8, waveform: String, freq_hz: f64, amplitude: f64, offset: f64, mode: String }
            let args = serde_wasm_bindgen::to_value(&Args {
                channel: channel.get_untracked(),
                waveform: waveform.get_untracked(),
                freq_hz: freq_hz.get_untracked(),
                amplitude: amplitude.get_untracked(),
                offset: offset.get_untracked(),
                mode: mode.get_untracked(),
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
                            <select class="dropdown" on:change=move |e| { set_channel.set(event_target_value(&e).parse().unwrap_or(0)); }>
                                {(0..4).map(|i| view! { <option value=i.to_string()>{format!("CH {}", CH_NAMES[i])}</option> }).collect::<Vec<_>>()}
                            </select>
                        </div>
                        <div class="config-row">
                            <label>"Output"</label>
                            <label class="toggle-wrap">
                                <span class="toggle-off-label">"Voltage"</span>
                                <div class="toggle" class:active={move || mode.get() == "current"}
                                    on:click=move |_| {
                                        let new = if mode.get_untracked() == "voltage" { "current" } else { "voltage" };
                                        set_mode.set(new.to_string());
                                        // Reset amplitude to safe value for new mode
                                        let m = if new == "current" { 12.5 } else { 5.0 };
                                        set_amplitude.set(m);
                                        set_edit_amp.set(format!("{:.1}", m));
                                    }
                                ><div class="toggle-thumb"></div></div>
                                <span class="toggle-on-label">"Current"</span>
                            </label>
                        </div>
                        <div class="config-row">
                            <label>"Waveform"</label>
                            <select class="dropdown" on:change=move |e| set_waveform.set(event_target_value(&e))>
                                <option value="sine">"Sine"</option>
                                <option value="square">"Square"</option>
                                <option value="triangle">"Triangle"</option>
                                <option value="sawtooth">"Sawtooth"</option>
                            </select>
                        </div>
                        <div class="config-row">
                            <label>"Freq (Hz)"</label>
                            <input type="text" class="number-input"
                                prop:value=move || edit_freq.get()
                                on:input=move |e| set_edit_freq.set(event_target_value(&e))
                                on:blur=move |_| commit_freq()
                                on:keydown=move |e: leptos::ev::KeyboardEvent| { if e.key() == "Enter" { commit_freq(); } }
                            />
                        </div>
                        <div class="config-row">
                            <label>{move || format!("Amp ({})", unit())}</label>
                            <input type="text" class="number-input"
                                prop:value=move || edit_amp.get()
                                on:input=move |e| set_edit_amp.set(event_target_value(&e))
                                on:blur=move |_| commit_amp()
                                on:keydown=move |e: leptos::ev::KeyboardEvent| { if e.key() == "Enter" { commit_amp(); } }
                            />
                        </div>
                        <div class="config-row">
                            <label>{move || format!("Offset ({})", unit())}</label>
                            <input type="text" class="number-input"
                                prop:value=move || edit_off.get()
                                on:input=move |e| set_edit_off.set(event_target_value(&e))
                                on:blur=move |_| commit_off()
                                on:keydown=move |e: leptos::ev::KeyboardEvent| { if e.key() == "Enter" { commit_off(); } }
                            />
                        </div>
                        <button class="btn wavegen-start-btn" class:wavegen-running=move || running.get() on:click=toggle>
                            <span class="scope-btn-dot" class:running=move || running.get()></span>
                            {move || if running.get() { "Stop Generator" } else { "Start Generator" }}
                        </button>
                        <p class="wavegen-hint">
                            {move || format!("Will set CH {} to {} mode on start",
                                CH_NAMES[channel.get() as usize],
                                if mode.get() == "current" { "IOUT" } else { "VOUT" }
                            )}
                        </p>
                    </div>
                </div>

                <div class="card wavegen-preview">
                    <div class="card-header"><span>"Preview"</span>
                        <span class="badge-hex">{move || format!("{} @ {:.1} Hz", waveform.get(), freq_hz.get())}</span>
                    </div>
                    <div class="card-body" style="padding: 0;">
                        <canvas node_ref=preview_ref class="wavegen-canvas"></canvas>
                    </div>
                </div>
            </div>
        </div>
    }
}
