use leptos::task::spawn_local;
use leptos::prelude::*;
use leptos::prelude::Callback;
use serde::{Deserialize, Serialize};
use wasm_bindgen::prelude::*;

// -----------------------------------------------------------------------------
// Tauri bridge
// -----------------------------------------------------------------------------

#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = ["window", "__TAURI__", "core"])]
    async fn invoke(cmd: &str, args: JsValue) -> JsValue;

    #[wasm_bindgen(js_namespace = ["window", "__TAURI__", "event"])]
    async fn listen(event: &str, handler: &Closure<dyn FnMut(JsValue)>) -> JsValue;
}

// -----------------------------------------------------------------------------
// Shared types (must match src-tauri/src/state.rs)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DiscoveredDevice {
    pub id: String,
    pub name: String,
    pub transport: String,
    pub address: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ConnectionStatus {
    pub mode: String,
    pub port_or_url: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ChannelState {
    pub function: u8,
    pub adc_raw: u32,
    pub adc_value: f32,
    pub adc_range: u8,
    pub adc_rate: u8,
    pub adc_mux: u8,
    pub dac_code: u16,
    pub dac_value: f32,
    pub din_state: bool,
    pub din_counter: u32,
    pub do_state: bool,
    pub channel_alert: u16,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DeviceState {
    pub spi_ok: bool,
    pub die_temperature: f32,
    pub alert_status: u16,
    pub supply_alert_status: u16,
    pub channels: Vec<ChannelState>,
}

// Helper to parse Tauri event payload
#[derive(Debug, Clone, Deserialize)]
struct TauriEvent<T> {
    payload: T,
}

// -----------------------------------------------------------------------------
// Channel function names
// -----------------------------------------------------------------------------

fn func_name(code: u8) -> &'static str {
    match code {
        0 => "HIGH_IMP",
        1 => "VOUT",
        2 => "IOUT",
        3 => "VIN",
        4 => "IIN_EXT",
        5 => "IIN_LOOP",
        7 => "RES_MEAS",
        8 => "DIN_LOGIC",
        9 => "DIN_LOOP",
        10 => "IOUT_HART",
        11 => "IIN_EXT_HART",
        12 => "IIN_LOOP_HART",
        _ => "UNKNOWN",
    }
}

// -----------------------------------------------------------------------------
// App Component
// -----------------------------------------------------------------------------

#[component]
pub fn App() -> impl IntoView {
    // Connection state
    let (devices, set_devices) = signal(Vec::<DiscoveredDevice>::new());
    let (conn_mode, set_conn_mode) = signal("Disconnected".to_string());
    let (conn_addr, set_conn_addr) = signal(String::new());
    let (scanning, set_scanning) = signal(false);

    // Device state (updated via Tauri events)
    let (device_state, set_device_state) = signal(DeviceState::default());

    // Scan for devices
    let scan = move |_| {
        set_scanning.set(true);
        spawn_local(async move {
            let result = invoke("discover_devices", JsValue::NULL).await;
            if let Ok(devs) = serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(result) {
                set_devices.set(devs);
            }
            set_scanning.set(false);
        });
    };

    // Connect to a device
    let connect = move |device_id: String| {
        #[derive(Serialize)]
        struct ConnectArgs {
            #[serde(rename = "deviceId")]
            device_id: String,
        }
        spawn_local(async move {
            let args = serde_wasm_bindgen::to_value(&ConnectArgs { device_id }).unwrap();
            let result = invoke("connect_device", args).await;
            if let Ok(status) = serde_wasm_bindgen::from_value::<ConnectionStatus>(result) {
                set_conn_mode.set(status.mode.clone());
                set_conn_addr.set(status.port_or_url.clone());
            }
        });
    };

    // Disconnect
    let disconnect = move |_| {
        spawn_local(async move {
            invoke("disconnect_device", JsValue::NULL).await;
            set_conn_mode.set("Disconnected".to_string());
            set_conn_addr.set(String::new());
            set_device_state.set(DeviceState::default());
        });
    };

    // Listen for device-state events from Tauri backend
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<DeviceState>>(event) {
                set_device_state.set(evt.payload);
            }
        });
        listen("device-state", &closure).await;
        closure.forget(); // Keep the closure alive
    });

    // Listen for connection-status events
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

    // Auto-scan on mount
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
                    <StatusIndicator mode=conn_mode addr=conn_addr state=device_state />
                </div>
            </header>

            // Main content
            <div class="content">
                {move || {
                    let mode = conn_mode.get();
                    if mode == "Disconnected" {
                        view! {
                            <ConnectionPanel
                                devices=devices
                                scanning=scanning
                                on_scan=Callback::new(scan)
                                on_connect=Callback::new(connect)
                            />
                        }.into_any()
                    } else {
                        view! {
                            <DevicePanel
                                state=device_state
                                on_disconnect=Callback::new(disconnect)
                            />
                        }.into_any()
                    }
                }}
            </div>
        </div>
    }
}

// -----------------------------------------------------------------------------
// Status Indicator (header bar)
// -----------------------------------------------------------------------------

#[component]
fn StatusIndicator(
    mode: ReadSignal<String>,
    addr: ReadSignal<String>,
    state: ReadSignal<DeviceState>,
) -> impl IntoView {
    view! {
        <div class="status-bar">
            {move || {
                let m = mode.get();
                let ds = state.get();
                if m == "Disconnected" {
                    view! {
                        <span class="status-dot disconnected"></span>
                        <span class="status-text">"Disconnected"</span>
                    }.into_any()
                } else {
                    let transport_badge = if m == "Usb" { "USB" } else { "HTTP" };
                    view! {
                        <span class="status-dot connected"></span>
                        <span class="status-badge">{transport_badge}</span>
                        <span class="status-text">{move || addr.get()}</span>
                        <span class="status-separator">"|"</span>
                        <span class={move || if state.get().spi_ok { "spi-ok" } else { "spi-err" }}>
                            {move || if state.get().spi_ok { "SPI OK" } else { "SPI ERR" }}
                        </span>
                        <span class="status-separator">"|"</span>
                        <span class="temp-value">
                            {move || format!("{:.1} °C", state.get().die_temperature)}
                        </span>
                    }.into_any()
                }
            }}
        </div>
    }
}

// -----------------------------------------------------------------------------
// Connection Panel (shown when disconnected)
// -----------------------------------------------------------------------------

#[component]
fn ConnectionPanel(
    devices: ReadSignal<Vec<DiscoveredDevice>>,
    scanning: ReadSignal<bool>,
    #[prop(into)] on_scan: leptos::prelude::Callback<()>,
    #[prop(into)] on_connect: leptos::prelude::Callback<String>,
) -> impl IntoView {
    view! {
        <div class="connection-panel">
            <div class="card">
                <h2>"Connect to BugBuster"</h2>
                <p class="hint">"Scanning for devices on USB and network..."</p>

                <button
                    class="btn btn-primary"
                    on:click=move |_| on_scan.run(())
                    disabled=move || scanning.get()
                >
                    {move || if scanning.get() { "Scanning..." } else { "Scan for Devices" }}
                </button>

                <div class="device-list">
                    {move || {
                        let devs = devices.get();
                        if devs.is_empty() {
                            view! {
                                <p class="no-devices">"No devices found. Connect via USB or join the BugBuster WiFi network."</p>
                            }.into_any()
                        } else {
                            view! {
                                <ul>
                                    {devs.into_iter().map(|dev| {
                                        let id = dev.id.clone();
                                        let icon = if dev.transport == "usb" { "🔌" } else { "📡" };
                                        view! {
                                            <li class="device-item" on:click=move |_| on_connect.run(id.clone())>
                                                <span class="device-icon">{icon}</span>
                                                <div class="device-info">
                                                    <span class="device-name">{dev.name.clone()}</span>
                                                    <span class="device-addr">{dev.address.clone()}</span>
                                                </div>
                                                <span class="device-transport">{dev.transport.to_uppercase()}</span>
                                            </li>
                                        }
                                    }).collect::<Vec<_>>()}
                                </ul>
                            }.into_any()
                        }
                    }}
                </div>
            </div>
        </div>
    }
}

// -----------------------------------------------------------------------------
// Device Panel (shown when connected)
// -----------------------------------------------------------------------------

#[component]
fn DevicePanel(
    state: ReadSignal<DeviceState>,
    #[prop(into)] on_disconnect: leptos::prelude::Callback<()>,
) -> impl IntoView {
    view! {
        <div class="device-panel">
            // Quick disconnect button
            <div class="toolbar">
                <button class="btn btn-danger btn-sm" on:click=move |_| on_disconnect.run(())>
                    "Disconnect"
                </button>
            </div>

            // Channel overview cards
            <div class="channel-grid">
                {move || {
                    let ds = state.get();
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        view! {
                            <div class="card channel-card">
                                <div class="card-header">
                                    <span class="channel-label">{format!("CH {}", ['A', 'B', 'C', 'D'][i])}</span>
                                    <span class="channel-func">{func_name(ch.function)}</span>
                                </div>
                                <div class="card-body">
                                    <div class="big-value">
                                        {format!("{:.4}", ch.adc_value)}
                                        <span class="unit">
                                            {if ch.function == 4 || ch.function == 5 { "mA" } else { "V" }}
                                        </span>
                                    </div>
                                    <div class="card-details">
                                        <span>"DAC: "{format!("{:.3}", ch.dac_value)}</span>
                                        <span>"Raw: 0x"{format!("{:06X}", ch.adc_raw)}</span>
                                    </div>
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>

            // Alert summary
            {move || {
                let ds = state.get();
                if ds.alert_status != 0 || ds.supply_alert_status != 0 {
                    view! {
                        <div class="card alert-card">
                            <div class="card-header">
                                <span class="alert-icon">"⚠"</span>
                                <span>"Active Alerts"</span>
                            </div>
                            <div class="card-body">
                                <span>"Alert Status: 0x"{format!("{:04X}", ds.alert_status)}</span>
                                <span>"Supply Alert: 0x"{format!("{:04X}", ds.supply_alert_status)}</span>
                            </div>
                        </div>
                    }.into_any()
                } else {
                    view! { <div></div> }.into_any()
                }
            }}
        </div>
    }
}
