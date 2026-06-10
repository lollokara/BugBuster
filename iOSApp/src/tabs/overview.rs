use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;

const SUPPLY_CONTROLS: [u8; 3] = [3, 0, 1]; // LOGIC_EN, VADJ1, VADJ2
const SUPPLY_COLORS: [&str; 3] = ["#10b981", "#06b6d4", "#ff4d6a"];
const SUPPLY_NAMES: [&str; 3] = ["VLOGIC", "V_ADJ1", "V_ADJ2"];
// Per-channel voltage limits when firmware reports 0/0 (uncalibrated first boot).
// ch0 = VLOGIC (TPS74601 1.8–5 V), ch1/2 = VADJ1/2 (LTM8063 3–15 V).
const SUPPLY_VMIN: [f32; 3] = [1.8, 3.0, 3.0];
const SUPPLY_VMAX: [f32; 3] = [5.0, 15.0, 15.0];

#[component]
pub fn OverviewTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (selftest, set_selftest) = signal(SelftestStatus::default());
    let (supplies, set_supplies) = signal(SelftestSuppliesCached::default());
    let (idac, set_idac) = signal(IdacState::default());
    let (ioexp, set_ioexp) = signal(IoExpState::default());
    let (hat, set_hat) = signal(HatStatus::default());
    let (hat_rails, set_hat_rails) = signal(Vec::<HatRailStatus>::new());
    
    let (quicksetup_supported, set_quicksetup_supported) = signal(None::<bool>);
    let (quicksetup_slots, set_quicksetup_slots) = signal(Vec::<QuickSetupSlot>::new());
    let (quicksetup_busy, set_quicksetup_busy) = signal(None::<u8>);
    
    // Target voltages (V) for the three IDAC-controlled supplies.
    // Updated from ch.target_v when not dirty; holds the user's choice when dirty.
    let supply_targets: [RwSignal<f32>; 3] = std::array::from_fn(|_| RwSignal::new(0.0f32));
    let supply_dirty: [RwSignal<bool>; 3] = std::array::from_fn(|_| RwSignal::new(false));
    let optimistic_en: [RwSignal<Option<bool>>; 3] = std::array::from_fn(|_| RwSignal::new(None));
    
    // HAT targets (mV)
    let hat_targets: [RwSignal<u16>; 2] = [RwSignal::new(3300), RwSignal::new(3300)];
    let hat_dirty: [RwSignal<bool>; 2] = [RwSignal::new(false), RwSignal::new(false)];

    // True while a selftest_worker_set command is in flight
    let worker_pending: RwSignal<bool> = RwSignal::new(false);

    spawn_local(async move {
        refresh_quicksetup_slots(set_quicksetup_supported, set_quicksetup_slots).await;
    });

    // Alive flag
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    let alive_poll = alive.clone();
    spawn_local(async move {
        loop {
            if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) { break; }
            let snap = state.get_untracked();
            if snap.spi_ok || !snap.channels.is_empty() {
                // Selftest & Supplies
                if let Some(st) = fetch_selftest_status().await {
                    if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) { break; }
                    if worker_pending.get_untracked() {
                        set_selftest.update(|s| { s.boot = st.boot; s.cal = st.cal; });
                    } else {
                        set_selftest.set(st);
                    }
                }
                
                if selftest.get_untracked().worker_enabled {
                    if let Some(sup) = fetch_selftest_supplies_cached().await {
                        if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) { break; }
                        set_supplies.set(sup);
                    }
                } else {
                    set_supplies.set(SelftestSuppliesCached::default());
                }

                // IDAC & PCA
                if let Some(st) = fetch_idac_status().await {
                    if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) { break; }
                    for (i, ch) in st.channels.iter().take(3).enumerate() {
                        // Track the firmware's applied target voltage so the
                        // non-dirty display always reflects what the board is doing.
                        if !supply_dirty[i].get_untracked() {
                            supply_targets[i].set(ch.target_v);
                        }
                    }
                    set_idac.set(st);
                }
                if let Some(st) = fetch_pca_status().await {
                    if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) { break; }
                    set_ioexp.set(st);
                }

                // HAT
                if let Some(st) = fetch_hat_status().await {
                    if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) { break; }
                    let detected = st.detected;
                    set_hat.set(st);
                    if detected {
                        if let Some(rls) = hat_get_rail_status().await {
                            for r in rls.iter() {
                                if r.rail_id == 1 || r.rail_id == 2 {
                                    let idx = (r.rail_id - 1) as usize;
                                    if !hat_dirty[idx].get_untracked() && r.voltage_mv > 0 {
                                        hat_targets[idx].set(r.voltage_mv);
                                    }
                                }
                            }
                            set_hat_rails.set(rls);
                        }
                    }
                }
            }
            overview_sleep_ms(500).await;
        }
    });

    let reset = move |_: leptos::ev::MouseEvent| {
        invoke_with_feedback("device_reset", wasm_bindgen::JsValue::NULL, "Device reset");
    };

    view! {
        <div class="tab-content ios-compact">
            <div class="summary-banner" style="justify-content: space-between; gap: 8px; padding: 10px; margin-bottom: 12px; border-radius: 12px">
                <div style="display: flex; align-items: center; gap: 10px; flex-wrap: wrap">
                    <StatusPill label="SPI" ok=move || state.get().spi_ok value=move || if state.get().spi_ok { "OK".to_string() } else { "ERR".to_string() } />
                    <StatusPill label="TEMP" ok=move || true value=move || format!("{:.0}C", state.get().die_temperature) />
                    
                    <div style="display: flex; align-items: center; gap: 6px; padding-right: 8px; border-right: 1px solid rgba(100,140,200,0.08)">
                        <span class="summary-label">"Monitor"</span>
                        <label class="toggle-wrap">
                            <div class="toggle toggle-sm" class:active=move || selftest.get().worker_enabled
                                on:click=move |_| {
                                    let enabled = !selftest.get_untracked().worker_enabled;
                                    set_selftest.update(|s| {
                                        s.worker_enabled = enabled;
                                        if !enabled { s.supply_monitor_active = false; }
                                    });
                                    worker_pending.set(true);
                                    spawn_local(async move {
                                        let actual = fetch_selftest_worker_set(enabled).await;
                                        set_selftest.update(|s| {
                                            s.worker_enabled = actual.unwrap_or(enabled);
                                            if !s.worker_enabled { s.supply_monitor_active = false; }
                                        });
                                        worker_pending.set(false);
                                        show_toast(if enabled { "Monitor enabled" } else { "Monitor disabled" }, "ok");
                                    });
                                }
                            ><div class="toggle-thumb"></div></div>
                        </label>
                    </div>
                </div>
                <button class="reset-btn" style="padding: 4px 10px" on:click=reset>
                    <span class="reset-icon">"↻"</span>
                </button>
            </div>

            // Supplies display (only when active)
            {move || {
                let st = selftest.get();
                let sup = supplies.get();
                if st.worker_enabled && !sup.rails.is_empty() {
                    let chips = sup.rails.iter().map(|r| {
                        let val = if r.voltage_v < 0.0 { "---".into() } else { format!("{:.2}V", r.voltage_v) };
                        view! {
                            <div class="status-pill-mini">
                                <span class="label">{r.name.clone()}</span>
                                <span class="value">{val}</span>
                            </div>
                        }
                    }).collect::<Vec<_>>();
                    view! {
                        <div class="mini-supplies-row">
                            {chips}
                        </div>
                    }.into_any()
                } else { ().into_any() }
            }}

            <SectionTitle title="Analog Channels" />
            <div class="channel-grid-compact">
                {move || {
                    let ds = state.get();
                    let monitor_active = selftest.get().supply_monitor_active;
                    ds.channels.into_iter().enumerate().map(|(i, ch)| {
                        let ch_idx = i as u8;
                        let fn_label = func_name(ch.function);
                        let is_active = ch.function != 0;
                        let unit = if matches!(ch.function, 4 | 5 | 11 | 12) { "mA" } else if ch.function == 7 { "Ω" } else { "V" };
                        let color = CH_COLORS[i];
                        let ch_reserved = monitor_active && i == 2;

                        view! {
                            <div class="card channel-card-ios" class:ch-active=is_active>
                                <div class="ch-header-ios">
                                    <span class="ch-badge-ios" style=format!("background: {}25; color: {}", color, color)>
                                        {CH_NAMES[i]}
                                    </span>
                                    <span class="ch-func-ios">{fn_label}</span>
                                </div>
                                <div class="ch-value-ios" class:dimmed=!is_active>
                                    {if is_active { format!("{:.3}", ch.adc_value) } else { "---".to_string() }}
                                    <span class="unit-ios">{if is_active { unit } else { "" }}</span>
                                </div>
                                {if !ch_reserved {
                                    view! {
                                        <select class="dropdown-ios"
                                            prop:value=ch.function.to_string()
                                            on:change=move |e| {
                                                let func: u8 = event_target_value(&e).parse().unwrap_or(0);
                                                #[derive(Serialize)] struct Args { channel: u8, function: u8 }
                                                let args = serde_wasm_bindgen::to_value(&Args { channel: ch_idx, function: func }).unwrap();
                                                invoke_with_feedback("set_channel_function", args, &format!("Set CH {} to {}", CH_NAMES[ch_idx as usize], func_name(func)));
                                            }
                                        >
                                            {FN_OPTIONS.iter().map(|(code, name)| {
                                                view! { <option value=code.to_string()>{*name}</option> }
                                            }).collect::<Vec<_>>()}
                                        </select>
                                    }.into_any()
                                } else {
                                    view! { <div class="diag-overlay-ios">"DIAG"</div> }.into_any()
                                }}
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>

            <SectionTitle title="Voltage Supplies" />
            <div class="supplies-container">
                // DS4424 Rails (Base)
                {move || {
                    let st = idac.get();
                    let pca = ioexp.get();
                    if !st.present { return ().into_any(); }

                    st.channels.into_iter().take(3).enumerate().map(|(i, ch)| {
                        let color = SUPPLY_COLORS[i];
                        let name = SUPPLY_NAMES[i];
                        let enabled = match i { 0 => pca.en_mux, 1 => pca.vadj1_en, 2 => pca.vadj2_en, _ => false };
                        let is_active = move || {
                            optimistic_en[i].get().unwrap_or(enabled)
                        };

                        // Voltage slider range: use firmware's v_min/v_max when available,
                        // fall back to hardware-spec defaults for that channel.
                        let slider_min = if ch.v_min > 0.0 { ch.v_min } else { SUPPLY_VMIN[i] };
                        let slider_max = if ch.v_max > slider_min { ch.v_max } else { SUPPLY_VMAX[i] };

                        // Preview: when dirty show the user's chosen voltage directly
                        // (no code→voltage conversion needed — firmware owns calibration).
                        // When not dirty show the firmware's own target_v.
                        let display_v = if supply_dirty[i].get() {
                            supply_targets[i].get()
                        } else {
                            ch.target_v
                        };

                        view! {
                            <div class="card supply-card-ios" style=format!("border-left: 4px solid {}", color)>
                                <div class="supply-header-ios">
                                    <div class="supply-name-ios" style=format!("color: {}", color)>{name}</div>
                                    <label class="toggle-wrap">
                                        <div class="toggle toggle-sm" class:active=is_active
                                            on:click=move |_| {
                                                let next_val = !is_active();
                                                optimistic_en[i].set(Some(next_val));
                                                send_pca_control(SUPPLY_CONTROLS[i], next_val);
                                                spawn_local(async move {
                                                    overview_sleep_ms(2500).await;
                                                    optimistic_en[i].set(None);
                                                });
                                            }
                                        ><div class="toggle-thumb"></div></div>
                                    </label>
                                </div>
                                <div class="supply-body-ios">
                                    <div class="supply-readout-ios">
                                        <span class="v-main">{format!("{:.3}V", display_v)}</span>
                                        {if supply_dirty[i].get() { view! { <span class="v-preview">"Preview"</span> }.into_any() } else { ().into_any() }}
                                    </div>
                                    <div class="slider-row">
                                        <input type="range" class="slider-ios"
                                            style=format!("--slider-color: {}", color)
                                            min=format!("{:.1}", slider_min)
                                            max=format!("{:.1}", slider_max)
                                            step="0.1"
                                            prop:value=move || format!("{:.3}", supply_targets[i].get())
                                            on:input=move |e| {
                                                if let Ok(v) = event_target_value(&e).parse::<f32>() {
                                                    supply_targets[i].set(v.clamp(slider_min, slider_max));
                                                    supply_dirty[i].set(true);
                                                }
                                            }
                                        />
                                        <button class="apply-btn-ios" 
                                            class:visible=move || supply_dirty[i].get()
                                            on:click=move |_| {
                                                send_idac_voltage(i as u8, supply_targets[i].get_untracked());
                                                supply_dirty[i].set(false);
                                            }
                                        >"SET"</button>
                                    </div>
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>().into_any()
                }}

                // HAT Rails (Optional)
                {move || {
                    let st = hat.get();
                    if !st.detected { return ().into_any(); }
                    
                    let rails = hat_rails.get();
                    (1..=2u8).map(|i| {
                        let color = if i == 1 { "#8b5cf6" } else { "#ec4899" };
                        let name = if i == 1 { "V_ADJ3" } else { "V_ADJ4" };
                        let rail_id = i; // 1 and 2 in HAT firmware
                        let rail_st = rails.iter().find(|r| r.rail_id == rail_id);
                        let enabled = rail_st.map(|r| r.enabled).unwrap_or(false);
                        let cur_mv = rail_st.map(|r| r.voltage_mv).unwrap_or(0);
                        let cur_ma = rail_st.map(|r| r.current_ma).unwrap_or(0);
                        
                        let idx = (i - 1) as usize;
                        let display_mv = if hat_dirty[idx].get() {
                            hat_targets[idx].get()
                        } else if cur_mv > 0 {
                            cur_mv
                        } else {
                            hat_targets[idx].get()
                        };

                        view! {
                            <div class="card supply-card-ios" style=format!("border-left: 4px solid {}", color)>
                                <div class="supply-header-ios">
                                    <div class="supply-name-ios" style=format!("color: {}", color)>{name}</div>
                                    <div style="display: flex; align-items: center; gap: 8px">
                                        <span class="supply-meta-ios">{format!("{}mA", cur_ma)}</span>
                                        <label class="toggle-wrap">
                                            <div class="toggle toggle-sm" class:active=enabled
                                                on:click=move |_| {
                                                    spawn_local(async move {
                                                        if let Some(r) = hat_set_rail_enable(rail_id, !enabled).await {
                                                            set_hat_rails.update(|current| {
                                                                for new_r in r {
                                                                    if new_r.rail_id == rail_id {
                                                                        if let Some(pos) = current.iter().position(|e| e.rail_id == new_r.rail_id) {
                                                                            current[pos] = new_r;
                                                                        } else {
                                                                            current.push(new_r);
                                                                        }
                                                                        break;
                                                                    }
                                                                }
                                                            });
                                                        }
                                                    });
                                                }
                                            ><div class="toggle-thumb"></div></div>
                                        </label>
                                    </div>
                                </div>
                                <div class="supply-body-ios">
                                    <div class="supply-readout-ios">
                                        // Show the user's pending target whenever the slider is dirty;
                                        // show measured cur_mv only when live (enabled + not dirty).
                                        <span class="v-main">
                                            {if hat_dirty[idx].get() || !enabled {
                                                format!("{:.2}V", display_mv as f32 / 1000.0)
                                            } else {
                                                format!("{:.2}V", cur_mv as f32 / 1000.0)
                                            }}
                                        </span>
                                        {if hat_dirty[idx].get() {
                                            view! { <span class="v-preview">"Preview"</span> }.into_any()
                                        } else if !enabled {
                                            view! { <span class="v-preview">"Target"</span> }.into_any()
                                        } else {
                                            ().into_any()
                                        }}
                                    </div>
                                    <div class="slider-row">
                                        <input type="range" class="slider-ios"
                                            style=format!("--slider-color: {}", color)
                                            min="0" max="36000" step="100"
                                            prop:value=move || {
                                                if hat_dirty[idx].get() {
                                                    hat_targets[idx].get().to_string()
                                                } else if cur_mv > 0 {
                                                    cur_mv.to_string()
                                                } else {
                                                    "3300".to_string()
                                                }
                                            }
                                            on:input=move |e| {
                                                if let Ok(v) = event_target_value(&e).parse::<u16>() {
                                                    hat_targets[idx].set(v);
                                                    hat_dirty[idx].set(true);
                                                }
                                            }
                                        />
                                        <button class="apply-btn-ios" 
                                            class:visible=move || hat_dirty[idx].get()
                                            on:click=move |_| {
                                                let mv = hat_targets[idx].get_untracked();
                                                spawn_local(async move {
                                                    if let Some(r) = hat_set_rail_voltage(rail_id, mv).await {
                                                        // Merge: only update the rail we changed.
                                                        set_hat_rails.update(|current| {
                                                            for new_r in r {
                                                                if new_r.rail_id == rail_id {
                                                                    if let Some(pos) = current.iter().position(|e| e.rail_id == new_r.rail_id) {
                                                                        current[pos] = new_r;
                                                                    } else {
                                                                        current.push(new_r);
                                                                    }
                                                                    break;
                                                                }
                                                            }
                                                        });
                                                        hat_dirty[idx].set(false);
                                                    }
                                                });
                                            }
                                        >"SET"</button>
                                    </div>
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>().into_any()
                }}
            </div>

            <SectionTitle title="Quick Setups" />
            <div class="quick-setup-row-ios">
                {move || match quicksetup_supported.get() {
                    Some(true) => quicksetup_slots.get().into_iter().map(|slot| {
                        let idx = slot.index;
                        let busy = quicksetup_busy.get() == Some(idx);
                        view! {
                            <button class="qs-btn-ios" class:occupied=slot.occupied disabled=busy
                                on:click=move |_| {
                                    if slot.occupied {
                                        set_quicksetup_busy.set(Some(idx));
                                        spawn_local(async move {
                                            let _ = quicksetup_apply_slot(idx).await;
                                            set_quicksetup_busy.set(None);
                                            show_toast(&format!("Applied Slot {}", idx+1), "ok");
                                        });
                                    } else {
                                        set_quicksetup_busy.set(Some(idx));
                                        spawn_local(async move {
                                            if quicksetup_save_slot(idx).await.is_some() {
                                                refresh_quicksetup_slots(set_quicksetup_supported, set_quicksetup_slots).await;
                                                show_toast(&format!("Saved Slot {}", idx+1), "ok");
                                            }
                                            set_quicksetup_busy.set(None);
                                        });
                                    }
                                }
                            >
                                <span class="qs-idx">{idx + 1}</span>
                                <span class="qs-status">{if slot.occupied { "Apply" } else { "Save" }}</span>
                            </button>
                        }
                    }).collect::<Vec<_>>().into_any(),
                    _ => view! { <div style="font-size: 11px; color: var(--text-dim)">"No slots available"</div> }.into_any(),
                }}
            </div>
        </div>
    }
}

#[component]
fn StatusPill<F, G>(label: &'static str, ok: F, value: G) -> impl IntoView
where
    F: Fn() -> bool + Copy + Send + Sync + 'static,
    G: Fn() -> String + Copy + Send + Sync + 'static,
{
    view! {
        <div style="display: flex; align-items: center; gap: 6px; padding-right: 10px; border-right: 1px solid rgba(100,140,200,0.08)">
            <span class="status-dot" class:connected=move || ok() class:disconnected=move || !ok()></span>
            <span class="summary-label">{label}</span>
            <span class="summary-value" class:ok=move || ok() class:err=move || !ok()>{move || value()}</span>
        </div>
    }
}

#[component]
fn SectionTitle(title: &'static str) -> impl IntoView {
    view! {
        <div style="font-size: 10px; font-weight: 700; color: var(--text-dim); margin: 16px 0 8px; letter-spacing: 1px; text-transform: uppercase">
            {title}
        </div>
    }
}

#[component]
fn DiagnosticOverlay() -> impl IntoView {
    view! {
        <div style="position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; text-align: center; padding: 14px; background: rgba(8,12,24,0.72); border: 1px solid rgba(245,158,11,0.26); color: #f59e0b; font-weight: 800; font-size: 12px; letter-spacing: 0.4px; z-index: 2">
            "CH-C Used for internal diagnostic"
        </div>
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
