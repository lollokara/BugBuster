use crate::components::ui::Switch;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use serde::Serialize;

/// GPIO claims only the edited IO slot when a write action is sent.
pub const SLOTS: &[u8] = &[];

#[component]
pub fn GpioTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="view">
            <section class="group group-flush">
                <div class="group-header">
                    <div class="group-title">"Digital IO"</div>
                    <span class="group-subtitle">"12 IOs routed through the analog MUX to the terminal blocks"</span>
                </div>
                <table class="table io-table io-table-gpio">
                    <thead>
                        <tr>
                            <th>"IO"</th>
                            <th>"Mode"</th>
                            <th>"Direction"</th>
                            <th>"Input"</th>
                            <th>"Output"</th>
                            <th>"Pull-down"</th>
                        </tr>
                    </thead>
                    <tbody>
                        {move || {
                            let ds = state.get();
                            ds.gpio.into_iter().enumerate().map(|(i, g)| {
                                let gpio_idx = i as u8;
                                let mode_name = GPIO_MODE_OPTIONS.iter()
                                    .find(|(c, _)| *c == g.mode)
                                    .map(|(_, n)| *n).unwrap_or("?");
                                let direction = match g.mode {
                                    1 => "Output",
                                    2 => "Input",
                                    0 => "High-Z",
                                    _ => "Other",
                                };
                                let is_output = g.mode == 1;

                                view! {
                                    <tr>
                                        <td class="io-name">{format!("IO {}", i + 1)}</td>
                                        <td>
                                            <select
                                                aria-label=format!("IO {} mode", i + 1)
                                                title=mode_name
                                                prop:value=g.mode.to_string()
                                                on:change=move |e| {
                                                    let mode: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                    send_gpio_config(gpio_idx, mode, g.pulldown);
                                                }
                                            >
                                                {GPIO_MODE_OPTIONS.iter().filter(|(c, _)| *c <= 2 || *c == g.mode).map(|(code, name)| {
                                                    view! { <option value=code.to_string()>{*name}</option> }
                                                }).collect::<Vec<_>>()}
                                            </select>
                                        </td>
                                        <td><span class="chip">{direction}</span></td>
                                        <td>
                                            <span class="io-state">
                                                <span class="dot" class:tone-green=g.input aria-hidden="true"></span>
                                                {if g.input { "High" } else { "Low" }}
                                            </span>
                                        </td>
                                        <td>
                                            {if is_output {
                                                view! {
                                                    <span class="io-state">
                                                        <Switch
                                                            checked=Signal::derive(move || g.output)
                                                            on_change=Callback::new(move |v: bool| send_gpio_value(gpio_idx, v))
                                                            aria_label="Output level"
                                                        />
                                                        {if g.output { "High" } else { "Low" }}
                                                    </span>
                                                }.into_any()
                                            } else {
                                                view! { <span class="subtle">"-"</span> }.into_any()
                                            }}
                                        </td>
                                        <td>
                                            <Switch
                                                checked=Signal::derive(move || g.pulldown)
                                                on_change=Callback::new(move |v: bool| send_gpio_config(gpio_idx, g.mode, v))
                                                aria_label="Pull-down"
                                            />
                                        </td>
                                    </tr>
                                }
                            }).collect::<Vec<_>>()
                        }}
                    </tbody>
                </table>
            </section>
        </div>
    }
}

#[derive(Serialize)]
struct GpioConfigArgs {
    gpio: u8,
    mode: u8,
    pulldown: bool,
}
#[derive(Serialize)]
struct GpioValueArgs {
    gpio: u8,
    value: bool,
}

fn send_gpio_config(gpio: u8, mode: u8, pulldown: bool) {
    let args = serde_wasm_bindgen::to_value(&GpioConfigArgs {
        gpio,
        mode,
        pulldown,
    })
    .unwrap();
    let mode_name = GPIO_MODE_OPTIONS
        .iter()
        .find(|(c, _)| *c == mode)
        .map(|(_, n)| *n)
        .unwrap_or("?");
    let label = format!(
        "Set IO {} to {}{}",
        gpio + 1,
        mode_name,
        if pulldown { " (pull-down)" } else { "" }
    );
    invoke_with_io_claim("set_gpio_config", args, &label, &[gpio]);
}

fn send_gpio_value(gpio: u8, value: bool) {
    let args = serde_wasm_bindgen::to_value(&GpioValueArgs { gpio, value }).unwrap();
    let label = format!("Set IO {} {}", gpio + 1, if value { "HIGH" } else { "LOW" });
    invoke_with_io_claim("set_gpio_value", args, &label, &[gpio]);
}
