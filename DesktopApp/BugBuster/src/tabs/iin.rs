use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;
use crate::components::channel_sparkline::ChannelSparkline;

const SPARK_CAP: usize = 120;

fn send_ch_func(ch: u8, func: u8) {
    #[derive(Serialize)]
    struct Args { channel: u8, function: u8 }
    let args = serde_wasm_bindgen::to_value(&Args { channel: ch, function: func }).unwrap();
    let label = format!("Set CH {} to {}", CH_NAMES[ch as usize], func_name(func));
    invoke_with_feedback("set_channel_function", args, &label);
}

fn send_adc_cfg(ch: u8, mux: u8, range: u8, rate: u8) {
    #[derive(Serialize)]
    struct Args { channel: u8, mux: u8, range: u8, rate: u8 }
    let args = serde_wasm_bindgen::to_value(&Args { channel: ch, mux, range, rate }).unwrap();
    let range_name = ADC_RANGE_OPTIONS.iter()
        .find(|(c, _, _, _)| *c == range)
        .map(|(_, n, _, _)| *n).unwrap_or("?");
    let label = format!("Set CH {} ADC: {}", CH_NAMES[ch as usize], range_name);
    invoke_with_feedback("set_adc_config", args, &label);
}

#[component]
pub fn IinTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    // Per-channel rolling history of adc_value (mA) for sparkline.
    let history: [RwSignal<Vec<f32>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));
    Effect::new(move |_| {
        let ds = state.get();
        for (i, ch) in ds.channels.iter().enumerate().take(4) {
            let v = ch.adc_value;
            history[i].update(|buf| {
                buf.push(v);
                if buf.len() > SPARK_CAP {
                    let d = buf.len() - SPARK_CAP;
                    buf.drain(0..d);
                }
            });
        }
    });

    view! {
        <div class="tab-content">
            <div class="tab-desc">"Current input measurement. Set channels to IIN External (device measures, external loop power) or IIN Loop (device powers the loop and measures). Reads 0-25 mA."</div>
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let is_iin = ch.function == 4 || ch.function == 5
                                  || ch.function == 11 || ch.function == 12;
                        let is_ext = ch.function == 4 || ch.function == 11;
                        let is_loop = ch.function == 5 || ch.function == 12;
                        let is_hart = ch.function == 11 || ch.function == 12;
                        let color = CH_COLORS[i];

                        // Current reading: ADC value is in mA for IIN modes
                        let current_ma = ch.adc_value;
                        let pct = (current_ma.abs() as f64 / 25.0 * 100.0).clamp(0.0, 100.0);

                        view! {
                            <div class="card channel-card" class:ch-disabled=!is_iin>
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("CH {}", CH_NAMES[i])}
                                    </div>
                                    <span class="channel-func">{func_name(ch.function)}</span>
                                </div>
                                <div class="card-body">
                                    {if !is_iin {
                                        view! {
                                            <div>
                                                <div class="mode-warning">
                                                    <span class="mode-warning-icon">"i"</span>
                                                    <span>"Set channel to Current In mode to measure current"</span>
                                                </div>

                                                <div style="display: flex; gap: 8px; margin-top: 12px">
                                                    <button class="scope-btn" style=format!("flex: 1; color: {}", color)
                                                        on:click=move |_| send_ch_func(ch_idx, 4)
                                                    >"IIN External"</button>
                                                    <button class="scope-btn" style=format!("flex: 1; color: {}", color)
                                                        on:click=move |_| send_ch_func(ch_idx, 5)
                                                    >"IIN Loop"</button>
                                                </div>
                                            </div>
                                        }.into_any()
                                    } else {
                                        view! {
                                            <div>
                                                // Mode indicator
                                                <div style="display: flex; gap: 8px; margin-bottom: 12px">
                                                    <div style=format!("flex: 1; padding: 6px 10px; border-radius: 6px; text-align: center; font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace; border: 1px solid {}; {}",
                                                        if is_ext { format!("{}88", color) } else { "var(--border)".into() },
                                                        if is_ext { format!("background: {}22; color: {}", color, color) } else { "color: var(--text-dim)".into() }
                                                    )>
                                                        "External Power"
                                                    </div>
                                                    <div style=format!("flex: 1; padding: 6px 10px; border-radius: 6px; text-align: center; font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace; border: 1px solid {}; {}",
                                                        if is_loop { format!("{}88", color) } else { "var(--border)".into() },
                                                        if is_loop { format!("background: {}22; color: {}", color, color) } else { "color: var(--text-dim)".into() }
                                                    )>
                                                        "Loop Powered"
                                                    </div>
                                                </div>

                                                {if is_hart {
                                                    view! {
                                                        <div style="font-size: 9px; color: var(--text-dim); text-align: center; margin-bottom: 8px; font-family: 'JetBrains Mono', monospace">
                                                            "HART enabled"
                                                        </div>
                                                    }.into_any()
                                                } else {
                                                    view! { <div></div> }.into_any()
                                                }}

                                                // Big current display
                                                <div class="big-value" style=format!("color: {}", color)>
                                                    {format!("{:.3}", current_ma)}<span class="unit">"mA"</span>
                                                </div>

                                                // Bar gauge
                                                <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                                    <div class="bar-fill-dynamic" style=format!("width: {}%", pct)></div>
                                                </div>
                                                <div class="slider-labels">
                                                    <span>"0 mA"</span>
                                                    <span>"25 mA"</span>
                                                </div>

                                                <ChannelSparkline
                                                    values=Signal::from(history[i])
                                                    min=Signal::derive(move || -25.0f32)
                                                    max=Signal::derive(move || 25.0f32)
                                                    color=color.to_string()
                                                />

                                                // Raw ADC info
                                                <div class="card-details" style="margin-top: 8px">
                                                    <span>{format!("ADC Raw: {} (0x{:06X})", ch.adc_raw, ch.adc_raw)}</span>
                                                </div>

                                                // ADC Configuration
                                                <div style="margin-top: 12px; padding-top: 10px; border-top: 1px solid var(--border)">
                                                    <div style="font-size: 10px; font-weight: 600; color: var(--text-dim); margin-bottom: 8px; letter-spacing: 1px; text-transform: uppercase">"ADC Config"</div>

                                                    // ADC Range
                                                    <div class="config-row">
                                                        <label>"Range"</label>
                                                        <select class="dropdown dropdown-sm"
                                                            prop:value=ch.adc_range.to_string()
                                                            on:change=move |e| {
                                                                if let Ok(r) = event_target_value(&e).parse::<u8>() {
                                                                    let ds = state.get_untracked();
                                                                    if let Some(c) = ds.channels.get(ch_idx as usize) {
                                                                        send_adc_cfg(ch_idx, c.adc_mux, r, c.adc_rate);
                                                                    }
                                                                }
                                                            }
                                                        >
                                                            {ADC_RANGE_OPTIONS.iter().map(|(code, name, _, _)| {
                                                                view! { <option value=code.to_string()>{*name}</option> }
                                                            }).collect::<Vec<_>>()}
                                                        </select>
                                                    </div>

                                                    // ADC Rate
                                                    <div class="config-row">
                                                        <label>"Rate"</label>
                                                        <select class="dropdown dropdown-sm"
                                                            prop:value=ch.adc_rate.to_string()
                                                            on:change=move |e| {
                                                                if let Ok(r) = event_target_value(&e).parse::<u8>() {
                                                                    let ds = state.get_untracked();
                                                                    if let Some(c) = ds.channels.get(ch_idx as usize) {
                                                                        send_adc_cfg(ch_idx, c.adc_mux, c.adc_range, r);
                                                                    }
                                                                }
                                                            }
                                                        >
                                                            {ADC_RATE_OPTIONS.iter().map(|(code, name)| {
                                                                view! { <option value=code.to_string()>{*name}</option> }
                                                            }).collect::<Vec<_>>()}
                                                        </select>
                                                    </div>

                                                    // ADC Mux
                                                    <div class="config-row">
                                                        <label>"Input"</label>
                                                        <select class="dropdown dropdown-sm"
                                                            prop:value=ch.adc_mux.to_string()
                                                            on:change=move |e| {
                                                                if let Ok(m) = event_target_value(&e).parse::<u8>() {
                                                                    let ds = state.get_untracked();
                                                                    if let Some(c) = ds.channels.get(ch_idx as usize) {
                                                                        send_adc_cfg(ch_idx, m, c.adc_range, c.adc_rate);
                                                                    }
                                                                }
                                                            }
                                                        >
                                                            {ADC_MUX_OPTIONS.iter().map(|(code, name)| {
                                                                view! { <option value=code.to_string()>{*name}</option> }
                                                            }).collect::<Vec<_>>()}
                                                        </select>
                                                    </div>
                                                </div>

                                                // Mode switch buttons
                                                <div style="margin-top: 12px; padding-top: 10px; border-top: 1px solid var(--border)">
                                                    <div style="display: flex; gap: 6px">
                                                        <button class="scope-btn" style="flex: 1; font-size: 10px"
                                                            on:click=move |_| send_ch_func(ch_idx, 4)
                                                        >"Switch to EXT"</button>
                                                        <button class="scope-btn" style="flex: 1; font-size: 10px"
                                                            on:click=move |_| send_ch_func(ch_idx, 5)
                                                        >"Switch to LOOP"</button>
                                                        <button class="scope-btn" style="flex: 1; font-size: 10px; color: var(--text-dim)"
                                                            on:click=move |_| send_ch_func(ch_idx, 0)
                                                        >"Disable"</button>
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
