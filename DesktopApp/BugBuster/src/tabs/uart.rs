use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

const BAUD_OPTIONS: &[u32] = &[300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
const PARITY_OPTIONS: &[(u8, &str)] = &[(0, "None"), (1, "Odd"), (2, "Even")];
const STOP_BITS_OPTIONS: &[(u8, &str)] = &[(0, "1"), (1, "1.5"), (2, "2")];

// PCB IO terminal numbering -> ESP32 GPIO mapping.
const UART_IO_MAP: &[(u8, u8)] = &[
    (1, 1), (2, 2), (3, 4),
    (4, 5), (5, 6), (6, 7),
    (7, 10), (8, 9), (9, 8),
    (10, 13), (11, 12), (12, 11),
];

fn io_label_for_gpio(gpio: u8) -> String {
    UART_IO_MAP
        .iter()
        .find(|(_, g)| *g == gpio)
        .map(|(io, g)| format!("IO{} (GPIO{})", io, g))
        .unwrap_or_else(|| format!("GPIO{}", gpio))
}

#[derive(Clone, Serialize)]
struct UartConfig {
    bridge_id: u8,
    uart_num: u8,
    tx_pin: u8,
    rx_pin: u8,
    baudrate: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
    enabled: bool,
}

#[component]
pub fn UartTab(
    uart_config: RwSignal<UartConfigState>,
) -> impl IntoView {
    let apply = move |_: leptos::ev::MouseEvent| {
        let cfg = uart_config.get();
        let args = serde_wasm_bindgen::to_value(&UartConfig {
            bridge_id: 0,
            uart_num: cfg.uart_num,
            tx_pin: cfg.tx_pin,
            rx_pin: cfg.rx_pin,
            baudrate: cfg.baud,
            data_bits: cfg.data_bits,
            parity: cfg.parity,
            stop_bits: cfg.stop_bits,
            enabled: cfg.enabled,
        }).unwrap();
        let label = format!("Apply UART config: {} baud", cfg.baud);
        invoke_with_feedback("set_uart_config", args, &label);
    };

    view! {
        <div class="tab-content">
            <div class="tab-desc">"UART bridge configuration. Bridge is disabled by default and can be routed to any PCB IO (IO1..IO12)."</div>
            <div class="uart-layout">
                <div class="card uart-card">
                    <div class="card-header">
                        <span>"UART Bridge #0"</span>
                        <div class={move || if uart_config.get().enabled { "uart-status uart-active" } else { "uart-status" }}>
                            {move || if uart_config.get().enabled { "Active" } else { "Disabled" }}
                        </div>
                    </div>
                    <div class="card-body">
                        <p class="uart-desc">"Transparent bridge: USB CDC #1 ↔ ESP32 UART"</p>

                        <div class="config-section">
                            <div class="config-row">
                                <label>"TX IO"</label>
                                <select class="dropdown"
                                    prop:value={move || uart_config.get().tx_pin.to_string()}
                                    on:change=move |e| uart_config.update(|c| c.tx_pin = event_target_value(&e).parse().unwrap_or(1))
                                >
                                    {UART_IO_MAP.iter().map(|(io, gpio)| {
                                        view! { <option value=gpio.to_string()>{format!("IO{} (GPIO{})", io, gpio)}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"RX IO"</label>
                                <select class="dropdown"
                                    prop:value={move || uart_config.get().rx_pin.to_string()}
                                    on:change=move |e| uart_config.update(|c| c.rx_pin = event_target_value(&e).parse().unwrap_or(2))
                                >
                                    {UART_IO_MAP.iter().map(|(io, gpio)| {
                                        view! { <option value=gpio.to_string()>{format!("IO{} (GPIO{})", io, gpio)}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Baud Rate"</label>
                                <select class="dropdown"
                                    prop:value={move || uart_config.get().baud.to_string()}
                                    on:change=move |e| uart_config.update(|c| c.baud = event_target_value(&e).parse().unwrap_or(115200))
                                >
                                    {BAUD_OPTIONS.iter().map(|b| {
                                        view! { <option value=b.to_string()>{format!("{}", b)}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Data Bits"</label>
                                <select class="dropdown"
                                    prop:value={move || uart_config.get().data_bits.to_string()}
                                    on:change=move |e| uart_config.update(|c| c.data_bits = event_target_value(&e).parse().unwrap_or(8))
                                >
                                    {[5u8, 6, 7, 8].iter().map(|b| {
                                        view! { <option value=b.to_string()>{format!("{}", b)}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Parity"</label>
                                <select class="dropdown"
                                    prop:value={move || uart_config.get().parity.to_string()}
                                    on:change=move |e| uart_config.update(|c| c.parity = event_target_value(&e).parse().unwrap_or(0))
                                >
                                    {PARITY_OPTIONS.iter().map(|(c, n)| {
                                        view! { <option value=c.to_string()>{*n}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Stop Bits"</label>
                                <select class="dropdown"
                                    prop:value={move || uart_config.get().stop_bits.to_string()}
                                    on:change=move |e| uart_config.update(|c| c.stop_bits = event_target_value(&e).parse().unwrap_or(0))
                                >
                                    {STOP_BITS_OPTIONS.iter().map(|(c, n)| {
                                        view! { <option value=c.to_string()>{*n}</option> }
                                    }).collect::<Vec<_>>()}
                                </select>
                            </div>
                            <div class="config-row">
                                <label>"Enabled"</label>
                                <label class="toggle-wrap">
                                    <div class="toggle" class:active={move || uart_config.get().enabled}
                                        on:click=move |_| uart_config.update(|c| c.enabled = !c.enabled)
                                    ><div class="toggle-thumb"></div></div>
                                </label>
                            </div>
                        </div>

                        <div class="uart-summary">
                            <span class="uart-config-str">
                                {move || {
                                    let c = uart_config.get();
                                    format!("{} → {} | {} {}{}{}",
                                        io_label_for_gpio(c.tx_pin),
                                        io_label_for_gpio(c.rx_pin),
                                        c.baud, c.data_bits,
                                        match c.parity { 1 => "O", 2 => "E", _ => "N" },
                                        match c.stop_bits { 1 => "1.5", 2 => "2", _ => "1" })
                                }}
                            </span>
                        </div>

                        <button class="btn btn-primary" style="width: 100%; margin-top: 12px;" on:click=apply>
                            "Apply Configuration"
                        </button>
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
                            <span class="uart-info-value">{move || format!("ESP32 UART1 (TX: {}, RX: {})", io_label_for_gpio(uart_config.get().tx_pin), io_label_for_gpio(uart_config.get().rx_pin))}</span>
                        </div>
                        <div class="uart-info-item">
                            <span class="uart-info-label">"Usage"</span>
                            <span class="uart-info-value">"Opens as a standard COM port. Connect your external device's TX to the RX pin and vice versa."</span>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    }
}

// State struct (owned by App, passed as RwSignal to persist across tab switches)
#[derive(Clone, Default)]
pub struct UartConfigState {
    pub uart_num: u8,
    pub tx_pin: u8,
    pub rx_pin: u8,
    pub baud: u32,
    pub data_bits: u8,
    pub parity: u8,
    pub stop_bits: u8,
    pub enabled: bool,
}

impl UartConfigState {
    pub fn new() -> Self {
        Self {
            uart_num: 1,
            tx_pin: 1,
            rx_pin: 2,
            baud: 115200,
            data_bits: 8,
            parity: 0,
            stop_bits: 0,
            enabled: false,
        }
    }
}
