use crate::components::icons::Icon;
use crate::components::ui::{EmptyState, Readout};
use crate::tauri_bridge::*;
use leptos::prelude::*;

#[component]
pub fn UsbPdTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (pd, set_pd) = signal(UsbPdState::default());

    // DESK-22: own 3 s poll instead of a refetch on every device-state tick.
    let _ = state;
    start_tab_poll(
        move || async move {
            if let Some(st) = fetch_usbpd_status().await {
                set_pd.try_set(st);
            }
        },
        || 3000,
    );

    let (sb, set_sb) = signal(None::<StandbyStatus>);
    let (sb_err, set_sb_err) = signal(String::new());
    // Standby is absent on older firmware: the card only shows once a status has been read.
    leptos::task::spawn_local(async move {
        let _ = standby_presence(true).await;
    });
    start_tab_poll(
        move || async move {
            if let Ok(st) = standby_status().await {
                set_sb.try_set(Some(st));
            }
        },
        || 3000,
    );
    let apply = move |r: Result<StandbyStatus, String>| match r {
        Ok(st) => {
            set_sb_err.set(String::new());
            set_sb.set(Some(st));
        }
        Err(e) => set_sb_err.set(e),
    };

    view! {
        <div class="view sy-pd">
            <p class="sy-lead">
                "USB Power Delivery status from the HUSB238 controller: the negotiated contract and the PDOs offered by the source. The board is powered at 20 V even without I2C communication."
            </p>

            {move || if !pd.get().present {
                view! {
                    <div class="group">
                        <EmptyState icon="plug-zap" title="HUSB238 not detected"
                            message="The USB PD controller did not answer on the I2C bus. Check the hardware connection." />
                    </div>
                }.into_any()
            } else {
                ().into_any()
            }}

            <Show when=move || pd.get().present>
                // ── Contract ──────────────────────────────────────────────────
                <div class="group sy-pd-contract">
                    <div class="group-header">
                        <span class="group-title"><Icon name="usb" size=15 />"Contract"</span>
                        <span class="sy-status">
                            <span class=move || if pd.get().attached { "dot tone-green" } else { "dot" }></span>
                            <span>{move || if pd.get().attached { "Attached" } else { "Not attached" }}</span>
                        </span>
                    </div>
                    <div class="sy-pd-readouts">
                        <Readout label="Voltage" size="lg" unit="V"
                            value=Signal::derive(move || format!("{:.1}", pd.get().voltage_v)) />
                        <Readout label="Current" size="lg" unit="A"
                            value=Signal::derive(move || format!("{:.2}", pd.get().current_a)) />
                        <Readout label="Power" size="lg" unit="W"
                            value=Signal::derive(move || format!("{:.1}", pd.get().power_w)) />
                        <Readout label="CC direction" size="lg"
                            value=Signal::derive(move || pd.get().cc) />
                    </div>
                </div>

                // ── Source PDOs ───────────────────────────────────────────────
                <div class="group group-flush sy-pd-pdos">
                    <div class="group-header">
                        <span class="group-title">"Source PDOs"</span>
                        <span class="group-subtitle">"Select a detected profile to request it from the source"</span>
                    </div>
                    <table class="table sy-pd-table">
                        <thead>
                            <tr>
                                <th>"Voltage"</th>
                                <th>"Availability"</th>
                                <th class="sy-num">"Max current"</th>
                                <th class="sy-num">"Max power"</th>
                                <th class="sy-act"><span class="sy-sr">"Action"</span></th>
                            </tr>
                        </thead>
                        <tbody>
                            {move || {
                                let st = pd.get();
                                let selected = st.selected_pdo;
                                st.source_pdos.into_iter().enumerate().map(|(i, pdo)| {
                                    let detected = pdo.detected;
                                    let voltage_idx = (i + 1) as u8; // 1=5V, 2=9V, ...
                                    let is_sel = selected == voltage_idx;
                                    let aria = format!("Select {} PDO", pdo.voltage);
                                    view! {
                                        <tr class:is-selected=is_sel class:is-unavailable=!detected>
                                            <td class="sy-pdo-v">
                                                {pdo.voltage.clone()}
                                                {is_sel.then(|| view! { <span class="badge tone-blue">"Selected"</span> })}
                                            </td>
                                            <td>
                                                <span class=if detected { "badge tone-green" } else { "badge" }>
                                                    {if detected { "Available" } else { "Not offered" }}
                                                </span>
                                            </td>
                                            <td class="sy-num">{format!("{:.1}", pdo.max_current_a)}<span class="sy-unit">" A"</span></td>
                                            <td class="sy-num">{format!("{:.0}", pdo.max_power_w)}<span class="sy-unit">" W"</span></td>
                                            <td class="sy-act">
                                                {if detected {
                                                    view! {
                                                        <button class=if is_sel { "btn btn-sm" } else { "btn btn-sm btn-tinted" }
                                                            aria-label=aria
                                                            on:click=move |_| {
                                                                send_usbpd_select_pdo(voltage_idx);
                                                            }
                                                        >"Select"</button>
                                                    }.into_any()
                                                } else {
                                                    ().into_any()
                                                }}
                                            </td>
                                        </tr>
                                    }
                                }).collect::<Vec<_>>()
                            }}
                        </tbody>
                    </table>
                </div>
            </Show>

            // ── System standby ────────────────────────────────────────────
            <Show when=move || sb.get().is_some()>
                <div class="group sy-standby">
                    <div class="group-header">
                        <span class="group-title"><Icon name="power" size=15 />"System standby"</span>
                        <span class="badge">{move || sb.get().map(|s| s.state).unwrap_or_default()}</span>
                    </div>
                    <div class="sy-standby-row" style="display:flex;align-items:center;gap:12px;padding:8px 14px 12px">
                        <label for="sb-timeout">"Sleep after inactivity"</label>
                        <select id="sb-timeout"
                            on:change=move |ev| {
                                if let Ok(secs) = event_target_value(&ev).parse::<u16>() {
                                    leptos::task::spawn_local(async move {
                                        apply(standby_set_timeout(secs).await);
                                    });
                                }
                            }
                        >
                            <option value="0" selected=move || sb.get().map(|s| s.timeout_seconds == 0).unwrap_or(false)>"Never"</option>
                            <option value="60" selected=move || sb.get().map(|s| s.timeout_seconds == 60).unwrap_or(false)>"1 minute"</option>
                            <option value="300" selected=move || sb.get().map(|s| s.timeout_seconds == 300).unwrap_or(false)>"5 minutes"</option>
                            <option value="900" selected=move || sb.get().map(|s| s.timeout_seconds == 900).unwrap_or(false)>"15 minutes"</option>
                        </select>
                        <button class="btn btn-sm btn-tinted"
                            disabled=move || sb.get().map(|s| s.state == "active").unwrap_or(true)
                            on:click=move |_| {
                                leptos::task::spawn_local(async move { apply(standby_wake().await); });
                            }
                        >"Wake"</button>
                    </div>
                    <Show when=move || !sb_err.get().is_empty()>
                        <p class="sy-lead">{move || sb_err.get()}</p>
                    </Show>
                </div>
            </Show>
        </div>
    }
}
