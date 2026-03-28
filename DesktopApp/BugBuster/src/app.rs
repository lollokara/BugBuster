use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::prelude::*;

use crate::tauri_bridge::*;
use crate::tabs::{overview::*, adc::*, diag::*, vdac::*, idac::*, din::*, dout::*, faults::*, gpio::*, uart::*, scope::*, wavegen::*};

const TABS: &[(&str, &str)] = &[
    ("overview", "Overview"),
    ("adc", "ADC"),
    ("diag", "Diagnostics"),
    ("vdac", "VDAC"),
    ("idac", "IDAC"),
    ("din", "DIN"),
    ("dout", "DOUT"),
    ("faults", "Faults"),
    ("gpio", "GPIO"),
    ("uart", "UART"),
    ("scope", "Scope"),
    ("wavegen", "WaveGen"),
];

#[component]
pub fn App() -> impl IntoView {
    let (devices, set_devices) = signal(Vec::<DiscoveredDevice>::new());
    let (conn_mode, set_conn_mode) = signal("Disconnected".to_string());
    let (conn_addr, set_conn_addr) = signal(String::new());
    let (scanning, set_scanning) = signal(false);
    let (device_state, set_device_state) = signal(DeviceState::default());
    let (active_tab, set_active_tab) = signal("overview".to_string());

    // Scan for devices
    let scan = move |_: ev::MouseEvent| {
        set_scanning.set(true);
        spawn_local(async move {
            let result = invoke("discover_devices", JsValue::NULL).await;
            if let Ok(devs) = serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(result) {
                set_devices.set(devs);
            }
            set_scanning.set(false);
        });
    };

    let disconnect = move |_: ev::MouseEvent| {
        spawn_local(async move { invoke("disconnect_device", JsValue::NULL).await; });
        set_conn_mode.set("Disconnected".to_string());
        set_conn_addr.set(String::new());
        set_device_state.set(DeviceState::default());
    };

    // Event listeners
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<DeviceState>>(event) {
                set_device_state.set(evt.payload);
            }
        });
        listen("device-state", &closure).await;
        closure.forget();
    });

    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<ConnectionStatus>>(event) {
                set_conn_mode.set(evt.payload.mode.clone());
                set_conn_addr.set(evt.payload.port_or_url.clone());
            }
        });
        listen("connection-status", &closure).await;
        closure.forget();
    });

    // Auto-scan
    spawn_local(async move {
        let result = invoke("discover_devices", JsValue::NULL).await;
        if let Ok(devs) = serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(result) {
            set_devices.set(devs);
        }
    });

    view! {
        <div class="app">
            // Header
            <header class="header">
                <div class="header-left">
                    <span class="logo-text">"BugBuster"</span>
                    <span class="subtitle">"AD74416H Controller"</span>
                </div>
                <div class="header-right">
                    {move || {
                        let m = conn_mode.get();
                        if m == "Disconnected" {
                            view! {
                                <div class="status-bar">
                                    <span class="status-dot disconnected"></span>
                                    <span class="status-text">"Disconnected"</span>
                                </div>
                            }.into_any()
                        } else {
                            let badge = if m == "Usb" { "USB" } else { "HTTP" };
                            view! {
                                <div class="status-bar">
                                    <span class="status-dot connected"></span>
                                    <span class="status-badge">{badge}</span>
                                    <span class="status-text">{move || conn_addr.get()}</span>
                                    <span class="status-separator">"|"</span>
                                    <span class={move || if device_state.get().spi_ok { "spi-ok" } else { "spi-err" }}>
                                        {move || if device_state.get().spi_ok { "SPI OK" } else { "SPI ERR" }}
                                    </span>
                                    <span class="status-separator">"|"</span>
                                    <span class="temp-value">
                                        {move || format!("{:.1} °C", device_state.get().die_temperature)}
                                    </span>
                                    <span class="status-separator">"|"</span>
                                    <button class="btn btn-danger btn-xs" on:click=disconnect>"Disconnect"</button>
                                </div>
                            }.into_any()
                        }
                    }}
                </div>
            </header>

            // Connection panel (when disconnected)
            <Show when=move || conn_mode.get() == "Disconnected">
                <div class="connection-panel">
                    <div class="card" style="max-width: 500px; width: 100%; text-align: center;">
                        <h2>"Connect to BugBuster"</h2>
                        <p class="hint">"Scanning for devices on USB and network..."</p>
                        <button class="btn btn-primary" on:click=scan disabled=move || scanning.get()>
                            {move || if scanning.get() { "Scanning..." } else { "Scan for Devices" }}
                        </button>
                        <div class="device-list">
                            <For
                                each=move || devices.get()
                                key=|dev| dev.id.clone()
                                children=move |dev: DiscoveredDevice| {
                                    let id = dev.id.clone();
                                    let icon = if dev.transport == "usb" { "🔌" } else { "📡" };
                                    let name = dev.name.clone();
                                    let addr = dev.address.clone();
                                    let tbadge = dev.transport.to_uppercase();
                                    view! {
                                        <button class="device-item" on:click=move |_| {
                                            do_connect(id.clone());
                                        }>
                                            <span class="device-icon">{icon}</span>
                                            <div class="device-info">
                                                <span class="device-name">{name}</span>
                                                <span class="device-addr">{addr}</span>
                                            </div>
                                            <span class="device-transport">{tbadge}</span>
                                        </button>
                                    }
                                }
                            />
                        </div>
                    </div>
                </div>
            </Show>

            // Main content (when connected)
            <Show when=move || conn_mode.get() != "Disconnected">
                // Tab bar
                <nav class="tab-bar">
                    {TABS.iter().map(|(id, label)| {
                        let id_str = id.to_string();
                        let id_click = id_str.clone();
                        view! {
                            <button class="tab-item"
                                class:active=move || active_tab.get() == id_str
                                on:click=move |_| set_active_tab.set(id_click.clone())
                            >{*label}</button>
                        }
                    }).collect::<Vec<_>>()}
                </nav>

                // Tab content
                <div class="tab-container">
                    {move || match active_tab.get().as_str() {
                        "overview" => view! { <OverviewTab state=device_state /> }.into_any(),
                        "adc" => view! { <AdcTab state=device_state /> }.into_any(),
                        "diag" => view! { <DiagTab state=device_state /> }.into_any(),
                        "vdac" => view! { <VdacTab state=device_state /> }.into_any(),
                        "idac" => view! { <IdacTab state=device_state /> }.into_any(),
                        "din" => view! { <DinTab state=device_state /> }.into_any(),
                        "dout" => view! { <DoutTab state=device_state /> }.into_any(),
                        "faults" => view! { <FaultsTab state=device_state /> }.into_any(),
                        "gpio" => view! { <GpioTab state=device_state /> }.into_any(),
                        "uart" => view! { <UartTab /> }.into_any(),
                        "scope" => view! { <ScopeTab state=device_state /> }.into_any(),
                        "wavegen" => view! { <WavegenTab /> }.into_any(),
                        _ => view! { <div>"Unknown tab"</div> }.into_any(),
                    }}
                </div>
            </Show>
        </div>
    }
}

fn do_connect(device_id: String) {
    use serde::Serialize;
    #[derive(Serialize)]
    struct Args { #[serde(rename = "deviceId")] device_id: String }
    spawn_local(async move {
        log(&format!("Connecting to: {}", device_id));
        let args = serde_wasm_bindgen::to_value(&Args { device_id }).unwrap();
        let _result = invoke("connect_device", args).await;
    });
}
