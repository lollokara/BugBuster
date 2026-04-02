use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::tauri_bridge::*;
use crate::components::controls::Dropdown;

const PIN_FUNCTION_OPTIONS: &[(&str, &str)] = &[
    ("0", "Disconnected"),
    ("1", "SWDIO"),
    ("2", "SWCLK"),
    ("3", "TRACE1 (SWO)"),
    ("4", "TRACE2"),
    ("5", "GPIO1"),
    ("6", "GPIO2"),
    ("7", "GPIO3"),
    ("8", "GPIO4"),
];

const IO_VOLTAGE_OPTIONS: &[(&str, &str)] = &[
    ("1200", "1.2V"),
    ("1800", "1.8V"),
    ("2500", "2.5V"),
    ("3300", "3.3V"),
    ("5000", "5.0V"),
];

fn hat_type_name(t: u8) -> &'static str {
    match t { 0 => "None", 1 => "SWD/GPIO", _ => "Unknown" }
}

fn func_name(f: u8) -> &'static str {
    match f {
        0 => "Disconnected", 1 => "SWDIO", 2 => "SWCLK", 3 => "TRACE1",
        4 => "TRACE2", 5 => "GPIO1", 6 => "GPIO2", 7 => "GPIO3", 8 => "GPIO4",
        _ => "?",
    }
}

fn status_dot(active: bool, color: &str) -> String {
    format!("width: 10px; height: 10px; border-radius: 50%; display: inline-block; {}",
        if active { format!("background: {}; box-shadow: 0 0 6px {}", color, color) }
        else { "background: var(--text-dim)".into() })
}

#[component]
pub fn HatTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (hat, set_hat) = signal(HatStatus::default());

    let set_hat_clone = set_hat;
    Effect::new(move |_| {
        let _ = state.get();
        spawn_local(async move {
            if let Some(st) = fetch_hat_status().await {
                set_hat_clone.set(st);
            }
        });
    });

    let pin_signals: Vec<(ReadSignal<String>, WriteSignal<String>)> = (0..4)
        .map(|_| signal("0".to_string()))
        .collect();

    let pin_setters: Vec<WriteSignal<String>> = pin_signals.iter().map(|(_, s)| *s).collect();
    Effect::new(move |_| {
        let st = hat.get();
        for (i, setter) in pin_setters.iter().enumerate() {
            setter.set(st.pin_config[i].to_string());
        }
    });

    let dropdown_options: Vec<(String, String)> = PIN_FUNCTION_OPTIONS
        .iter().map(|(v, l)| (v.to_string(), l.to_string())).collect();

    let io_volt_options: Vec<(String, String)> = IO_VOLTAGE_OPTIONS
        .iter().map(|(v, l)| (v.to_string(), l.to_string())).collect();

    view! {
        <div class="tab-content">
            <div class="tab-desc">
                "HAT Expansion Board — Power management, SWD debug setup, and EXP_EXT I/O configuration."
            </div>

            {move || {
                let st = hat.get();

                // ── Status Bar ──
                let status_card = view! {
                    <div class="card" style="margin-bottom: 16px">
                        <div class="card-header">
                            <span>"HAT Status"</span>
                            <button class="btn btn-sm" style="font-size: 10px; padding: 2px 8px"
                                on:click=move |_| {
                                    spawn_local(async move {
                                        if let Some(s) = fetch_hat_status().await { set_hat.set(s); }
                                    });
                                }
                            >"Refresh"</button>
                        </div>
                        <div class="card-body">
                            <div style="display: grid; grid-template-columns: repeat(6, 1fr); gap: 10px">
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"Detected"</div>
                                    <div style=status_dot(st.detected, "#10b981")></div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"UART"</div>
                                    <div style=status_dot(st.connected, "#3b82f6")></div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"DAP"</div>
                                    <div style=status_dot(st.dap_connected, "#8b5cf6")></div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"Target"</div>
                                    <div style=status_dot(st.target_detected, "#f59e0b")></div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"Type"</div>
                                    <div style="font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                        {hat_type_name(st.hat_type)}
                                    </div>
                                </div>
                                <div style="text-align: center; padding: 6px; border-radius: 6px; background: var(--bg-secondary)">
                                    <div style="font-size: 9px; color: var(--text-dim); margin-bottom: 3px">"FW"</div>
                                    <div style="font-size: 11px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                        {format!("v{}.{}", st.fw_major, st.fw_minor)}
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                };

                if !st.detected {
                    return view! {
                        {status_card}
                        <div class="card">
                            <div class="card-body" style="text-align: center; padding: 32px; color: var(--text-dim)">
                                <div style="font-size: 24px; margin-bottom: 8px">"No HAT Detected"</div>
                                <div style="font-size: 12px">"Connect a HAT board to the expansion header and click Refresh."</div>
                            </div>
                        </div>
                    }.into_any();
                }

                // ── Main Content (HAT detected) ──
                let opts = dropdown_options.clone();
                let io_opts = io_volt_options.clone();

                // Cache connector state for closures (avoid move issues with Vec)
                let conn_a_en = st.connectors.get(0).map(|c| c.enabled).unwrap_or(false);
                let conn_b_en = st.connectors.get(1).map(|c| c.enabled).unwrap_or(false);
                let conn_a_ma = st.connectors.get(0).map(|c| c.current_ma).unwrap_or(0.0);
                let conn_b_ma = st.connectors.get(1).map(|c| c.current_ma).unwrap_or(0.0);
                let conn_a_fault = st.connectors.get(0).map(|c| c.fault).unwrap_or(false);
                let conn_b_fault = st.connectors.get(1).map(|c| c.fault).unwrap_or(false);

                view! {
                    {status_card}

                    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 16px">
                        // ── Target Power Card ──
                        <div class="card">
                            <div class="card-header"><span>"Target Power"</span></div>
                            <div class="card-body">
                                // Connector A
                                <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 10px; padding: 8px; border-radius: 6px; background: var(--bg-secondary)">
                                    <span style="font-size: 11px; font-weight: 600; width: 80px">"Conn A"</span>
                                    <button
                                        class="btn btn-sm"
                                        style=format!("font-size: 10px; padding: 2px 10px; {}",
                                            if conn_a_en { "background: #10b98130; color: #10b981; border: 1px solid #10b98150" }
                                            else { "" })
                                        on:click=move |_| { send_hat_set_power(0, !conn_a_en); }
                                    >{if conn_a_en { "ON" } else { "OFF" }}</button>
                                    <span style="font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">
                                        {format!("{:.0}mA", conn_a_ma)}
                                    </span>
                                    {if conn_a_fault {
                                        view! { <span style="font-size: 9px; color: #ef4444; background: #ef444420; padding: 1px 4px; border-radius: 3px">"FAULT"</span> }.into_any()
                                    } else { view! { <span></span> }.into_any() }}
                                </div>
                                // Connector B
                                <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 10px; padding: 8px; border-radius: 6px; background: var(--bg-secondary)">
                                    <span style="font-size: 11px; font-weight: 600; width: 80px">"Conn B"</span>
                                    <button
                                        class="btn btn-sm"
                                        style=format!("font-size: 10px; padding: 2px 10px; {}",
                                            if conn_b_en { "background: #10b98130; color: #10b981; border: 1px solid #10b98150" }
                                            else { "" })
                                        on:click=move |_| { send_hat_set_power(1, !conn_b_en); }
                                    >{if conn_b_en { "ON" } else { "OFF" }}</button>
                                    <span style="font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">
                                        {format!("{:.0}mA", conn_b_ma)}
                                    </span>
                                    {if conn_b_fault {
                                        view! { <span style="font-size: 9px; color: #ef4444; background: #ef444420; padding: 1px 4px; border-radius: 3px">"FAULT"</span> }.into_any()
                                    } else { view! { <span></span> }.into_any() }}
                                </div>
                                // I/O Voltage
                                <div style="display: flex; align-items: center; gap: 8px; padding: 8px; border-radius: 6px; background: var(--bg-secondary)">
                                    <span style="font-size: 11px; font-weight: 600; width: 80px">"I/O Level"</span>
                                    <Dropdown
                                        value=Signal::derive(move || st.io_voltage_mv.to_string())
                                        on_change=Callback::new(move |val: String| {
                                            if let Ok(mv) = val.parse::<u16>() {
                                                send_hat_set_io_voltage(mv);
                                            }
                                        })
                                        options=io_opts.clone()
                                    />
                                </div>
                            </div>
                        </div>

                        // ── SWD Debug Card ──
                        <div class="card">
                            <div class="card-header"><span>"SWD Debug"</span></div>
                            <div class="card-body">
                                <div style="padding: 8px; border-radius: 6px; background: var(--bg-secondary); margin-bottom: 10px">
                                    <div style="display: flex; align-items: center; gap: 8px; margin-bottom: 6px">
                                        <div style=status_dot(st.dap_connected, "#8b5cf6")></div>
                                        <span style="font-size: 11px">
                                            {if st.dap_connected { "CMSIS-DAP host connected" } else { "No debug host connected" }}
                                        </span>
                                    </div>
                                    <div style="display: flex; align-items: center; gap: 8px">
                                        <div style=status_dot(st.target_detected, "#f59e0b")></div>
                                        <span style="font-size: 11px">
                                            {if st.target_detected {
                                                format!("Target: DPIDR 0x{:08X}", st.target_dpidr)
                                            } else {
                                                "No target detected".into()
                                            }}
                                        </span>
                                    </div>
                                </div>
                                // Quick Setup Buttons
                                <div style="font-size: 10px; color: var(--text-dim); margin-bottom: 6px">"Quick Setup"</div>
                                <div style="display: flex; gap: 6px; flex-wrap: wrap">
                                    <button class="btn" style="font-size: 10px; padding: 4px 12px; background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf650"
                                        on:click=move |_| { send_hat_setup_swd(3300, 0); }
                                    >"SWD 3.3V (A)"</button>
                                    <button class="btn" style="font-size: 10px; padding: 4px 12px; background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf650"
                                        on:click=move |_| { send_hat_setup_swd(1800, 0); }
                                    >"SWD 1.8V (A)"</button>
                                    <button class="btn" style="font-size: 10px; padding: 4px 12px; background: #8b5cf620; color: #8b5cf6; border: 1px solid #8b5cf650"
                                        on:click=move |_| { send_hat_setup_swd(3300, 1); }
                                    >"SWD 3.3V (B)"</button>
                                </div>
                            </div>
                        </div>
                    </div>

                    // ── Pin Configuration Card ──
                    <div class="card">
                        <div class="card-header">
                            <span>"EXP_EXT Pin Configuration"</span>
                            <div style="display: flex; gap: 6px; align-items: center">
                                {if st.config_confirmed {
                                    view! { <span style="font-size: 9px; color: #10b981; padding: 1px 5px; background: #10b98120; border-radius: 3px">"Confirmed"</span> }.into_any()
                                } else {
                                    view! { <span style="font-size: 9px; color: #f59e0b; padding: 1px 5px; background: #f59e0b20; border-radius: 3px">"Pending"</span> }.into_any()
                                }}
                                <button class="btn btn-sm" style="font-size: 9px; padding: 1px 6px; background: #ef444420; color: #ef4444; border: 1px solid #ef444450"
                                    on:click=move |_| { send_hat_reset(); }
                                >"Reset All"</button>
                            </div>
                        </div>
                        <div class="card-body">
                            <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px; margin-bottom: 12px">
                                {(0..4u8).map(|i| {
                                    let idx = i as usize;
                                    let (read_sig, _) = pin_signals[idx];
                                    let opts_clone = opts.clone();
                                    view! {
                                        <div style="padding: 10px; border-radius: 6px; background: var(--bg-secondary); border: 1px solid var(--border-color, #333)">
                                            <div style="font-size: 10px; font-weight: 600; margin-bottom: 6px; color: #3b82f6">
                                                {format!("EXP_EXT_{}", i + 1)}
                                            </div>
                                            <Dropdown
                                                value=Signal::derive(move || read_sig.get())
                                                on_change=Callback::new(move |val: String| {
                                                    if let Ok(func) = val.parse::<u8>() { send_hat_set_pin(i, func); }
                                                })
                                                options=opts_clone
                                            />
                                            <div style="font-size: 9px; color: var(--text-dim); margin-top: 4px">
                                                {func_name(st.pin_config[idx])}
                                            </div>
                                        </div>
                                    }
                                }).collect::<Vec<_>>()}
                            </div>
                            // Presets
                            <div style="display: flex; gap: 6px; flex-wrap: wrap">
                                <button class="btn" style="font-size: 10px; padding: 4px 12px"
                                    on:click=move |_| { for (i, f) in [1u8,2,3,4].iter().enumerate() { send_hat_set_pin(i as u8, *f); } }
                                >"SWD Debug"</button>
                                <button class="btn" style="font-size: 10px; padding: 4px 12px"
                                    on:click=move |_| { for (i, f) in [5u8,6,7,8].iter().enumerate() { send_hat_set_pin(i as u8, *f); } }
                                >"GPIO Mode"</button>
                                <button class="btn" style="font-size: 10px; padding: 4px 12px"
                                    on:click=move |_| { for (i, f) in [1u8,2,3,8].iter().enumerate() { send_hat_set_pin(i as u8, *f); } }
                                >"SWD + SWO"</button>
                                <button class="btn" style="font-size: 10px; padding: 4px 12px; background: #ef444420; color: #ef4444; border: 1px solid #ef444450"
                                    on:click=move |_| { for i in 0..4u8 { send_hat_set_pin(i, 0); } }
                                >"Disconnect All"</button>
                            </div>
                        </div>
                    </div>
                }.into_any()
            }}
        </div>
    }
}
