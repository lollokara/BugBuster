use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::tauri_bridge::*;

async fn sleep_ms(ms: u32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window().unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms as i32)
            .unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(promise).await.unwrap();
}

#[derive(Clone, Copy, PartialEq)]
enum CalState {
    Idle,       // Show status + start buttons
    Running,    // Auto-calibration in progress
    Complete,   // Done, show results
    Failed,     // Calibration failed
}

#[component]
pub fn CalibrationTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (cal_state, set_cal_state) = signal(CalState::Idle);
    let (idac, set_idac) = signal(IdacState::default());
    let (cal_channel, set_cal_channel) = signal(0u8);
    let (cal_log, set_cal_log) = signal(Vec::<String>::new());
    let (cal_points, set_cal_points) = signal(0u32);
    let (cal_error_mv, set_cal_error_mv) = signal(0.0f32);

    // Poll IDAC status
    Effect::new(move |_| {
        let _ = state.get();
        spawn_local(async move {
            if let Some(st) = fetch_idac_status().await {
                set_idac.set(st);
            }
        });
    });

    let add_log = move |msg: String| {
        set_cal_log.update(|l| { l.push(msg); if l.len() > 200 { l.remove(0); } });
    };

    // Start auto-calibration for a given IDAC channel
    let start_cal = move |ch: u8| {
        set_cal_channel.set(ch);
        set_cal_state.set(CalState::Running);
        set_cal_log.set(Vec::new());
        set_cal_points.set(0);
        set_cal_error_mv.set(0.0);

        let ch_name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "IDAC" };
        add_log(format!("Starting auto-calibration for {} (IDAC channel {})", ch_name, ch));
        add_log("Safety: disabling level shifter OE and all e-fuses...".to_string());
        add_log("Sweeping IDAC codes and measuring via U23 self-test MUX...".to_string());

        spawn_local(async move {
            // Call firmware auto-calibrate command
            // This blocks until calibration is complete (~4-5 seconds)
            let args = serde_wasm_bindgen::to_value(
                &serde_json::json!({"channel": ch})
            ).unwrap();

            match invoke("selftest_auto_calibrate", args).await {
                Ok(result) => {
                    // Parse result
                    if let Ok(r) = serde_wasm_bindgen::from_value::<serde_json::Value>(result) {
                        let status = r.get("status").and_then(|v| v.as_u64()).unwrap_or(3) as u8;
                        let points = r.get("points").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
                        let error = r.get("errorMv").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32;

                        set_cal_points.set(points);
                        set_cal_error_mv.set(error);

                        if status == 2 { // CAL_STATUS_SUCCESS
                            add_log(format!("Calibration complete: {} points, error = {:.1} mV", points, error));
                            add_log("Calibration saved to NVS (persists across reboots).".to_string());
                            add_log("Safety: level shifter OE and e-fuses restored.".to_string());
                            set_cal_state.set(CalState::Complete);
                        } else {
                            add_log(format!("Calibration failed (status={})", status));
                            set_cal_state.set(CalState::Failed);
                        }
                    }
                }
                Err(e) => {
                    add_log(format!("Error: {:?}", e));
                    set_cal_state.set(CalState::Failed);
                }
            }
        });
    };

    view! {
        <div class="tab-content">
            <div class="tab-desc">
                "Automatic DCDC voltage calibration via the on-board self-test MUX (U23). "
                "No external connections needed — the device measures its own supply rails "
                "using AD74416H Channel D through U23."
            </div>
            {move || {
                match cal_state.get() {
                    // ===== IDLE / STATUS =====
                    CalState::Idle => {
                        let idac_st = idac.get();
                        view! {
                            <div class="card" style="max-width: 800px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700; font-size: 16px">"DCDC Auto-Calibration"</span>
                                </div>
                                <div class="card-body">
                                    <p style="color: var(--text-dim); margin-bottom: 16px; font-size: 12px">
                                        "Calibrate the DCDC converters by automatically sweeping IDAC codes and measuring "
                                        "actual output voltages via the U23 self-test switch matrix. This compensates for "
                                        "component tolerances in the DS4424 feedback network."
                                    </p>

                                    <div class="mode-warning" style="margin-bottom: 16px; border-color: #3b82f644; background: #3b82f60a">
                                        <span class="mode-warning-icon" style="color: #3b82f6">"ℹ"</span>
                                        <span style="font-size: 11px">
                                            "During calibration, the level shifter OE and all e-fuses are automatically disabled "
                                            "for safety. They are restored when calibration completes."
                                        </span>
                                    </div>

                                    // Channel status cards
                                    <div style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; margin-bottom: 20px">
                                        {[("VADJ1", 1u8, "#06b6d4"), ("VADJ2", 2u8, "#ff4d6a")].into_iter().map(|(name, ch, color)| {
                                            let has_cal = idac_st.channels.get(ch as usize).map(|c| c.calibrated).unwrap_or(false);
                                            let midpoint = idac_st.channels.get(ch as usize).map(|c| c.midpoint_v).unwrap_or(0.0);
                                            let v_min = idac_st.channels.get(ch as usize).map(|c| c.v_min).unwrap_or(0.0);
                                            let v_max = idac_st.channels.get(ch as usize).map(|c| c.v_max).unwrap_or(0.0);
                                            let start_cal_fn = start_cal.clone();
                                            view! {
                                                <div class="card" style=format!("border-left: 3px solid {}", color)>
                                                    <div class="card-body" style="padding: 16px">
                                                        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                                            <span style=format!("font-weight: 700; color: {}; font-size: 14px", color)>{name}</span>
                                                            {if has_cal {
                                                                view! { <span style="color: #10b981; font-size: 12px; font-weight: 600">"Calibrated ✓"</span> }.into_any()
                                                            } else {
                                                                view! { <span style="color: #f59e0b; font-size: 12px">"Not calibrated"</span> }.into_any()
                                                            }}
                                                        </div>
                                                        <div style="font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); margin-bottom: 12px">
                                                            <div>{format!("Midpoint: {:.2} V", midpoint)}</div>
                                                            <div>{format!("Range: {:.1} V – {:.1} V", v_min, v_max)}</div>
                                                            <div>{format!("IDAC channel: {}", ch)}</div>
                                                        </div>
                                                        <button class="btn btn-primary" style="width: 100%; padding: 10px; font-size: 12px"
                                                            disabled=move || !idac.get().present
                                                            on:click=move |_| start_cal_fn(ch)
                                                        >{format!("Auto-Calibrate {}", name)}</button>
                                                    </div>
                                                </div>
                                            }
                                        }).collect::<Vec<_>>()}
                                    </div>

                                    {if !idac_st.present {
                                        view! {
                                            <div class="mode-warning" style="margin-bottom: 12px">
                                                <span class="mode-warning-icon">"⚠"</span>
                                                <span>"DS4424 not detected. Check I2C connection."</span>
                                            </div>
                                        }.into_any()
                                    } else {
                                        view! { <div></div> }.into_any()
                                    }}
                                </div>
                            </div>
                        }.into_any()
                    }

                    // ===== RUNNING =====
                    CalState::Running => {
                        let ch = cal_channel.get();
                        let ch_name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "IDAC" };
                        view! {
                            <div class="card" style="max-width: 700px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700">{format!("Calibrating {} (IDAC channel {})", ch_name, ch)}</span>
                                </div>
                                <div class="card-body" style="text-align: center; padding: 30px">
                                    <div style="margin-bottom: 20px">
                                        <div class="spinner" style="width: 48px; height: 48px; border: 4px solid var(--border); border-top-color: #3b82f6; border-radius: 50%; animation: spin 1s linear infinite; margin: 0 auto"></div>
                                    </div>
                                    <div style="font-size: 16px; font-weight: 700; color: #3b82f6; margin-bottom: 8px">
                                        "Auto-Calibrating..."
                                    </div>
                                    <p style="font-size: 12px; color: var(--text-dim); margin-bottom: 8px">
                                        "The device is sweeping IDAC codes and measuring the actual output "
                                        "voltage via the U23 self-test MUX and AD74416H Channel D."
                                    </p>
                                    <div style="font-size: 11px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">
                                        <div>"Level shifter OE: OFF (safety)"</div>
                                        <div>"E-fuses: ALL OFF (safety)"</div>
                                        <div>{format!("Measurement path: IDAC{} → DCDC → R divider → U23 → Ch D", ch)}</div>
                                    </div>

                                    // Log
                                    <div style="margin-top: 16px; max-height: 150px; overflow-y: auto; background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left">
                                        {move || cal_log.get().iter().rev().take(20).map(|l| {
                                            view! { <div>{l.clone()}</div> }
                                        }).collect::<Vec<_>>()}
                                    </div>
                                </div>
                            </div>
                            <style>"@keyframes spin { to { transform: rotate(360deg) } }"</style>
                        }.into_any()
                    }

                    // ===== COMPLETE =====
                    CalState::Complete => {
                        let ch = cal_channel.get();
                        let ch_name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "IDAC" };
                        let pts = cal_points.get();
                        let err = cal_error_mv.get();

                        view! {
                            <div class="card" style="max-width: 700px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700; color: #10b981">"Calibration Complete"</span>
                                </div>
                                <div class="card-body" style="text-align: center; padding: 24px">
                                    <div style="font-size: 48px; margin-bottom: 8px; color: #10b981">"✓"</div>
                                    <div style="font-size: 16px; font-weight: 700; margin-bottom: 4px">
                                        {format!("{} calibrated successfully", ch_name)}
                                    </div>
                                    <div style="font-size: 13px; color: var(--text-dim); margin-bottom: 16px">
                                        {format!("{} calibration points · verification error: {:.1} mV", pts, err)}
                                    </div>

                                    <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-bottom: 16px; font-family: 'JetBrains Mono', monospace; font-size: 11px">
                                        <div class="card" style="padding: 8px">
                                            <div style="color: var(--text-dim)">"Points"</div>
                                            <div style="font-weight: 700; font-size: 16px">{format!("{}", pts)}</div>
                                        </div>
                                        <div class="card" style="padding: 8px">
                                            <div style="color: var(--text-dim)">"Error"</div>
                                            <div style=format!("font-weight: 700; font-size: 16px; color: {}", if err < 50.0 { "#10b981" } else { "#f59e0b" })>
                                                {format!("{:.1} mV", err)}
                                            </div>
                                        </div>
                                        <div class="card" style="padding: 8px">
                                            <div style="color: var(--text-dim)">"Saved"</div>
                                            <div style="font-weight: 700; font-size: 16px; color: #10b981">"NVS ✓"</div>
                                        </div>
                                    </div>

                                    <p style="font-size: 11px; color: var(--text-dim); margin-bottom: 16px">
                                        "Calibration data persists across reboots. Level shifter and e-fuses have been restored."
                                    </p>

                                    // Log
                                    <div style="margin-bottom: 16px; max-height: 200px; overflow-y: auto; background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left">
                                        {move || cal_log.get().iter().map(|l| {
                                            view! { <div>{l.clone()}</div> }
                                        }).collect::<Vec<_>>()}
                                    </div>

                                    <div style="display: flex; gap: 8px; justify-content: center">
                                        <button class="btn" on:click=move |_| set_cal_state.set(CalState::Idle)>"Done"</button>
                                        <button class="btn btn-primary" on:click=move |_| {
                                            set_cal_log.set(Vec::new());
                                            set_cal_state.set(CalState::Idle);
                                        }>"Calibrate Another"</button>
                                    </div>
                                </div>
                            </div>
                        }.into_any()
                    }

                    // ===== FAILED =====
                    CalState::Failed => {
                        let ch = cal_channel.get();
                        let ch_name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "IDAC" };

                        view! {
                            <div class="card" style="max-width: 700px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700; color: #ef4444">"Calibration Failed"</span>
                                </div>
                                <div class="card-body" style="text-align: center; padding: 24px">
                                    <div style="font-size: 48px; margin-bottom: 8px; color: #ef4444">"✗"</div>
                                    <div style="font-size: 14px; font-weight: 700; margin-bottom: 8px">
                                        {format!("{} calibration failed", ch_name)}
                                    </div>
                                    <p style="font-size: 12px; color: var(--text-dim); margin-bottom: 12px">
                                        "Check that the supply is enabled and the self-test MUX (U23) is connected. "
                                        "Ensure IO 10 is not in analog mode (safety interlock)."
                                    </p>

                                    <div style="margin-bottom: 16px; max-height: 200px; overflow-y: auto; background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left">
                                        {move || cal_log.get().iter().map(|l| {
                                            view! { <div>{l.clone()}</div> }
                                        }).collect::<Vec<_>>()}
                                    </div>

                                    <button class="btn" on:click=move |_| set_cal_state.set(CalState::Idle)>"Back"</button>
                                </div>
                            </div>
                        }.into_any()
                    }
                }
            }}
        </div>
    }
}
