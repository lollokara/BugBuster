use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

#[component]
pub fn AdcTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="tab-content">
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let has_adc = matches!(ch.function, 3 | 4 | 5 | 7); // VIN, IIN_EXT, IIN_LOOP, RES
                        let color = CH_COLORS[i];
                        let range_info = ADC_RANGE_OPTIONS.iter().find(|r| r.0 == ch.adc_range);
                        let (rng_min, rng_max) = range_info.map(|r| (r.2, r.3)).unwrap_or((0.0, 12.0));
                        let span = rng_max - rng_min;
                        let pct = if span > 0.0 { ((ch.adc_value - rng_min) / span * 100.0).clamp(0.0, 100.0) } else { 0.0 };
                        let unit = if ch.function == 4 || ch.function == 5 { "mA" } else { "V" };

                        view! {
                            <div class="card channel-card" class:ch-disabled=!has_adc>
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("CH {}", CH_NAMES[i])}
                                    </div>
                                    <span class="channel-func">{func_name(ch.function)}</span>
                                </div>
                                <div class="card-body">
                                    {if !has_adc {
                                        view! {
                                            <div class="mode-warning">
                                                <span class="mode-warning-icon">"ℹ"</span>
                                                <span>"Set channel to an input mode (VIN, IIN) to read ADC"</span>
                                            </div>
                                        }.into_any()
                                    } else {
                                        view! {
                                            <div>
                                                <div class="big-value">{format!("{:.4}", ch.adc_value)}<span class="unit">{unit}</span></div>
                                                <div class="card-details">
                                                    <span>"Raw: 0x"{format!("{:06X}", ch.adc_raw)}</span>
                                                    <span>"Code: "{format!("{}", ch.adc_raw)}</span>
                                                </div>
                                                <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                                    <div class="bar-fill-dynamic" style=format!("width: {}%", pct)></div>
                                                </div>

                                                <div class="config-section">
                                                    <div class="config-row">
                                                        <label>"Range"</label>
                                                        <select class="dropdown"
                                                            prop:value=ch.adc_range.to_string()
                                                            on:change=move |e| {
                                                                let range: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                                send_adc_config(ch_idx, ch.adc_mux, range, ch.adc_rate);
                                                            }
                                                        >
                                                            {ADC_RANGE_OPTIONS.iter().map(|(code, name, _, _)| {
                                                                view! { <option value=code.to_string()>{*name}</option> }
                                                            }).collect::<Vec<_>>()}
                                                        </select>
                                                    </div>
                                                    <div class="config-row">
                                                        <label>"Rate"</label>
                                                        <select class="dropdown"
                                                            prop:value=ch.adc_rate.to_string()
                                                            on:change=move |e| {
                                                                let rate: u8 = event_target_value(&e).parse().unwrap_or(1);
                                                                send_adc_config(ch_idx, ch.adc_mux, ch.adc_range, rate);
                                                            }
                                                        >
                                                            {ADC_RATE_OPTIONS.iter().map(|(code, name)| {
                                                                view! { <option value=code.to_string()>{*name}</option> }
                                                            }).collect::<Vec<_>>()}
                                                        </select>
                                                    </div>
                                                    <div class="config-row">
                                                        <label>"Mux"</label>
                                                        <select class="dropdown"
                                                            prop:value=ch.adc_mux.to_string()
                                                            on:change=move |e| {
                                                                let mux: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                                send_adc_config(ch_idx, mux, ch.adc_range, ch.adc_rate);
                                                            }
                                                        >
                                                            {ADC_MUX_OPTIONS.iter().map(|(code, name)| {
                                                                view! { <option value=code.to_string()>{*name}</option> }
                                                            }).collect::<Vec<_>>()}
                                                        </select>
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
struct AdcConfigArgs { channel: u8, mux: u8, range: u8, rate: u8 }

fn send_adc_config(ch: u8, mux: u8, range: u8, rate: u8) {
    let args = serde_wasm_bindgen::to_value(&AdcConfigArgs { channel: ch, mux, range, rate }).unwrap();
    invoke_void("set_adc_config", args);
}
