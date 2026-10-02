use crate::components::channel_sparkline::{
    ch_key, ch_var, set_channel_function, ChannelEmpty, ChannelHead, ChannelSparkline,
};
use crate::components::ui::{Readout, SegmentedControl};
use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

const SPARK_CAP: usize = 120;

fn send_adc_cfg(ch: u8, mux: u8, range: u8, rate: u8) {
    #[derive(Serialize)]
    struct Args {
        channel: u8,
        mux: u8,
        range: u8,
        rate: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        channel: ch,
        mux,
        range,
        rate,
    })
    .unwrap();
    let range_name = ADC_RANGE_OPTIONS
        .iter()
        .find(|(c, _, _, _)| *c == range)
        .map(|(_, n, _, _)| *n)
        .unwrap_or("?");
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
        <div class="view">
            <p class="group-subtitle">"Current inputs, 0 to 25 mA. External power measures a loop powered elsewhere; loop powered supplies the loop."</p>
            <div class="grid-2">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let is_iin = ch.function == 4 || ch.function == 5
                                  || ch.function == 11 || ch.function == 12;
                        let is_loop = ch.function == 5 || ch.function == 12;
                        let is_hart = ch.function == 11 || ch.function == 12;

                        // Current reading: ADC value is in mA for IIN modes
                        let current_ma = ch.adc_value;
                        let pct = (current_ma.abs() as f64 / 25.0 * 100.0).clamp(0.0, 100.0);

                        let body = if !is_iin {
                            view! {
                                <ChannelEmpty icon="arrow-down-to-line" message="Set the channel to a current input mode to measure it.">
                                    <button class="btn btn-sm" on:click=move |_| set_channel_function(ch_idx, 4)>"External power"</button>
                                    <button class="btn btn-sm" on:click=move |_| set_channel_function(ch_idx, 5)>"Loop powered"</button>
                                </ChannelEmpty>
                            }.into_any()
                        } else {
                            view! {
                                <div class="io-body">
                                    <div class="io-primary">
                                        <Readout
                                            label="Current"
                                            value=Signal::derive(move || format!("{:.3}", current_ma))
                                            unit="mA"
                                            size="lg"
                                        />
                                        <div class="io-meta">
                                            <span>"Raw "<span class="io-code">{format!("0x{:06X}", ch.adc_raw)}</span></span>
                                            <span>"Code "{format!("{}", ch.adc_raw)}</span>
                                        </div>
                                    </div>
                                    <div class="meter io-meter" aria-hidden="true">
                                        <span style=format!("width: {}%", pct)></span>
                                    </div>

                                    <ChannelSparkline
                                        values=Signal::from(history[i])
                                        min=Signal::derive(move || -25.0f32)
                                        max=Signal::derive(move || 25.0f32)
                                        color_var=ch_var(i)
                                    />

                                    <div class="rows io-rows">
                                        <div class="row">
                                            <span class="row-label">
                                                "Power"
                                                {is_hart.then(|| view! { <span class="row-hint">"HART enabled"</span> })}
                                            </span>
                                            <SegmentedControl
                                                options=vec![(4u8, "External"), (5u8, "Loop")]
                                                value=Signal::derive(move || if is_loop { 5u8 } else { 4u8 })
                                                on_change=Callback::new(move |f: u8| set_channel_function(ch_idx, f))
                                                aria_label="Current input power mode"
                                            />
                                        </div>
                                        <label class="row">
                                            <span class="row-label">"Range"</span>
                                            <select
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
                                        </label>
                                        <label class="row">
                                            <span class="row-label">"Rate"</span>
                                            <select
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
                                        </label>
                                        <label class="row">
                                            <span class="row-label">"Input"</span>
                                            <select
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
                                        </label>
                                    </div>
                                </div>
                            }.into_any()
                        };

                        view! {
                            <section class="group io-card" data-ch=ch_key(i)>
                                <ChannelHead idx=i func=func_name(ch.function)>
                                    {is_iin.then(|| view! {
                                        <button class="btn btn-plain btn-xs" title="Set channel to high impedance"
                                            on:click=move |_| set_channel_function(ch_idx, 0)
                                        >"Disable"</button>
                                    })}
                                </ChannelHead>
                                {body}
                            </section>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}
