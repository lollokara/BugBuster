// =============================================================================
// daq_cal.rs — SMU factory-calibration wizard (modal) for the DAQ HAT.
//
// Drives the interactive voltage / current-limit calibration over the S3 BBP
// control plane (see src-tauri/src/daq_commands.rs daq_cal_*). The firmware
// pauses on an operator prompt (disconnect the load / short the output) until
// the user clicks Continue.
// =============================================================================

use leptos::prelude::*;
use leptos::task::spawn_local;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

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
        2 => "Running…",
        3 => "Success",
        4 => "Failed",
        _ => "—",
    }
}

fn mode_name(m: u8) -> &'static str {
    match m {
        0 => "Voltage",
        1 => "Current limit",
        _ => "—",
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
        1 => "Saving…",
        2 => "Saved to NVM",
        3 => "Save failed",
        _ => "—",
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
        (0x0100, "USB-PD too low — current cal needs 20 V / 3 A"),
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

#[component]
pub fn CalibrationWizard(open: RwSignal<bool>) -> impl IntoView {
    let status = RwSignal::new(Option::<DaqCalStatus>::None);
    let starting = RwSignal::new(false);

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
                    if let Some(st) = daq_cal_status().await {
                        status.set(Some(st));
                    }
                }
                slp(400).await;
            }
        });
    }

    let start = move |mode: u8| {
        starting.set(true);
        spawn_local(async move {
            daq_cal_start(mode).await;
            slp(200).await;
            starting.set(false);
        });
    };
    let ack = move |_| spawn_local(async move { daq_cal_ack().await });
    let abort = move |_| spawn_local(async move { daq_cal_abort().await });

    // Derived view helpers.
    let phase = move || status.get().map(|s| s.phase).unwrap_or(0);
    let running = move || matches!(phase(), 1 | 2);

    view! {
        <Show when=move || open.get()>
            <div
                style="position:fixed;inset:0;background:rgba(0,0,0,0.55);z-index:1000;\
                       display:flex;align-items:center;justify-content:center;"
                on:click=move |_| open.set(false)
            >
                <div
                    style="width:440px;max-width:92vw;background:#0f172a;border:1px solid #334155;\
                           border-radius:10px;padding:18px;color:#e2e8f0;font-size:13px;"
                    on:click=move |ev| ev.stop_propagation()
                >
                    <div style="display:flex;justify-content:space-between;align-items:center;">
                        <h3 style="margin:0;font-size:15px;">"SMU Calibration"</h3>
                        <button class="btn btn-ghost btn-sm" on:click=move |_| open.set(false)>"✕"</button>
                    </div>

                    <p style="color:#94a3b8;margin:8px 0 12px;">
                        "Calibrate voltage first, then current. Each run pauses for a \
                         hardware step — follow the on-screen prompt."
                    </p>

                    // Start buttons (disabled while a run is active).
                    <div style="display:flex;gap:8px;margin-bottom:12px;">
                        <button
                            class="btn btn-primary btn-sm"
                            prop:disabled=move || running() || starting.get()
                            on:click=move |_| start(0)
                        >
                            "Calibrate Voltage"
                        </button>
                        <button
                            class="btn btn-primary btn-sm"
                            prop:disabled=move || running() || starting.get()
                            on:click=move |_| start(1)
                        >
                            "Calibrate Current"
                        </button>
                    </div>

                    // Live status block.
                    <Show when=move || status.get().is_some()>
                        {move || {
                            let s = status.get().unwrap_or_default();
                            let flags = flag_names(s.flags);
                            view! {
                                <div style="background:#1e293b;border-radius:8px;padding:10px;">
                                    <div style="display:flex;justify-content:space-between;">
                                        <span>"Mode: " <b>{mode_name(s.mode)}</b></span>
                                        <span>"Phase: " <b>{phase_name(s.phase)}</b></span>
                                    </div>
                                    <div style="margin-top:6px;height:8px;background:#334155;border-radius:4px;overflow:hidden;">
                                        <div style=move || format!(
                                            "height:100%;width:{}%;background:#3b82f6;transition:width .2s;",
                                            s.progress.min(100))
                                        ></div>
                                    </div>
                                    <div style="display:flex;justify-content:space-between;margin-top:6px;color:#94a3b8;">
                                        <span>{format!("Point {} (code {})", s.point, s.code)}</span>
                                        <span>{format!("{:.4} {}", s.measured, unit(s.mode))}</span>
                                    </div>
                                    <div style="color:#94a3b8;">
                                        {format!("Span {:.3} … {:.3} {}", s.min, s.max, unit(s.mode))}
                                    </div>

                                    // Operator prompt.
                                    <Show when=move || s.phase == 1>
                                        <div style="margin-top:10px;padding:10px;background:#422006;\
                                                    border:1px solid #b45309;border-radius:6px;">
                                            <div style="font-weight:600;margin-bottom:6px;">"⚠ Action required"</div>
                                            <div>{prompt_text(s.prompt)}</div>
                                            <button class="btn btn-warning btn-sm" style="margin-top:8px;"
                                                on:click=ack>
                                                "I've done it — Continue"
                                            </button>
                                        </div>
                                    </Show>

                                    // Result.
                                    <Show when=move || s.phase == 3>
                                        <div style="margin-top:10px;color:#34d399;">
                                            {format!("✓ Calibration complete — {}", persist_name(s.persist))}
                                            {format!(" ({} V-pts, {} I-pts stored)", s.vcount, s.icount)}
                                        </div>
                                    </Show>
                                    <Show when=move || s.phase == 4>
                                        <div style="margin-top:10px;color:#f87171;">
                                            {if flags.is_empty() {
                                                "✕ Calibration failed.".to_string()
                                            } else {
                                                format!("✕ Calibration failed: {}", flags.join(", "))
                                            }}
                                        </div>
                                    </Show>
                                </div>
                            }
                        }}
                    </Show>

                    // Footer controls.
                    <div style="display:flex;gap:8px;margin-top:12px;justify-content:flex-end;">
                        <Show when=move || running()>
                            <button class="btn btn-ghost btn-sm" on:click=abort>"Abort"</button>
                        </Show>
                        <button class="btn btn-ghost btn-sm" on:click=move |_| open.set(false)>"Close"</button>
                    </div>
                </div>
            </div>
        </Show>
    }
}
