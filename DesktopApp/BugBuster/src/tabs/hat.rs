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

const HAT_TYPE_NAMES: &[&str] = &["None", "SWD/GPIO"];

fn hat_type_name(t: u8) -> &'static str {
    match t {
        0 => "None",
        1 => "SWD/GPIO",
        _ => "Unknown",
    }
}

fn func_name(f: u8) -> &'static str {
    match f {
        0 => "Disconnected",
        1 => "SWDIO",
        2 => "SWCLK",
        3 => "TRACE1",
        4 => "TRACE2",
        5 => "GPIO1",
        6 => "GPIO2",
        7 => "GPIO3",
        8 => "GPIO4",
        _ => "?",
    }
}

#[component]
pub fn HatTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (hat, set_hat) = signal(HatStatus::default());

    // Poll HAT status when device state changes
    let set_hat_clone = set_hat;
    Effect::new(move |_| {
        let _ = state.get();
        spawn_local(async move {
            if let Some(st) = fetch_hat_status().await {
                set_hat_clone.set(st);
            }
        });
    });

    // Per-pin dropdown signals (initialized from hat state)
    let pin_signals: Vec<(ReadSignal<String>, WriteSignal<String>)> = (0..4)
        .map(|_| signal("0".to_string()))
        .collect();

    // Sync dropdown values when hat state updates
    let pin_setters: Vec<WriteSignal<String>> = pin_signals.iter().map(|(_, s)| *s).collect();
    Effect::new(move |_| {
        let st = hat.get();
        for (i, setter) in pin_setters.iter().enumerate() {
            setter.set(st.pin_config[i].to_string());
        }
    });

    let dropdown_options: Vec<(String, String)> = PIN_FUNCTION_OPTIONS
        .iter()
        .map(|(v, l)| (v.to_string(), l.to_string()))
        .collect();

    view! {
        <div class="tab-content">
            <div class="tab-desc">
                "HAT Expansion Board — Configure the EXP_EXT I/O lines on the attached HAT. "
                "Each pin can be independently assigned to SWD, trace, or GPIO functions."
            </div>

            {move || {
                let st = hat.get();

                // Status card
                let status_card = view! {
                    <div class="card" style="margin-bottom: 16px">
                        <div class="card-header">
                            <span>"HAT Status"</span>
                            <button
                                class="btn btn-sm"
                                style="font-size: 10px; padding: 2px 8px"
                                on:click=move |_| {
                                    spawn_local(async move {
                                        if let Some(st) = fetch_hat_status().await {
                                            set_hat.set(st);
                                        }
                                    });
                                }
                            >"Refresh"</button>
                        </div>
                        <div class="card-body">
                            <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px">
                                // Detected indicator
                                <div style="text-align: center; padding: 8px; border-radius: 8px; background: var(--bg-secondary)">
                                    <div style="font-size: 10px; color: var(--text-dim); margin-bottom: 4px">"Detected"</div>
                                    <div style=format!("width: 12px; height: 12px; border-radius: 50%; margin: 0 auto; {}",
                                        if st.detected { "background: #10b981; box-shadow: 0 0 6px #10b981" }
                                        else { "background: var(--text-dim)" }
                                    )></div>
                                </div>
                                // Connected indicator
                                <div style="text-align: center; padding: 8px; border-radius: 8px; background: var(--bg-secondary)">
                                    <div style="font-size: 10px; color: var(--text-dim); margin-bottom: 4px">"Connected"</div>
                                    <div style=format!("width: 12px; height: 12px; border-radius: 50%; margin: 0 auto; {}",
                                        if st.connected { "background: #3b82f6; box-shadow: 0 0 6px #3b82f6" }
                                        else { "background: var(--text-dim)" }
                                    )></div>
                                </div>
                                // Type
                                <div style="text-align: center; padding: 8px; border-radius: 8px; background: var(--bg-secondary)">
                                    <div style="font-size: 10px; color: var(--text-dim); margin-bottom: 4px">"Type"</div>
                                    <div style="font-size: 13px; font-weight: 600; font-family: 'JetBrains Mono', monospace">
                                        {hat_type_name(st.hat_type)}
                                    </div>
                                </div>
                                // Detect voltage
                                <div style="text-align: center; padding: 8px; border-radius: 8px; background: var(--bg-secondary)">
                                    <div style="font-size: 10px; color: var(--text-dim); margin-bottom: 4px">"Detect V"</div>
                                    <div style="font-size: 13px; font-weight: 600; font-family: 'JetBrains Mono', monospace; color: #f59e0b">
                                        {format!("{:.2}V", st.detect_voltage)}
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

                // Pin configuration card
                let opts = dropdown_options.clone();
                view! {
                    {status_card}

                    <div class="card">
                        <div class="card-header">
                            <span>"EXP_EXT Pin Configuration"</span>
                            <div style="display: flex; gap: 8px">
                                {if st.config_confirmed {
                                    view! {
                                        <span style="font-size: 10px; color: #10b981; padding: 2px 6px; background: #10b98120; border-radius: 4px">
                                            "Confirmed"
                                        </span>
                                    }.into_any()
                                } else {
                                    view! {
                                        <span style="font-size: 10px; color: #f59e0b; padding: 2px 6px; background: #f59e0b20; border-radius: 4px">
                                            "Pending"
                                        </span>
                                    }.into_any()
                                }}
                                <button
                                    class="btn btn-sm"
                                    style="font-size: 10px; padding: 2px 8px; background: #ef444420; color: #ef4444; border: 1px solid #ef444450"
                                    on:click=move |_| { send_hat_reset(); }
                                >"Reset All"</button>
                            </div>
                        </div>
                        <div class="card-body">
                            <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 16px">
                                {(0..4u8).map(|i| {
                                    let idx = i as usize;
                                    let (read_sig, _write_sig) = pin_signals[idx];
                                    let opts_clone = opts.clone();
                                    view! {
                                        <div style="padding: 12px; border-radius: 8px; background: var(--bg-secondary); border: 1px solid var(--border-color, #333)">
                                            <div style="font-size: 11px; font-weight: 600; margin-bottom: 8px; color: #3b82f6">
                                                {format!("EXP_EXT_{}", i + 1)}
                                            </div>
                                            <Dropdown
                                                value=Signal::from(read_sig)
                                                on_change=Callback::new(move |val: String| {
                                                    if let Ok(func) = val.parse::<u8>() {
                                                        send_hat_set_pin(i, func);
                                                    }
                                                })
                                                options=opts_clone
                                            />
                                            <div style="font-size: 10px; color: var(--text-dim); margin-top: 6px">
                                                {func_name(st.pin_config[idx])}
                                            </div>
                                        </div>
                                    }
                                }).collect::<Vec<_>>()}
                            </div>
                        </div>
                    </div>

                    // Quick presets
                    <div class="card" style="margin-top: 16px">
                        <div class="card-header"><span>"Quick Presets"</span></div>
                        <div class="card-body">
                            <div style="display: flex; gap: 8px; flex-wrap: wrap">
                                <button
                                    class="btn"
                                    style="font-size: 11px; padding: 6px 16px"
                                    on:click=move |_| {
                                        // SWD preset: SWDIO, SWCLK, TRACE1, TRACE2
                                        for (i, func) in [1u8, 2, 3, 4].iter().enumerate() {
                                            send_hat_set_pin(i as u8, *func);
                                        }
                                    }
                                >"SWD Debug"</button>
                                <button
                                    class="btn"
                                    style="font-size: 11px; padding: 6px 16px"
                                    on:click=move |_| {
                                        // GPIO preset: GPIO1-4
                                        for (i, func) in [5u8, 6, 7, 8].iter().enumerate() {
                                            send_hat_set_pin(i as u8, *func);
                                        }
                                    }
                                >"GPIO Mode"</button>
                                <button
                                    class="btn"
                                    style="font-size: 11px; padding: 6px 16px"
                                    on:click=move |_| {
                                        // SWD + SWO: SWDIO, SWCLK, TRACE1, GPIO4
                                        for (i, func) in [1u8, 2, 3, 8].iter().enumerate() {
                                            send_hat_set_pin(i as u8, *func);
                                        }
                                    }
                                >"SWD + SWO"</button>
                                <button
                                    class="btn"
                                    style="font-size: 11px; padding: 6px 16px; background: #ef444420; color: #ef4444; border: 1px solid #ef444450"
                                    on:click=move |_| {
                                        for i in 0..4u8 {
                                            send_hat_set_pin(i, 0);
                                        }
                                    }
                                >"Disconnect All"</button>
                            </div>
                        </div>
                    </div>
                }.into_any()
            }}
        </div>
    }
}
