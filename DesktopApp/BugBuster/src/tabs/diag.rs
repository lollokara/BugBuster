use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

const ALERT_BITS: &[(usize, &str, &str)] = &[
    (0, "RESET", "amber"), (1, "CAL_MEM", "rose"), (2, "SPI_CRC", "rose"),
    (3, "SPI_SCLK", "rose"), (4, "ADC_ERR", "amber"), (5, "SUPPLY", "rose"),
    (6, "TEMP", "amber"), (7, "CH_D", "blue"), (8, "CH_C", "blue"),
    (9, "CH_B", "blue"), (10, "CH_A", "blue"),
];

const SUPPLY_BITS: &[(usize, &str, &str)] = &[
    (0, "AVDD_HI", "rose"), (1, "AVDD_LO", "rose"), (2, "AVSS", "rose"),
    (3, "AVCC", "amber"), (4, "DVCC", "amber"), (5, "IOVDD", "amber"), (6, "REFIO", "amber"),
];

fn led_color(name: &str) -> &'static str {
    match name {
        "rose" => "var(--rose)",
        "amber" => "var(--amber)",
        "blue" => "var(--blue)",
        _ => "var(--green)",
    }
}

#[component]
pub fn DiagTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="tab-content">
            // Top row: Temp + Status LEDs
            <div class="diag-top">
                // Temperature
                <div class="card diag-temp-card">
                    <div class="temp-gauge-wrap">
                        <div class="temp-value-hero">
                            {move || format!("{:.1}", state.get().die_temperature)}
                            <span class="temp-unit">"°C"</span>
                        </div>
                        <div class="temp-thermometer">
                            <div class="temp-thermo-fill" style=move || {
                                let t = state.get().die_temperature;
                                let pct = ((t + 25.0) / 150.0 * 100.0).clamp(0.0, 100.0);
                                let color = if t > 100.0 { "var(--rose)" } else if t > 70.0 { "var(--amber)" } else { "var(--green)" };
                                format!("height: {}%; background: linear-gradient(to top, {}, {}88)", pct, color, color)
                            }></div>
                        </div>
                        <div class="temp-label" style=move || {
                            let t = state.get().die_temperature;
                            let color = if t > 100.0 { "var(--rose)" } else if t > 70.0 { "var(--amber)" } else { "var(--green)" };
                            format!("color: {}", color)
                        }>
                            {move || {
                                let t = state.get().die_temperature;
                                if t > 100.0 { "HOT" } else if t > 70.0 { "Warm" } else { "Normal" }
                            }}
                        </div>
                    </div>
                </div>

                // Alert Status
                <div class="card">
                    <div class="card-header"><span>"Alert Status"</span>
                        <span class="badge-hex">{move || format!("0x{:04X}", state.get().alert_status)}</span>
                    </div>
                    <div class="card-body status-led-grid">
                        {ALERT_BITS.iter().map(|(bit, name, color)| {
                            let bit = *bit;
                            let lc = led_color(color);
                            view! {
                                <div class="status-led-item">
                                    <div class="led"
                                        class:led-on=move || (state.get().alert_status >> bit) & 1 != 0
                                        style=move || if (state.get().alert_status >> bit) & 1 != 0 {
                                            format!("background: {}; box-shadow: 0 0 8px {}", lc, lc)
                                        } else { String::new() }
                                    ></div>
                                    <span class="led-label">{*name}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>

                // Supply Alert
                <div class="card">
                    <div class="card-header"><span>"Supply Status"</span>
                        <span class="badge-hex">{move || format!("0x{:04X}", state.get().supply_alert_status)}</span>
                    </div>
                    <div class="card-body status-led-grid">
                        {SUPPLY_BITS.iter().map(|(bit, name, color)| {
                            let bit = *bit;
                            let lc = led_color(color);
                            view! {
                                <div class="status-led-item">
                                    <div class="led"
                                        class:led-on=move || (state.get().supply_alert_status >> bit) & 1 != 0
                                        style=move || if (state.get().supply_alert_status >> bit) & 1 != 0 {
                                            format!("background: {}; box-shadow: 0 0 8px {}", lc, lc)
                                        } else { String::new() }
                                    ></div>
                                    <span class="led-label">{*name}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>
            </div>

            // Diagnostic slots with source dropdowns
            <h3 class="section-title">"Diagnostic Channels"</h3>
            <div class="channel-grid">
                {move || {
                    let ds = state.get();
                    ds.diag.into_iter().enumerate().map(|(i, d)| {
                        let slot = i as u8;
                        let source_name = DIAG_SOURCE_OPTIONS.iter()
                            .find(|(c, _)| *c == d.source)
                            .map(|(_, n)| *n).unwrap_or("?");
                        let unit = if d.source == 1 { "°C" } else { "V" };
                        let color = CH_COLORS[i];

                        view! {
                            <div class="card">
                                <div class="card-header">
                                    <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                        {format!("Slot {}", i)}
                                    </div>
                                    <select class="dropdown dropdown-sm"
                                        prop:value=d.source.to_string()
                                        on:change=move |e| {
                                            let src: u8 = event_target_value(&e).parse().unwrap_or(0);
                                            #[derive(Serialize)]
                                            struct Args { slot: u8, source: u8 }
                                            let args = serde_wasm_bindgen::to_value(&Args { slot, source: src }).unwrap();
                                            invoke_void("set_diag_config", args);
                                        }
                                    >
                                        {DIAG_SOURCE_OPTIONS.iter().map(|(code, name)| {
                                            view! { <option value=code.to_string()>{*name}</option> }
                                        }).collect::<Vec<_>>()}
                                    </select>
                                </div>
                                <div class="card-body">
                                    <div class="big-value">{format!("{:.3}", d.value)}<span class="unit">{unit}</span></div>
                                    <div class="card-details">
                                        <span>"Source: "{source_name}</span>
                                        <span>"Raw: 0x"{format!("{:04X}", d.raw_code)}</span>
                                    </div>
                                    <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                        <div class="bar-fill-dynamic" style={
                                            let pct = if d.source == 1 {
                                                ((d.value + 25.0) / 150.0 * 100.0).clamp(0.0, 100.0)
                                            } else {
                                                (d.value / 5.0 * 100.0).clamp(0.0, 100.0)
                                            };
                                            format!("width: {}%", pct)
                                        }></div>
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
