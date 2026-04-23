use leptos::prelude::*;
use serde::Serialize;
use crate::tauri_bridge::*;

#[component]
pub fn GpioTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    view! {
        <div class="tab-content">
            <div class="tab-desc">"ESP32 Direct Digital IO status and control. These 12 IOs are routed through the analog MUX matrix to the terminal blocks."</div>
            <div class="channel-grid">
                {move || {
                    let ds = state.get();
                    ds.gpio.into_iter().enumerate().map(|(i, g)| {
                        let gpio_idx = i as u8;
                        let mode_name = GPIO_MODE_OPTIONS.iter()
                            .find(|(c, _)| *c == g.mode)
                            .map(|(_, n)| *n).unwrap_or("?");

                        view! {
                            <div class="card">
                                <div class="card-header">
                                    <span class="channel-label">{format!("IO {}", i + 1)}</span>
                                    <span class="channel-func">{mode_name}</span>
                                </div>
                                <div class="card-body">
                                    // Mode selector
                                    <div class="config-row">
                                        <label>"Mode"</label>
                                        <select class="dropdown"
                                            prop:value=g.mode.to_string()
                                            on:change=move |e| {
                                                let mode: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                send_gpio_config(gpio_idx, mode, g.pulldown);
                                            }
                                        >
                                            {GPIO_MODE_OPTIONS.iter().filter(|(c, _)| *c <= 2).map(|(code, name)| {
                                                view! { <option value=code.to_string()>{*name}</option> }
                                            }).collect::<Vec<_>>()}
                                        </select>
                                    </div>

                                    // Input display
                                    <div class="config-row">
                                        <label>"Input"</label>
                                        <div class="led-wrap">
                                            <div class="led"
                                                class:led-on=g.input
                                                style=if g.input {
                                                    "background: var(--green); box-shadow: 0 0 8px var(--green)"
                                                } else { "" }
                                            ></div>
                                            <span class="led-label">{if g.input { "HIGH" } else { "LOW" }}</span>
                                        </div>
                                    </div>

                                    // Output toggle (when mode = OUTPUT)
                                    {if g.mode == 1 {
                                        Some(view! {
                                            <div class="config-row">
                                                <label>"Output"</label>
                                                <button class="do-toggle do-toggle-sm" class:do-on=g.output
                                                    on:click=move |_| {
                                                        send_gpio_value(gpio_idx, !g.output);
                                                    }
                                                >
                                                    {if g.output { "HIGH" } else { "LOW" }}
                                                </button>
                                            </div>
                                        })
                                    } else { None }}

                                    // Pulldown toggle
                                    <div class="config-row">
                                        <label>"Pull-down"</label>
                                        <label class="toggle-wrap">
                                            <div class="toggle" class:active=g.pulldown
                                                on:click=move |_| {
                                                    send_gpio_config(gpio_idx, g.mode, !g.pulldown);
                                                }
                                            ><div class="toggle-thumb"></div></div>
                                        </label>
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

#[derive(Serialize)]
struct GpioConfigArgs { gpio: u8, mode: u8, pulldown: bool }
#[derive(Serialize)]
struct GpioValueArgs { gpio: u8, value: bool }

fn send_gpio_config(gpio: u8, mode: u8, pulldown: bool) {
    let args = serde_wasm_bindgen::to_value(&GpioConfigArgs { gpio, mode, pulldown }).unwrap();
    let mode_name = GPIO_MODE_OPTIONS.iter()
        .find(|(c, _)| *c == mode)
        .map(|(_, n)| *n).unwrap_or("?");
    let label = format!("Set IO {} to {}{}", gpio + 1, mode_name,
        if pulldown { " (pull-down)" } else { "" });
    invoke_with_feedback("set_gpio_config", args, &label);
}

fn send_gpio_value(gpio: u8, value: bool) {
    let args = serde_wasm_bindgen::to_value(&GpioValueArgs { gpio, value }).unwrap();
    let label = format!("Set IO {} {}", gpio + 1, if value { "HIGH" } else { "LOW" });
    invoke_with_feedback("set_gpio_value", args, &label);
}
