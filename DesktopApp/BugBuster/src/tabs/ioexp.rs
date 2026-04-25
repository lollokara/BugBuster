use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::tauri_bridge::*;

/// PCA control index mapping (matches http_transport.rs ctrl_names):
/// 0 = vadj1, 1 = vadj2, 2 = 15v, 3 = mux, 4 = usb, 5 = efuse1, 6 = efuse2, 7 = efuse3, 8 = efuse4
const SUPPLY_CONTROLS: &[(u8, &str, &str, &str)] = &[
    (0, "VADJ1",  "#10b981", "3-15V Rail A"),
    (1, "VADJ2",  "#06b6d4", "3-15V Rail B"),
    (2, "+/-15V", "#f59e0b", "AD74416H Analog"),
    (3, "LOGIC_EN", "#a855f7", "Main Logic Enable"),
    (4, "USB Hub", "#3b82f6", "USB Hub IC"),
];

#[component]
pub fn IoExpTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (ioexp, set_ioexp) = signal(IoExpState::default());

    // Poll PCA status whenever device state updates
    let set_ioexp_clone = set_ioexp;
    Effect::new(move |_| {
        let _ = state.get(); // subscribe to state changes
        spawn_local(async move {
            if let Some(st) = fetch_pca_status().await {
                set_ioexp_clone.set(st);
            }
        });
    });

    view! {
        <div class="tab-content">
            <div class="tab-desc">"PCA9535 16-bit GPIO expander status and control. Manages power supply enables (V_ADJ1, V_ADJ2, +/-15V, LOGIC_EN), E-Fuse output protection per connector (P1-P4), and monitors power-good signals."</div>

            {move || {
                let st = ioexp.get();

                if !st.present {
                    return view! {
                        <div class="card">
                            <div class="card-header">
                                <span class="channel-func">"PCA9535 IO Expander"</span>
                            </div>
                            <div class="card-body">
                                <div class="mode-warning">
                                    <span class="mode-warning-icon">"!"</span>
                                    <span>"PCA9535 not detected on I2C bus (0x23). Check hardware connection."</span>
                                </div>
                            </div>
                        </div>
                    }.into_any();
                }

                // Extract enable states for supply controls
                let enable_vals = [st.vadj1_en, st.vadj2_en, st.en_15v, st.en_mux, st.en_usb_hub];
                let efuses = st.efuses.clone();
                let logic_pg = st.logic_pg;
                let vadj1_pg = st.vadj1_pg;
                let vadj2_pg = st.vadj2_pg;

                view! {
                    <div>
                        // Header bar
                        <div style="display: flex; align-items: center; justify-content: space-between; padding: 12px 16px; margin-bottom: 16px; border-radius: 8px; background: var(--bg-secondary, #1a1a2e); border: 1px solid var(--border)">
                            <div>
                                <span style="font-size: 14px; font-weight: 700; color: #3b82f6">"PCA9535AHF"</span>
                                <span style="font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim); margin-left: 8px">"16-bit I2C GPIO Expander (0x23)"</span>
                            </div>
                            <div style="display: flex; align-items: center; gap: 8px">
                                <span style="font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim)">"Present:"</span>
                                <div style="width: 10px; height: 10px; border-radius: 50%; background: #10b981; box-shadow: 0 0 6px #10b981"></div>
                            </div>
                        </div>

                        // Power Good Status
                        <div style="margin-bottom: 16px">
                            <div style="font-size: 10px; font-weight: 600; color: var(--text-dim); margin-bottom: 8px; letter-spacing: 1px; text-transform: uppercase">"Power Good Status"</div>
                            <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px">
                                {render_power_good("LOGIC_EN", "Main Logic Enable", logic_pg)}
                                {render_power_good("VADJ1_PG", "LTM8063 #1 -> P1,P2", vadj1_pg)}
                                {render_power_good("VADJ2_PG", "LTM8063 #2 -> P3,P4", vadj2_pg)}
                            </div>
                        </div>

                        // Supply Enables
                        <div style="margin-bottom: 16px">
                            <div style="font-size: 10px; font-weight: 600; color: var(--text-dim); margin-bottom: 8px; letter-spacing: 1px; text-transform: uppercase">"Supply Enables"</div>
                            <div style="display: grid; grid-template-columns: repeat(5, 1fr); gap: 12px">
                                {SUPPLY_CONTROLS.iter().enumerate().map(|(i, (ctrl_idx, name, color, desc))| {
                                    let is_on = enable_vals[i];
                                    let ctrl = *ctrl_idx;
                                    let c = *color;
                                    view! {
                                        <div style=format!("padding: 12px; border-radius: 8px; text-align: center; cursor: pointer; transition: all 0.3s; {}",
                                            if is_on {
                                                format!("background: {}15; border: 1px solid {}50; box-shadow: 0 0 12px {}30", c, c, c)
                                            } else {
                                                "background: var(--bg-secondary, #1a1a2e); border: 1px solid var(--border)".into()
                                            }
                                        )
                                            on:click=move |_| { send_pca_control(ctrl, !is_on); }
                                        >
                                            <div style=format!("width: 8px; height: 8px; border-radius: 50%; margin: 0 auto 6px; transition: all 0.3s; {}",
                                                if is_on { format!("background: {}; box-shadow: 0 0 8px {}", c, c) } else { "background: #1e293b".into() }
                                            )></div>
                                            <div style=format!("font-size: 11px; font-weight: 700; font-family: 'JetBrains Mono', monospace; margin-bottom: 4px; {}",
                                                if is_on { format!("color: {}", c) } else { "color: var(--text-dim)".into() }
                                            )>{*name}</div>
                                            <div style="font-size: 10px; color: var(--text-dim)">{*desc}</div>
                                            <div style=format!("font-size: 10px; font-weight: 700; margin-top: 6px; {}",
                                                if is_on { format!("color: {}", c) } else { "color: var(--text-dim)".into() }
                                            )>{if is_on { "ON" } else { "OFF" }}</div>
                                        </div>
                                    }
                                }).collect::<Vec<_>>()}
                            </div>
                        </div>

                        // E-Fuse Output Protection
                        <div style="margin-bottom: 16px">
                            <div style="font-size: 10px; font-weight: 600; color: var(--text-dim); margin-bottom: 8px; letter-spacing: 1px; text-transform: uppercase">"E-Fuse Output Protection (TPS1641 x 4)"</div>
                            <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px">
                                {efuses.into_iter().enumerate().map(|(i, ef)| {
                                    let color = CH_COLORS[i];
                                    let efuse_ctrl = (5 + i) as u8; // efuse1=5, efuse2=6, etc.
                                    let rail = if ef.id <= 2 { "VADJ1" } else { "VADJ2" };
                                    let enabled = ef.enabled;
                                    let fault = ef.fault;

                                    view! {
                                        <div style=format!("padding: 16px; border-radius: 8px; {}",
                                            if fault {
                                                "background: rgba(239,68,68,0.1); border: 2px solid rgba(239,68,68,0.5); box-shadow: 0 0 16px rgba(239,68,68,0.3)"
                                            } else if enabled {
                                                "background: rgba(16,185,129,0.08); border: 2px solid rgba(16,185,129,0.3); box-shadow: 0 0 12px rgba(16,185,129,0.2)"
                                            } else {
                                                "background: var(--bg-secondary, #1a1a2e); border: 2px solid var(--border)"
                                            }
                                        )>
                                            // Header
                                            <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px">
                                                <div>
                                                    <span style=format!("font-size: 18px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: {}", color)>
                                                        {format!("P{}", ef.id)}
                                                    </span>
                                                    <div style="font-size: 10px; font-family: 'JetBrains Mono', monospace; color: var(--text-dim)">
                                                        {format!("TPS1641 . {}", rail)}
                                                    </div>
                                                </div>
                                                <div style=format!("width: 14px; height: 14px; border-radius: 50%; transition: all 0.3s; {}",
                                                    if fault {
                                                        "background: #ef4444; box-shadow: 0 0 12px rgba(239,68,68,0.7)"
                                                    } else if enabled {
                                                        "background: #10b981; box-shadow: 0 0 8px rgba(16,185,129,0.5)"
                                                    } else {
                                                        "background: #1e293b"
                                                    }
                                                )></div>
                                            </div>

                                            // Enable row
                                            <div style=format!("display: flex; align-items: center; justify-content: space-between; padding: 8px; border-radius: 6px; margin-bottom: 8px; {}",
                                                if enabled { "background: rgba(16,185,129,0.08)" } else { "background: var(--bg-secondary, #0f0f23)" }
                                            )>
                                                <div>
                                                    <div style=format!("font-size: 11px; font-weight: 700; {}",
                                                        if enabled { "color: #10b981" } else { "color: var(--text-dim)" }
                                                    )>"Output Enable"</div>
                                                    <div style="font-size: 10px; color: var(--text-dim)">{format!("Power to connector P{}", ef.id)}</div>
                                                </div>
                                                <button
                                                    class="scope-btn"
                                                    style=format!("font-size: 10px; padding: 4px 12px; {}",
                                                        if enabled { "color: #10b981; border-color: #10b98144" } else { "" }
                                                    )
                                                    on:click=move |_| { send_pca_control(efuse_ctrl, !enabled); }
                                                >
                                                    {if enabled { "ON" } else { "OFF" }}
                                                </button>
                                            </div>

                                            // Fault row
                                            <div style=format!("display: flex; align-items: center; justify-content: space-between; padding: 8px; border-radius: 6px; {}",
                                                if fault { "background: rgba(239,68,68,0.12)" } else { "background: var(--bg-secondary, #0f0f23)" }
                                            )>
                                                <div>
                                                    <div style=format!("font-size: 11px; font-weight: 700; {}",
                                                        if fault { "color: #ef4444" } else { "color: var(--text-dim)" }
                                                    )>"Overcurrent Fault"</div>
                                                    <div style="font-size: 10px; color: var(--text-dim)">{format!("Protection on connector P{}", ef.id)}</div>
                                                </div>
                                                <div style="display: flex; align-items: center; gap: 6px">
                                                    <div style=format!("width: 8px; height: 8px; border-radius: 50%; {}",
                                                        if fault { "background: #ef4444; box-shadow: 0 0 6px #ef4444" } else { "background: #1e293b" }
                                                    )></div>
                                                    <span style=format!("font-size: 11px; font-weight: 700; font-family: 'JetBrains Mono', monospace; {}",
                                                        if fault { "color: #ef4444" } else { "color: #10b981" }
                                                    )>{if fault { "FAULT" } else { "OK" }}</span>
                                                </div>
                                            </div>

                                            // Status text
                                            <div style=format!("font-size: 10px; text-align: center; margin-top: 8px; font-weight: 700; {}",
                                                if fault { "color: #ef4444" } else if enabled { "color: #10b981" } else { "color: var(--text-dim)" }
                                            )>
                                                {if fault {
                                                    format!("Overcurrent on connector P{} -- disconnect load", ef.id)
                                                } else if enabled {
                                                    format!("Power flowing -> Connector P{}", ef.id)
                                                } else {
                                                    format!("Connector P{} disconnected", ef.id)
                                                }}
                                            </div>
                                        </div>
                                    }
                                }).collect::<Vec<_>>()}
                            </div>
                        </div>
                    </div>
                }.into_any()
            }}
        </div>
    }
}

fn render_power_good(name: &str, desc: &str, is_ok: bool) -> impl IntoView {
    let border_style = if is_ok {
        "background: rgba(16,185,129,0.08); border: 1px solid rgba(16,185,129,0.3)"
    } else {
        "background: var(--bg-secondary, #1a1a2e); border: 1px solid var(--border)"
    };
    let dot_style = if is_ok {
        "background: #10b981; box-shadow: 0 0 8px rgba(16,185,129,0.6)"
    } else {
        "background: #1e293b"
    };
    let name_color = if is_ok { "color: #10b981" } else { "color: var(--text-dim)" };
    let status_style = if is_ok { "color: #10b981" } else { "color: #ef4444" };
    let status_text = if is_ok { "OK" } else { "FAIL" };
    let name = name.to_string();
    let desc = desc.to_string();

    view! {
        <div style=format!("padding: 12px; border-radius: 8px; text-align: center; {}", border_style)>
            <div style="display: flex; align-items: center; justify-content: center; gap: 8px; margin-bottom: 4px">
                <div style=format!("width: 10px; height: 10px; border-radius: 50%; transition: all 0.3s; {}", dot_style)></div>
                <span style=format!("font-size: 13px; font-weight: 700; font-family: 'JetBrains Mono', monospace; {}", name_color)>{name}</span>
            </div>
            <div style="font-size: 10px; color: var(--text-dim)">{desc}</div>
            <div style=format!("font-size: 11px; font-weight: 700; margin-top: 4px; {}", status_style)>{status_text}</div>
        </div>
    }
}
