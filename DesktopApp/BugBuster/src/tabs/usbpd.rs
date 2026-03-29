use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::tauri_bridge::*;

#[component]
pub fn UsbPdTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (pd, set_pd) = signal(UsbPdState::default());

    // Poll USB PD status whenever device state updates
    let set_pd_clone = set_pd.clone();
    Effect::new(move |_| {
        let _ = state.get(); // subscribe to state changes
        spawn_local(async move {
            if let Some(st) = fetch_usbpd_status().await {
                set_pd_clone.set(st);
            }
        });
    });

    view! {
        <div class="tab-content">
            <div class="tab-desc">"USB Power Delivery status from the HUSB238 controller. Shows the negotiated voltage/current contract and available source PDOs. The board is powered at 20V even without I2C communication."</div>

            {move || {
                let st = pd.get();
                if !st.present {
                    return view! {
                        <div class="card">
                            <div class="card-header">
                                <span class="channel-func">"HUSB238 USB PD"</span>
                            </div>
                            <div class="card-body">
                                <div class="mode-warning">
                                    <span class="mode-warning-icon">"!"</span>
                                    <span>"HUSB238 not detected on I2C bus. Check hardware connection."</span>
                                </div>
                            </div>
                        </div>
                    }.into_any();
                }

                let attached = st.attached;
                let voltage_v = st.voltage_v;
                let current_a = st.current_a;
                let power_w = st.power_w;
                let cc = st.cc.clone();
                let pdos = st.source_pdos.clone();

                view! {
                    <div>
                        // Status + readings in a 2-column grid
                        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 16px">
                            // Left card: PD contract info
                            <div class="card">
                                <div class="card-header" style="display: flex; align-items: center; justify-content: space-between">
                                    <span style="font-size: 14px; font-weight: 700">"USB Power Delivery"</span>
                                    <div style="display: flex; align-items: center; gap: 8px">
                                        <div style=format!("width: 10px; height: 10px; border-radius: 50%; transition: all 0.3s; {}",
                                            if attached { "background: #10b981; box-shadow: 0 0 6px #10b981" } else { "background: var(--text-dim)" }
                                        )></div>
                                        <span style="font-size: 11px; font-family: 'JetBrains Mono', monospace">
                                            {if attached { "Attached" } else { "Not attached" }}
                                        </span>
                                    </div>
                                </div>
                                <div class="card-body">
                                    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 12px">
                                        // Voltage
                                        <div style="text-align: center; padding: 12px; border-radius: 8px; background: var(--bg-secondary, #1a1a2e)">
                                            <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 4px">"Voltage"</div>
                                            <div style="font-size: 24px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #3b82f6">
                                                {format!("{:.1}V", voltage_v)}
                                            </div>
                                        </div>
                                        // Current
                                        <div style="text-align: center; padding: 12px; border-radius: 8px; background: var(--bg-secondary, #1a1a2e)">
                                            <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 4px">"Current"</div>
                                            <div style="font-size: 24px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #10b981">
                                                {format!("{:.2}A", current_a)}
                                            </div>
                                        </div>
                                        // Power
                                        <div style="text-align: center; padding: 12px; border-radius: 8px; background: var(--bg-secondary, #1a1a2e)">
                                            <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 4px">"Power"</div>
                                            <div style="font-size: 24px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #f59e0b">
                                                {format!("{:.1}W", power_w)}
                                            </div>
                                        </div>
                                        // CC Direction
                                        <div style="text-align: center; padding: 12px; border-radius: 8px; background: var(--bg-secondary, #1a1a2e)">
                                            <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 4px">"CC Dir"</div>
                                            <div style="font-size: 24px; font-weight: 700; font-family: 'JetBrains Mono', monospace; color: #a855f7">
                                                {cc}
                                            </div>
                                        </div>
                                    </div>
                                </div>
                            </div>

                            // Right card: Source PDOs table
                            <div class="card">
                                <div class="card-header">
                                    <span style="font-size: 14px; font-weight: 700">"Source PDOs"</span>
                                </div>
                                <div class="card-body" style="padding: 0">
                                    <table style="width: 100%; border-collapse: collapse; font-size: 12px; font-family: 'JetBrains Mono', monospace">
                                        <thead>
                                            <tr style="color: var(--text-dim); border-bottom: 1px solid var(--border)">
                                                <th style="text-align: left; padding: 8px 12px">"Voltage"</th>
                                                <th style="text-align: left; padding: 8px 6px">"Detected"</th>
                                                <th style="text-align: left; padding: 8px 6px">"Max A"</th>
                                                <th style="text-align: left; padding: 8px 6px">"Max W"</th>
                                                <th style="padding: 8px 12px"></th>
                                            </tr>
                                        </thead>
                                        <tbody>
                                            {pdos.into_iter().enumerate().map(|(i, pdo)| {
                                                let detected = pdo.detected;
                                                let voltage_idx = (i + 1) as u8; // 1=5V, 2=9V, ...
                                                view! {
                                                    <tr style="border-top: 1px solid var(--border)">
                                                        <td style="padding: 8px 12px; font-weight: 700">{pdo.voltage.clone()}</td>
                                                        <td style="padding: 8px 6px">
                                                            <div style=format!("width: 8px; height: 8px; border-radius: 50%; {}",
                                                                if detected { "background: #10b981; box-shadow: 0 0 4px #10b981" } else { "background: #1e293b" }
                                                            )></div>
                                                        </td>
                                                        <td style="padding: 8px 6px">{format!("{:.1}A", pdo.max_current_a)}</td>
                                                        <td style="padding: 8px 6px">{format!("{:.0}W", pdo.max_power_w)}</td>
                                                        <td style="padding: 8px 12px">
                                                            {if detected {
                                                                view! {
                                                                    <button class="scope-btn" style="font-size: 10px; padding: 4px 10px"
                                                                        on:click=move |_| {
                                                                            send_usbpd_select_pdo(voltage_idx);
                                                                        }
                                                                    >"Select"</button>
                                                                }.into_any()
                                                            } else {
                                                                view! { <span></span> }.into_any()
                                                            }}
                                                        </td>
                                                    </tr>
                                                }
                                            }).collect::<Vec<_>>()}
                                        </tbody>
                                    </table>
                                </div>
                            </div>
                        </div>
                    </div>
                }.into_any()
            }}
        </div>
    }
}
