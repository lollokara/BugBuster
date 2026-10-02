use crate::components::channel_sparkline::{
    ch_key, ch_var, set_channel_function, ChannelEmpty, ChannelHead, ChannelSparkline,
};
use crate::components::ui::Readout;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

const SPARK_CAP: usize = 120;

#[component]
pub fn IdacTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let slider_vals: [RwSignal<f64>; 4] = std::array::from_fn(|_| RwSignal::new(0.0));
    let dirty: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));

    // Per-channel history of dac_value (mA for IOUT) for sparkline.
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

    view! {
        <div class="view">
            <p class="group-subtitle">"Programmable current outputs, 0 to 25 mA, for driving 4-20 mA loops."</p>
            <div class="grid-2">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let is_iout = ch.function == 2 || ch.function == 10; // IOUT or IOUT_HART

                        if !dirty[i].get() {
                            slider_vals[i].set(ch.dac_value as f64);
                        }
                        let display_v = if dirty[i].get() { slider_vals[i].get() } else { ch.dac_value as f64 };
                        let pct = (display_v / 25.0 * 100.0).clamp(0.0, 100.0);

                        let body = if !is_iout {
                            view! {
                                <ChannelEmpty icon="zap" message="Set the channel to current output to drive it.">
                                    <button class="btn btn-sm" on:click=move |_| set_channel_function(ch_idx, 2)>"Current out"</button>
                                </ChannelEmpty>
                            }.into_any()
                        } else {
                            view! {
                                <div class="io-body">
                                    <div class="io-primary">
                                        <Readout
                                            label="Output"
                                            value=Signal::derive(move || format!("{:.3}", display_v))
                                            unit="mA"
                                            size="lg"
                                        />
                                        <div class="io-meta">
                                            <span>"DAC code "{format!("{}", ch.dac_code)}</span>
                                            <span>{format!("ADC {:.3} V", ch.adc_value)}</span>
                                        </div>
                                    </div>
                                    <div class="meter io-meter" aria-hidden="true">
                                        <span style=format!("width: {}%", pct)></span>
                                    </div>

                                    <ChannelSparkline
                                        values=Signal::from(history[i])
                                        min=Signal::derive(move || 0.0f32)
                                        max=Signal::derive(move || 25.0f32)
                                        color_var=ch_var(i)
                                    />

                                    <div class="rows io-rows">
                                        <div class="row io-setpoint-row">
                                            <span class="row-label">
                                                "Setpoint"
                                                <span class="row-hint">{format!("Readback {:.3} mA", ch.dac_value)}</span>
                                            </span>
                                            <div class="number-input-wrap">
                                                <input type="number" class="number-input"
                                                    aria-label="Setpoint in milliamps"
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

                                    <div class="slider-section io-slider">
                                        <input type="range"
                                            aria-label="Setpoint slider"
                                            style=move || format!("--fill: {}%", (slider_vals[i].get() / 25.0 * 100.0).clamp(0.0, 100.0))
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
                                            <span>"0 mA"</span><span>"25 mA"</span>
                                        </div>
                                    </div>
                                </div>
                            }.into_any()
                        };

                        view! {
                            <section class="group io-card" data-ch=ch_key(i)>
                                <ChannelHead idx=i func=if is_iout { "IOUT" } else { func_name(ch.function) } />
                                {body}
                            </section>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DacCurrentArgs {
    channel: u8,
    current_ma: f32,
}

fn send_dac_current(ch: u8, current_ma: f32) {
    let args = serde_wasm_bindgen::to_value(&DacCurrentArgs {
        channel: ch,
        current_ma,
    })
    .unwrap();
    let label = format!("Set CH {} to {:.3}mA", CH_NAMES[ch as usize], current_ma);
    invoke_with_feedback("set_dac_current", args, &label);
}
