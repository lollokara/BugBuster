use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

/// Decode channel_alert bits (per ad74416h.h:294-300) for UI display.
const CHANNEL_ALERT_BITS: &[(u16, &str)] = &[
    (0x0001, "DIN_SC"),
    (0x0002, "DIN_OC"),
    (0x0004, "DO_SC"),
    (0x0008, "DO_TIMEOUT"),
    (0x0010, "AIO_SC"),
    (0x0020, "AIO_OC"),
    (0x0040, "VIOUT_SHUTDOWN"),
];

fn decode_channel_alert(bits: u16) -> String {
    let names: Vec<&str> = CHANNEL_ALERT_BITS
        .iter()
        .filter_map(|(b, n)| if bits & b != 0 { Some(*n) } else { None })
        .collect();
    if names.is_empty() { format!("0x{:04X}", bits) } else { names.join(",") }
}

/// Hoisted Wavegen UI state — lives in app-level context so signals survive
/// tab switches (Bug Issue 5). Use `use_context::<WavegenUiState>()` inside
/// `WavegenTab`.
#[derive(Clone, Copy)]
pub struct WavegenUiState {
    pub channel: RwSignal<u8>,
    pub waveform: RwSignal<String>,
    pub mode: RwSignal<String>, // "voltage" or "current"
    pub freq_hz: RwSignal<f64>,
    pub amplitude: RwSignal<f64>,
    pub offset: RwSignal<f64>,
    pub running: RwSignal<bool>,
    pub sending: RwSignal<bool>,
    pub edit_freq: RwSignal<String>,
    pub edit_amp: RwSignal<String>,
    pub edit_off: RwSignal<String>,
}

impl WavegenUiState {
    pub fn new() -> Self {
        Self {
            channel: RwSignal::new(0u8),
            waveform: RwSignal::new("sine".to_string()),
            mode: RwSignal::new("voltage".to_string()),
            freq_hz: RwSignal::new(1.0f64),
            amplitude: RwSignal::new(5.0f64),
            offset: RwSignal::new(0.0f64),
            running: RwSignal::new(false),
            sending: RwSignal::new(false),
            edit_freq: RwSignal::new("1.0".to_string()),
            edit_amp: RwSignal::new("5.0".to_string()),
            edit_off: RwSignal::new("0.0".to_string()),
        }
    }
}

#[component]
pub fn WavegenTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let ui = use_context::<WavegenUiState>()
        .expect("WavegenUiState not provided — call provide_context(WavegenUiState::new()) in App");

    let channel = ui.channel.read_only();
    let set_channel = ui.channel.write_only();
    let waveform = ui.waveform.read_only();
    let set_waveform = ui.waveform.write_only();
    let mode = ui.mode.read_only();
    let set_mode = ui.mode.write_only();
    let freq_hz = ui.freq_hz.read_only();
    let set_freq_hz = ui.freq_hz.write_only();
    let amplitude = ui.amplitude.read_only();
    let set_amplitude = ui.amplitude.write_only();
    let offset = ui.offset.read_only();
    let set_offset = ui.offset.write_only();
    let running = ui.running.read_only();
    let set_running = ui.running.write_only();
    let sending = ui.sending.read_only();
    let set_sending = ui.sending.write_only();

    let edit_freq = ui.edit_freq.read_only();
    let set_edit_freq = ui.edit_freq.write_only();
    let edit_amp = ui.edit_amp.read_only();
    let set_edit_amp = ui.edit_amp.write_only();
    let edit_off = ui.edit_off.read_only();
    let set_edit_off = ui.edit_off.write_only();

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
        if let Ok(v) = edit_freq.get_untracked().parse::<f64>() { set_freq_hz.set(v.clamp(0.01, 100.0)); }
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
        if sending.get_untracked() { return; } // guard double-click
        set_sending.set(true);
        let new_state = !running.get_untracked();
        if new_state {
            #[derive(Serialize)]
            #[serde(rename_all = "camelCase")]
            struct Args { channel: u8, waveform: String, freq_hz: f64, amplitude: f64, offset: f64, mode: String }
            let ch_idx = channel.get_untracked();
            let wf_val = waveform.get_untracked();
            let mode_val = mode.get_untracked();
            let f_val = freq_hz.get_untracked();
            let a_val = amplitude.get_untracked();
            let o_val = offset.get_untracked();
            // AIO_SC diagnostic log: user reports short-circuit fault when wavegen
            // is active. Record request context so we can correlate with backend
            // [wavegen_start] + [faults] edges.
            web_sys::console::log_1(&format!(
                "[wavegen_start] ch={} mode={} wf={} freq={} amp={} off={}",
                ch_idx, mode_val, wf_val, f_val, a_val, o_val
            ).into());
            let args = serde_wasm_bindgen::to_value(&Args {
                channel: ch_idx,
                waveform: wf_val.clone(),
                freq_hz: f_val,
                amplitude: a_val,
                offset: o_val,
                mode: mode_val,
            }).unwrap();
            let ch_name = CH_NAMES[ch_idx as usize];
            let label = format!("Start {} {}Hz on CH {}", wf_val, f_val, ch_name);
            spawn_local(async move {
                let _ = invoke("start_wavegen", args).await;
                set_running.set(true);
                set_sending.set(false);
                show_toast(&label, "ok");
            });
        } else {
            spawn_local(async move {
                let _ = invoke("stop_wavegen", wasm_bindgen::JsValue::NULL).await;
                set_running.set(false);
                set_sending.set(false);
                show_toast("Stop wavegen", "ok");
            });
        }
    };

    view! {
        <div class="tab-content">
            <div class="tab-desc">"Waveform generator. Outputs sine, square, triangle, or sawtooth waveforms through the DAC. Select a channel, set frequency (0.01-100 Hz), amplitude, and offset. The channel is automatically set to VOUT or IOUT mode."</div>
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
                        <button class="btn wavegen-start-btn"
                            class:wavegen-running=move || running.get()
                            prop:disabled=move || sending.get()
                            on:click=toggle>
                            <span class="scope-btn-dot" class:running=move || running.get()></span>
                            {move || if sending.get() { "Sending..." } else if running.get() { "Stop Generator" } else { "Start Generator" }}
                        </button>

                        // Fault badge for the active wavegen channel (Fix 3).
                        {move || {
                            let ch_idx = channel.get() as usize;
                            let ds = state.get();
                            let alert = ds.channels.get(ch_idx).map(|c| c.channel_alert).unwrap_or(0);
                            if alert != 0 {
                                let decoded = decode_channel_alert(alert);
                                Some(view! {
                                    <div style="margin-top: 8px; padding: 6px 10px; background: #ef444425; color: #ef4444; border: 1px solid #ef444480; border-radius: 4px; font-size: 11px; font-family: 'JetBrains Mono', monospace">
                                        {format!("FAULT: {}", decoded)}
                                    </div>
                                })
                            } else { None }
                        }}

                        // Clear faults for the active wavegen channel (Fix 3).
                        <button class="btn btn-sm" style="margin-top: 6px; background: #f59e0b25; color: #f59e0b; border: 1px solid #f59e0b50"
                            on:click=move |_| {
                                let ch_idx = channel.get_untracked();
                                #[derive(Serialize)]
                                struct Args { channel: u8 }
                                let args = serde_wasm_bindgen::to_value(&Args { channel: ch_idx }).unwrap();
                                let label = format!("Clear faults on CH {}", CH_NAMES[ch_idx as usize]);
                                invoke_with_feedback("clear_channel_alert", args, &label);
                            }
                        >"Clear faults"</button>
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
