use crate::components::icons::Icon;
use crate::components::ui::{Callout, Switch};
use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

const BAUD_OPTIONS: &[u32] = &[
    300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
];
const PARITY_OPTIONS: &[(u8, &str)] = &[(0, "None"), (1, "Odd"), (2, "Even")];
const STOP_BITS_OPTIONS: &[(u8, &str)] = &[(0, "1"), (1, "1.5"), (2, "2")];

// PCB IO terminal numbering -> ESP32 GPIO mapping.
const UART_IO_MAP: &[(u8, u8)] = &[
    (1, 4),
    (2, 2),
    (3, 1),
    (4, 7),
    (5, 6),
    (6, 5),
    (7, 8),
    (8, 9),
    (9, 10),
    (10, 11),
    (11, 12),
    (12, 13),
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
pub fn UartTab(uart_config: RwSignal<UartConfigState>) -> impl IntoView {
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
        })
        .unwrap();
        let label = format!("Apply UART config: {} baud", cfg.baud);
        invoke_with_feedback("set_uart_config", args, &label);
    };

    // Inline validation (advisory; Apply behaviour is unchanged).
    let same_pin = move || {
        let c = uart_config.get();
        c.tx_pin == c.rx_pin
    };
    let stop_mismatch = move || {
        let c = uart_config.get();
        c.stop_bits == 1 && c.data_bits != 5
    };

    view! {
        <div class="view sy-uart">
            <p class="sy-lead">"UART bridge configuration. The bridge is disabled by default and can be routed to any PCB IO (IO1 to IO12)."</p>
            <div class="sy-uart-layout">
                <div class="group sy-uart-config">
                    <div class="group-header">
                        <span class="group-title"><Icon name="terminal" size=15 />"UART Bridge #0"</span>
                        <span class=move || if uart_config.get().enabled { "badge tone-green" } else { "badge" }>
                            {move || if uart_config.get().enabled { "Active" } else { "Disabled" }}
                        </span>
                    </div>
                    <p class="sy-uart-note">"Transparent bridge: USB CDC #1 ↔ ESP32 UART"</p>

                    <div class="sy-form-section">"Pins"</div>
                    <div class="rows">
                        <div class="row">
                            <label class="row-label" for="uart-tx">"TX IO"<span class="row-hint">"Device output to your target's RX"</span></label>
                            <select id="uart-tx" class="dropdown"
                                aria-invalid=move || if same_pin() { "true" } else { "false" }
                                prop:value={move || uart_config.get().tx_pin.to_string()}
                                on:change=move |e| uart_config.update(|c| c.tx_pin = event_target_value(&e).parse().unwrap_or(1))
                            >
                                {UART_IO_MAP.iter().map(|(io, gpio)| {
                                    view! { <option value=gpio.to_string()>{format!("IO{} (GPIO{})", io, gpio)}</option> }
                                }).collect::<Vec<_>>()}
                            </select>
                        </div>
                        <div class="row">
                            <label class="row-label" for="uart-rx">"RX IO"<span class="row-hint">"Device input from your target's TX"</span></label>
                            <select id="uart-rx" class="dropdown"
                                aria-invalid=move || if same_pin() { "true" } else { "false" }
                                prop:value={move || uart_config.get().rx_pin.to_string()}
                                on:change=move |e| uart_config.update(|c| c.rx_pin = event_target_value(&e).parse().unwrap_or(2))
                            >
                                {UART_IO_MAP.iter().map(|(io, gpio)| {
                                    view! { <option value=gpio.to_string()>{format!("IO{} (GPIO{})", io, gpio)}</option> }
                                }).collect::<Vec<_>>()}
                            </select>
                        </div>
                    </div>
                    {move || same_pin().then(|| view! {
                        <div class="sy-field-msg"><Callout tone="orange">"TX and RX are routed to the same IO. Pick two different pins."</Callout></div>
                    })}

                    <div class="sy-form-section">"Line settings"</div>
                    <div class="rows">
                        <div class="row">
                            <label class="row-label" for="uart-baud">"Baud rate"</label>
                            <select id="uart-baud" class="dropdown"
                                prop:value={move || uart_config.get().baud.to_string()}
                                on:change=move |e| uart_config.update(|c| c.baud = event_target_value(&e).parse().unwrap_or(115200))
                            >
                                {BAUD_OPTIONS.iter().map(|b| {
                                    view! { <option value=b.to_string()>{format!("{}", b)}</option> }
                                }).collect::<Vec<_>>()}
                            </select>
                        </div>
                        <div class="row">
                            <label class="row-label" for="uart-bits">"Data bits"</label>
                            <select id="uart-bits" class="dropdown"
                                prop:value={move || uart_config.get().data_bits.to_string()}
                                on:change=move |e| uart_config.update(|c| c.data_bits = event_target_value(&e).parse().unwrap_or(8))
                            >
                                {[5u8, 6, 7, 8].iter().map(|b| {
                                    view! { <option value=b.to_string()>{format!("{}", b)}</option> }
                                }).collect::<Vec<_>>()}
                            </select>
                        </div>
                        <div class="row">
                            <label class="row-label" for="uart-parity">"Parity"</label>
                            <select id="uart-parity" class="dropdown"
                                prop:value={move || uart_config.get().parity.to_string()}
                                on:change=move |e| uart_config.update(|c| c.parity = event_target_value(&e).parse().unwrap_or(0))
                            >
                                {PARITY_OPTIONS.iter().map(|(c, n)| {
                                    view! { <option value=c.to_string()>{*n}</option> }
                                }).collect::<Vec<_>>()}
                            </select>
                        </div>
                        <div class="row">
                            <label class="row-label" for="uart-stop">"Stop bits"</label>
                            <select id="uart-stop" class="dropdown"
                                aria-invalid=move || if stop_mismatch() { "true" } else { "false" }
                                prop:value={move || uart_config.get().stop_bits.to_string()}
                                on:change=move |e| uart_config.update(|c| c.stop_bits = event_target_value(&e).parse().unwrap_or(0))
                            >
                                {STOP_BITS_OPTIONS.iter().map(|(c, n)| {
                                    view! { <option value=c.to_string()>{*n}</option> }
                                }).collect::<Vec<_>>()}
                            </select>
                        </div>
                    </div>
                    {move || stop_mismatch().then(|| view! {
                        <div class="sy-field-msg"><Callout tone="orange">"1.5 stop bits is normally only valid with 5 data bits."</Callout></div>
                    })}

                    <div class="sy-form-section">"Bridge"</div>
                    <div class="rows">
                        <div class="row">
                            <span class="row-label">"Enabled"<span class="row-hint">"Routes the pins to USB CDC #1 once applied"</span></span>
                            <Switch
                                checked=Signal::derive(move || uart_config.get().enabled)
                                aria_label="Enable UART bridge"
                                on_change=Callback::new(move |_: bool| uart_config.update(|c| c.enabled = !c.enabled))
                            />
                        </div>
                    </div>

                    <div class="sy-uart-summary">
                        <span class="sy-uart-summary-label">"Configuration"</span>
                        <span class="uart-config-str sy-uart-config-str">
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

                    <button class="btn btn-primary btn-lg btn-block" on:click=apply>
                        "Apply Configuration"
                    </button>
                </div>

                <div class="group sy-uart-info">
                    <div class="group-header"><span class="group-title">"Connection info"</span></div>
                    <dl class="sy-info">
                        <div class="sy-info-item">
                            <dt>"Host side"</dt>
                            <dd>"USB CDC #1 (second serial port)"</dd>
                        </div>
                        <div class="sy-info-item">
                            <dt>"Device side"</dt>
                            <dd>{move || format!("ESP32 UART1 (TX: {}, RX: {})", io_label_for_gpio(uart_config.get().tx_pin), io_label_for_gpio(uart_config.get().rx_pin))}</dd>
                        </div>
                        <div class="sy-info-item">
                            <dt>"Usage"</dt>
                            <dd>"Opens as a standard COM port. Connect your external device's TX to the RX pin and vice versa."</dd>
                        </div>
                    </dl>
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
