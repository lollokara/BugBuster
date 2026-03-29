use leptos::prelude::*;
use serde::Serialize;
use wasm_bindgen::JsValue;
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
            <div class="tab-desc">"Internal diagnostic ADC channels. Select what to measure per slot: die temperature, supply voltages (DVCC, AVCC, AVDD), or sense voltages. Useful for verifying power rail health."</div>
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
                                            let src_name = DIAG_SOURCE_OPTIONS.iter()
                                                .find(|(c, _)| *c == src)
                                                .map(|(_, n)| *n).unwrap_or("?");
                                            let label = format!("Set Diag {} to {}", slot, src_name);
                                            invoke_with_feedback("set_diag_config", args, &label);
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

            // WiFi section
            <h3 class="section-title">"WiFi"</h3>
            <WifiSection />
        </div>
    }
}

#[component]
fn WifiSection() -> impl IntoView {
    let wifi = RwSignal::new(WifiState::default());
    let connect_ssid = RwSignal::new(String::new());
    let connect_pass = RwSignal::new(String::new());
    let connect_status = RwSignal::new(String::new());

    // Poll WiFi status every 2 seconds
    let poll = move || {
        leptos::task::spawn_local(async move {
            if let Some(ws) = fetch_wifi_status().await {
                wifi.set(ws);
            }
        });
    };
    poll();
    let _interval = leptos::prelude::set_interval_with_handle(
        move || poll(),
        std::time::Duration::from_secs(2),
    );

    view! {
        <div class="channel-grid" style="grid-template-columns: 1fr 1fr">
            // AP Mode card
            <div class="card">
                <div class="card-header"><span>"Access Point"</span></div>
                <div class="card-body">
                    <div class="card-details" style="gap: 0.5rem">
                        <div>"SSID: "<strong>{move || wifi.get().ap_ssid.clone()}</strong></div>
                        <div>"IP: "<span class="mono">{move || wifi.get().ap_ip.clone()}</span></div>
                        <div>"MAC: "<span class="mono">{move || wifi.get().ap_mac.clone()}</span></div>
                    </div>
                </div>
            </div>

            // STA Mode card
            <div class="card">
                <div class="card-header">
                    <span>"Station"</span>
                    <div class="led"
                        class:led-on=move || wifi.get().connected
                        style=move || if wifi.get().connected {
                            "background: var(--green); box-shadow: 0 0 8px var(--green)".to_string()
                        } else {
                            String::new()
                        }
                    ></div>
                </div>
                <div class="card-body">
                    <div class="card-details" style="gap: 0.5rem">
                        <div>"Status: "
                            <strong style=move || if wifi.get().connected {
                                "color: var(--green)"
                            } else { "color: var(--rose)" }>
                                {move || if wifi.get().connected { "Connected" } else { "Disconnected" }}
                            </strong>
                        </div>
                        <div>"SSID: "<strong>{move || wifi.get().sta_ssid.clone()}</strong></div>
                        <div>"IP: "<span class="mono">{move || wifi.get().sta_ip.clone()}</span></div>
                        <div style="display: flex; align-items: center; gap: 0.5rem">
                            "RSSI: "<span class="mono">{move || format!("{} dBm", wifi.get().rssi)}</span>
                            <div class="bar-gauge" style="flex: 1; --bar-color: var(--blue)">
                                <div class="bar-fill-dynamic" style=move || {
                                    let rssi = wifi.get().rssi;
                                    let pct = (((rssi + 100) as f32 / 60.0) * 100.0).clamp(0.0, 100.0);
                                    format!("width: {}%", pct)
                                }></div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>

        // Connect form
        <div class="card" style="margin-top: 0.5rem">
            <div class="card-header"><span>"Connect to Network"</span></div>
            <div class="card-body">
                <div style="display: flex; gap: 0.5rem; align-items: center; flex-wrap: wrap">
                    <input type="text" class="input" placeholder="SSID"
                        style="flex: 1; min-width: 120px"
                        prop:value=move || connect_ssid.get()
                        on:input=move |e| connect_ssid.set(event_target_value(&e))
                    />
                    <input type="password" class="input" placeholder="Password"
                        style="flex: 1; min-width: 120px"
                        prop:value=move || connect_pass.get()
                        on:input=move |e| connect_pass.set(event_target_value(&e))
                    />
                    <button class="btn btn-sm"
                        on:click=move |_| {
                            let ssid = connect_ssid.get();
                            let pass = connect_pass.get();
                            if ssid.is_empty() {
                                connect_status.set("Enter an SSID".to_string());
                                return;
                            }
                            connect_status.set("Connecting...".to_string());
                            leptos::task::spawn_local(async move {
                                #[derive(serde::Serialize)]
                                struct Args { ssid: String, password: String }
                                let args = serde_wasm_bindgen::to_value(
                                    &Args { ssid: ssid.clone(), password: pass }
                                ).unwrap();
                                let result = invoke("wifi_connect", args).await;
                                let ok: bool = serde_wasm_bindgen::from_value(result).unwrap_or(false);
                                if ok {
                                    connect_status.set(format!("Connected to {}", ssid));
                                } else {
                                    connect_status.set(format!("Failed to connect to {}", ssid));
                                }
                            });
                        }
                    >"Connect"</button>
                </div>
                <div class="text-xs" style="margin-top: 0.5rem; color: var(--text3)">
                    {move || connect_status.get()}
                </div>
            </div>
        </div>
    }
}
