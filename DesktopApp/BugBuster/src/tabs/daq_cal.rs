// =============================================================================
// daq_cal.rs — SMU factory-calibration wizard (dialog) for the DAQ HAT.
//
// Drives the interactive voltage / current-limit calibration over the S3 BBP
// control plane (see src-tauri/src/daq_commands.rs daq_cal_*). The firmware
// pauses on an operator prompt (disconnect the load / short the output) until
// the user clicks Continue. Starting a run replaces the stored calibration, so
// it is confirmed; aborting a run in progress is confirmed too.
// =============================================================================

use leptos::prelude::*;
use leptos::task::spawn_local;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use crate::components::icons::Icon;
use crate::components::ui::{Callout, SegmentedControl};
use crate::tauri_bridge::*;

async fn slp(ms: u32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        if let Some(w) = web_sys::window() {
            w.set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms as i32)
                .ok();
        }
    });
    wasm_bindgen_futures::JsFuture::from(p).await.ok();
}

fn phase_name(p: u8) -> &'static str {
    match p {
        0 => "Idle",
        1 => "Action required",
        2 => "Running",
        3 => "Success",
        4 => "Failed",
        _ => "Unknown",
    }
}

fn phase_tone(p: u8) -> &'static str {
    match p {
        1 => "badge tone-orange",
        2 => "badge tone-blue",
        3 => "badge tone-green",
        4 => "badge tone-red",
        _ => "badge",
    }
}

fn mode_name(m: u8) -> &'static str {
    match m {
        0 => "Voltage",
        1 => "Current limit",
        _ => "Unknown",
    }
}

fn prompt_title(p: u8) -> &'static str {
    match p {
        1 => "Disconnect the load",
        2 => "Short the output",
        _ => "Action required",
    }
}

fn prompt_text(p: u8) -> &'static str {
    match p {
        1 => "Disconnect any load from the DUT supply output, then click Continue.",
        2 => "Short the DUT supply output (a low-resistance link across the \
              terminals), then click Continue. The supply will ramp current up \
              to 2.5 A into the short.",
        _ => "",
    }
}

fn persist_name(p: u8) -> &'static str {
    match p {
        0 => "RAM only",
        1 => "Saving",
        2 => "Saved to NVM",
        3 => "Save failed",
        _ => "Unknown",
    }
}

fn flag_names(f: u16) -> Vec<&'static str> {
    const BITS: [(u16, &str); 9] = [
        (0x0001, "too few points"),
        (0x0002, "low coverage"),
        (0x0004, "high coverage"),
        (0x0008, "non-monotonic"),
        (0x0010, "gap too large"),
        (0x0020, "did not settle"),
        (0x0040, "target unreached"),
        (0x0080, "hardware unavailable"),
        (0x0100, "USB-PD too low: current cal needs 20 V / 3 A"),
    ];
    BITS.iter()
        .filter(|(b, _)| f & b != 0)
        .map(|(_, n)| *n)
        .collect()
}

/// Unit label for the active calibration mode (0 = voltage → V, 1 = current → A).
fn unit(mode: u8) -> &'static str {
    if mode == 1 {
        "A"
    } else {
        "V"
    }
}

fn step_class(n: u8, cur: u8) -> &'static str {
    if n < cur {
        "dq-step done"
    } else if n == cur {
        "dq-step active"
    } else {
        "dq-step"
    }
}

/// One numbered wizard step: marker, title, and a body shown only when `show`.
#[component]
fn Step(
    n: u8,
    title: &'static str,
    cur: Signal<u8>,
    #[prop(into)] show: Signal<bool>,
    children: Children,
) -> impl IntoView {
    view! {
        <li class=move || step_class(n, cur.get())>
            <span class="dq-step-n" aria-hidden="true">
                {move || if n < cur.get() { view!{ <Icon name="check" size=12 /> }.into_any() } else { view!{ {n.to_string()} }.into_any() }}
            </span>
            <div class="dq-step-main">
                <div class="dq-step-title">{title}</div>
                <div class="dq-step-body" prop:hidden=move || !show.get()>{children()}</div>
            </div>
        </li>
    }
}

#[component]
pub fn CalibrationWizard(open: RwSignal<bool>) -> impl IntoView {
    let status = RwSignal::new(Option::<DaqCalStatus>::None);
    let starting = RwSignal::new(false);
    // Which calibration the user picked, and the two inline confirmations.
    let pick = RwSignal::new(0u8);
    let confirm_start = RwSignal::new(false);
    let confirm_abort = RwSignal::new(false);

    let alive = Arc::new(AtomicBool::new(true));
    on_cleanup({
        let a = alive.clone();
        move || a.store(false, Ordering::SeqCst)
    });

    // Poll the calibration status at ~2.5 Hz while the wizard is open.
    {
        let alive = alive.clone();
        spawn_local(async move {
            loop {
                if !alive.load(Ordering::SeqCst) {
                    break;
                }
                if open.get_untracked() {
                    let st = daq_cal_status().await;
                    // The tab may have unmounted during the await.
                    if !alive.load(Ordering::SeqCst) {
                        break;
                    }
                    if let Some(st) = st {
                        status.set(Some(st));
                    }
                }
                slp(400).await;
            }
        });
    }

    // try_set: the tab may unmount while the start request is in flight.
    let start = move |mode: u8| {
        starting.set(true);
        spawn_local(async move {
            daq_cal_start(mode).await;
            slp(200).await;
            starting.try_set(false);
        });
    };
    let ack = move |_| spawn_local(async move { daq_cal_ack().await });
    let abort = move |_| {
        confirm_abort.set(false);
        spawn_local(async move { daq_cal_abort().await });
    };

    // Derived view helpers.
    let phase = move || status.get().map(|s| s.phase).unwrap_or(0);
    let running = move || matches!(phase(), 1 | 2);
    let cur = Signal::derive(move || match phase() {
        1 => 2u8,
        2 => 3,
        3 | 4 => 4,
        _ => 1,
    });
    let busy = move || running() || starting.get();

    view! {
        <Show when=move || open.get()>
            <div class="scrim" on:click=move |_| open.set(false)></div>
            <div class="dialog popover dq-cal" role="dialog" aria-modal="true" aria-labelledby="dq-cal-title">
                <div class="dq-cal-head">
                    <h3 id="dq-cal-title">"SMU calibration"</h3>
                    <button type="button" class="btn btn-plain btn-sm btn-icon" aria-label="Close calibration" title="Close"
                        on:click=move |_| open.set(false)>
                        <Icon name="x" size=15 />
                    </button>
                </div>
                <p class="dq-cal-intro">
                    "Calibrate voltage first, then the current limit. Each run pauses for a hardware step, so follow the prompt."
                </p>

                <ol class="dq-steps">
                    <Step n=1 title="Choose calibration" cur=cur show=Signal::derive(move || matches!(cur.get(), 1 | 4))>
                        <SegmentedControl
                            options=vec![(0u8, "Voltage"), (1u8, "Current limit")]
                            value=pick
                            on_change=Callback::new(move |v: u8| { pick.set(v); confirm_start.set(false); })
                            block=true
                            aria_label="Calibration type"
                        />
                        <p class="dq-step-text">
                            {move || if pick.get() == 1 {
                                "Forces the 50 mohm shunt and ramps the current limit up to 2.5 A into a short. Needs a 20 V / 3 A USB-PD source."
                            } else {
                                "Sweeps the voltage DAC and reads V_DUT back through the ADC. The DUT load must be disconnected."
                            }}
                        </p>
                        {move || if confirm_start.get() {
                            view!{
                                <Callout tone="orange">
                                    {move || format!(
                                        "This replaces the stored {} calibration on the device.",
                                        if pick.get() == 1 { "current-limit" } else { "voltage" })}
                                </Callout>
                                <div class="dq-step-actions">
                                    <button type="button" class="btn btn-sm" on:click=move |_| confirm_start.set(false)>"Cancel"</button>
                                    <button type="button" class="btn btn-sm btn-tinted tone-red"
                                        prop:disabled=busy
                                        on:click=move |_| { confirm_start.set(false); start(pick.get_untracked()); }>
                                        "Replace calibration"
                                    </button>
                                </div>
                            }.into_any()
                        } else {
                            view!{
                                <div class="dq-step-actions">
                                    <button type="button" class="btn btn-sm btn-primary"
                                        prop:disabled=busy
                                        on:click=move |_| confirm_start.set(true)>
                                        {move || if pick.get() == 1 { "Start current calibration" } else { "Start voltage calibration" }}
                                    </button>
                                </div>
                            }.into_any()
                        }}
                    </Step>

                    <Step n=2 title="Prepare the output" cur=cur show=Signal::derive(move || cur.get() == 2)>
                        {move || {
                            let p = status.get().map(|s| s.prompt).unwrap_or(0);
                            view!{
                                <Callout tone="orange">
                                    <strong>{prompt_title(p)}</strong>
                                    <div>{prompt_text(p)}</div>
                                </Callout>
                            }
                        }}
                        <div class="dq-step-actions">
                            <button type="button" class="btn btn-sm btn-primary" on:click=ack>"Continue"</button>
                        </div>
                    </Step>

                    <Step n=3 title="Run the sweep" cur=cur show=Signal::derive(move || cur.get() == 3)>
                        {move || {
                            let s = status.get().unwrap_or_default();
                            view!{
                                <div class="progress" role="progressbar" aria-valuemin="0" aria-valuemax="100"
                                    aria-valuenow=s.progress.min(100).to_string()>
                                    <span style=format!("width:{}%", s.progress.min(100))></span>
                                </div>
                                <dl class="kv">
                                    <dt>"Progress"</dt><dd>{format!("{}%", s.progress.min(100))}</dd>
                                    <dt>"Point"</dt><dd>{format!("{} (code {})", s.point, s.code)}</dd>
                                    <dt>"Measured"</dt><dd>{format!("{:.4} {}", s.measured, unit(s.mode))}</dd>
                                    <dt>"Span"</dt><dd>{format!("{:.3} to {:.3} {}", s.min, s.max, unit(s.mode))}</dd>
                                </dl>
                            }
                        }}
                    </Step>

                    <Step n=4 title="Result" cur=cur show=Signal::derive(move || cur.get() == 4)>
                        {move || {
                            let s = status.get().unwrap_or_default();
                            if s.phase == 3 {
                                view!{
                                    <Callout tone="green">
                                        {format!(
                                            "Calibration complete: {}. {} V-pts, {} I-pts stored.",
                                            persist_name(s.persist), s.vcount, s.icount)}
                                    </Callout>
                                }.into_any()
                            } else if s.phase == 4 {
                                let flags = flag_names(s.flags);
                                view!{
                                    <Callout tone="red">
                                        {if flags.is_empty() {
                                            "Calibration failed.".to_string()
                                        } else {
                                            format!("Calibration failed: {}.", flags.join(", "))
                                        }}
                                    </Callout>
                                }.into_any()
                            } else {
                                view!{ <span></span> }.into_any()
                            }
                        }}
                    </Step>
                </ol>

                <div class="dq-cal-foot">
                    <span class="dq-cal-state">
                        {move || status.get().map(|s| view!{
                            <span class="dq-cal-state-item">{mode_name(s.mode)}</span>
                            <span class=phase_tone(s.phase)>{phase_name(s.phase)}</span>
                        })}
                    </span>
                    <span class="dq-spacer"></span>
                    {move || running().then(|| if confirm_abort.get() {
                        view!{
                            <span class="dq-cal-ask">"Abort this run?"</span>
                            <button type="button" class="btn btn-sm" on:click=move |_| confirm_abort.set(false)>"Keep running"</button>
                            <button type="button" class="btn btn-sm btn-tinted tone-red" on:click=abort>"Abort now"</button>
                        }.into_any()
                    } else {
                        view!{
                            <button type="button" class="btn btn-sm" on:click=move |_| confirm_abort.set(true)>"Abort"</button>
                        }.into_any()
                    })}
                    <button type="button" class="btn btn-sm btn-plain" on:click=move |_| open.set(false)>"Close"</button>
                </div>
            </div>
        </Show>
    }
}
