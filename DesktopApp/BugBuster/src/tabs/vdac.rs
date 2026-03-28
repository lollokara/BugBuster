use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

#[component]
pub fn VdacTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let slider_vals: [RwSignal<f64>; 4] = std::array::from_fn(|_| RwSignal::new(0.0));
    let dirty: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));
    let bipolar: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));

    view! {
        <div class="tab-content">
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
                                                        min=(min_v * 1000.0) max=(max_v * 1000.0) step="1"
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
    let args = serde_wasm_bindgen::to_value(&DacVoltageArgs { channel: ch, voltage, bipolar }).unwrap();
    invoke_void("set_dac_voltage", args);
}
fn send_vout_range(ch: u8, bipolar: bool) {
    let args = serde_wasm_bindgen::to_value(&VoutRangeArgs { channel: ch, bipolar }).unwrap();
    invoke_void("set_vout_range", args);
}
