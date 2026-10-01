// E-fuse output strip with the IMON current monitor (one e-fuse at a time,
// routed through U23 to AD74416H channel C/physical D). Shown on Overview.
use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;

const EFUSE_COLORS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];

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
        <div class="card" style="margin-bottom: 16px">
            <div class="card-header" style="display: flex; justify-content: space-between; align-items: center">
                <div>
                    <span class="channel-func">"E-Fuse Outputs"</span>
                    <div style="font-size: 10px; color: var(--text-dim); margin-top: 2px">
                        {move || match imon.get().efuse {
                            0 => "Pick one connector to measure its output current (IMON via CH C).".to_string(),
                            n => format!("Measuring P{}  ·  CH C reserved  ·  self-test and calibration paused", n),
                        }}
                    </div>
                </div>
                <button class="scope-btn" style="font-size: 10px; padding: 4px 10px"
                    disabled=move || imon.get().efuse == 0 || busy.get()
                    on:click=move |_| request(0, false)
                >"Stop monitor"</button>
            </div>
            <div class="card-body">
                <div style="display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px">
                    {(1u8..=4).map(|id| {
                        let color = EFUSE_COLORS[(id - 1) as usize];
                        let rail = if id <= 2 { "VADJ1" } else { "VADJ2" };
                        let state = move || ioexp.get().efuses.into_iter().find(|e| e.id == id).unwrap_or_default();
                        let monitored = move || imon.get().efuse == id;
                        view! {
                            <div style=move || format!(
                                "padding: 12px; border-radius: 10px; transition: all 0.2s; {}",
                                if monitored() {
                                    format!("background: {color}12; border: 1px solid {color}80; box-shadow: 0 0 14px {color}30")
                                } else {
                                    "background: rgba(100,140,200,0.035); border: 1px solid rgba(100,140,200,0.10)".to_string()
                                }
                            )>
                                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px">
                                    <div>
                                        <span style=format!("font-size: 15px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: {color}")>
                                            {format!("P{}", id)}
                                        </span>
                                        <span style="font-size: 10px; color: var(--text-dim); margin-left: 6px">{rail}</span>
                                    </div>
                                    {move || {
                                        let s = state();
                                        let (label, c) = if s.fault { ("FAULT", "#ef4444") }
                                            else if s.enabled { ("ON", "#10b981") }
                                            else { ("OFF", "var(--text-dim)") };
                                        view! {
                                            <button class="scope-btn"
                                                style=format!("font-size: 10px; padding: 3px 10px; color: {c}; border-color: {c}55")
                                                title="Toggle connector power"
                                                on:click=move |_| send_pca_control(4 + id, !s.enabled)
                                            >{label}</button>
                                        }
                                    }}
                                </div>

                                <div style="text-align: center; min-height: 34px; display: flex; align-items: center; justify-content: center">
                                    {move || {
                                        if !monitored() {
                                            return view! {
                                                <span style="font-size: 18px; font-weight: 700; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">"— mA"</span>
                                            }.into_any();
                                        }
                                        let st = imon.get();
                                        let (text, kind) = efuse_imon_display(&st);
                                        let c = match kind { "sat" => "#ef4444", "settling" => "var(--text-dim)", _ => color };
                                        view! {
                                            <span style=format!("font-size: 24px; font-weight: 800; font-family: 'JetBrains Mono', monospace; color: {c}")>{text}</span>
                                        }.into_any()
                                    }}
                                </div>

                                <button class="scope-btn"
                                    style=move || format!("width: 100%; margin-top: 8px; font-size: 10px; {}",
                                        if monitored() { format!("color: {color}; border-color: {color}80") } else { String::new() })
                                    disabled=move || busy.get()
                                    on:click=move |_| request(if monitored() { 0 } else { id }, false)
                                >{move || if monitored() { "Monitoring" } else { "Monitor current" }}</button>
                            </div>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>

            {move || confirm.get().map(|(target, old)| {
                let msg = if old == 0 {
                    format!("E-Fuse P{target} is ON. Attaching the current monitor turns it OFF, switches the monitor, then turns it back ON. The output drops for about 0.5 s.")
                } else if target == 0 {
                    format!("E-Fuse P{old} is ON. Stopping the monitor turns it OFF, detaches the monitor, then turns it back ON. The output drops for about 0.5 s.")
                } else {
                    format!("Moving the monitor from P{old} to P{target} power-cycles whichever of them is ON. The output drops for about 0.5 s.")
                };
                view! {
                    <div style="position: fixed; inset: 0; background: rgba(0,0,0,0.6); display: flex; align-items: center; justify-content: center; z-index: 1000"
                        on:click=move |_| confirm.set(None)
                    >
                        <div style="background: var(--bg-secondary); border: 1px solid #f59e0b60; border-radius: 14px; padding: 20px; max-width: 380px; box-shadow: 0 0 40px rgba(0,0,0,0.6)"
                            on:click=move |e| e.stop_propagation()
                        >
                            <div style="font-size: 13px; font-weight: 700; color: #f59e0b; margin-bottom: 8px">"Output will be power-cycled"</div>
                            <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 16px; line-height: 1.5">{msg}</div>
                            <div style="display: flex; gap: 8px; justify-content: flex-end">
                                <button class="btn btn-sm" style="font-size: 11px; padding: 5px 14px"
                                    on:click=move |_| confirm.set(None)
                                >"Cancel"</button>
                                <button class="btn btn-sm" style="font-size: 11px; padding: 5px 14px; background: #f59e0b25; color: #f59e0b; border: 1px solid #f59e0b60"
                                    on:click=move |_| { confirm.set(None); request(target, true); }
                                >"Power-cycle and continue"</button>
                            </div>
                        </div>
                    </div>
                }
            })}
        </div>
    }
}
