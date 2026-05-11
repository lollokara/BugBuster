use leptos::prelude::*;
use leptos::task::spawn_local;
use crate::tauri_bridge::{io_claim, io_force_release};

/// Human-readable name for an IO owner kind code (mirrors io_owner_kind_t in io_owner.h).
pub fn owner_kind_name(kind: u8) -> &'static str {
    match kind {
        0 => "None",
        1 => "USB",
        2 => "HTTP",
        3 => "Script",
        4 => "CLI",
        5 => "Internal",
        _ => "Unknown",
    }
}

/// Banner displayed when `io_claim` is rejected because another interface holds the slots.
///
/// Rendered by `app.rs` when `io_blocked` signal is `Some(slots)`.
/// Provides a "Force take (admin)" button that calls `io_force_release(0xFF)` then retries
/// `io_claim` for the given slots.
#[component]
pub fn IoBlockedBanner(
    /// The slots that could not be claimed (displayed to the user).
    slots: Vec<u8>,
    /// Owner kind code from the firmware (0 = unknown/None). When non-zero a
    /// "blocked by <kind>" subtitle is shown so the user knows which interface
    /// holds the lock.
    #[prop(default = 0u8)]
    owner_kind: u8,
    /// Called when the claim succeeds after a force-release so the parent can clear the banner.
    on_claimed: Callback<()>,
) -> impl IntoView {
    let slots_display = slots
        .iter()
        .map(|s| {
            if *s < 12 {
                format!("IO{}", s + 1)
            } else {
                format!("CH{}", s - 12)
            }
        })
        .collect::<Vec<_>>()
        .join(", ");

    let kind_subtitle = if owner_kind > 0 {
        Some(format!(" — blocked by {}", owner_kind_name(owner_kind)))
    } else {
        None
    };

    let slots_for_force = slots.clone();
    let on_force = move |_| {
        let slots_inner = slots_for_force.clone();
        let on_claimed = on_claimed;
        spawn_local(async move {
            // Force-release all slots (0xFF = all) and retry claim.
            let released = io_force_release(0xFF).await;
            if !released {
                web_sys::console::warn_1(
                    &"[IoBlockedBanner] io_force_release returned false".into(),
                );
            }
            // Retry claim with a 5 s lease after force-release.
            let ok = io_claim(&slots_inner, 5000, "desktop-force").await;
            if ok {
                on_claimed.run(());
            } else {
                web_sys::console::warn_1(
                    &"[IoBlockedBanner] io_claim after force-release still failed".into(),
                );
            }
        });
    };

    view! {
        <div class="card io-blocked-banner" style="border: 1px solid #f59e0b; background: rgba(245,158,11,0.08); padding: 12px 16px; display: flex; align-items: center; gap: 12px; margin-bottom: 8px;">
            <span style="color: #f59e0b; font-size: 1.1em;">{"⚠"}</span>
            <div style="flex: 1; color: #f3f4f6; display: flex; flex-direction: column; gap: 2px;">
                <span>
                    {"IO slots held by another interface: "}
                    <strong>{slots_display}</strong>
                    {". This tab cannot control these IOs until the other session releases them."}
                </span>
                {kind_subtitle.map(|s| view! {
                    <span style="color: #fbbf24; font-size: 0.85em;">{s}</span>
                })}
            </div>
            <button
                class="btn btn-danger btn-xs"
                on:click=on_force
                title="Force-release all IO slots (requires admin token on device)"
            >
                {"Force take (admin)"}
            </button>
        </div>
    }
}
