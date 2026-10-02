use crate::components::channel_sparkline::ch_key;
use crate::components::ui::{Callout, Switch};
use crate::tauri_bridge::*;
use leptos::prelude::*;

/// PCA control index mapping (matches http_transport.rs ctrl_names):
/// 0 = vadj1, 1 = vadj2, 2 = 15v, 3 = mux, 4 = usb, 5 = efuse1, 6 = efuse2, 7 = efuse3, 8 = efuse4
const SUPPLY_CONTROLS: &[(u8, &str, &str)] = &[
    (0, "VADJ1", "3-15V Rail A"),
    (1, "VADJ2", "3-15V Rail B"),
    (2, "+/-15V", "AD74416H Analog"),
    (3, "LOGIC_EN", "Main Logic Enable"),
    (4, "USB Hub", "USB Hub IC"),
];

const EFUSE_LABELS: [&str; 4] = [
    "Output enable P1",
    "Output enable P2",
    "Output enable P3",
    "Output enable P4",
];

#[component]
pub fn IoExpTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (ioexp, set_ioexp) = signal(IoExpState::default());

    // DESK-22: own 2 s poll instead of a refetch on every device-state tick.
    let _ = state;
    start_tab_poll(
        move || async move {
            if let Some(st) = fetch_pca_status().await {
                set_ioexp.try_set(st);
            }
        },
        || 2000,
    );

    let imon = RwSignal::new(EfuseImonStatus::default());
    start_efuse_imon_poll(imon);

    view! {
        <div class="view">
            {move || {
                let st = ioexp.get();

                if !st.present {
                    return view! {
                        <Callout tone="orange">
                            "PCA9535 not detected on the I2C bus (0x23). Check the hardware connection."
                        </Callout>
                    }.into_any();
                }

                // Extract enable states for supply controls
                let enable_vals = [st.vadj1_en, st.vadj2_en, st.en_15v, st.en_mux, st.en_usb_hub];
                let efuses = st.efuses.clone();
                let logic_pg = st.logic_pg;
                let vadj1_pg = st.vadj1_pg;
                let vadj2_pg = st.vadj2_pg;

                view! {
                    <div class="hstack">
                        <span class="chip">"PCA9535AHF"</span>
                        <span class="group-subtitle">"16-bit I2C GPIO expander at 0x23"</span>
                        <span class="spacer"></span>
                        <span class="badge tone-green"><span class="dot tone-green" aria-hidden="true"></span>"Present"</span>
                    </div>

                    <div class="grid-2">
                        <section class="group group-flush">
                            <div class="group-header">
                                <div class="group-title">"Supply enables"</div>
                            </div>
                            <table class="table io-table">
                                <thead>
                                    <tr><th>"Signal"</th><th>"Description"</th><th>"State"</th></tr>
                                </thead>
                                <tbody>
                                    {SUPPLY_CONTROLS.iter().enumerate().map(|(i, (ctrl_idx, name, desc))| {
                                        let is_on = enable_vals[i];
                                        let ctrl = *ctrl_idx;
                                        view! {
                                            <tr>
                                                <td class="io-name">{*name}</td>
                                                <td class="muted">{*desc}</td>
                                                <td>
                                                    <span class="io-state">
                                                        <Switch
                                                            checked=Signal::derive(move || is_on)
                                                            on_change=Callback::new(move |v: bool| send_pca_control(ctrl, v))
                                                            aria_label=*name
                                                        />
                                                        {if is_on { "On" } else { "Off" }}
                                                    </span>
                                                </td>
                                            </tr>
                                        }
                                    }).collect::<Vec<_>>()}
                                </tbody>
                            </table>
                        </section>

                        <section class="group group-flush">
                            <div class="group-header">
                                <div class="group-title">"Power good"</div>
                            </div>
                            <table class="table io-table">
                                <thead>
                                    <tr><th>"Signal"</th><th>"Description"</th><th>"Status"</th></tr>
                                </thead>
                                <tbody>
                                    {render_power_good("LOGIC_EN", "Main Logic Enable", logic_pg)}
                                    {render_power_good("VADJ1_PG", "LTM8063 #1 to P1, P2", vadj1_pg)}
                                    {render_power_good("VADJ2_PG", "LTM8063 #2 to P3, P4", vadj2_pg)}
                                </tbody>
                            </table>
                        </section>
                    </div>

                    <div class="hstack">
                        <span class="section-label io-section-label">"E-fuse output protection (TPS1641 x 4)"</span>
                        <span class="spacer"></span>
                        <span class="group-subtitle">"Current monitor: Overview tab"</span>
                    </div>
                    <div class="grid-4">
                        {efuses.into_iter().enumerate().map(|(i, ef)| {
                            let efuse_ctrl = (5 + i) as u8; // efuse1=5, efuse2=6, etc.
                            let rail = if ef.id <= 2 { "VADJ1" } else { "VADJ2" };
                            let enabled = ef.enabled;
                            let fault = ef.fault;
                            let ef_id = ef.id;

                            view! {
                                <section class="group io-efuse" class:fault=fault data-ch=ch_key(i)>
                                    <div class="group-header">
                                        <div class="group-title">
                                            <span class="io-dot" aria-hidden="true"></span>
                                            {format!("P{}", ef.id)}
                                        </div>
                                        <span class="chip">{format!("TPS1641 · {}", rail)}</span>
                                    </div>

                                    <div class="rows">
                                        <div class="row">
                                            <span class="row-label">"Output enable"</span>
                                            <span class="io-state">
                                                <Switch
                                                    checked=Signal::derive(move || enabled)
                                                    on_change=Callback::new(move |v: bool| send_pca_control(efuse_ctrl, v))
                                                    aria_label=EFUSE_LABELS[i.min(3)]
                                                />
                                                {if enabled { "On" } else { "Off" }}
                                            </span>
                                        </div>

                                        {move || {
                                            let st = imon.get();
                                            if st.efuse != ef_id {
                                                return ().into_any();
                                            }
                                            let (text, kind) = efuse_imon_display(&st);
                                            let tone = match kind {
                                                "sat" => "red",
                                                "settling" => "",
                                                _ => "green",
                                            };
                                            view! {
                                                <div class="row">
                                                    <span class="row-label">"Current"<span class="row-hint">"IMON via channel D"</span></span>
                                                    <span class=format!("row-value io-code tone-text-{}", tone)>{text}</span>
                                                </div>
                                            }.into_any()
                                        }}

                                        <div class="row">
                                            <span class="row-label">"Overcurrent"</span>
                                            <span class=if fault { "badge tone-red" } else { "badge tone-green" }>
                                                <span class=if fault { "dot tone-red" } else { "dot tone-green" } aria-hidden="true"></span>
                                                {if fault { "Fault" } else { "OK" }}
                                            </span>
                                        </div>
                                    </div>

                                    <p class=if fault { "io-efuse-status tone-text-red" } else { "io-efuse-status" }>
                                        {if fault {
                                            format!("Overcurrent on connector P{} - disconnect load", ef.id)
                                        } else if enabled {
                                            format!("Power flowing to connector P{}", ef.id)
                                        } else {
                                            format!("Connector P{} disconnected", ef.id)
                                        }}
                                    </p>
                                </section>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                }.into_any()
            }}
        </div>
    }
}

fn render_power_good(name: &'static str, desc: &'static str, is_ok: bool) -> impl IntoView {
    view! {
        <tr>
            <td class="io-name">{name}</td>
            <td class="muted">{desc}</td>
            <td>
                <span class=if is_ok { "badge tone-green" } else { "badge tone-red" }>
                    <span class=if is_ok { "dot tone-green" } else { "dot tone-red" } aria-hidden="true"></span>
                    {if is_ok { "OK" } else { "Fail" }}
                </span>
            </td>
        </tr>
    }
}
