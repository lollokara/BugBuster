use leptos::prelude::*;
use leptos::task::spawn_local;
use std::time::Duration;
use wasm_bindgen::JsValue;
use crate::tauri_bridge::*;

#[derive(Clone, Copy, PartialEq)]
enum CalState {
    Idle,
    Running,
    Complete,
    Failed,
}

const CAL_TOTAL_POINTS: u32 = 100;

#[allow(clippy::too_many_arguments)]
fn do_auto_cal(
    ch: u8,
    set_cal_state: WriteSignal<CalState>,
    set_cal_channel: WriteSignal<u8>,
    set_cal_points: WriteSignal<u32>,
    set_cal_last_voltage_v: WriteSignal<f32>,
    set_cal_error_mv: WriteSignal<f32>,
    set_last_points: WriteSignal<u32>,
    set_cal_log: WriteSignal<Vec<String>>,
) {
    set_cal_channel.set(ch);
    set_cal_state.set(CalState::Running);
    set_cal_log.set(Vec::new());
    set_cal_points.set(0);
    set_cal_last_voltage_v.set(-1.0);
    set_cal_error_mv.set(0.0);
    set_last_points.set(0);

    let ch_name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "IDAC" };
    set_cal_log.update(|l| l.push(format!("Starting auto-calibration for {} (IDAC ch {})", ch_name, ch)));
    set_cal_log.update(|l| l.push("Safety: disabling level shifter OE and all e-fuses...".into()));
    set_cal_log.update(|l| l.push("Measuring via U23 self-test MUX + AD74416H Channel D...".into()));

    spawn_local(async move {
        let args = serde_wasm_bindgen::to_value(
            &serde_json::json!({"channel": ch})
        ).unwrap();

        let result = invoke("selftest_auto_calibrate", args).await;

        if let Ok(r) = serde_wasm_bindgen::from_value::<serde_json::Value>(result) {
            let status = r.get("status").and_then(|v| v.as_u64()).unwrap_or(3) as u8;
            if status == 3 {
                set_cal_log.update(|l| l.push("Calibration start rejected (busy/interlock/error).".into()));
                set_cal_state.set(CalState::Failed);
            } else {
                set_cal_log.update(|l| l.push("Calibration started. Polling status...".into()));
            }
        } else {
            set_cal_log.update(|l| l.push("Error: failed to start calibration".into()));
            set_cal_state.set(CalState::Failed);
        }
    });
}

#[component]
pub fn CalibrationTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (cal_state, set_cal_state) = signal(CalState::Idle);
    let (idac, set_idac) = signal(IdacState::default());
    let (cal_channel, set_cal_channel) = signal(0u8);
    let (cal_log, set_cal_log) = signal(Vec::<String>::new());
    let (cal_points, set_cal_points) = signal(0u32);
    let (cal_last_voltage_v, set_cal_last_voltage_v) = signal(-1.0f32);
    let (cal_error_mv, set_cal_error_mv) = signal(0.0f32);
    let (last_points, set_last_points) = signal(0u32);
    let poll_handle = RwSignal::new(None::<IntervalHandle>);

    Effect::new(move |_| {
        let _ = state.get();
        spawn_local(async move {
            if let Some(st) = fetch_idac_status().await {
                set_idac.set(st);
            }
        });
    });

    // While calibration is running, poll selftest status on a fixed interval.
    Effect::new(move |_| {
        let running = cal_state.get() == CalState::Running;
        if running {
            if poll_handle.get_untracked().is_none() {
                let handle = leptos::prelude::set_interval_with_handle(
                    move || {
                        if cal_state.get_untracked() != CalState::Running {
                            return;
                        }
                        spawn_local(async move {
                            let result = invoke("selftest_status", JsValue::NULL).await;
                            if let Ok(v) = serde_wasm_bindgen::from_value::<serde_json::Value>(result) {
                                let cal = v.get("cal").cloned().unwrap_or(serde_json::Value::Null);
                                let status = cal.get("status").and_then(|x| x.as_u64()).unwrap_or(0) as u8;
                                let points = cal.get("points").and_then(|x| x.as_u64()).unwrap_or(0) as u32;
                                let measured_v = cal.get("lastVoltageV").and_then(|x| x.as_f64()).unwrap_or(-1.0) as f32;
                                let error = cal.get("errorMv").and_then(|x| x.as_f64()).unwrap_or(0.0) as f32;
                                set_cal_points.set(points);
                                set_cal_last_voltage_v.set(measured_v);
                                set_cal_error_mv.set(error);

                                let prev_points = last_points.get_untracked();
                                if points > prev_points {
                                    set_cal_log.update(|l| l.push(format!(
                                        "Progress: {}/{} points, measured {:.4} V",
                                        points, CAL_TOTAL_POINTS, measured_v
                                    )));
                                    set_last_points.set(points);
                                }

                                if status == 2 {
                                    set_cal_log.update(|l| l.push(format!("Complete: {} points, error = {:.1} mV", points, error)));
                                    set_cal_log.update(|l| l.push("Saved to NVS. Level shifter and e-fuses restored.".into()));
                                    set_cal_state.set(CalState::Complete);
                                } else if status == 3 {
                                    set_cal_log.update(|l| l.push(format!("Failed (status={})", status)));
                                    set_cal_state.set(CalState::Failed);
                                }
                            }
                        });
                    },
                    Duration::from_millis(400),
                ).ok();
                poll_handle.set(handle);
            }
        } else if let Some(h) = poll_handle.get_untracked() {
            h.clear();
            poll_handle.set(None);
        }
    });

    view! {
        <div class="tab-content">
            <div class="tab-desc">
                "Automatic DCDC voltage calibration via the on-board self-test MUX (U23). "
                "No external connections needed — the device measures its own supply rails internally."
            </div>
            {move || {
                match cal_state.get() {
                    CalState::Idle => {
                        let idac_st = idac.get();
                        view! {
                            <div class="card" style="max-width: 800px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700; font-size: 16px">"DCDC Auto-Calibration"</span>
                                </div>
                                <div class="card-body">
                                    <p style="color: var(--text-dim); margin-bottom: 16px; font-size: 12px">
                                        "Calibrate the DCDC converters by sweeping IDAC codes and measuring "
                                        "actual output voltages via the U23 self-test switch matrix and AD74416H Channel D. "
                                        "This compensates for component tolerances in the DS4424 feedback network."
                                    </p>

                                    <div class="mode-warning" style="margin-bottom: 16px; border-color: #3b82f644; background: #3b82f60a">
                                        <span class="mode-warning-icon" style="color: #3b82f6">"ℹ"</span>
                                        <span style="font-size: 11px">
                                            "During calibration, the level shifter OE and all e-fuses are automatically "
                                            "disabled for safety. They are restored when calibration completes. "
                                            "Disconnect external loads before starting."
                                        </span>
                                    </div>

                                    <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; margin-bottom: 20px">
                                        // VLOGIC
                                        {
                                            let has_cal = idac_st.channels.first().map(|c| c.calibrated).unwrap_or(false);
                                            let midpoint = idac_st.channels.first().map(|c| c.midpoint_v).unwrap_or(0.0);
                                            view! {
                                                <div class="card" style="border-left: 3px solid #10b981">
                                                    <div class="card-body" style="padding: 16px">
                                                        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                                            <span style="font-weight: 700; color: #10b981; font-size: 14px">"VLOGIC"</span>
                                                            {if has_cal {
                                                                view! { <span style="color: #10b981; font-size: 12px; font-weight: 600">"Calibrated ✓"</span> }.into_any()
                                                            } else {
                                                                view! { <span style="color: #f59e0b; font-size: 12px">"Not calibrated"</span> }.into_any()
                                                            }}
                                                        </div>
                                                        <div style="font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); margin-bottom: 12px">
                                                            {format!("Midpoint: {:.2} V | IDAC channel 0", midpoint)}
                                                        </div>
                                                        <button class="btn btn-primary" style="width: 100%; padding: 10px; font-size: 12px"
                                                            disabled=move || !idac.get().present
                                                            on:click=move |_| do_auto_cal(0, set_cal_state, set_cal_channel, set_cal_points, set_cal_last_voltage_v, set_cal_error_mv, set_last_points, set_cal_log)
                                                        >"Auto-Calibrate VLOGIC"</button>
                                                    </div>
                                                </div>
                                            }
                                        }
                                        // VADJ1
                                        {
                                            let has_cal = idac_st.channels.get(1).map(|c| c.calibrated).unwrap_or(false);
                                            let midpoint = idac_st.channels.get(1).map(|c| c.midpoint_v).unwrap_or(0.0);
                                            view! {
                                                <div class="card" style="border-left: 3px solid #06b6d4">
                                                    <div class="card-body" style="padding: 16px">
                                                        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                                            <span style="font-weight: 700; color: #06b6d4; font-size: 14px">"VADJ1"</span>
                                                            {if has_cal {
                                                                view! { <span style="color: #10b981; font-size: 12px; font-weight: 600">"Calibrated ✓"</span> }.into_any()
                                                            } else {
                                                                view! { <span style="color: #f59e0b; font-size: 12px">"Not calibrated"</span> }.into_any()
                                                            }}
                                                        </div>
                                                        <div style="font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); margin-bottom: 12px">
                                                            {format!("Midpoint: {:.2} V | IDAC channel 1", midpoint)}
                                                        </div>
                                                        <button class="btn btn-primary" style="width: 100%; padding: 10px; font-size: 12px"
                                                            disabled=move || !idac.get().present
                                                            on:click=move |_| do_auto_cal(1, set_cal_state, set_cal_channel, set_cal_points, set_cal_last_voltage_v, set_cal_error_mv, set_last_points, set_cal_log)
                                                        >"Auto-Calibrate VADJ1"</button>
                                                    </div>
                                                </div>
                                            }
                                        }
                                        // VADJ2
                                        {
                                            let has_cal = idac_st.channels.get(2).map(|c| c.calibrated).unwrap_or(false);
                                            let midpoint = idac_st.channels.get(2).map(|c| c.midpoint_v).unwrap_or(0.0);
                                            view! {
                                                <div class="card" style="border-left: 3px solid #ff4d6a">
                                                    <div class="card-body" style="padding: 16px">
                                                        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                                            <span style="font-weight: 700; color: #ff4d6a; font-size: 14px">"VADJ2"</span>
                                                            {if has_cal {
                                                                view! { <span style="color: #10b981; font-size: 12px; font-weight: 600">"Calibrated ✓"</span> }.into_any()
                                                            } else {
                                                                view! { <span style="color: #f59e0b; font-size: 12px">"Not calibrated"</span> }.into_any()
                                                            }}
                                                        </div>
                                                        <div style="font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); margin-bottom: 12px">
                                                            {format!("Midpoint: {:.2} V | IDAC channel 2", midpoint)}
                                                        </div>
                                                        <button class="btn btn-primary" style="width: 100%; padding: 10px; font-size: 12px"
                                                            disabled=move || !idac.get().present
                                                            on:click=move |_| do_auto_cal(2, set_cal_state, set_cal_channel, set_cal_points, set_cal_last_voltage_v, set_cal_error_mv, set_last_points, set_cal_log)
                                                        >"Auto-Calibrate VADJ2"</button>
                                                    </div>
                                                </div>
                                            }
                                        }
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

                    CalState::Running => {
                        let ch = cal_channel.get();
                        let ch_name = match ch { 1 => "VADJ1", 2 => "VADJ2", _ => "IDAC" };
                        view! {
                            <div class="card" style="max-width: 700px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700">{format!("Calibrating {} ...", ch_name)}</span>
                                </div>
                                <div class="card-body" style="text-align: center; padding: 30px">
                                    <div style="font-size: 16px; font-weight: 700; color: #3b82f6; margin-bottom: 12px">
                                        "Auto-Calibrating"
                                    </div>
                                    <p style="font-size: 12px; color: var(--text-dim); margin-bottom: 16px">
                                        "Sweeping IDAC codes and measuring actual output voltage via U23 self-test MUX. "
                                        "Level shifter OE and all e-fuses are OFF for safety."
                                    </p>
                                    <div style="font-size: 11px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 16px">
                                        {format!("IDAC{} → DCDC → R divider → U23 → Ch D ADC", ch)}
                                    </div>
                                    <div style="font-size: 11px; color: #3b82f6; font-family: 'JetBrains Mono', monospace; margin-bottom: 8px">
                                        {move || format!("Progress: {}/{} points", cal_points.get(), CAL_TOTAL_POINTS)}
                                    </div>
                                    <div style="font-size: 11px; color: #10b981; font-family: 'JetBrains Mono', monospace; margin-bottom: 8px">
                                        {move || format!("Measured: {:.4} V", cal_last_voltage_v.get())}
                                    </div>
                                    <div style="max-height: 120px; overflow-y: auto; background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left">
                                        {move || cal_log.get().iter().rev().take(20).map(|l| {
                                            view! { <div>{l.clone()}</div> }
                                        }).collect::<Vec<_>>()}
                                    </div>
                                </div>
                            </div>
                        }.into_any()
                    }

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
                                    <div style="font-size: 16px; font-weight: 700; margin-bottom: 8px">
                                        {format!("{} calibrated — {} points, error {:.1} mV", ch_name, pts, err)}
                                    </div>
                                    <p style="font-size: 12px; color: var(--text-dim); margin-bottom: 16px">
                                        "Saved to NVS flash. Persists across reboots. Level shifter and e-fuses restored."
                                    </p>
                                    <div style="max-height: 150px; overflow-y: auto; background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left; margin-bottom: 16px">
                                        {move || cal_log.get().iter().map(|l| {
                                            view! { <div>{l.clone()}</div> }
                                        }).collect::<Vec<_>>()}
                                    </div>
                                    <div style="display: flex; gap: 8px; justify-content: center">
                                        <button class="btn" on:click=move |_| set_cal_state.set(CalState::Idle)>"Done"</button>
                                        <button class="btn btn-primary" on:click=move |_| set_cal_state.set(CalState::Idle)>"Calibrate Another"</button>
                                    </div>
                                </div>
                            </div>
                        }.into_any()
                    }

                    CalState::Failed => {
                        view! {
                            <div class="card" style="max-width: 700px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700; color: #ef4444">"Calibration Failed"</span>
                                </div>
                                <div class="card-body" style="text-align: center; padding: 24px">
                                    <p style="font-size: 12px; color: var(--text-dim); margin-bottom: 16px">
                                        "Check that the supply is enabled and U23 is connected. "
                                        "Ensure IO 9 is not in analog mode (safety interlock)."
                                    </p>
                                    <div style="max-height: 150px; overflow-y: auto; background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left; margin-bottom: 16px">
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
