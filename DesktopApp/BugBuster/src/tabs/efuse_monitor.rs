// E-fuse output table with the IMON current monitor (one e-fuse at a time,
// routed through U23 to AD74416H channel C/physical D). Shown on Overview.
use crate::components::ui::Switch;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;

const EFUSE_SWITCH_LABELS: [&str; 4] = [
    "Toggle P1 power",
    "Toggle P2 power",
    "Toggle P3 power",
    "Toggle P4 power",
];

#[component]
pub fn EfuseMonitorStrip(
    ioexp: ReadSignal<IoExpState>,
    imon: RwSignal<EfuseImonStatus>,
) -> impl IntoView {
    // (target, previously monitored) awaiting power-cycle confirmation.
    let confirm = RwSignal::new(None::<(u8, u8)>);
    let busy = RwSignal::new(false);

    let request = move |target: u8, ok_to_cycle: bool| {
        if busy.get_untracked() {
            return;
        }
        busy.set(true);
        spawn_local(async move {
            let old = imon.get_untracked().efuse;
            if let Some(st) = send_efuse_imon_set(target, ok_to_cycle).await {
                let result = st.result.clone();
                imon.try_set(st);
                match result.as_str() {
                    "ok" => {}
                    "needs_confirm" => {
                        confirm.try_set(Some((target, old)));
                    }
                    other => show_toast(efuse_imon_error_text(other), "err"),
                }
            }
            busy.try_set(false);
        });
    };

    view! {
        <div class="group group-flush ov-fuse">
            <div class="group-header">
                <div class="ov-fuse-head">
                    <span class="group-title">"E-fuse outputs"</span>
                    <span class="group-subtitle">
                        {move || match imon.get().efuse {
                            0 => "Pick one connector to measure its output current (IMON via CH C).".to_string(),
                            n => format!("Measuring P{}. CH C reserved, self-test and calibration paused.", n),
                        }}
                    </span>
                </div>
                <button class="btn btn-sm"
                    disabled=move || imon.get().efuse == 0 || busy.get()
                    on:click=move |_| request(0, false)
                >"Stop monitor"</button>
            </div>
            <table class="table ov-fuse-table">
                <thead>
                    <tr>
                        <th>"Output"</th>
                        <th>"State"</th>
                        <th class="ov-num">"Current"</th>
                        <th class="ov-act">"Monitor"</th>
                    </tr>
                </thead>
                <tbody>
                    {(1u8..=4).map(|id| {
                        let rail = if id <= 2 { "VADJ1" } else { "VADJ2" };
                        let state = move || ioexp.get().efuses.into_iter().find(|e| e.id == id).unwrap_or_default();
                        let monitored = move || imon.get().efuse == id;
                        view! {
                            <tr class:is-monitored=monitored>
                                <td>
                                    <span class="ov-fuse-name">{format!("P{}", id)}</span>
                                    <span class="chip">{rail}</span>
                                </td>
                                <td>
                                    <div class="hstack">
                                        <Switch
                                            checked=Signal::derive(move || state().enabled)
                                            aria_label=EFUSE_SWITCH_LABELS[(id - 1) as usize]
                                            on_change=Callback::new(move |_: bool| send_pca_control(4 + id, !state().enabled))
                                        />
                                        {move || {
                                            let s = state();
                                            if s.fault {
                                                view! { <span class="badge tone-red">"Fault"</span> }.into_any()
                                            } else if s.enabled {
                                                view! { <span class="badge tone-green">"On"</span> }.into_any()
                                            } else {
                                                view! { <span class="badge">"Off"</span> }.into_any()
                                            }
                                        }}
                                    </div>
                                </td>
                                <td class="ov-num">
                                    {move || {
                                        if !monitored() {
                                            return view! { <span class="muted">"-"</span> }.into_any();
                                        }
                                        let st = imon.get();
                                        let (text, kind) = efuse_imon_display(&st);
                                        let class = match kind { "sat" => "tone-text-red", "settling" => "muted", _ => "" };
                                        view! { <span class=class>{text}</span> }.into_any()
                                    }}
                                </td>
                                <td class="ov-act">
                                    <button class="btn btn-sm"
                                        class:btn-tinted=monitored
                                        disabled=move || busy.get()
                                        on:click=move |_| request(if monitored() { 0 } else { id }, false)
                                    >{move || if monitored() { "Monitoring" } else { "Monitor current" }}</button>
                                </td>
                            </tr>
                        }
                    }).collect::<Vec<_>>()}
                </tbody>
            </table>
        </div>

        {move || confirm.get().map(|(target, old)| {
            let msg = if old == 0 {
                format!("E-fuse P{target} is ON. Attaching the current monitor turns it OFF, switches the monitor, then turns it back ON. The output drops for about 0.5 s.")
            } else if target == 0 {
                format!("E-fuse P{old} is ON. Stopping the monitor turns it OFF, detaches the monitor, then turns it back ON. The output drops for about 0.5 s.")
            } else {
                format!("Moving the monitor from P{old} to P{target} power-cycles whichever of them is ON. The output drops for about 0.5 s.")
            };
            view! {
                <div class="scrim" on:click=move |_| confirm.set(None)></div>
                <div class="dialog ov-dialog" role="dialog" aria-modal="true" aria-labelledby="ov-fuse-dialog-title">
                    <div class="group">
                        <div class="stack">
                            <div class="text-headline" id="ov-fuse-dialog-title">"Output will be power-cycled"</div>
                            <div class="muted">{msg}</div>
                            <div class="hstack ov-dialog-actions">
                                <button class="btn" on:click=move |_| confirm.set(None)>"Cancel"</button>
                                <button class="btn btn-tinted tone-orange"
                                    on:click=move |_| { confirm.set(None); request(target, true); }
                                >"Power-cycle and continue"</button>
                            </div>
                        </div>
                    </div>
                </div>
            }
        })}
    }
}
