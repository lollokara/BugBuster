use crate::components::channel_sparkline::{
    ch_key, ch_var, set_channel_function, ChannelEmpty, ChannelHead, ChannelSparkline,
};
use crate::components::ui::Readout;
use crate::tauri_bridge::{self, *};
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;

/// ADC does not claim analog slots merely for viewing live state.
pub const SLOTS: &[u8] = &[];

const SPARK_CAP: usize = 120;

/// DESK-28: set when this tab (re)starts the firmware ADC stream, so closing
/// the tab stops only a stream it owns.
static STREAM_STARTED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

#[component]
pub fn AdcTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    on_cleanup(move || {
        if STREAM_STARTED.swap(false, std::sync::atomic::Ordering::Relaxed) {
            spawn_local(async move {
                let _ = try_invoke("stop_adc_stream", wasm_bindgen::JsValue::NULL).await;
            });
        }
    });
    // Per-channel rolling ring buffers of recent ADC values (capped at SPARK_CAP).
    let history: [RwSignal<Vec<f32>>; 4] = std::array::from_fn(|_| RwSignal::new(Vec::new()));

    // Push new sample whenever DeviceState.channels[i].adc_value changes.
    Effect::new(move |_| {
        let ds = state.get();
        for (i, ch) in ds.channels.iter().enumerate().take(4) {
            let v = ch.adc_value;
            history[i].update(|buf| {
                buf.push(v);
                if buf.len() > SPARK_CAP {
                    let drop = buf.len() - SPARK_CAP;
                    buf.drain(0..drop);
                }
            });
        }
    });

    // DESK-23: the grid rebuilds only when a channel's configuration changes;
    // readings update in place through `live`.
    let cfg = Memo::new(move |_| {
        state.with(|s| {
            s.channels
                .iter()
                .map(|c| (c.function, c.adc_range, c.adc_rate, c.adc_mux, c.rtd_excitation_ua))
                .collect::<Vec<_>>()
        })
    });
    let live = move |i: usize| {
        state.with(|s| s.channels.get(i).map(|c| (c.adc_value, c.adc_raw)).unwrap_or_default())
    };

    view! {
        <div class="view">
            <p class="group-subtitle">"Live readings for all four channels. Range, rate and input mux are set per channel."</p>
            <div class="grid-2">
                {move || {
                    let _ = cfg.get();
                    let ds = state.get_untracked();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let has_adc = matches!(ch.function, 3 | 4 | 5 | 7 | 11 | 12);
                        let is_res = ch.function == 7;
                        let range_info = ADC_RANGE_OPTIONS.iter().find(|r| r.0 == ch.adc_range);
                        let (rng_min, rng_max) = range_info.map(|r| (r.2, r.3)).unwrap_or((0.0, 12.0));
                        let (bar_min, bar_max) = if is_res {
                            let excitation_ua = if ch.rtd_excitation_ua > 0 { ch.rtd_excitation_ua } else { 1000 };
                            let i_exc = excitation_ua as f32 * 1e-6;
                            (0.0_f32, rng_max / i_exc)
                        } else {
                            (rng_min, rng_max)
                        };
                        let span = bar_max - bar_min;
                        let pct = move || {
                            let v = live(i).0;
                            if span > 0.0 { ((v - bar_min) / span * 100.0).clamp(0.0, 100.0) } else { 0.0 }
                        };
                        let unit = if matches!(ch.function, 4 | 5 | 11 | 12) { "mA" } else if is_res { "Ω" } else { "V" };
                        let quantity = if is_res { "Resistance" } else if unit == "mA" { "Current" } else { "Voltage" };
                        let exc_ua = if ch.rtd_excitation_ua > 0 { ch.rtd_excitation_ua } else { 1000 };
                        let hist = history[i];

                        let body = if !has_adc {
                            view! {
                                <ChannelEmpty icon="gauge" message="Set the channel to an input function to read it.">
                                    <button class="btn btn-sm" on:click=move |_| set_channel_function(ch_idx, 3)>"Voltage in"</button>
                                    <button class="btn btn-sm" on:click=move |_| set_channel_function(ch_idx, 4)>"Current in"</button>
                                    <button class="btn btn-sm" on:click=move |_| set_channel_function(ch_idx, 7)>"Resistance"</button>
                                </ChannelEmpty>
                            }.into_any()
                        } else {
                            // Fix 4: For VOUT (1) / VIN (3) channels the only valid mux
                            // is LF_TO_AGND (0). Force it there and disable the dropdown.
                            let force_mux_zero = ch.function == 1 || ch.function == 3;
                            if force_mux_zero && ch.adc_mux != 0 {
                                send_adc_config(ch_idx, 0, ch.adc_range, ch.adc_rate);
                            }
                            view! {
                                <div class="io-body">
                                    <div class="io-primary">
                                        <Readout
                                            label=quantity
                                            value=Signal::derive(move || format!("{:.4}", live(i).0))
                                            unit=unit
                                            size="lg"
                                        />
                                        <div class="io-meta">
                                            <span>"Raw "<span class="io-code">{move || format!("0x{:06X}", live(i).1)}</span></span>
                                            <span>"Code "{move || format!("{}", live(i).1)}</span>
                                        </div>
                                    </div>
                                    <div class="meter io-meter" aria-hidden="true">
                                        <span style=move || format!("width: {}%", pct())></span>
                                    </div>

                                    <ChannelSparkline
                                        values=Signal::from(hist)
                                        min=Signal::derive(move || bar_min)
                                        max=Signal::derive(move || bar_max)
                                        color_var=ch_var(i)
                                    />

                                    <div class="rows io-rows">
                                        <label class="row">
                                            <span class="row-label">"Range"</span>
                                            <select
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
                                        </label>
                                        <label class="row">
                                            <span class="row-label">"Rate"</span>
                                            <select
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
                                        </label>
                                        <label class="row">
                                            <span class="row-label">"Mux"</span>
                                            <select
                                                prop:value=move || if force_mux_zero { "0".to_string() } else { ch.adc_mux.to_string() }
                                                prop:disabled=force_mux_zero
                                                on:change=move |e| {
                                                    if force_mux_zero { return; }
                                                    let mux: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                    send_adc_config(ch_idx, mux, ch.adc_range, ch.adc_rate);
                                                }
                                            >
                                                {ADC_MUX_OPTIONS.iter().map(|(code, name)| {
                                                    view! { <option value=code.to_string()>{*name}</option> }
                                                }).collect::<Vec<_>>()}
                                            </select>
                                        </label>
                                        {if is_res { Some(view! {
                                            <label class="row">
                                                <span class="row-label">"Excitation"</span>
                                                <select
                                                    prop:value=exc_ua.to_string()
                                                    on:change=move |e| {
                                                        let ua: u16 = event_target_value(&e).parse().unwrap_or(1000);
                                                        tauri_bridge::send_set_rtd_config(ch_idx, ua);
                                                    }
                                                >
                                                    {RTD_EXCITATION_OPTIONS.iter().map(|(ua, name)| {
                                                        view! { <option value=ua.to_string()>{*name}</option> }
                                                    }).collect::<Vec<_>>()}
                                                </select>
                                            </label>
                                        })} else { None }}
                                    </div>
                                </div>
                            }.into_any()
                        };

                        view! {
                            <section class="group io-card" data-ch=ch_key(i)>
                                <ChannelHead idx=i func=func_name(ch.function) />
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
struct AdcConfigArgs {
    channel: u8,
    mux: u8,
    range: u8,
    rate: u8,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AdcStartArgs {
    channel_mask: u8,
    divider: u8,
}

fn send_adc_config(ch: u8, mux: u8, range: u8, rate: u8) {
    // Stop ADC stream, apply config, restart stream to avoid conflicts.
    // (Bug 6) — must restart the stream after config, or UI reads go stale.
    spawn_local(async move {
        let _ = try_invoke("stop_adc_stream", wasm_bindgen::JsValue::NULL).await;
        sleep_ms(200).await;

        let args = serde_wasm_bindgen::to_value(&AdcConfigArgs {
            channel: ch,
            mux,
            range,
            rate,
        })
        .unwrap();
        let _ = try_invoke("set_adc_config", args).await;

        // Resume stream for all 4 channels (mask 0b1111). Divider 0 = default rate.
        let start_args = serde_wasm_bindgen::to_value(&AdcStartArgs {
            channel_mask: 0x0F,
            divider: 0,
        })
        .unwrap();
        STREAM_STARTED.store(true, std::sync::atomic::Ordering::Relaxed);
        let _ = try_invoke("start_adc_stream", start_args).await;
        log(&format!(
            "[send_adc_config] ch={} mux={} range={} rate={} (stream restarted)",
            ch, mux, range, rate
        ));
    });
}

async fn sleep_ms(ms: u32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window()
            .unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms as i32)
            .unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(promise).await.unwrap();
}
