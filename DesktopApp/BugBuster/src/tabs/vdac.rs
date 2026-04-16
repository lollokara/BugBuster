use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use crate::tauri_bridge::*;
use crate::components::channel_sparkline::ChannelSparkline;

const SPARK_CAP: usize = 120;

#[component]
pub fn VdacTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let slider_vals: [RwSignal<f64>; 4] = std::array::from_fn(|_| RwSignal::new(0.0));
    let dirty: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));
    let bipolar: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));

    // Per-channel rolling history of dac_value for the sparkline.
    let history: [RwSignal<Vec<f32>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));
    Effect::new(move |_| {
        let ds = state.get();
        for (i, ch) in ds.channels.iter().enumerate().take(4) {
            let v = ch.dac_value;
            history[i].update(|buf| {
                buf.push(v);
                if buf.len() > SPARK_CAP {
                    let d = buf.len() - SPARK_CAP;
                    buf.drain(0..d);
                }
            });
        }
    });

    // Fix 4: On VDAC tab mount, auto-fix any VOUT channel whose ADC mux is not
    // 0 (LF_TO_AGND). Mux != 0 for VOUT means the ADC will measure the wrong
    // node (typically reading 0V). Log to console so the user sees the auto-fix.
    Effect::new(move |_| {
        let ds = state.get();
        for (i, ch) in ds.channels.iter().enumerate().take(4) {
            if ch.function == 1 && ch.adc_mux != 0 {
                let ch_idx = i as u8;
                let range = ch.adc_range;
                let rate = ch.adc_rate;
                web_sys::console::log_1(&format!(
                    "[VDAC] auto-fix: CH {} VOUT mux was {} — forcing 0 (LF_TO_AGND)",
                    i, ch.adc_mux
                ).into());
                spawn_local(async move {
                    #[derive(Serialize)]
                    struct A { channel: u8, mux: u8, range: u8, rate: u8 }
                    let args = serde_wasm_bindgen::to_value(&A { channel: ch_idx, mux: 0, range, rate }).unwrap();
                    let _ = invoke("set_adc_config", args).await;
                });
            }
        }
    });

    view! {
        <div class="tab-content">
            <div class="tab-desc">"Voltage output control. Set each channel to VOUT mode to output a programmable voltage (0-12V unipolar or +/-12V bipolar). Use the slider or type a value and click SET."</div>
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let is_vout = ch.function == 1; // VOUT
                        let is_bipolar = bipolar[i].get();
                        let max_v: f64 = 12.0;
                        let min_v: f64 = if is_bipolar { -12.0 } else { 0.0 };
                        let span = max_v - min_v;
                        let color = CH_COLORS[i];

                        if !dirty[i].get() {
                            slider_vals[i].set(ch.dac_value as f64);
                        }

                        let display_v = if dirty[i].get() { slider_vals[i].get() } else { ch.dac_value as f64 };
                        let pct = if span > 0.0 { ((display_v - min_v) / span * 100.0).clamp(0.0, 100.0) } else { 0.0 };

                        view! {
                            <div class="card channel-card" class:ch-disabled=!is_vout>
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("CH {}", CH_NAMES[i])}
                                    </div>
                                    <span class="channel-func">{if is_vout { "VOUT" } else { func_name(ch.function) }}</span>
                                </div>
                                <div class="card-body">
                                    {if !is_vout {
                                        view! {
                                            <div class="mode-warning">
                                                <span class="mode-warning-icon">"ℹ"</span>
                                                <span>"Set channel to VOUT mode to control voltage output"</span>
                                            </div>
                                        }.into_any()
                                    } else {
                                        view! {
                                            <div>
                                                <div class="big-value">{format!("{:.3}", display_v)}<span class="unit">"V"</span></div>
                                                <div class="card-details"><span>"DAC Code: "{format!("{}", ch.dac_code)}</span></div>
                                                <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                                    <div class="bar-fill-dynamic" style=format!("width: {}%", pct)></div>
                                                </div>

                                                <div class="card-details" style="margin-top: 4px;">
                                                    <span>{format!("Readback (DAC): {:.3} V", ch.dac_value)}</span>
                                                </div>
                                                <div class="card-details" style="margin-top: 2px;">
                                                    <span>{format!("ADC (ext): {:.3} V", ch.adc_value)}</span>
                                                </div>
                                                <ChannelSparkline
                                                    values=Signal::from(history[i])
                                                    min=Signal::derive(move || min_v as f32)
                                                    max=Signal::derive(move || max_v as f32)
                                                    color=color.to_string()
                                                />

                                                <div class="config-row">
                                                    <label>"Range"</label>
                                                    <label class="toggle-wrap">
                                                        <span class="toggle-off-label">"0–12V"</span>
                                                        <div class="toggle" class:active=move || bipolar[i].get()
                                                            on:click=move |_| {
                                                                let new_val = !bipolar[i].get_untracked();
                                                                bipolar[i].set(new_val);
                                                                send_vout_range(ch_idx, new_val);
                                                            }
                                                        ><div class="toggle-thumb"></div></div>
                                                        <span class="toggle-on-label">"±12V"</span>
                                                    </label>
                                                </div>

                                                <div class="slider-section">
                                                    <input type="range" class="slider slider-colored"
                                                        style=format!("--slider-color: {}", color)
                                                        min=min_v * 1000.0 max=max_v * 1000.0 step="1"
                                                        prop:value=move || (slider_vals[i].get() * 1000.0) as i64
                                                        on:input=move |e| {
                                                            if let Ok(v) = event_target_value(&e).parse::<f64>() {
                                                                slider_vals[i].set(v / 1000.0);
                                                                dirty[i].set(true);
                                                            }
                                                        }
                                                        on:change=move |e| {
                                                            if let Ok(v) = event_target_value(&e).parse::<f64>() {
                                                                send_dac_voltage(ch_idx, (v / 1000.0) as f32, bipolar[i].get_untracked());
                                                                dirty[i].set(false);
                                                            }
                                                        }
                                                    />
                                                    <div class="slider-labels">
                                                        <span>{format!("{:.0}V", min_v)}</span>
                                                        <span>{format!("{:.0}V", max_v)}</span>
                                                    </div>
                                                </div>

                                                <div class="config-row">
                                                    <label>"Set V"</label>
                                                    <div class="number-input-wrap">
                                                        <input type="number" class="number-input"
                                                            min=min_v max=max_v step="0.001"
                                                            prop:value=move || format!("{:.3}", slider_vals[i].get())
                                                            on:input=move |e| {
                                                                if let Ok(v) = event_target_value(&e).parse::<f64>() {
                                                                    slider_vals[i].set(v);
                                                                    dirty[i].set(true);
                                                                }
                                                            }
                                                        />
                                                        <span class="number-unit">"V"</span>
                                                        <button class="btn btn-sm btn-primary"
                                                            on:click=move |_| {
                                                                send_dac_voltage(ch_idx, slider_vals[i].get_untracked() as f32, bipolar[i].get_untracked());
                                                                dirty[i].set(false);
                                                            }
                                                        >"Set"</button>
                                                    </div>
                                                </div>
                                            </div>
                                        }.into_any()
                                    }}
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}

#[derive(Serialize)]
struct DacVoltageArgs { channel: u8, voltage: f32, bipolar: bool }
#[derive(Serialize)]
struct VoutRangeArgs { channel: u8, bipolar: bool }

fn send_dac_voltage(ch: u8, voltage: f32, bipolar: bool) {
    web_sys::console::log_1(&format!("[VDAC] ch={} V={:.3} bipolar={}", ch, voltage, bipolar).into());
    let args = serde_wasm_bindgen::to_value(&DacVoltageArgs { channel: ch, voltage, bipolar }).unwrap();
    let label = format!("Set CH {} to {:.3}V", CH_NAMES[ch as usize], voltage);
    invoke_with_feedback("set_dac_voltage", args, &label);
}
fn send_vout_range(ch: u8, bipolar: bool) {
    let args = serde_wasm_bindgen::to_value(&VoutRangeArgs { channel: ch, bipolar }).unwrap();
    let label = format!("Set CH {} range to {}", CH_NAMES[ch as usize], if bipolar { "+/-12V" } else { "0-12V" });
    invoke_with_feedback("set_vout_range", args, &label);
}
