use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::tauri_bridge::*;

/// DS4424 IDAC voltage calculator + control tab.
/// Shows 3 channels with voltage math, slider control, and calibration status.
#[component]
pub fn VoltagesTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (idac, set_idac) = signal(IdacState::default());
    let slider_vals: [RwSignal<f64>; 3] = std::array::from_fn(|_| RwSignal::new(0.0));
    let dirty: [RwSignal<bool>; 3] = std::array::from_fn(|_| RwSignal::new(false));
    let code_vals: [RwSignal<i32>; 3] = std::array::from_fn(|_| RwSignal::new(0));
    let code_dirty: [RwSignal<bool>; 3] = std::array::from_fn(|_| RwSignal::new(false));

    // Poll IDAC status periodically
    let set_idac_clone = set_idac.clone();
    Effect::new(move |_| {
        let _ = state.get(); // subscribe to state changes
        spawn_local(async move {
            if let Some(st) = fetch_idac_status().await {
                set_idac_clone.set(st);
            }
        });
    });

    let ch_colors = ["#10b981", "#06b6d4", "#ff4d6a"];
    let ch_titles = [
        "Level Shifter Voltage (LTM8078 Out2)",
        "V_ADJ1 — Domain A (LTM8063 #1 → P1, P2)",
        "V_ADJ2 — Domain B (LTM8063 #2 → P3, P4)",
    ];
    let ch_vfb = [0.8f32, 0.774, 0.774];
    let ch_rint = 249.0f32; // kΩ for all

    view! {
        <div class="tab-content">
            <div class="tab-desc">"DS4424 IDAC voltage regulator control. Adjusts the output voltage of the DCDC converters (V_ADJ1, V_ADJ2) and the level shifter voltage by injecting current into the feedback network. Calibrate first for accurate voltage setting."</div>
            {move || {
                let st = idac.get();
                if !st.present {
                    return view! {
                        <div class="card">
                            <div class="card-header">
                                <span class="channel-func">"DS4424 IDAC"</span>
                            </div>
                            <div class="card-body">
                                <div class="mode-warning">
                                    <span class="mode-warning-icon">"⚠"</span>
                                    <span>"DS4424 not detected on I2C bus (0x20). Check hardware connection."</span>
                                </div>
                            </div>
                        </div>
                    }.into_any();
                }

                view! {
                    <div class="channel-grid-wide" style="grid-template-columns: repeat(3, 1fr)">
                        {st.channels.into_iter().enumerate().map(|(i, ch)| {
                            let color = ch_colors[i];
                            let v_fb = ch_vfb[i];

                            // Sync slider from device if not being dragged
                            if !dirty[i].get() {
                                slider_vals[i].set(ch.target_v as f64);
                            }
                            if !code_dirty[i].get() {
                                code_vals[i].set(ch.code as i32);
                            }

                            let display_code = if code_dirty[i].get() { code_vals[i].get() as i8 } else { ch.code };
                            // Compute preview voltage from code using calibration data
                            let code_voltage = idac_interpolate_voltage(&ch, display_code);
                            let raw_display_v = if code_dirty[i].get() {
                                code_voltage
                            } else if dirty[i].get() {
                                slider_vals[i].get() as f32
                            } else {
                                ch.target_v
                            };
                            let display_v = raw_display_v.clamp(ch.v_min, ch.v_max);

                            // Compute R_FB
                            let r_fb = ch_rint / (ch.midpoint_v / v_fb - 1.0);
                            // Compute R_FS (I_FS = 50µA)
                            let ifs_ua = 50.0f32;
                            let r_fs = (0.976 * 127.0) / (16.0 * ifs_ua * 1e-6) / 1000.0;

                            // Voltage limits: always use hardware limits (3-15V for VADJ, 1.7-5.2V for LShift)
                            let safe_min = ch.v_min;
                            let safe_max = ch.v_max;

                            let pct = if safe_max > safe_min {
                                ((display_v - safe_min) / (safe_max - safe_min) * 100.0).clamp(0.0, 100.0)
                            } else { 50.0 };
                            // Compute DAC code limits for safe voltage range
                            // Sink (negative code) raises voltage, Source (positive code) lowers voltage
                            let step_v = ch.step_mv / 1000.0;
                            // Max sink code: how many steps from midpoint to safe_max
                            let max_sink_code = if step_v > 0.0 {
                                (((safe_max - ch.midpoint_v) / step_v).floor() as i32).clamp(0, 127)
                            } else { 0 };
                            // Max source code: how many steps from midpoint down to safe_min
                            let max_src_code = if step_v > 0.0 {
                                (((ch.midpoint_v - safe_min) / step_v).floor() as i32).clamp(0, 127)
                            } else { 0 };
                            // DAC code range: -max_sink_code (raises V) to +max_src_code (lowers V)
                            let code_min: i32 = -(max_sink_code as i32);  // most negative = highest voltage
                            let code_max: i32 = max_src_code as i32;      // most positive = lowest voltage

                            let ch_idx = i as u8;

                            view! {
                                <div class="card" style=format!("border-top: 3px solid {}", color)>
                                    <div class="card-header" style="flex-direction: column; align-items: flex-start; gap: 2px">
                                        <div style=format!("font-weight: 700; color: {}", color)>
                                            {ch_titles[i]}
                                        </div>
                                        <div style="display: flex; gap: 16px; font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim)">
                                            <span>{format!("V_FB: {}V", v_fb)}</span>
                                            <span>{format!("R_int: {}kΩ", ch_rint)}</span>
                                        </div>
                                    </div>
                                    <div class="card-body">
                                        // Midpoint + R_FB + I_FS + R_FS
                                        <div style="display: grid; grid-template-columns: auto 1fr; gap: 4px 12px; font-size: 11px; font-family: 'JetBrains Mono', monospace; margin-bottom: 12px">
                                            <span style="color: var(--text-dim)">"Midpoint:"</span>
                                            <span style="font-weight: 600">{format!("{:.1}V", ch.midpoint_v)}</span>
                                            <span style="color: var(--text-dim)">"R_FB:"</span>
                                            <span>{format!("{:.1}kΩ", r_fb)}<span style="color: var(--text-dim); font-size: 9px">" (FB→GND)"</span></span>
                                            <span style="color: var(--text-dim)">"I_FS:"</span>
                                            <span>{format!("{:.0}µA", ifs_ua)}</span>
                                            <span style="color: var(--text-dim)">"R_FS:"</span>
                                            <span>{format!("{:.0}kΩ", r_fs)}</span>
                                        </div>

                                        // Step info box
                                        <div style="background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 8px 12px; font-size: 10px; font-family: 'JetBrains Mono', monospace; margin-bottom: 12px; line-height: 1.8">
                                            <div>"Step: "<span style=format!("color: {}", color)>{format!("{:.2}mV", ch.step_mv)}</span>"/step"</div>
                                            <div>"Safe range: "<span style=format!("color: {}", color)>{format!("{:.2}V – {:.2}V", safe_min, safe_max)}</span>
                                                {format!(" (Δ={:.0}mV)", (safe_max - safe_min) * 1000.0)}</div>
                                            <div>{format!("Codes: {} to {} (sink {} / source {})", code_min, code_max, max_sink_code, max_src_code)}</div>
                                        </div>

                                        // DAC code display
                                        <div style="text-align: center; font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); margin-bottom: 4px">
                                            {format!("DAC: 0x{:02X} — {} · {:.2}mV/step",
                                                if display_code >= 0 { 0x80u8 | display_code as u8 } else { (-display_code) as u8 },
                                                if display_code == 0 { "zero".to_string() }
                                                else if display_code < 0 { format!("sink {} (↑V)", -display_code) }
                                                else { format!("source {} (↓V)", display_code) },
                                                ch.step_mv
                                            )}
                                        </div>

                                        // Code slider (preview only — right = higher V)
                                        <input type="range" class="slider slider-colored"
                                            style=format!("--slider-color: {}; width: 100%", color)
                                            min=(-code_max) max=(max_sink_code) step="1"
                                            prop:value=move || (-code_vals[i].get()).clamp(-code_max, max_sink_code as i32)
                                            on:input=move |e| {
                                                if let Ok(v) = event_target_value(&e).parse::<i32>() {
                                                    let clamped = v.clamp(-code_max, max_sink_code as i32);
                                                    code_vals[i].set(-clamped);
                                                    code_dirty[i].set(true);
                                                    dirty[i].set(true);
                                                }
                                            }
                                        />
                                        <div class="slider-labels" style="font-size: 9px">
                                            <span>{format!("{:.1}V", safe_min)}</span>
                                            <span>{format!("{:.1}V", ch.midpoint_v)}</span>
                                            <span>{format!("{:.1}V", safe_max)}</span>
                                        </div>

                                        // Big voltage preview
                                        <div style=format!("text-align: center; font-size: 32px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: {}; padding: 8px 0 2px; letter-spacing: -1px", color)>
                                            {format!("{:.3}V", display_v)}
                                        </div>
                                        <div style="text-align: center; font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; margin-bottom: 4px">
                                            {format!("DAC code: {} | ", display_code)}
                                            {if display_code == 0 { "Midpoint".to_string() }
                                             else if display_code < 0 { format!("Sink {}", -display_code) }
                                             else { format!("Source {}", display_code) }}
                                        </div>

                                        // "Preview" indicator when dirty
                                        {if code_dirty[i].get() {
                                            view! {
                                                <div style="text-align: center; font-size: 10px; color: #f59e0b; font-weight: 600; margin-bottom: 4px">
                                                    "⚠ Preview — not applied yet"
                                                </div>
                                            }.into_any()
                                        } else {
                                            view! { <div></div> }.into_any()
                                        }}

                                        // Voltage bar
                                        <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                            <div class="bar-fill-dynamic" style=format!("width: {}%", pct)></div>
                                        </div>
                                        <div class="slider-labels" style="font-size: 9px">
                                            <span>{format!("{:.1}V", safe_min)}</span>
                                            <span>{format!("{:.1}V", safe_max)}</span>
                                        </div>

                                        // Voltage input + SET button
                                        <div style="display: flex; gap: 8px; align-items: center; margin-top: 10px">
                                            <input type="number" class="number-input" style="flex: 1"
                                                min=safe_min max=safe_max step="0.01"
                                                value=format!("{:.3}", display_v)
                                                on:change=move |e| {
                                                    if let Ok(v) = event_target_value(&e).parse::<f64>() {
                                                        slider_vals[i].set(v);
                                                        dirty[i].set(true);
                                                        code_dirty[i].set(false);
                                                    }
                                                }
                                            />
                                            <span style="font-size: 12px; font-weight: 600; color: var(--text-dim)">"V"</span>
                                            <button
                                                style=format!("padding: 6px 20px; border-radius: 6px; border: none; background: {}; color: #0f1117; font-weight: 700; font-size: 12px; cursor: pointer; font-family: 'JetBrains Mono', monospace", color)
                                                on:click=move |_| {
                                                    if code_dirty[i].get_untracked() {
                                                        let code = code_vals[i].get_untracked();
                                                        send_idac_code(ch_idx, code as i8);
                                                    } else if dirty[i].get_untracked() {
                                                        let v = slider_vals[i].get_untracked() as f32;
                                                        let clamped = v.clamp(safe_min, safe_max);
                                                        send_idac_voltage(ch_idx, clamped);
                                                    }
                                                    dirty[i].set(false);
                                                    code_dirty[i].set(false);
                                                }
                                            >"SET"</button>
                                        </div>

                                        // Calibration status
                                        <div style="font-size: 10px; font-family: 'JetBrains Mono', monospace; margin-top: 8px; padding-top: 8px; border-top: 1px solid var(--border); color: var(--text-dim)">
                                            <span>"Calibration: "</span>
                                            {if ch.calibrated {
                                                view! { <span style="color: #10b981; font-weight: 600">"Active ✓"</span> }.into_any()
                                            } else {
                                                view! { <span style="color: var(--text-dim)">"Not calibrated (using formula)"</span> }.into_any()
                                            }}
                                        </div>

                                        // Formula reference
                                        <div style="font-size: 9px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); line-height: 1.8; margin-top: 6px">
                                            <div><b style="color: var(--text)">{if i == 0 { "LTM8078:" } else { "LTM8063:" }}</b>
                                                {format!(" R_FB={}k/(V_mid/{}-1)", ch_rint, v_fb)}</div>
                                            <div><b style="color: var(--text)">"DS4424:"</b>" R_FS=(0.976×127)/(16×I_FS)"</div>
                                            <div><b style="color: var(--text)">"Range:"</b>
                                                {format!(" {:.1}V–{:.1}V", ch.v_min, ch.v_max)}
                                                {if ch.calibrated { " (calibrated)" } else { " (formula)" }}</div>
                                        </div>
                                    </div>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                }.into_any()
            }}
        </div>
    }
}
