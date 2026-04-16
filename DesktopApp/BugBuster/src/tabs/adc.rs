use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use crate::tauri_bridge::{self, *};
use crate::components::channel_sparkline::ChannelSparkline;

const SPARK_CAP: usize = 120;

#[component]
pub fn AdcTab(state: ReadSignal<DeviceState>) -> impl IntoView {
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

    view! {
        <div class="tab-content">
            <div class="tab-desc">"Analog-to-Digital Converter readings for all 4 channels. Configure the ADC range, sampling rate, and input multiplexer per channel. Values update in real-time."</div>
            <div class="channel-grid-wide">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let has_adc = matches!(ch.function, 3 | 4 | 5 | 7 | 11 | 12);
                        let color = CH_COLORS[i];
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
                        let pct = if span > 0.0 { ((ch.adc_value - bar_min) / span * 100.0).clamp(0.0, 100.0) } else { 0.0 };
                        let unit = if matches!(ch.function, 4 | 5 | 11 | 12) { "mA" } else if is_res { "Ω" } else { "V" };
                        let exc_ua = if ch.rtd_excitation_ua > 0 { ch.rtd_excitation_ua } else { 1000 };
                        let hist = history[i];
                        let spark_color = color.to_string();

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

                                                <ChannelSparkline
                                                    values=Signal::from(hist)
                                                    min=Signal::derive(move || bar_min)
                                                    max=Signal::derive(move || bar_max)
                                                    color=spark_color
                                                />

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
                                                        // Fix 4: For VOUT (1) / VIN (3) channels the only valid mux
                                                        // is LF_TO_AGND (0). Force it there and disable the dropdown.
                                                        {
                                                            let force_mux_zero = ch.function == 1 || ch.function == 3;
                                                            if force_mux_zero && ch.adc_mux != 0 {
                                                                send_adc_config(ch_idx, 0, ch.adc_range, ch.adc_rate);
                                                            }
                                                            view! {
                                                                <select class="dropdown"
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
                                                            }
                                                        }
                                                    </div>
                                                    {if is_res { Some(view! {
                                                        <div class="config-row">
                                                            <label>"Excitation"</label>
                                                            <select class="dropdown"
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
                                                        </div>
                                                    })} else { None }}
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

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AdcStartArgs { channel_mask: u8, divider: u8 }

fn send_adc_config(ch: u8, mux: u8, range: u8, rate: u8) {
    // Stop ADC stream, apply config, restart stream to avoid conflicts.
    // (Bug 6) — must restart the stream after config, or UI reads go stale.
    spawn_local(async move {
        let _ = invoke("stop_adc_stream", wasm_bindgen::JsValue::NULL).await;
        sleep_ms(200).await;

        let args = serde_wasm_bindgen::to_value(&AdcConfigArgs { channel: ch, mux, range, rate }).unwrap();
        let _ = invoke("set_adc_config", args).await;

        // Resume stream for all 4 channels (mask 0b1111). Divider 0 = default rate.
        let start_args = serde_wasm_bindgen::to_value(&AdcStartArgs { channel_mask: 0x0F, divider: 0 }).unwrap();
        let _ = invoke("start_adc_stream", start_args).await;
        log(&format!("[send_adc_config] ch={} mux={} range={} rate={} (stream restarted)", ch, mux, range, rate));
    });
}

async fn sleep_ms(ms: u32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window().unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms as i32)
            .unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(promise).await.unwrap();
}
