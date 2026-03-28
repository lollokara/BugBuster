use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

#[component]
pub fn IdacTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let slider_vals: [RwSignal<f64>; 4] = std::array::from_fn(|_| RwSignal::new(0.0));
    let dirty: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));

    view! {
        <div class="tab-content">
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        if !dirty[i].get_untracked() {
                            slider_vals[i].set(ch.dac_value as f64);
                        }
                        let display_v = if dirty[i].get() { slider_vals[i].get() } else { ch.dac_value as f64 };
                        let pct = (display_v / 25.0 * 100.0).clamp(0.0, 100.0);

                        view! {
                            <div class="card channel-card">
                                <div class="card-header">
                                    <span class="channel-label">{format!("CH {}", CH_NAMES[i])}</span>
                                    <span class="channel-func">"IOUT"</span>
                                </div>
                                <div class="card-body">
                                    <div class="big-value">{format!("{:.3}", display_v)}<span class="unit">"mA"</span></div>
                                    <div class="card-details">
                                        <span>"DAC Code: "{format!("{}", ch.dac_code)}</span>
                                        <span>"ADC: "{format!("{:.3} V", ch.adc_value)}</span>
                                    </div>
                                    <div class="bar-gauge bar-purple">
                                        <div class="bar-fill" style=format!("width: {}%", pct)></div>
                                    </div>

                                    <div class="slider-section">
                                        <input type="range" class="slider"
                                            min="0" max="25000" step="1"
                                            prop:value=move || (slider_vals[i].get() * 1000.0) as i64
                                            on:input=move |e| {
                                                if let Ok(v) = event_target_value(&e).parse::<f64>() {
                                                    slider_vals[i].set(v / 1000.0);
                                                    dirty[i].set(true);
                                                }
                                            }
                                            on:change=move |e| {
                                                if let Ok(v) = event_target_value(&e).parse::<f64>() {
                                                    send_dac_current(ch_idx, (v / 1000.0) as f32);
                                                    dirty[i].set(false);
                                                }
                                            }
                                        />
                                        <div class="slider-labels">
                                            <span>"0 mA"</span>
                                            <span>"25 mA"</span>
                                        </div>
                                    </div>

                                    <div class="config-row">
                                        <label>"Current"</label>
                                        <div class="number-input-wrap">
                                            <input type="number" class="number-input"
                                                min="0" max="25" step="0.001"
                                                prop:value=move || format!("{:.3}", slider_vals[i].get())
                                                on:input=move |e| {
                                                    if let Ok(v) = event_target_value(&e).parse::<f64>() {
                                                        slider_vals[i].set(v);
                                                        dirty[i].set(true);
                                                    }
                                                }
                                            />
                                            <span class="number-unit">"mA"</span>
                                            <button class="btn btn-sm btn-primary"
                                                on:click=move |_| {
                                                    send_dac_current(ch_idx, slider_vals[i].get_untracked() as f32);
                                                    dirty[i].set(false);
                                                }
                                            >"Set"</button>
                                        </div>
                                    </div>
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
struct DacCurrentArgs { channel: u8, current_ma: f32 }

fn send_dac_current(ch: u8, current_ma: f32) {
    let args = serde_wasm_bindgen::to_value(&DacCurrentArgs { channel: ch, current_ma }).unwrap();
    invoke_void("set_dac_current", args);
}
