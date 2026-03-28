use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::{Deserialize, Serialize};
use wasm_bindgen::JsValue;
use crate::tauri_bridge::*;

const BAUD_OPTIONS: &[u32] = &[300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
const PARITY_OPTIONS: &[(u8, &str)] = &[(0, "None"), (1, "Odd"), (2, "Even")];
const STOP_BITS_OPTIONS: &[(u8, &str)] = &[(0, "1"), (1, "1.5"), (2, "2")];

// Note: UART bridge config is not yet in the device state polling.
// This tab uses local state for now. A full implementation would
// poll via a dedicated command or add UART config to DeviceState.

#[component]
pub fn UartTab() -> impl IntoView {
    let (baud, set_baud) = signal(115200u32);
    let (data_bits, set_data_bits) = signal(8u8);
    let (parity, set_parity) = signal(0u8);
    let (stop_bits, set_stop_bits) = signal(0u8);
    let (enabled, set_enabled) = signal(true);
    let (status_msg, set_status_msg) = signal(String::new());

    view! {
        <div class="tab-content">
            <div class="uart-layout">
                <div class="card uart-card">
                    <div class="card-header">
                        <span>"UART Bridge #0"</span>
                        <div class={move || if enabled.get() { "uart-status uart-active" } else { "uart-status" }}>
                            {move || if enabled.get() { "Active" } else { "Disabled" }}
                        </div>
                    </div>
                    <div class="card-body">
                        <p class="uart-desc">"Transparent bridge: USB CDC #1 ↔ ESP32 UART"</p>

                        <div class="config-section">
                            <div class="config-row">
                                <label>"Baud Rate"</label>
                                <select class="dropdown"
                                    prop:value={move || baud.get().to_string()}
                                    on:change=move |e| set_baud.set(event_target_value(&e).parse().unwrap_or(115200))
                                >
                                    {BAUD_OPTIONS.iter().map(|b| {
                                        view! { <option value=b.to_string()>{format!("{}", b)}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Data Bits"</label>
                                <select class="dropdown"
                                    prop:value={move || data_bits.get().to_string()}
                                    on:change=move |e| set_data_bits.set(event_target_value(&e).parse().unwrap_or(8))
                                >
                                    {[5u8, 6, 7, 8].iter().map(|b| {
                                        view! { <option value=b.to_string()>{format!("{}", b)}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Parity"</label>
                                <select class="dropdown"
                                    prop:value={move || parity.get().to_string()}
                                    on:change=move |e| set_parity.set(event_target_value(&e).parse().unwrap_or(0))
                                >
                                    {PARITY_OPTIONS.iter().map(|(c, n)| {
                                        view! { <option value=c.to_string()>{*n}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Stop Bits"</label>
                                <select class="dropdown"
                                    prop:value={move || stop_bits.get().to_string()}
                                    on:change=move |e| set_stop_bits.set(event_target_value(&e).parse().unwrap_or(0))
                                >
                                    {STOP_BITS_OPTIONS.iter().map(|(c, n)| {
                                        view! { <option value=c.to_string()>{*n}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Enabled"</label>
                                <label class="toggle-wrap">
                                    <div class="toggle" class:active={move || enabled.get()}
                                        on:click=move |_| set_enabled.update(|e| *e = !*e)
                                    ><div class="toggle-thumb"></div></div>
                                </label>
                            </div>
                        </div>

                        <div class="uart-summary">
                            <span class="uart-config-str">
                                {move || format!("{} {}{}{}", baud.get(), data_bits.get(),
                                    match parity.get() { 1 => "O", 2 => "E", _ => "N" },
                                    match stop_bits.get() { 1 => "1.5", 2 => "2", _ => "1" }
                                )}
                            </span>
                        </div>

                        {move || {
                            let msg = status_msg.get();
                            if !msg.is_empty() {
                                Some(view! { <p class="uart-status-msg">{msg}</p> })
                            } else { None }
                        }}
                    </div>
                </div>

                <div class="card uart-info-card">
                    <div class="card-header"><span>"Connection Info"</span></div>
                    <div class="card-body">
                        <div class="uart-info-item">
                            <span class="uart-info-label">"Host Side"</span>
                            <span class="uart-info-value">"USB CDC #1 (second serial port)"</span>
                        </div>
                        <div class="uart-info-item">
                            <span class="uart-info-label">"Device Side"</span>
                            <span class="uart-info-value">"ESP32 UART1 (TX: GPIO17, RX: GPIO18)"</span>
                        </div>
                        <div class="uart-info-item">
                            <span class="uart-info-label">"Note"</span>
                            <span class="uart-info-value">"CDC #1 may not appear if the ESP32's TinyUSB CDC count is set to 1 in sdkconfig. Check CONFIG_TINYUSB_CDC_COUNT."</span>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    }
}
