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

// Connector pin definitions (excluding GND = pin 5)
// Pin 1 = VBUS (power), Pin 2-4 = MUX signal outputs
const CONNECTOR_PINS: &[(&str, &[(&str, &str)])] = &[
    ("P1", &[("VBUS (V_ADJ1)", "vbus"), ("Main", "sig"), ("Aux1", "sig"), ("Aux2", "sig")]),
    ("P2", &[("VBUS (V_ADJ1)", "vbus"), ("Main", "sig"), ("Aux1", "sig"), ("Aux2", "sig")]),
    ("P3", &[("VBUS (V_ADJ2)", "vbus"), ("Main", "sig"), ("Aux1", "sig"), ("Aux2", "sig")]),
    ("P4", &[("VBUS (V_ADJ2)", "vbus"), ("Main", "sig"), ("Aux1", "sig"), ("Aux2", "sig")]),
];

// PCA9535 control IDs
const PCA_VADJ1_EN: u8 = 0;
const PCA_VADJ2_EN: u8 = 1;
const PCA_EFUSE1_EN: u8 = 5;

// Map connector index to DCDC channel and E-Fuse
// P1→VADJ1+EFUSE1, P2→VADJ1+EFUSE2, P3→VADJ2+EFUSE3, P4→VADJ2+EFUSE4
fn connector_to_dcdc(conn: usize) -> (u8, u8, u8) {
    // Returns (idac_channel, pca_dcdc_enable, pca_efuse_enable)
    match conn {
        0 => (1, PCA_VADJ1_EN, PCA_EFUSE1_EN),     // P1 → IDAC1, VADJ1, EFUSE1
        1 => (1, PCA_VADJ1_EN, PCA_EFUSE1_EN + 1),  // P2 → IDAC1, VADJ1, EFUSE2
        2 => (2, PCA_VADJ2_EN, PCA_EFUSE1_EN + 2),  // P3 → IDAC2, VADJ2, EFUSE3
        3 => (2, PCA_VADJ2_EN, PCA_EFUSE1_EN + 3),  // P4 → IDAC2, VADJ2, EFUSE4
        _ => (0, 0, 0),
    }
}

// Signal pins go through level shifter (IDAC0 controls the voltage)
fn signal_pin_idac_channel() -> u8 { 0 }

#[derive(Clone, Copy, PartialEq)]
enum WizardStep {
    Status,         // Show cal status + Start button
    ConnectProbe,   // Ask user to connect ADC Ch A
    SelectOutput,   // Choose which connector pin
    Stabilizing,    // Waiting 5s for DCDC to stabilize
    Running,        // Sweeping DAC codes
    Complete,       // Done, show results
}

#[derive(Clone)]
struct CalTarget {
    connector: usize,  // 0-3 (P1-P4)
    pin: usize,        // 0-3 (pin 1-4)
    is_vbus: bool,     // true = power pin, false = signal pin
    idac_ch: u8,       // Which IDAC channel to calibrate
    dcdc_en: u8,       // PCA control for DCDC enable
    efuse_en: u8,      // PCA control for E-Fuse enable
}

#[component]
pub fn CalibrationTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (step, set_step) = signal(WizardStep::Status);
    let (idac, set_idac) = signal(IdacState::default());
    let (target, set_target) = signal::<Option<CalTarget>>(None);
    let (progress, set_progress) = signal(0u32);
    let (total_steps, set_total_steps) = signal(0u32);
    let (current_code, set_current_code) = signal(0i32);
    let (current_voltage, set_current_voltage) = signal(0.0f32);
    let (points_collected, set_points_collected) = signal(0u32);
    let (cal_log, set_cal_log) = signal(Vec::<String>::new());
    let (stabilize_countdown, set_stabilize_countdown) = signal(5u32);

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
        set_cal_log.update(|l| { l.push(msg); if l.len() > 100 { l.remove(0); } });
    };

    // Stabilization countdown timer
    let set_step_clone = set_step;
    let cd_read = stabilize_countdown;
    Effect::new(move |_| {
        let s = step.get();
        let cd = cd_read.get();
        if s == WizardStep::Stabilizing && cd > 0 {
            let set_cd = set_stabilize_countdown;
            let set_s = set_step_clone;
            let cur_cd = cd;
            spawn_local(async move {
                sleep_ms(1000).await;
                let new_cd = cur_cd.saturating_sub(1);
                set_cd.set(new_cd);
                if new_cd == 0 {
                    set_s.set(WizardStep::Running);
                }
            });
        }
    });

    // Main calibration sweep - triggers when step becomes Running
    Effect::new(move |_| {
        if step.get() != WizardStep::Running { return; }
        let tgt = match target.get() { Some(t) => t, None => return };

        let ds = state.get_untracked();
        let idac_st = idac.get_untracked();
        let ch = tgt.idac_ch as usize;

        if ch >= idac_st.channels.len() { return; }
        let ch_cfg = &idac_st.channels[ch];
        let v_min = ch_cfg.v_min;
        let v_max = ch_cfg.v_max;
        // For VADJ1/2 (ch 1,2), ADC max is 12V so clamp cal to 12V
        let cal_max = if tgt.idac_ch >= 1 { v_max.min(12.0) } else { v_max };
        let cal_min = v_min;
        let idac_ch = tgt.idac_ch;
        let dac_step: i32 = 3;

        // Calculate total steps: sweep sink (0 to -127 by 3) + source (0 to 127 by 3)
        let sink_steps = 127 / dac_step;
        let src_steps = 127 / dac_step;
        let total = 1 + sink_steps + src_steps; // +1 for midpoint
        set_total_steps.set(total as u32);
        set_progress.set(0);
        set_points_collected.set(0);

        add_log(format!("Starting calibration IDAC{} range [{:.2}V, {:.2}V] cal_max={:.2}V", idac_ch, cal_min, v_max, cal_max));

        // Clear old calibration
        spawn_local(async move {
            // Clear existing cal data
            let args = serde_wasm_bindgen::to_value(&serde_json::json!({"channel": idac_ch})).unwrap();
            let _ = invoke("idac_cal_clear", args).await;

            // Set to midpoint first
            send_idac_code(idac_ch, 0);
            sleep_ms(200).await;

            let mut pts = 0u32;
            let mut step_count = 0u32;

            // Helper: read ADC 3 times and average
            async fn read_adc_avg(state_sig: ReadSignal<DeviceState>) -> f32 {
                let mut sum = 0.0f32;
                for _ in 0..3 {
                    sleep_ms(60).await;
                    let ds = state_sig.get_untracked();
                    if !ds.channels.is_empty() {
                        sum += ds.channels[0].adc_value;
                    }
                }
                sum / 3.0
            }

            // Midpoint reading
            sleep_ms(100).await;
            let v = read_adc_avg(state).await;
            send_idac_cal_add_point(idac_ch, 0, v);
            pts += 1;
            step_count += 1;
            set_progress.set(step_count);
            set_current_code.set(0);
            set_current_voltage.set(v);
            set_points_collected.set(pts);
            set_cal_log.update(|l| l.push(format!("  DAC=0 → {:.4}V (midpoint)", v)));

            // Sweep SINK direction (negative codes → raise voltage)
            let mut code: i32 = -dac_step;
            while code >= -127 {
                send_idac_code(idac_ch, code as i8);
                sleep_ms(100).await;
                let v = read_adc_avg(state).await;

                send_idac_cal_add_point(idac_ch, code as i8, v);
                pts += 1;
                step_count += 1;
                set_progress.set(step_count);
                set_current_code.set(code);
                set_current_voltage.set(v);
                set_points_collected.set(pts);

                if v >= cal_max {
                    set_cal_log.update(|l| l.push(format!("  DAC={} → {:.4}V (hit cal max)", code, v)));
                    break;
                }
                code -= dac_step;
            }

            // Return to midpoint
            send_idac_code(idac_ch, 0);
            sleep_ms(200).await;

            // Sweep SOURCE direction (positive codes → lower voltage)
            code = dac_step;
            while code <= 127 {
                send_idac_code(idac_ch, code as i8);
                sleep_ms(100).await;
                let v = read_adc_avg(state).await;

                send_idac_cal_add_point(idac_ch, code as i8, v);
                pts += 1;
                step_count += 1;
                set_progress.set(step_count);
                set_current_code.set(code);
                set_current_voltage.set(v);
                set_points_collected.set(pts);

                if v <= cal_min {
                    set_cal_log.update(|l| l.push(format!("  DAC={} → {:.4}V (hit cal min)", code, v)));
                    break;
                }
                code += dac_step;
            }

            // Return to midpoint
            send_idac_code(idac_ch, 0);

            // Save to NVS
            send_idac_cal_save();
            set_cal_log.update(|l| l.push(format!("Calibration complete: {} points saved to NVS", pts)));

            set_step.set(WizardStep::Complete);
        });
    });

    view! {
        <div class="tab-content">
            {move || {
                let st = step.get();
                match st {
                    // ===== STATUS SCREEN =====
                    WizardStep::Status => {
                        let idac_st = idac.get();
                        view! {
                            <div class="card" style="max-width: 800px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700; font-size: 16px">"DCDC Voltage Calibration"</span>
                                </div>
                                <div class="card-body">
                                    <p style="color: var(--text-dim); margin-bottom: 16px; font-size: 12px">
                                        "Calibrate the 3 DCDC converters by measuring actual output voltages via the AD74416H ADC. "
                                        "This compensates for component tolerances in the DS4424 → DCDC feedback network."
                                    </p>

                                    <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; margin-bottom: 20px">
                                        {["Level Shifter (IDAC0)", "V_ADJ1 (IDAC1)", "V_ADJ2 (IDAC2)"].into_iter().enumerate().map(|(i, name)| {
                                            let colors = ["#10b981", "#06b6d4", "#ff4d6a"];
                                            let has_cal = idac_st.channels.get(i).map(|c| c.calibrated).unwrap_or(false);
                                            let midpoint = idac_st.channels.get(i).map(|c| c.midpoint_v).unwrap_or(0.0);
                                            view! {
                                                <div class="card" style=format!("border-left: 3px solid {}", colors[i])>
                                                    <div class="card-body" style="padding: 12px">
                                                        <div style=format!("font-weight: 600; color: {}; margin-bottom: 4px", colors[i])>{name}</div>
                                                        <div style="font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim)">
                                                            {format!("Midpoint: {:.1}V", midpoint)}
                                                        </div>
                                                        <div style="margin-top: 6px; font-size: 12px; font-weight: 600">
                                                            {if has_cal {
                                                                view! { <span style="color: #10b981">"Calibrated ✓"</span> }.into_any()
                                                            } else {
                                                                view! { <span style="color: #f59e0b">"Not calibrated"</span> }.into_any()
                                                            }}
                                                        </div>
                                                    </div>
                                                </div>
                                            }
                                        }).collect::<Vec<_>>()}
                                    </div>

                                    {if !idac_st.present {
                                        view! {
                                            <div class="mode-warning" style="margin-bottom: 12px">
                                                <span class="mode-warning-icon">"⚠"</span>
                                                <span>"DS4424 not detected. Connect hardware before calibrating."</span>
                                            </div>
                                        }.into_any()
                                    } else {
                                        view! { <div></div> }.into_any()
                                    }}

                                    <button class="btn btn-primary" style="width: 100%; padding: 12px; font-size: 14px"
                                        disabled=move || !idac.get().present
                                        on:click=move |_| {
                                            set_cal_log.set(Vec::new());
                                            set_step.set(WizardStep::ConnectProbe);
                                        }
                                    >"Start Calibration"</button>
                                </div>
                            </div>
                        }.into_any()
                    }

                    // ===== CONNECT PROBE =====
                    WizardStep::ConnectProbe => {
                        view! {
                            <div class="card" style="max-width: 700px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700">"Step 1: Connect ADC Probe"</span>
                                </div>
                                <div class="card-body">
                                    <div style="text-align: center; padding: 20px 0">
                                        <div style="font-size: 40px; margin-bottom: 12px">"🔌"</div>
                                        <p style="font-size: 14px; margin-bottom: 8px">
                                            "Connect "<b>"AD74416H Channel A"</b>" to the output you want to measure."
                                        </p>
                                        <p style="font-size: 12px; color: var(--text-dim); margin-bottom: 20px">
                                            "This can be a VBUS power pin (to calibrate V_ADJ1/V_ADJ2) or a logic signal pin (to calibrate the level shifter voltage)."
                                        </p>
                                    </div>

                                    <div style="display: flex; gap: 8px; justify-content: center">
                                        <button class="btn" on:click=move |_| set_step.set(WizardStep::Status)>"Cancel"</button>
                                        <button class="btn btn-primary" on:click=move |_| {
                                            // Configure ADC Ch A for VIN, 0-12V range, 200 SPS
                                            send_set_channel_function(0, 3); // VIN
                                            send_set_adc_config(0, 0, 0, 4); // LF-AGND, 0-12V, 200SPS
                                            set_step.set(WizardStep::SelectOutput);
                                        }>"Confirm — Probe Connected"</button>
                                    </div>
                                </div>
                            </div>
                        }.into_any()
                    }

                    // ===== SELECT OUTPUT =====
                    WizardStep::SelectOutput => {
                        view! {
                            <div class="card" style="max-width: 900px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700">"Step 2: Select Measurement Point"</span>
                                </div>
                                <div class="card-body">
                                    <p style="color: var(--text-dim); font-size: 12px; margin-bottom: 16px">
                                        "Click the connector pin where the ADC probe is connected. The device will enable the correct DCDC and E-Fuse."
                                    </p>

                                    <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px">
                                        {CONNECTOR_PINS.iter().enumerate().map(|(ci, (conn_name, pins))| {
                                            let dcdc_label = if ci < 2 { "V_ADJ1" } else { "V_ADJ2" };
                                            view! {
                                                <div class="card" style="border-color: var(--border)">
                                                    <div class="card-header" style="padding: 8px 12px">
                                                        <span style="font-weight: 700; font-size: 13px">{*conn_name}</span>
                                                        <span style="font-size: 10px; color: var(--text-dim)">{dcdc_label}</span>
                                                    </div>
                                                    <div class="card-body" style="padding: 8px; display: flex; flex-direction: column; gap: 4px">
                                                        {pins.iter().enumerate().map(|(pi, (pin_name, pin_type))| {
                                                            let is_vbus = *pin_type == "vbus";
                                                            let color = if is_vbus { "#ff4d6a" } else { "#10b981" };
                                                            let ci2 = ci;
                                                            let pi2 = pi;
                                                            view! {
                                                                <button
                                                                    style=format!("width: 100%; padding: 8px; border-radius: 6px; border: 1px solid {}44; background: {}11; color: {}; font-size: 11px; font-weight: 600; cursor: pointer; font-family: 'JetBrains Mono', monospace", color, color, color)
                                                                    on:click=move |_| {
                                                                        let (idac_ch, dcdc_en, efuse_en) = if is_vbus {
                                                                            connector_to_dcdc(ci2)
                                                                        } else {
                                                                            (signal_pin_idac_channel(), PCA_VADJ1_EN, PCA_EFUSE1_EN + ci2 as u8)
                                                                        };

                                                                        let tgt = CalTarget {
                                                                            connector: ci2, pin: pi2, is_vbus,
                                                                            idac_ch, dcdc_en, efuse_en,
                                                                        };

                                                                        // Enable hardware
                                                                        if is_vbus {
                                                                            send_pca_control(dcdc_en, true);
                                                                            send_pca_control(efuse_en, true);
                                                                            set_cal_log.update(|l| l.push(format!("Enabled DCDC (ctrl {}) + E-Fuse (ctrl {})", dcdc_en, efuse_en)));
                                                                        } else {
                                                                            // Enable level shifter + assert pin
                                                                            // Level shifter OE is GPIO14 (not available via PCA, skip for now)
                                                                            // Enable the DCDC that powers this connector
                                                                            let (_, vbus_dcdc, vbus_efuse) = connector_to_dcdc(ci2);
                                                                            send_pca_control(vbus_dcdc, true);
                                                                            send_pca_control(vbus_efuse, true);
                                                                            // Assert GPIO via MUX: set the direct switch for this pin's MUX device
                                                                            // MUX device = connector index, switch depends on pin
                                                                            let sw = match pi2 {
                                                                                1 => 0, // S1: direct GPIO for main output
                                                                                2 => 4, // S5: direct GPIO for aux1
                                                                                3 => 6, // S7: direct GPIO for aux2
                                                                                _ => 0,
                                                                            };
                                                                            send_mux_set_switch(ci2 as u8, sw, true);
                                                                            set_cal_log.update(|l| l.push(format!("Enabled DCDC + MUX{} SW{} for signal pin", ci2, sw)));
                                                                        }

                                                                        set_target.set(Some(tgt));
                                                                        set_stabilize_countdown.set(5);
                                                                        set_step.set(WizardStep::Stabilizing);
                                                                    }
                                                                >
                                                                    {format!("Pin {} — {}", pi2 + 1, pin_name)}
                                                                </button>
                                                            }
                                                        }).collect::<Vec<_>>()}
                                                    </div>
                                                </div>
                                            }
                                        }).collect::<Vec<_>>()}
                                    </div>

                                    <div style="margin-top: 12px; text-align: center">
                                        <button class="btn" on:click=move |_| set_step.set(WizardStep::Status)>"Cancel"</button>
                                    </div>
                                </div>
                            </div>
                        }.into_any()
                    }

                    // ===== STABILIZING =====
                    WizardStep::Stabilizing => {
                        let cd = stabilize_countdown.get();
                        let ds = state.get();
                        let adc_v = ds.channels.first().map(|c| c.adc_value).unwrap_or(0.0);
                        let tgt = target.get();
                        let tgt_name = tgt.as_ref().map(|t| {
                            let conn = CONNECTOR_PINS[t.connector].0;
                            let pin = CONNECTOR_PINS[t.connector].1[t.pin].0;
                            format!("{} Pin {} ({})", conn, t.pin + 1, pin)
                        }).unwrap_or_default();
                        let idac_ch = tgt.as_ref().map(|t| t.idac_ch).unwrap_or(0);
                        let idac_name = ["Level Shifter", "V_ADJ1", "V_ADJ2"][idac_ch as usize];

                        view! {
                            <div class="card" style="max-width: 600px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700">"Stabilizing..."</span>
                                </div>
                                <div class="card-body" style="text-align: center; padding: 30px">
                                    <div style="font-size: 48px; font-weight: 800; color: #f59e0b; margin-bottom: 8px">
                                        {format!("{}s", cd)}
                                    </div>
                                    <p style="font-size: 12px; color: var(--text-dim); margin-bottom: 12px">
                                        "Waiting for DCDC output to stabilize before sweeping..."
                                    </p>
                                    <div style="font-family: 'JetBrains Mono', monospace; font-size: 12px">
                                        <div>"Target: "<b>{tgt_name}</b></div>
                                        <div>"Calibrating: "<b>{idac_name}</b></div>
                                        <div>"ADC reading: "<b style="color: #3b82f6">{format!("{:.4}V", adc_v)}</b></div>
                                    </div>
                                </div>
                            </div>
                        }.into_any()
                    }

                    // ===== RUNNING =====
                    WizardStep::Running => {
                        let prog = progress.get();
                        let total = total_steps.get().max(1);
                        let pct = (prog as f64 / total as f64 * 100.0).min(100.0);
                        let code = current_code.get();
                        let voltage = current_voltage.get();
                        let pts = points_collected.get();
                        let tgt = target.get();
                        let idac_ch = tgt.as_ref().map(|t| t.idac_ch).unwrap_or(0);
                        let idac_name = ["Level Shifter", "V_ADJ1", "V_ADJ2"][idac_ch as usize];

                        view! {
                            <div class="card" style="max-width: 700px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700">{format!("Calibrating {} (IDAC{})", idac_name, idac_ch)}</span>
                                </div>
                                <div class="card-body">
                                    <div class="bar-gauge" style="--bar-color: #3b82f6; margin-bottom: 12px; height: 12px">
                                        <div class="bar-fill-dynamic" style=format!("width: {}%", pct)></div>
                                    </div>
                                    <div style="text-align: center; font-family: 'JetBrains Mono', monospace">
                                        <div style="font-size: 28px; font-weight: 800; color: #3b82f6; margin-bottom: 4px">
                                            {format!("{:.4}V", voltage)}
                                        </div>
                                        <div style="font-size: 11px; color: var(--text-dim)">
                                            {format!("DAC Code: {} | Step {}/{} | {} points", code, prog, total, pts)}
                                        </div>
                                    </div>

                                    // Log
                                    <div style="margin-top: 16px; max-height: 150px; overflow-y: auto; background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim)">
                                        {move || cal_log.get().iter().rev().take(20).map(|l| {
                                            view! { <div>{l.clone()}</div> }
                                        }).collect::<Vec<_>>()}
                                    </div>
                                </div>
                            </div>
                        }.into_any()
                    }

                    // ===== COMPLETE =====
                    WizardStep::Complete => {
                        let pts = points_collected.get();
                        let tgt = target.get();
                        let idac_ch = tgt.as_ref().map(|t| t.idac_ch).unwrap_or(0);
                        let idac_name = ["Level Shifter", "V_ADJ1", "V_ADJ2"][idac_ch as usize];

                        view! {
                            <div class="card" style="max-width: 600px; margin: 0 auto">
                                <div class="card-header">
                                    <span style="font-weight: 700; color: #10b981">"Calibration Complete"</span>
                                </div>
                                <div class="card-body" style="text-align: center; padding: 24px">
                                    <div style="font-size: 40px; margin-bottom: 8px">"✓"</div>
                                    <div style="font-size: 16px; font-weight: 700; margin-bottom: 8px">
                                        {format!("{} calibrated with {} points", idac_name, pts)}
                                    </div>
                                    <p style="font-size: 12px; color: var(--text-dim); margin-bottom: 16px">
                                        "Calibration data has been saved to NVS flash. It will persist across reboots."
                                    </p>

                                    // Log
                                    <div style="margin-bottom: 16px; max-height: 200px; overflow-y: auto; background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); text-align: left">
                                        {move || cal_log.get().iter().map(|l| {
                                            view! { <div>{l.clone()}</div> }
                                        }).collect::<Vec<_>>()}
                                    </div>

                                    <div style="display: flex; gap: 8px; justify-content: center">
                                        <button class="btn" on:click=move |_| {
                                            // Disable hardware: turn off DCDC and E-Fuses
                                            if let Some(tgt) = target.get_untracked() {
                                                send_pca_control(tgt.dcdc_en, false);
                                                send_pca_control(tgt.efuse_en, false);
                                            }
                                            // Return ADC ch A to high-Z
                                            send_set_channel_function(0, 0); // HIGH_IMP
                                            set_step.set(WizardStep::Status);
                                        }>"Done"</button>
                                        <button class="btn btn-primary" on:click=move |_| {
                                            // Disable current hardware
                                            if let Some(tgt) = target.get_untracked() {
                                                send_pca_control(tgt.dcdc_en, false);
                                                send_pca_control(tgt.efuse_en, false);
                                            }
                                            set_cal_log.set(Vec::new());
                                            set_step.set(WizardStep::ConnectProbe);
                                        }>"Calibrate Another"</button>
                                    </div>
                                </div>
                            </div>
                        }.into_any()
                    }
                }
            }}
        </div>
    }
}
