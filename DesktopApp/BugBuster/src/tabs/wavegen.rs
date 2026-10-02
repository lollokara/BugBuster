use crate::components::icons::Icon;
use crate::components::ui::*;
use crate::tauri_bridge::*;
use crate::theme::{css_var, use_theme};
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::JsCast;
use web_sys::{CanvasRenderingContext2d, HtmlCanvasElement};

/// WaveGen claims only the selected logical channel when the generator starts.
pub const SLOTS: &[u8] = &[];

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
    if names.is_empty() {
        format!("0x{:04X}", bits)
    } else {
        names.join(",")
    }
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
    pub active_channel: RwSignal<Option<u8>>,
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
            active_channel: RwSignal::new(None),
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
    let active_channel = ui.active_channel.read_only();
    let set_active_channel = ui.active_channel.write_only();

    let edit_freq = ui.edit_freq.read_only();
    let set_edit_freq = ui.edit_freq.write_only();
    let edit_amp = ui.edit_amp.read_only();
    let set_edit_amp = ui.edit_amp.write_only();
    let edit_off = ui.edit_off.read_only();
    let set_edit_off = ui.edit_off.write_only();

    let preview_ref = NodeRef::<leptos::html::Canvas>::new();

    let unit = move || if mode.get() == "current" { "mA" } else { "V" };
    let max_amp = move || if mode.get() == "current" { 25.0 } else { 12.0 };

    let theme = use_theme();

    // Draw preview
    Effect::new(move || {
        let wf = waveform.get();
        let amp = amplitude.get();
        let off = offset.get();
        let ch = channel.get() as usize;
        let is_current = mode.get() == "current";
        let _ = theme.resolved.get();
        let Some(canvas) = preview_ref.get() else {
            return;
        };
        let canvas: HtmlCanvasElement = canvas;
        let dpr = web_sys::window().unwrap().device_pixel_ratio();
        let rect = canvas.get_bounding_client_rect();
        let w = rect.width();
        let h = rect.height();
        if w < 10.0 {
            return;
        }
        canvas.set_width((w * dpr) as u32);
        canvas.set_height((h * dpr) as u32);

        let ctx: CanvasRenderingContext2d = canvas
            .get_context("2d")
            .unwrap()
            .unwrap()
            .dyn_into()
            .unwrap();
        ctx.scale(dpr, dpr).unwrap();

        let c_bg = css_var("--surface-plot");
        let c_grid = css_var("--grid-line");
        let c_zero = css_var("--sep-strong");
        let c_label = css_var("--label-2");
        let c_trace = css_var(["--ch-a", "--ch-b", "--ch-c", "--ch-d"][ch.min(3)]);

        ctx.set_fill_style_str(&c_bg);
        ctx.fill_rect(0.0, 0.0, w, h);

        // Grid: 8 divisions across, 4 down.
        ctx.set_stroke_style_str(&c_grid);
        ctx.set_line_width(1.0);
        for i in 1..8 {
            let x = (w * i as f64 / 8.0).round() + 0.5;
            ctx.begin_path();
            ctx.move_to(x, 0.0);
            ctx.line_to(x, h);
            ctx.stroke();
        }
        for i in 1..4 {
            let y = (h * i as f64 / 4.0).round() + 0.5;
            ctx.begin_path();
            ctx.move_to(0.0, y);
            ctx.line_to(w, y);
            ctx.stroke();
        }

        // Zero line
        ctx.set_stroke_style_str(&c_zero);
        ctx.begin_path();
        ctx.move_to(0.0, h / 2.0);
        ctx.line_to(w, h / 2.0);
        ctx.stroke();

        let y_range = ((amp.abs() + off.abs()) * 1.3).max(0.1);
        let y_of = |v: f64| h / 2.0 - (v / y_range) * (h / 2.0 - 14.0);

        ctx.set_stroke_style_str(&c_trace);
        ctx.set_line_width(2.0);
        ctx.begin_path();

        let steps = w as usize;
        for px in 0..steps {
            let t = px as f64 / steps as f64;
            let v = match wf.as_str() {
                "sine" => off + amp * (t * std::f64::consts::TAU).sin(),
                "square" => off + if t < 0.5 { amp } else { -amp },
                "triangle" => {
                    let v = if t < 0.25 {
                        t * 4.0
                    } else if t < 0.75 {
                        2.0 - t * 4.0
                    } else {
                        t * 4.0 - 4.0
                    };
                    off + amp * v
                }
                "sawtooth" => off + amp * (2.0 * t - 1.0),
                _ => 0.0,
            };
            let y = y_of(v);
            if px == 0 {
                ctx.move_to(px as f64, y);
            } else {
                ctx.line_to(px as f64, y);
            }
        }
        ctx.stroke();

        ctx.set_fill_style_str(&c_label);
        ctx.set_font("11px Inter, system-ui, sans-serif");
        ctx.set_text_align("left");
        let u = if is_current { "mA" } else { "V" };
        let top = amp + off;
        let bot = -amp + off;
        let _ = ctx.fill_text(&format!("{:+.1} {}", top, u), 6.0, (y_of(top) - 5.0).max(12.0));
        let _ = ctx.fill_text(&format!("{:+.1} {}", bot, u), 6.0, (y_of(bot) + 14.0).min(h - 4.0));
    });

    let commit_freq = move || {
        if let Ok(v) = edit_freq.get_untracked().parse::<f64>() {
            set_freq_hz.set(v.clamp(0.1, 100.0));
        }
        set_edit_freq.set(format!("{:.1}", freq_hz.get_untracked()));
    };
    let commit_amp = move || {
        if let Ok(v) = edit_amp.get_untracked().parse::<f64>() {
            set_amplitude.set(v.clamp(0.0, max_amp()));
        }
        set_edit_amp.set(format!("{:.1}", amplitude.get_untracked()));
    };
    let commit_off = move || {
        if let Ok(v) = edit_off.get_untracked().parse::<f64>() {
            set_offset.set(v.clamp(-max_amp(), max_amp()));
        }
        set_edit_off.set(format!("{:.1}", offset.get_untracked()));
    };

    let toggle = move |_: leptos::ev::MouseEvent| {
        if sending.get_untracked() {
            return;
        } // guard double-click
        set_sending.set(true);
        let new_state = !running.get_untracked();
        if new_state {
            #[derive(Serialize)]
            #[serde(rename_all = "camelCase")]
            struct Args {
                channel: u8,
                waveform: String,
                freq_hz: f64,
                amplitude: f64,
                offset: f64,
                mode: String,
            }
            let ch_idx = channel.get_untracked();
            let slot = 12 + ch_idx;
            let wf_val = waveform.get_untracked();
            let mode_val = mode.get_untracked();
            let f_val = freq_hz.get_untracked();
            let a_val = amplitude.get_untracked();
            let o_val = offset.get_untracked();
            // AIO_SC diagnostic log: user reports short-circuit fault when wavegen
            // is active. Record request context so we can correlate with backend
            // [wavegen_start] + [faults] edges.
            web_sys::console::log_1(
                &format!(
                    "[wavegen_start] ch={} mode={} wf={} freq={} amp={} off={}",
                    ch_idx, mode_val, wf_val, f_val, a_val, o_val
                )
                .into(),
            );
            let args = serde_wasm_bindgen::to_value(&Args {
                channel: ch_idx,
                waveform: wf_val.clone(),
                freq_hz: f_val,
                amplitude: a_val,
                offset: o_val,
                mode: mode_val,
            })
            .unwrap();
            let ch_name = CH_NAMES[ch_idx as usize];
            let label = format!("Start {} {}Hz on CH {}", wf_val, f_val, ch_name);
            spawn_local(async move {
                if !io_claim(&[slot], 5000, "wavegen").await {
                    set_sending.set(false);
                    show_toast(&format!("CH {} is held by another interface", ch_name), "err");
                    return;
                }

                if try_invoke("start_wavegen", args).await.is_some() {
                    set_active_channel.set(Some(ch_idx));
                    set_running.set(true);
                    show_toast(&label, "ok");
                } else {
                    io_release(&[slot]).await;
                    show_toast(&format!("Failed to start wavegen on CH {}", ch_name), "err");
                }
                set_sending.set(false);
            });
        } else {
            let stop_ch = active_channel
                .get_untracked()
                .unwrap_or_else(|| channel.get_untracked());
            let stop_slot = 12 + stop_ch;
            spawn_local(async move {
                let _ = try_invoke("stop_wavegen", wasm_bindgen::JsValue::NULL).await;
                io_release(&[stop_slot]).await;
                set_active_channel.set(None);
                set_running.set(false);
                set_sending.set(false);
                show_toast("Stop wavegen", "ok");
            });
        }
    };

    let wf_options = vec![
        ("sine".to_string(), "Sine"),
        ("square".to_string(), "Square"),
        ("triangle".to_string(), "Triangle"),
        ("sawtooth".to_string(), "Sawtooth"),
    ];
    let mode_options = vec![
        ("voltage".to_string(), "Voltage"),
        ("current".to_string(), "Current"),
    ];
    let freq_invalid = move || edit_freq.get().trim().parse::<f64>().is_err();
    let amp_invalid = move || edit_amp.get().trim().parse::<f64>().is_err();
    let off_invalid = move || edit_off.get().trim().parse::<f64>().is_err();
    let period_label = move || {
        let f = freq_hz.get().max(0.001);
        let p = 1.0 / f;
        if p < 1.0 {
            format!("{:.1} ms", p * 1000.0)
        } else {
            format!("{:.2} s", p)
        }
    };

    view! {
        <div class="view view-narrow wg-root">
            <div class="wg-grid">
                // ============ LEFT: waveform ============
                <section class="group wg-wave" aria-label="Waveform">
                    <div class="group-header">
                        <h3 class="group-title">"Waveform"</h3>
                        <div class="group-actions">
                            <span class="badge num">{move || format!("{:.1} Hz", freq_hz.get())}</span>
                        </div>
                    </div>
                    <div class="wg-preview">
                        <canvas node_ref=preview_ref class="wg-canvas" aria-label="Waveform preview"></canvas>
                    </div>
                    <SegmentedControl
                        options=wf_options
                        value=waveform
                        on_change=Callback::new(move |v: String| set_waveform.set(v))
                        block=true
                        aria_label="Waveform shape"
                    />
                    <div class="wg-readouts">
                        <Readout label="Period" value=Signal::derive(period_label) size="sm" />
                        <Readout label="Peak to peak"
                            value=Signal::derive(move || format!("{:.1} {}", amplitude.get() * 2.0, unit()))
                            size="sm" />
                        <Readout label="Swing"
                            value=Signal::derive(move || format!("{:+.1} to {:+.1} {}", offset.get() - amplitude.get(), offset.get() + amplitude.get(), unit()))
                            size="sm" />
                    </div>
                </section>

                // ============ RIGHT: output ============
                <section class="group wg-output" aria-label="Output">
                    <div class="group-header">
                        <h3 class="group-title">"Output"</h3>
                        <div class="group-actions">
                            {move || if running.get() {
                                let ch_name = CH_NAMES[active_channel.get().unwrap_or(channel.get()) as usize];
                                view! {
                                    <span class="badge tone-green"><span class="dot tone-green live"></span>{format!("Running on CH {}", ch_name)}</span>
                                }.into_any()
                            } else {
                                view! { <span class="badge"><span class="dot"></span>"Stopped"</span> }.into_any()
                            }}
                        </div>
                    </div>
                    <div class="wg-rows">
                        <div class="config-row">
                            <label for="wg-channel">"Channel"</label>
                            <select id="wg-channel" class="dropdown" on:change=move |e| { set_channel.set(event_target_value(&e).parse().unwrap_or(0)); }>
                                {(0..4).map(|i| view! {
                                    <option value=i.to_string() selected=move || channel.get() == i as u8>{format!("CH {}", CH_NAMES[i])}</option>
                                }).collect::<Vec<_>>()}
                            </select>
                        </div>
                        <div class="config-row">
                            <label id="wg-output-lbl">"Output"</label>
                            <SegmentedControl
                                options=mode_options
                                value=mode
                                on_change=Callback::new(move |new: String| {
                                    if mode.get_untracked() == new { return; }
                                    set_mode.set(new.clone());
                                    // Reset amplitude to safe value for new mode
                                    let m = if new == "current" { 12.5 } else { 5.0 };
                                    set_amplitude.set(m);
                                    set_edit_amp.set(format!("{:.1}", m));
                                })
                                aria_label="Output mode"
                            />
                        </div>
                        <div class="config-row">
                            <div class="wg-label">
                                <label for="wg-freq">"Frequency"</label>
                                <span class="row-hint">"0.1 to 100 Hz"</span>
                            </div>
                            <div class="number-input-wrap">
                                <input id="wg-freq" type="text" inputmode="decimal" class="number-input"
                                    aria-invalid=move || if freq_invalid() { "true" } else { "false" }
                                    prop:value=move || edit_freq.get()
                                    on:input=move |e| set_edit_freq.set(event_target_value(&e))
                                    on:blur=move |_| commit_freq()
                                    on:keydown=move |e: leptos::ev::KeyboardEvent| { if e.key() == "Enter" { commit_freq(); } }
                                />
                                <span class="number-unit">"Hz"</span>
                            </div>
                        </div>
                        <div class="config-row">
                            <div class="wg-label">
                                <label for="wg-amp">"Amplitude"</label>
                                <span class="row-hint">{move || format!("0 to {:.0} {}", max_amp(), unit())}</span>
                            </div>
                            <div class="number-input-wrap">
                                <input id="wg-amp" type="text" inputmode="decimal" class="number-input"
                                    aria-invalid=move || if amp_invalid() { "true" } else { "false" }
                                    prop:value=move || edit_amp.get()
                                    on:input=move |e| set_edit_amp.set(event_target_value(&e))
                                    on:blur=move |_| commit_amp()
                                    on:keydown=move |e: leptos::ev::KeyboardEvent| { if e.key() == "Enter" { commit_amp(); } }
                                />
                                <span class="number-unit">{unit}</span>
                            </div>
                        </div>
                        <div class="config-row">
                            <div class="wg-label">
                                <label for="wg-off">"Offset"</label>
                                <span class="row-hint">{move || format!("-{:.0} to {:.0} {}", max_amp(), max_amp(), unit())}</span>
                            </div>
                            <div class="number-input-wrap">
                                <input id="wg-off" type="text" inputmode="decimal" class="number-input"
                                    aria-invalid=move || if off_invalid() { "true" } else { "false" }
                                    prop:value=move || edit_off.get()
                                    on:input=move |e| set_edit_off.set(event_target_value(&e))
                                    on:blur=move |_| commit_off()
                                    on:keydown=move |e: leptos::ev::KeyboardEvent| { if e.key() == "Enter" { commit_off(); } }
                                />
                                <span class="number-unit">{unit}</span>
                            </div>
                        </div>
                        <div class="config-row">
                            <div class="wg-label">
                                <span class="row-label">"Faults"</span>
                                <span class="row-hint">"Alert flags of the selected channel"</span>
                            </div>
                            <div class="wg-fault-actions">
                                {move || {
                                    let ch_idx = channel.get() as usize;
                                    let ds = state.get();
                                    let alert = ds.channels.get(ch_idx).map(|c| c.channel_alert).unwrap_or(0);
                                    if alert != 0 {
                                        view! { <span class="badge tone-red wg-fault-code">{decode_channel_alert(alert)}</span> }.into_any()
                                    } else {
                                        view! { <span class="badge tone-green">"None"</span> }.into_any()
                                    }
                                }}
                                // Clear faults for the active wavegen channel.
                                <button class="btn btn-sm"
                                    on:click=move |_| {
                                        let ch_idx = channel.get_untracked();
                                        #[derive(Serialize)]
                                        struct Args { channel: u8 }
                                        let args = serde_wasm_bindgen::to_value(&Args { channel: ch_idx }).unwrap();
                                        let label = format!("Clear faults on CH {}", CH_NAMES[ch_idx as usize]);
                                        invoke_with_feedback("clear_channel_alert", args, &label);
                                    }
                                >"Clear faults"</button>
                            </div>
                        </div>
                    </div>

                    <div class="wg-actions">
                        <button class="btn btn-lg btn-block wavegen-start-btn"
                            class:btn-primary=move || !running.get()
                            class:btn-danger=move || running.get()
                            class:wavegen-running=move || running.get()
                            prop:disabled=move || sending.get()
                            on:click=toggle>
                            {move || if running.get() {
                                view! { <Icon name="square" size=14 /> }.into_any()
                            } else {
                                view! { <Icon name="play" size=14 /> }.into_any()
                            }}
                            {move || if sending.get() { "Sending..." } else if running.get() { "Stop generator" } else { "Start generator" }}
                        </button>
                        <p class="wg-hint">
                            {move || format!("Will set CH {} to {} mode on start",
                                CH_NAMES[channel.get() as usize],
                                if mode.get() == "current" { "IOUT" } else { "VOUT" }
                            )}
                        </p>
                    </div>
                </section>
            </div>
        </div>
    }
}
