use crate::components::icons::Icon;
use crate::components::ui::{Callout, Switch};
use crate::tabs::efuse_monitor::EfuseMonitorStrip;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::JsCast;

const SUPPLY_CONTROLS: [u8; 3] = [3, 0, 1]; // LOGIC_EN, VADJ1, VADJ2
const SUPPLY_NAMES: [&str; 3] = ["VLOGIC", "V_ADJ1", "V_ADJ2"];
const SUPPLY_SWITCH_LABELS: [&str; 3] = ["Enable VLOGIC", "Enable V_ADJ1", "Enable V_ADJ2"];

#[component]
pub fn OverviewTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (selftest, set_selftest) = signal(SelftestStatus::default());
    let (supplies, set_supplies) = signal(SelftestSuppliesCached::default());
    let (idac, set_idac) = signal(IdacState::default());
    let idac_loaded = RwSignal::new(false);
    let imon = RwSignal::new(EfuseImonStatus::default());
    start_efuse_imon_poll(imon);
    let (ioexp, set_ioexp) = signal(IoExpState::default());
    let (quicksetup_supported, set_quicksetup_supported) = signal(None::<bool>);
    let (quicksetup_slots, set_quicksetup_slots) = signal(Vec::<QuickSetupSlot>::new());
    let (quicksetup_detail, set_quicksetup_detail) = signal(None::<QuickSetupPayload>);
    let (quicksetup_busy, set_quicksetup_busy) = signal(None::<u8>);
    let supply_codes: [RwSignal<i32>; 3] = std::array::from_fn(|_| RwSignal::new(0));
    let supply_dirty: [RwSignal<bool>; 3] = std::array::from_fn(|_| RwSignal::new(false));
    // True while a selftest_worker_set command is in flight — prevents the 2 s
    // poll loop from overwriting the optimistic worker_enabled toggle state.
    let worker_pending: RwSignal<bool> = RwSignal::new(false);

    spawn_local(async move {
        refresh_quicksetup_slots(set_quicksetup_supported, set_quicksetup_slots).await;
    });

    // Alive flag — flips false on tab unmount so the 2 s status poll terminates.
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    let alive_poll = alive.clone();
    spawn_local(async move {
        // Initial grace period: the S3 firmware shares its UART bus (UART0 @
        // 921600) between RP2040 HAT commands and P4 DAQ commands. IDAC/PCA
        // I2C reads fired immediately on mount arrive while the firmware is
        // still initializing its own bus state, causing 2000ms host-side
        // timeouts that cascade into status-poll failures. Wait 8 seconds
        // before the first poll cycle so the firmware can settle.
        overview_sleep_ms(8000).await;
        loop {
            if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                break;
            }
            // Skip fetches when disconnected to avoid spamming failed BBP commands.
            let snap = state.get_untracked();
            if snap.spi_ok || !snap.channels.is_empty() {
                if let Some(st) = fetch_selftest_status().await {
                    if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                        break;
                    }
                    if worker_pending.get_untracked() {
                        // A toggle command is in flight — update boot/cal but
                        // don't overwrite the optimistic worker_enabled state.
                        set_selftest.update(|s| {
                            s.boot = st.boot;
                            s.cal = st.cal;
                        });
                    } else {
                        set_selftest.set(st);
                    }
                } else if let Some(enabled) = fetch_selftest_worker_enabled().await {
                    if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                        break;
                    }
                    if !worker_pending.get_untracked() {
                        set_selftest.update(|s| s.worker_enabled = enabled);
                    }
                }
                if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                    break;
                }
                if selftest.get_untracked().worker_enabled {
                    if let Some(sup) = fetch_selftest_supplies_cached().await {
                        if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                            break;
                        }
                        set_supplies.set(sup);
                    }
                } else {
                    set_supplies.set(SelftestSuppliesCached::default());
                }
                if let Some(st) = fetch_idac_status().await {
                    if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                        break;
                    }
                    set_idac.set(st);
                    idac_loaded.set(true);
                }
                if let Some(st) = fetch_pca_status().await {
                    if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                        break;
                    }
                    set_ioexp.set(st);
                }
            }
            overview_sleep_ms(2000).await;
        }
    });

    let reset = move |_: leptos::ev::MouseEvent| {
        invoke_with_feedback("device_reset", wasm_bindgen::JsValue::NULL, "Device reset");
    };

    let alerts_total = move || {
        let ds = state.get();
        (ds.alert_status.count_ones()
            + ds.supply_alert_status.count_ones()
            + ds
                .channels
                .iter()
                .map(|c| c.channel_alert.count_ones())
                .sum::<u32>()) as usize
    };

    view! {
        <div class="view ov">
            <div class="section-label">"Health"</div>
            <div class="group ov-health">
                <StatusCell label="SPI link"
                    tone=move || if state.get().spi_ok { "green" } else { "red" }
                    value=move || if state.get().spi_ok { "OK".to_string() } else { "Error".to_string() } />
                <div class="ov-cell">
                    <span class="ov-cell-label">"Die temperature"</span>
                    <span class="ov-cell-value">{move || format!("{:.1} \u{00B0}C", state.get().die_temperature)}</span>
                </div>
                <div class="ov-cell">
                    <span class="ov-cell-label">"Alerts"</span>
                    <span class="ov-cell-value">
                        <span class=move || if alerts_total() == 0 { "dot tone-green" } else { "dot tone-red" }></span>
                        {move || { let n = alerts_total(); if n == 0 { "None".to_string() } else { format!("{n} active") } }}
                        <span class="ov-cell-meta">{move || format!("0x{:04X}", state.get().alert_status)}</span>
                        <button class="btn btn-plain btn-sm ov-link" title="Open Faults" aria-label="Open Faults view"
                            on:click=move |_| go_to_view("faults")
                        >"View"<Icon name="chevron-right" size=12 /></button>
                    </span>
                </div>
                <StatusCell label="Supply"
                    tone=move || if state.get().supply_alert_status == 0 { "green" } else { "red" }
                    value=move || {
                        let s = state.get().supply_alert_status;
                        if s == 0 { "OK".to_string() } else { format!("0x{:04X}", s) }
                    } />
                <div class="ov-cell">
                    <span class="ov-cell-label">"Supply monitor"</span>
                    <span class="ov-cell-value">
                        <Switch
                            checked=Signal::derive(move || selftest.try_get().map_or(false, |s| s.worker_enabled))
                            aria_label="Supply monitor"
                            on_change=Callback::new(move |_: bool| {
                                let enabled = !selftest.get_untracked().worker_enabled;
                                // Optimistic update
                                set_selftest.update(|s| {
                                    s.worker_enabled = enabled;
                                    if !enabled { s.supply_monitor_active = false; }
                                });
                                worker_pending.set(true);
                                spawn_local(async move {
                                    let actual = fetch_selftest_worker_set(enabled).await;
                                    // Confirm with device-returned state (or keep optimistic on error)
                                    set_selftest.update(|s| {
                                        s.worker_enabled = actual.unwrap_or(enabled);
                                        if !s.worker_enabled { s.supply_monitor_active = false; }
                                    });
                                    worker_pending.set(false);
                                    let label = if enabled { "Enable supply monitor" } else { "Disable supply monitor" };
                                    let kind = if actual.is_some() { "ok" } else { "err" };
                                    show_toast(label, kind);
                                });
                            })
                        />
                        <span class:tone-text-green=move || selftest.try_get().map_or(false, |s| s.supply_monitor_active)>
                            {move || { let st = selftest.try_get().unwrap_or_default(); if st.supply_monitor_active { "Active" } else if st.worker_enabled { "Enabled" } else { "Off" } }}
                        </span>
                    </span>
                </div>
                {move || {
                    let st = selftest.try_get().unwrap_or_default();
                    let sup = supplies.try_get().unwrap_or_default();
                    if st.worker_enabled && !sup.rails.is_empty() {
                        sup.rails.iter().map(|r| {
                            let label = r.name.clone();
                            let val = if r.voltage_v < 0.0 {
                                "-".to_string()
                            } else {
                                format!("{:.2} V", r.voltage_v)
                            };
                            view! {
                                <div class="ov-cell">
                                    <span class="ov-cell-label">{label}</span>
                                    <span class="ov-cell-value">{val}</span>
                                </div>
                            }
                        }).collect::<Vec<_>>().into_any()
                    } else {
                        ().into_any()
                    }
                }}
                <div class="ov-health-actions">
                    <button class="btn btn-sm" title="Reset device" on:click=reset>
                        <Icon name="refresh-cw" size=14 />"Reset"
                    </button>
                </div>
            </div>

            <div class="section-label">"Analog channels"</div>
            <div class="grid-4 ov-ch-grid">
                {move || {
                    let ds = state.get();
                    let monitor_active = selftest.try_get().map_or(false, |s| s.supply_monitor_active);
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let fn_label = func_name(ch.function);
                        let is_active = ch.function != 0;
                        let is_res = ch.function == 7;
                        let unit = if matches!(ch.function, 4 | 5 | 11 | 12) { "mA" } else if is_res { "ohm" } else { "V" };
                        let range_abs_max = ADC_RANGE_OPTIONS.iter()
                            .find(|(code, _, _, _)| *code == ch.adc_range)
                            .map(|(_, _, min_v, max_v)| min_v.abs().max(max_v.abs()) as f64)
                            .unwrap_or(12.0);
                        let range_max: f64 = if is_res {
                            let excitation_ua = if ch.rtd_excitation_ua > 0 { ch.rtd_excitation_ua } else { 1000 };
                            range_abs_max / (excitation_ua as f64 * 1e-6)
                        } else {
                            range_abs_max
                        };
                        let pct = if range_max > 0.0 { (ch.adc_value.abs() as f64 / range_max * 100.0).min(100.0) } else { 0.0 };
                        let ch_reserved = (monitor_active || imon.get().efuse != 0) && i == 2;

                        view! {
                            <div class="group ov-ch" data-ch=i.to_string() class:is-off=!is_active>
                                <div class="ov-ch-body" class:is-dim=ch_reserved>
                                    <div class="group-header">
                                        <span class="ov-ch-name">
                                            <span class="ov-ch-dot"></span>
                                            {format!("Channel {}", CH_NAMES[i])}
                                        </span>
                                        <span class="chip">{fn_label}</span>
                                    </div>
                                    <div class="readout readout-lg ov-ch-readout">
                                        <span class="readout-value">
                                            {if is_active { format!("{:.4}", ch.adc_value) } else { "---".to_string() }}
                                            <span class="unit">{if is_active { unit } else { "" }}</span>
                                        </span>
                                    </div>
                                    <div class="meter ov-meter">
                                        <span style=format!("width: {}%", if is_active { pct } else { 0.0 })></span>
                                    </div>
                                    <div class="card-details">
                                        <span>"DAC "{format!("{:.3}", ch.dac_value)}</span>
                                        <span class="ov-mono">"Raw 0x"{format!("{:06X}", ch.adc_raw)}</span>
                                    </div>
                                    <div class="row ov-ch-fn">
                                        <label for=format!("ov-fn-{}", ch_idx)>"Function"</label>
                                        <select class="dropdown"
                                            id=format!("ov-fn-{}", ch_idx)
                                            prop:value=ch.function.to_string()
                                            disabled=ch_reserved
                                            on:change=move |e| {
                                                let func: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                #[derive(Serialize)]
                                                struct Args { channel: u8, function: u8 }
                                                let args = serde_wasm_bindgen::to_value(&Args { channel: ch_idx, function: func }).unwrap();
                                                let label = format!("Set CH {} to {}", CH_NAMES[ch_idx as usize], func_name(func));
                                                invoke_with_feedback("set_channel_function", args, &label);
                                            }
                                        >
                                            {FN_OPTIONS.iter().map(|(code, name)| {
                                                view! { <option value=code.to_string()>{*name}</option> }
                                            }).collect::<Vec<_>>()}
                                        </select>
                                    </div>
                                </div>
                                {ch_reserved.then(|| view! {
                                    <div class="ov-ch-overlay">
                                        <div class="ov-ch-callout">
                                            <Callout tone="orange">
                                                <strong>"Reserved for internal diagnostics"</strong>
                                                <div class="text-footnote muted">"The supply monitor or e-fuse current monitor is using this channel."</div>
                                            </Callout>
                                        </div>
                                    </div>
                                })}
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>

            <div class="section-label">"Digital IO"</div>
            <div class="group">
                <div class="ov-io-grid">
                    {move || {
                        let ds = state.get();
                        ds.gpio.into_iter().enumerate().map(|(i, g)| {
                            let mode_name = GPIO_MODE_OPTIONS.iter()
                                .find(|(c, _)| *c == g.mode)
                                .map(|(_, n)| *n).unwrap_or("?");
                            let high = g.input || g.output;
                            let dir_icon = match g.mode {
                                1 | 4 => "log-out",
                                2 => "log-in",
                                3 => "arrow-left-right",
                                _ => "minus",
                            };
                            view! {
                                <div class="ov-io" class:is-high=high>
                                    <div class="ov-io-head">
                                        <span>{format!("IO{}", i + 1)}</span>
                                        <span class="ov-io-dir" title=mode_name><Icon name=dir_icon size=13 /></span>
                                    </div>
                                    <div class="ov-io-mode" title=mode_name>{mode_name}</div>
                                    <div class="ov-io-state">
                                        <span class=if high { "dot tone-green" } else { "dot" }></span>
                                        {if high { "High" } else { "Low" }}
                                    </div>
                                </div>
                            }
                        }).collect::<Vec<_>>()
                    }}
                </div>
            </div>

            <div class="section-label">"Supplies"</div>
            <div class="grid-3 ov-supply-grid">
                {move || {
                    let st = idac.get();
                    let pca = ioexp.get();
                    if !idac_loaded.get() {
                        return view! {
                            <div class="group ov-span-all">
                                <div class="hstack muted"><span class="spinner"></span>"Reading DS4424 supply state..."</div>
                            </div>
                        }.into_any();
                    }
                    if !st.present {
                        return view! {
                            <div class="ov-span-all">
                                <Callout tone="orange">"DS4424 not detected on I2C bus."</Callout>
                            </div>
                        }.into_any();
                    }

                    st.channels.into_iter().take(3).enumerate().map(|(i, ch)| {
                        let name = SUPPLY_NAMES[i];
                        let enabled = match i {
                            0 => pca.en_mux,
                            1 => pca.vadj1_en,
                            2 => pca.vadj2_en,
                            _ => false,
                        };
                        if !supply_dirty[i].get() {
                            supply_codes[i].set(ch.code as i32);
                        }
                        let display_code = supply_codes[i].get() as i8;
                        // Uncalibrated rails fall back to the nominal formula so the slider still tracks.
                        let calibrated = idac_interpolate_voltage_opt(&ch, display_code).is_some();
                        let display_v = Some(idac_interpolate_voltage(&ch, display_code).clamp(ch.v_min, ch.v_max));
                        let ctrl = SUPPLY_CONTROLS[i];
                        let ch_idx = i as u8;
                        view! {
                            <div class="group ov-supply" data-supply=i.to_string()>
                                <div class="group-header">
                                    <span class="ov-supply-name">
                                        <span class="ov-supply-dot"></span>
                                        {name}
                                    </span>
                                    <Switch
                                        checked=Signal::derive(move || enabled)
                                        aria_label=SUPPLY_SWITCH_LABELS[i]
                                        on_change=Callback::new(move |_: bool| { send_pca_control(ctrl, !enabled); })
                                    />
                                </div>
                                <div class="ov-supply-value">
                                    <div class="readout readout-lg">
                                        <span class="readout-value">
                                            {display_v.map(|v| format!("{:.3}", v)).unwrap_or_else(|| "---".to_string())}
                                            <span class="unit">"V"</span>
                                        </span>
                                    </div>
                                    <div class="ov-supply-tags">
                                        {(!enabled).then(|| view! { <span class="chip">"Preview"</span> })}
                                        {(!calibrated).then(|| view! { <span class="chip">"Estimated"</span> })}
                                    </div>
                                </div>
                                <input type="range" class="ov-slider"
                                    aria-label=format!("{} setpoint", name)
                                    min="-127" max="127" step="1"
                                    // Keep UX consistent: slider right = higher voltage.
                                    // DS4424 rails use negative code for higher V, so invert UI mapping.
                                    prop:value=move || -supply_codes[i].get()
                                    on:input=move |e| {
                                        if let Ok(v) = event_target_value(&e).parse::<i32>() {
                                            supply_codes[i].set((-v).clamp(-127, 127));
                                            supply_dirty[i].set(true);
                                        }
                                    }
                                />
                                <div class="card-details">
                                    <span>{format!("Range {:.1} - {:.1} V", ch.v_min, ch.v_max)}</span>
                                    <span class="ov-mono">{format!("code {}", display_code)}</span>
                                </div>
                                <button class="btn btn-tinted btn-block"
                                    disabled=move || !supply_dirty[i].get()
                                    on:click=move |_| {
                                        send_idac_code(ch_idx, supply_codes[i].get_untracked() as i8);
                                        supply_dirty[i].set(false);
                                    }
                                >"Apply"</button>
                            </div>
                        }
                    }).collect::<Vec<_>>().into_any()
                }}
            </div>

            <div class="section-label">"E-fuse outputs and current"</div>
            <EfuseMonitorStrip ioexp=ioexp imon=imon />

            <div class="section-label">"Quick setups"</div>
            {move || match quicksetup_supported.get() {
                None => view! {
                    <div class="group">
                        <div class="hstack muted"><span class="spinner"></span>"Detecting quick-setup support..."</div>
                    </div>
                }.into_any(),
                Some(false) => view! {
                    <Callout tone="orange">"Quick setups are unavailable on this firmware."</Callout>
                }.into_any(),
                Some(true) => view! {
                    <div class="grid-4 ov-qs-grid">
                        {move || quicksetup_slots.get().into_iter().map(|slot| {
                            let slot_idx = slot.index;
                            let display_idx = slot_idx + 1;
                            let occupied = slot.occupied;
                            let summary_hash = slot.summary_hash;

                            view! {
                                <div class="group ov-qs">
                                    <div class="group-header">
                                        <span class="group-title">{format!("Slot {}", display_idx)}</span>
                                        {if occupied {
                                            view! { <span class="badge tone-green">"Saved "<span class="ov-mono">{format!("{:02X}", summary_hash)}</span></span> }.into_any()
                                        } else {
                                            view! { <span class="badge">"Empty"</span> }.into_any()
                                        }}
                                    </div>
                                    <div class="ov-qs-actions">
                                        <button class="btn btn-sm" disabled=move || quicksetup_busy.get() == Some(slot_idx)
                                            on:click=move |_| {
                                                set_quicksetup_busy.set(Some(slot_idx));
                                                spawn_local(async move {
                                                    let saved = quicksetup_save_slot(slot_idx).await;
                                                    if let Some(payload) = saved {
                                                        set_quicksetup_detail.set(Some(payload));
                                                        show_toast(&format!("Saved Slot {}", display_idx), "ok");
                                                        refresh_quicksetup_slots(set_quicksetup_supported, set_quicksetup_slots).await;
                                                    } else {
                                                        show_toast(&format!("Failed: Save Slot {}", display_idx), "err");
                                                    }
                                                    set_quicksetup_busy.set(None);
                                                });
                                            }
                                        >"Save"</button>
                                        <button class="btn btn-sm btn-tinted" disabled=move || !occupied || quicksetup_busy.get() == Some(slot_idx)
                                            on:click=move |_| {
                                                set_quicksetup_busy.set(Some(slot_idx));
                                                spawn_local(async move {
                                                    match quicksetup_apply_slot(slot_idx).await {
                                                        Some(result) if result.ok => show_toast(&format!("Applied Slot {}", display_idx), "ok"),
                                                        Some(result) => show_toast(&format!("Failed: {}", result.message), "err"),
                                                        None => show_toast(&format!("Failed: Apply Slot {}", display_idx), "err"),
                                                    }
                                                    refresh_quicksetup_slots(set_quicksetup_supported, set_quicksetup_slots).await;
                                                    set_quicksetup_busy.set(None);
                                                });
                                            }
                                        >"Apply"</button>
                                        <button class="btn btn-sm" disabled=move || !occupied || quicksetup_busy.get() == Some(slot_idx)
                                            on:click=move |_| {
                                                set_quicksetup_busy.set(Some(slot_idx));
                                                spawn_local(async move {
                                                    if let Some(payload) = quicksetup_get_slot(slot_idx).await {
                                                        set_quicksetup_detail.set(Some(payload));
                                                        show_toast(&format!("Loaded Slot {}", display_idx), "ok");
                                                    } else {
                                                        show_toast(&format!("Failed: Load Slot {}", display_idx), "err");
                                                    }
                                                    set_quicksetup_busy.set(None);
                                                });
                                            }
                                        >"View"</button>
                                        <button class="btn btn-sm btn-tinted tone-red" disabled=move || !occupied || quicksetup_busy.get() == Some(slot_idx)
                                            on:click=move |_| {
                                                set_quicksetup_busy.set(Some(slot_idx));
                                                spawn_local(async move {
                                                    match quicksetup_delete_slot(slot_idx).await {
                                                        Some(result) if result.ok => {
                                                            set_quicksetup_detail.set(None);
                                                            show_toast(&format!("Deleted Slot {}", display_idx), "ok");
                                                        }
                                                        Some(result) => show_toast(&format!("Failed: {}", result.message), "err"),
                                                        None => show_toast(&format!("Failed: Delete Slot {}", display_idx), "err"),
                                                    }
                                                    refresh_quicksetup_slots(set_quicksetup_supported, set_quicksetup_slots).await;
                                                    set_quicksetup_busy.set(None);
                                                });
                                            }
                                        >"Delete"</button>
                                    </div>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                }.into_any(),
            }}
            {move || quicksetup_detail.get().map(|payload| {
                view! {
                    <div class="group">
                        <div class="group-header">
                            <span class="group-title">{format!("Slot {}", payload.slot + 1)}</span>
                            <span class="chip">{format!("{} B", payload.byte_len)}</span>
                        </div>
                        <div class="text-footnote muted">
                            {payload.name.unwrap_or_else(|| "Unnamed setup".to_string())}
                        </div>
                        <pre class="ov-pre">{payload.json}</pre>
                    </div>
                }
            })}
        </div>
    }
}

/// Dot + word status cell used in the health row.
#[component]
fn StatusCell<F, G>(label: &'static str, tone: F, value: G) -> impl IntoView
where
    F: Fn() -> &'static str + Copy + Send + Sync + 'static,
    G: Fn() -> String + Copy + Send + Sync + 'static,
{
    view! {
        <div class="ov-cell">
            <span class="ov-cell-label">{label}</span>
            <span class="ov-cell-value">
                <span class=move || format!("dot tone-{}", tone())></span>
                {move || value()}
            </span>
        </div>
    }
}

/// Activates a sidebar entry; the shell owns navigation state.
fn go_to_view(id: &str) {
    let target = web_sys::window()
        .and_then(|w| w.document())
        .and_then(|d| d.query_selector(&format!("[data-nav-view=\"{id}\"]")).ok().flatten())
        .and_then(|el| el.dyn_into::<web_sys::HtmlElement>().ok());
    if let Some(el) = target {
        el.click();
    }
}

async fn overview_sleep_ms(ms: u32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window()
            .unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms as i32)
            .ok();
    });
    wasm_bindgen_futures::JsFuture::from(promise).await.ok();
}

async fn refresh_quicksetup_slots(
    set_supported: WriteSignal<Option<bool>>,
    set_slots: WriteSignal<Vec<QuickSetupSlot>>,
) {
    if let Some(list) = fetch_quicksetup_list().await {
        set_supported.set(Some(list.supported));
        set_slots.set(list.slots);
    } else {
        set_supported.set(Some(false));
        set_slots.set(Vec::new());
    }
}
