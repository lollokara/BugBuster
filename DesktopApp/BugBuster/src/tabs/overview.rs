use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;

const SUPPLY_CONTROLS: [u8; 3] = [3, 0, 1]; // LOGIC_EN, VADJ1, VADJ2
const SUPPLY_COLORS: [&str; 3] = ["#10b981", "#06b6d4", "#ff4d6a"];
const SUPPLY_NAMES: [&str; 3] = ["VLOGIC", "V_ADJ1", "V_ADJ2"];

#[component]
pub fn OverviewTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (selftest, set_selftest) = signal(SelftestStatus::default());
    let (idac, set_idac) = signal(IdacState::default());
    let (ioexp, set_ioexp) = signal(IoExpState::default());
    let (quicksetup_supported, set_quicksetup_supported) = signal(None::<bool>);
    let (quicksetup_slots, set_quicksetup_slots) = signal(Vec::<QuickSetupSlot>::new());
    let (quicksetup_detail, set_quicksetup_detail) = signal(None::<QuickSetupPayload>);
    let (quicksetup_busy, set_quicksetup_busy) = signal(None::<u8>);
    let supply_codes: [RwSignal<i32>; 3] = std::array::from_fn(|_| RwSignal::new(0));
    let supply_dirty: [RwSignal<bool>; 3] = std::array::from_fn(|_| RwSignal::new(false));

    spawn_local(async move {
        refresh_quicksetup_slots(set_quicksetup_supported, set_quicksetup_slots).await;
    });

    // Alive flag — flips false on tab unmount so the 2 s status poll terminates.
    let alive: RwSignal<bool> = RwSignal::new(true);
    on_cleanup(move || alive.set(false));

    spawn_local(async move {
        loop {
            if !alive.get_untracked() { break; }
            // Skip fetches when disconnected to avoid spamming failed BBP commands.
            let snap = state.get_untracked();
            if snap.spi_ok || !snap.channels.is_empty() {
                if let Some(st) = fetch_selftest_status().await {
                    set_selftest.set(st);
                } else if let Some(enabled) = fetch_selftest_worker_enabled().await {
                    set_selftest.update(|s| s.worker_enabled = enabled);
                }
                if let Some(st) = fetch_idac_status().await {
                    set_idac.set(st);
                }
                if let Some(st) = fetch_pca_status().await {
                    set_ioexp.set(st);
                }
            }
            overview_sleep_ms(2000).await;
        }
    });

    let reset = move |_: leptos::ev::MouseEvent| {
        invoke_with_feedback("device_reset", wasm_bindgen::JsValue::NULL, "Device reset");
    };

    view! {
        <div class="tab-content">
            <div class="summary-banner" style="justify-content: space-between; gap: 10px; padding: 8px 12px; margin-bottom: 14px">
                <div style="display: flex; align-items: center; gap: 14px; flex-wrap: wrap">
                    <StatusPill label="SPI" ok=move || state.get().spi_ok value=move || if state.get().spi_ok { "OK".to_string() } else { "ERROR".to_string() } />
                    <StatusPill label="TEMP" ok=move || true value=move || format!("{:.1} C", state.get().die_temperature) />
                    <StatusPill label="ALERTS" ok=move || state.get().alert_status == 0 value=move || {
                        let s = state.get().alert_status;
                        if s == 0 { "None".to_string() } else { format!("0x{:04X}", s) }
                    } />
                    <StatusPill label="SUPPLY" ok=move || state.get().supply_alert_status == 0 value=move || {
                        let s = state.get().supply_alert_status;
                        if s == 0 { "OK".to_string() } else { format!("0x{:04X}", s) }
                    } />
                    <div style="display: flex; align-items: center; gap: 8px">
                        <span class="summary-label">"Supply Monitor"</span>
                        <label class="toggle-wrap">
                            <div class="toggle" class:active=move || selftest.get().worker_enabled
                                on:click=move |_| {
                                    let enabled = !selftest.get_untracked().worker_enabled;
                                    set_selftest.update(|s| {
                                        s.worker_enabled = enabled;
                                        if !enabled {
                                            s.supply_monitor_active = false;
                                        }
                                    });
                                    send_selftest_worker(enabled);
                                }
                            ><div class="toggle-thumb"></div></div>
                        </label>
                        <span class="summary-value" class:ok=move || selftest.get().supply_monitor_active>
                            {move || if selftest.get().supply_monitor_active { "Active" } else if selftest.get().worker_enabled { "Enabled" } else { "Off" }}
                        </span>
                    </div>
                </div>
                <button class="reset-btn" on:click=reset>
                    <span class="reset-icon">"↻"</span>
                    <span>"Reset"</span>
                </button>
            </div>

            <SectionTitle title="Analog Channels" />
            <div class="channel-grid">
                {move || {
                    let ds = state.get();
                    let monitor_active = selftest.get().supply_monitor_active;
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
                        let color = CH_COLORS[i];
                        let ch_d_reserved = monitor_active && i == 3;

                        view! {
                            <div class="card channel-card" class:ch-active=is_active style="position: relative; overflow: hidden">
                                <div style=move || if ch_d_reserved { "opacity: 0.35; pointer-events: none" } else { "" }>
                                    <div class="card-header">
                                        <div class="ch-badge" style=format!("background: {}22; color: {}; border: 1px solid {}44", color, color, color)>
                                            {format!("CH {}", CH_NAMES[i])}
                                        </div>
                                        <span class="channel-func">{fn_label}</span>
                                    </div>
                                    <div class="card-body">
                                        <div class="big-value" class:dimmed=!is_active>
                                            {if is_active { format!("{:.4}", ch.adc_value) } else { "---".to_string() }}
                                            <span class="unit">{if is_active { unit } else { "" }}</span>
                                        </div>
                                        <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                            <div class="bar-fill-dynamic" style=format!("width: {}%", if is_active { pct } else { 0.0 })></div>
                                        </div>
                                        <div class="card-details">
                                            <span>"DAC: "{format!("{:.3}", ch.dac_value)}</span>
                                            <span>"Raw: 0x"{format!("{:06X}", ch.adc_raw)}</span>
                                        </div>
                                        <div class="config-row" style="margin-top: 8px;">
                                            <label>"Function"</label>
                                            <select class="dropdown"
                                                prop:value=ch.function.to_string()
                                                disabled=ch_d_reserved
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
                                </div>
                                {if ch_d_reserved {
                                    view! { <DiagnosticOverlay /> }.into_any()
                                } else {
                                    view! { <></> }.into_any()
                                }}
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>

            <SectionTitle title="Digital IO" />
            <div class="card" style="margin-bottom: 16px">
                <div class="card-body">
                    <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(64px, 1fr)); gap: 8px">
                        {move || {
                            let ds = state.get();
                            ds.gpio.into_iter().enumerate().map(|(i, g)| {
                                let mode_name = GPIO_MODE_OPTIONS.iter()
                                    .find(|(c, _)| *c == g.mode)
                                    .map(|(_, n)| *n).unwrap_or("?");
                                let active = g.input || g.output;
                                view! {
                                    <div style=format!("padding: 8px; border-radius: 8px; background: {}; border: 1px solid {}; min-height: 64px",
                                        if active { "rgba(16,185,129,0.08)" } else { "rgba(100,140,200,0.035)" },
                                        if active { "rgba(16,185,129,0.25)" } else { "rgba(100,140,200,0.08)" }
                                    )>
                                        <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 4px">
                                            <span class="uppercase-tag">{format!("IO{}", i + 1)}</span>
                                            <span class="led" class:led-on=active></span>
                                        </div>
                                        <div style="font-size: 10px; color: var(--text-dim); white-space: nowrap; overflow: hidden; text-overflow: ellipsis">{mode_name}</div>
                                        <div style="font-size: 11px; font-family: 'JetBrains Mono', monospace; color: var(--text); margin-top: 4px">
                                            {if g.input { "IN:H" } else if g.output { "OUT:H" } else { "LOW" }}
                                        </div>
                                    </div>
                                }
                            }).collect::<Vec<_>>()
                        }}
                    </div>
                </div>
            </div>

            <SectionTitle title="Supply Sliders" />
            <div class="channel-grid-wide" style="grid-template-columns: repeat(3, minmax(220px, 1fr))">
                {move || {
                    let st = idac.get();
                    let pca = ioexp.get();
                    if !st.present {
                        return view! {
                            <div class="card" style="grid-column: 1 / -1">
                                <div class="card-header"><span class="channel-func">"DS4424 IDAC"</span></div>
                                <div class="card-body">
                                    <div class="mode-warning">
                                        <span class="mode-warning-icon">"!"</span>
                                        <span>"DS4424 not detected on I2C bus."</span>
                                    </div>
                                </div>
                            </div>
                        }.into_any();
                    }

                    st.channels.into_iter().take(3).enumerate().map(|(i, ch)| {
                        let color = SUPPLY_COLORS[i];
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
                        let display_v = idac_interpolate_voltage_opt(&ch, display_code)
                            .map(|v| v.clamp(ch.v_min, ch.v_max));
                        let pct = display_v
                            .map(|v| if ch.v_max > ch.v_min { ((v - ch.v_min) / (ch.v_max - ch.v_min) * 100.0).clamp(0.0, 100.0) } else { 0.0 })
                            .unwrap_or(0.0);
                        let ctrl = SUPPLY_CONTROLS[i];
                        let ch_idx = i as u8;
                        view! {
                            <div class="card" style=format!("border-top: 3px solid {}", color)>
                                <div class="card-header">
                                    <div>
                                        <div style=format!("font-weight: 700; color: {}", color)>{name}</div>
                                        <div style="font-size: 10px; color: var(--text-dim); font-family: 'JetBrains Mono', monospace">
                                            {format!("code {} | {:.1}-{:.1} V", display_code, ch.v_min, ch.v_max)}
                                        </div>
                                    </div>
                                    <label class="toggle-wrap">
                                        <div class="toggle" class:active=enabled
                                            on:click=move |_| { send_pca_control(ctrl, !enabled); }
                                        ><div class="toggle-thumb"></div></div>
                                    </label>
                                </div>
                                <div class="card-body">
                                    <div style=format!("text-align: center; font-size: 26px; font-weight: 800; color: {}; font-family: 'JetBrains Mono', monospace", color)>
                                        {display_v.map(|v| format!("{:.3} V", v)).unwrap_or_else(|| "--- V".to_string())}
                                    </div>
                                    <input type="range" class="slider slider-colored"
                                        style=format!("--slider-color: {}; width: 100%", color)
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
                                    <div class="bar-gauge" style=format!("--bar-color: {}", color)>
                                        <div class="bar-fill-dynamic" style=format!("width: {}%", pct)></div>
                                    </div>
                                    <button class="scope-btn" style=format!("color: {}; border-color: {}55", color, color)
                                        disabled=move || !supply_dirty[i].get()
                                        on:click=move |_| {
                                            send_idac_code(ch_idx, supply_codes[i].get_untracked() as i8);
                                            supply_dirty[i].set(false);
                                        }
                                    >"Apply"</button>
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>().into_any()
                }}
            </div>

            <SectionTitle title="Quick Setups" />
            {move || match quicksetup_supported.get() {
                None => view! {
                    <div class="card" style="margin-bottom: 16px">
                        <div class="card-body" style="color: var(--text-dim); font-size: 12px">
                            "Detecting quick-setup support..."
                        </div>
                    </div>
                }.into_any(),
                Some(false) => view! {
                    <div class="card" style="margin-bottom: 16px">
                        <div class="card-body">
                            <div class="mode-warning">
                                <span class="mode-warning-icon">"!"</span>
                                <span>"Quick Setups unavailable on this firmware."</span>
                            </div>
                        </div>
                    </div>
                }.into_any(),
                Some(true) => view! {
                    <div class="channel-grid-wide" style="grid-template-columns: repeat(4, minmax(150px, 1fr)); margin-bottom: 16px">
                        {move || quicksetup_slots.get().into_iter().map(|slot| {
                            let slot_idx = slot.index;
                            let display_idx = slot_idx + 1;
                            let occupied = slot.occupied;
                            let summary_hash = slot.summary_hash;
                            let status_label = if occupied { "Saved" } else { "Empty" };
                            let status_color = if occupied { "#10b981" } else { "var(--text-dim)" };

                            view! {
                                <div class="card" style=format!("border-top: 3px solid {}", if occupied { "#10b981" } else { "rgba(100,140,200,0.20)" })>
                                    <div class="card-header">
                                        <div>
                                            <div style="font-weight: 800">{format!("Slot {}", display_idx)}</div>
                                            <div style=format!("font-size: 10px; color: {}; font-family: 'JetBrains Mono', monospace", status_color)>
                                                {if occupied { format!("{} · {:02X}", status_label, summary_hash) } else { status_label.to_string() }}
                                            </div>
                                        </div>
                                    </div>
                                    <div class="card-body">
                                        <div style="display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 6px">
                                            <button class="scope-btn" disabled=move || quicksetup_busy.get() == Some(slot_idx)
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
                                            <button class="scope-btn" disabled=move || !occupied || quicksetup_busy.get() == Some(slot_idx)
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
                                            <button class="scope-btn" disabled=move || !occupied || quicksetup_busy.get() == Some(slot_idx)
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
                                            <button class="scope-btn" style="color: #ef4444; border-color: #ef444455" disabled=move || !occupied || quicksetup_busy.get() == Some(slot_idx)
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
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                }.into_any(),
            }}
            {move || quicksetup_detail.get().map(|payload| {
                view! {
                    <div class="card" style="margin-bottom: 16px">
                        <div class="card-header">
                            <span>{format!("Slot {}", payload.slot + 1)}</span>
                            <span class="uppercase-tag">{format!("{} B", payload.byte_len)}</span>
                        </div>
                        <div class="card-body">
                            <div style="font-size: 11px; color: var(--text-dim); margin-bottom: 6px">
                                {payload.name.unwrap_or_else(|| "Unnamed setup".to_string())}
                            </div>
                            <pre style="max-height: 120px; overflow: auto; margin: 0; padding: 8px; border-radius: 6px; background: rgba(8,12,24,0.45); font-size: 10px; color: var(--text); white-space: pre-wrap">{payload.json}</pre>
                        </div>
                    </div>
                }
            })}
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
            "CH-D Used for internal diagnostic"
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
