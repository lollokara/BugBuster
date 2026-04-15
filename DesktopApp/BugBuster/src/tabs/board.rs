use leptos::prelude::*;
use leptos::either::Either;
use serde::{Serialize, Deserialize};
use crate::tauri_bridge::*;

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq)]
pub enum PinMode {
    NC,
    GPIO,
    GPI,
    GPO,
    Analog,
}

impl PinMode {
    pub fn to_str(&self) -> &'static str {
        match self {
            PinMode::NC => "NC",
            PinMode::GPIO => "GPIO",
            PinMode::GPI => "GPI",
            PinMode::GPO => "GPO",
            PinMode::Analog => "Analog",
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct BoardConfig {
    pub name: String,
    pub description: String,
    pub vlogic: f32,
    pub vlogic_locked: bool,
    pub vadj1: f32,
    pub vadj1_locked: bool,
    pub vadj2: f32,
    pub vadj2_locked: bool,
    pub pins: Vec<PinMode>, // 12 pins
    pub pin_names: Vec<String>, // 12 names
}

impl Default for BoardConfig {
    fn default() -> Self {
        Self {
            name: "New Board".to_string(),
            description: "Custom DUT profile".to_string(),
            vlogic: 3.3,
            vlogic_locked: true,
            vadj1: 3.3,
            vadj1_locked: false,
            vadj2: 5.0,
            vadj2_locked: true,
            pins: vec![PinMode::NC; 12],
            pin_names: (1..=12).map(|i| format!("Port {}", i)).collect(),
        }
    }
}

#[component]
pub fn BoardTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (config, set_config) = signal(BoardConfig::default());

    let export_json = move |_| {
        let json = serde_json::to_string_pretty(&config.get()).unwrap();
        let args = serde_wasm_bindgen::to_value(&json).unwrap();
        invoke_with_feedback("save_board_profile", args, "Exporting board profile...");
    };

    let update_pin = move |idx: usize, mode: PinMode| {
        set_config.update(|c| c.pins[idx] = mode);
    };

    let update_pin_name = move |idx: usize, name: String| {
        set_config.update(|c| c.pin_names[idx] = name);
    };

    view! {
        <div class="tab-content" style="height: calc(100vh - 120px); overflow: hidden; display: flex; flex-direction: column;">
            <div class="tab-desc" style="margin-bottom: 10px;">"Configure your Device Under Test (DUT). Map pins and lock safety limits."</div>
            
            <div class="board-layout" style="display: grid; grid-template-columns: 1fr 400px; gap: 15px; flex: 1; min-height: 0;">
                
                // LEFT SIDE: Visualization + Power/General Info
                <div class="left-column" style="display: flex; flex-direction: column; gap: 15px; min-height: 0; overflow-y: auto;">
                    // Visualization
                    <div class="card" style="padding: 0; overflow: hidden; background: rgba(6, 10, 20, 0.4); flex: 1;">
                        <div class="card-header" style="padding: 8px 16px;">
                            <span class="channel-label" style="font-size: 0.8rem;">"Hardware Mapping"</span>
                            <div class="badge-hex">"BUGBUSTER_S3_V4"</div>
                        </div>
                        
                        <div class="board-schematic" style="position: relative; padding: 30px; display: flex; flex-direction: column; gap: 30px; align-items: center; justify-content: center; min-height: 450px; flex: 1;">
                            
                            // VADJ1 Rail Group (IO 1-6)
                            <div style="width: 100%; border: 1px dashed rgba(59, 130, 246, 0.3); border-radius: 12px; padding: 35px 20px; position: relative; background: rgba(59, 130, 246, 0.02);">
                                <div style="position: absolute; top: -10px; left: 20px; background: #0c1222; padding: 0 12px; color: var(--blue); font-family: 'JetBrains Mono', monospace; font-size: 11px; font-weight: 800; letter-spacing: 1px; border: 1px solid rgba(59, 130, 246, 0.2); border-radius: 4px;">"POWER DOMAIN: VADJ1 (3.3V SAFE)"</div>
                                
                                <div style="display: flex; gap: 40px; justify-content: space-around;">
                                    { (0..2).map(|block_idx| {
                                        view! {
                                            <div class="io-block-visual" style="flex: 1; background: rgba(20, 30, 52, 0.6); border: 1px solid var(--border-bright); border-radius: 10px; padding: 20px; display: flex; flex-direction: column; gap: 15px; box-shadow: inset 0 0 20px rgba(0,0,0,0.2);">
                                                <div style="font-size: 10px; color: var(--text-muted); font-weight: 800; letter-spacing: 2px; text-align: center; border-bottom: 1px solid var(--border); padding-bottom: 8px;">{format!("BLOCK {}", block_idx + 1)}</div>
                                                <div style="display: flex; justify-content: space-around; gap: 15px;">
                                                    { (0..3).map(|pin_offset| {
                                                        let i = block_idx * 3 + pin_offset;
                                                        let is_analog = i == 0 || i == 3;
                                                        let mode = move || config.get().pins[i];
                                                        let name = move || config.get().pin_names[i].clone();
                                                        let color = move || if is_analog { "var(--green)" } else { "var(--blue)" };
                                                        let glow = move || if is_analog { "0 0 15px var(--green)" } else { "0 0 8px rgba(59, 130, 246, 0.3)" };
                                                        
                                                        view! {
                                                            <div style="display: flex; flex-direction: column; align-items: center; gap: 8px; width: 85px;">
                                                                <div style=move || format!("width: 22px; height: 22px; border-radius: 4px; border: 3px solid {}; box-shadow: {}; background: rgba(0,0,0,0.6); transition: all 0.2s;", color(), glow())></div>
                                                                <div style="display: flex; flex-direction: column; align-items: center; gap: 3px; width: 100%;">
                                                                    <span style="font-size: 11px; font-weight: 800; color: var(--text); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; width: 100%; text-align: center; letter-spacing: -0.2px;">{name}</span>
                                                                    <span style="font-size: 10px; font-weight: 700; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; background: rgba(0,0,0,0.3); padding: 1px 6px; border-radius: 3px;">{move || mode().to_str()}</span>
                                                                </div>
                                                            </div>
                                                        }
                                                    }).collect::<Vec<_>>() }
                                                </div>
                                            </div>
                                        }
                                    }).collect::<Vec<_>>() }
                                </div>
                            </div>

                            // VADJ2 Rail Group (IO 7-12)
                            <div style="width: 100%; border: 1px dashed rgba(168, 85, 247, 0.3); border-radius: 12px; padding: 35px 20px; position: relative; background: rgba(168, 85, 247, 0.02);">
                                <div style="position: absolute; top: -10px; left: 20px; background: #0c1222; padding: 0 12px; color: var(--purple); font-family: 'JetBrains Mono', monospace; font-size: 11px; font-weight: 800; letter-spacing: 1px; border: 1px solid rgba(168, 85, 247, 0.2); border-radius: 4px;">"POWER DOMAIN: VADJ2 (HIGH VOLTAGE)"</div>
                                
                                <div style="display: flex; gap: 40px; justify-content: space-around;">
                                    { (2..4).map(|block_idx| {
                                        view! {
                                            <div class="io-block-visual" style="flex: 1; background: rgba(20, 30, 52, 0.6); border: 1px solid var(--border-bright); border-radius: 10px; padding: 20px; display: flex; flex-direction: column; gap: 15px; box-shadow: inset 0 0 20px rgba(0,0,0,0.2);">
                                                <div style="font-size: 10px; color: var(--text-muted); font-weight: 800; letter-spacing: 2px; text-align: center; border-bottom: 1px solid var(--border); padding-bottom: 8px;">{format!("BLOCK {}", block_idx + 1)}</div>
                                                <div style="display: flex; justify-content: space-around; gap: 15px;">
                                                    { (0..3).map(|pin_offset| {
                                                        let i = block_idx * 3 + pin_offset;
                                                        let is_analog = i == 6 || i == 9;
                                                        let mode = move || config.get().pins[i];
                                                        let name = move || config.get().pin_names[i].clone();
                                                        let color = move || if is_analog { "var(--green)" } else { "var(--purple)" };
                                                        let glow = move || if is_analog { "0 0 15px var(--green)" } else { "0 0 8px rgba(168, 85, 247, 0.3)" };
                                                        
                                                        view! {
                                                            <div style="display: flex; flex-direction: column; align-items: center; gap: 8px; width: 85px;">
                                                                <div style=move || format!("width: 22px; height: 22px; border-radius: 4px; border: 3px solid {}; box-shadow: {}; background: rgba(0,0,0,0.6); transition: all 0.2s;", color(), glow())></div>
                                                                <div style="display: flex; flex-direction: column; align-items: center; gap: 3px; width: 100%;">
                                                                    <span style="font-size: 11px; font-weight: 800; color: var(--text); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; width: 100%; text-align: center; letter-spacing: -0.2px;">{name}</span>
                                                                    <span style="font-size: 10px; font-weight: 700; color: var(--text-dim); font-family: 'JetBrains Mono', monospace; background: rgba(0,0,0,0.3); padding: 1px 6px; border-radius: 3px;">{move || mode().to_str()}</span>
                                                                </div>
                                                            </div>
                                                        }
                                                    }).collect::<Vec<_>>() }
                                                </div>
                                            </div>
                                        }
                                    }).collect::<Vec<_>>() }
                                </div>
                            </div>
                        </div>
                    </div>

                    // General Info & Power Row (Side-by-side)
                    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 15px;">
                        <div class="card" style="padding: 10px;">
                            <div class="card-header" style="margin-bottom: 5px; padding-bottom: 4px;"><span class="channel-label" style="font-size: 0.75rem;">"General"</span></div>
                            <div class="card-body" style="gap: 4px;">
                                <div class="config-row" style="margin-bottom: 2px;"><label style="min-width: 50px; font-size: 0.7rem;">"Name"</label><input type="text" class="dropdown" style="max-width: none; height: 24px; font-size: 0.75rem;" prop:value=move || config.get().name on:input=move |e| set_config.update(|c| c.name = event_target_value(&e)) /></div>
                                <div class="config-row" style="margin-bottom: 0;"><label style="min-width: 50px; font-size: 0.7rem;">"Desc"</label><input type="text" class="dropdown" style="max-width: none; height: 24px; font-size: 0.75rem;" prop:value=move || config.get().description on:input=move |e| set_config.update(|c| c.description = event_target_value(&e)) /></div>
                            </div>
                        </div>
                        <div class="card" style="padding: 10px;">
                            <div class="card-header" style="margin-bottom: 5px; padding-bottom: 4px;"><span class="channel-label" style="font-size: 0.75rem;">"Power"</span></div>
                            <div class="card-body" style="gap: 2px;">
                                <div class="config-row" style="margin-bottom: 0;">
                                    <label style="min-width: 45px; font-size: 0.7rem;">"VLOG"</label>
                                    <input type="number" step="0.1" class="dropdown" style="width: 55px; height: 22px; padding: 0 4px; font-size: 0.75rem;" prop:value=move || format!("{:.1}", config.get().vlogic) on:input=move |e| set_config.update(|c| c.vlogic = event_target_value(&e).parse().unwrap_or(3.3)) />
                                    <div class="toggle" style="transform: scale(0.6); margin-left: -5px;" class:active=move || config.get().vlogic_locked on:click=move |_| set_config.update(|c| c.vlogic_locked = !c.vlogic_locked)><div class="toggle-thumb"></div></div>
                                </div>
                                <div class="config-row" style="margin-bottom: 0;">
                                    <label style="min-width: 45px; font-size: 0.7rem;">"VADJ1"</label>
                                    <input type="number" step="0.1" class="dropdown" style="width: 55px; height: 22px; padding: 0 4px; font-size: 0.75rem;" prop:value=move || format!("{:.1}", config.get().vadj1) on:input=move |e| set_config.update(|c| c.vadj1 = event_target_value(&e).parse().unwrap_or(3.3)) />
                                    <div class="toggle" style="transform: scale(0.6); margin-left: -5px;" class:active=move || config.get().vadj1_locked on:click=move |_| set_config.update(|c| c.vadj1_locked = !c.vadj1_locked)><div class="toggle-thumb"></div></div>
                                </div>
                                <div class="config-row" style="margin-bottom: 0;">
                                    <label style="min-width: 45px; font-size: 0.7rem;">"VADJ2"</label>
                                    <input type="number" step="0.1" class="dropdown" style="width: 55px; height: 22px; padding: 0 4px; font-size: 0.75rem;" prop:value=move || format!("{:.1}", config.get().vadj2) on:input=move |e| set_config.update(|c| c.vadj2 = event_target_value(&e).parse().unwrap_or(5.0)) />
                                    <div class="toggle" style="transform: scale(0.6); margin-left: -5px;" class:active=move || config.get().vadj2_locked on:click=move |_| set_config.update(|c| c.vadj2_locked = !c.vadj2_locked)><div class="toggle-thumb"></div></div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                // RIGHT SIDE: Dense IO Mapping (2-column grid)
                <div class="card" style="height: 100%; display: flex; flex-direction: column; padding: 10px;">
                    <div class="card-header" style="margin-bottom: 10px;">
                        <span class="channel-label" style="font-size: 0.8rem;">"IO Mapping"</span>
                        <button class="btn btn-primary btn-sm" on:click=export_json style="height: 28px; font-size: 0.7rem;">
                            "💾 EXPORT"
                        </button>
                    </div>
                    <div class="card-body" style="padding: 0; display: grid; grid-template-columns: 1fr 1fr; gap: 8px; min-height: 0; overflow-y: auto; padding-right: 4px;">
                        {move || (0..12).map(|i| {
                            let is_analog_capable = i == 0 || i == 3 || i == 6 || i == 9;
                            view! {
                                <div style="display: flex; flex-direction: column; gap: 3px; padding: 6px; background: rgba(255,255,255,0.02); border-radius: 4px; border: 1px solid var(--border);">
                                    <div style="display: flex; align-items: center; justify-content: space-between;">
                                        <span style="font-family: 'JetBrains Mono', monospace; font-size: 9px; font-weight: 800; color: var(--blue);">{format!("IO{:02}", i + 1)}</span>
                                    </div>
                                    <input type="text" class="dropdown" style="max-width: none; height: 22px; padding: 0 6px; font-size: 10px;"
                                        placeholder="Name..."
                                        prop:value=move || config.get().pin_names[i].clone()
                                        on:input=move |e| update_pin_name(i, event_target_value(&e)) />
                                    <select class="dropdown" style="max-width: none; height: 22px; font-size: 10px; padding: 0 4px;"
                                        prop:value=move || config.get().pins[i].to_str().to_string()
                                        on:change=move |e| {
                                            let val = event_target_value(&e);
                                            let mode = match val.as_str() {
                                                "GPIO" => PinMode::GPIO, "GPI" => PinMode::GPI, "GPO" => PinMode::GPO, "Analog" => PinMode::Analog, _ => PinMode::NC,
                                            };
                                            update_pin(i, mode);
                                        }
                                    >
                                        <option value="NC">"NC"</option>
                                        <option value="GPIO">"GPIO"</option>
                                        <option value="GPI">"GPI"</option>
                                        <option value="GPO">"GPO"</option>
                                        {if is_analog_capable { Either::Left(view! { <option value="Analog">"Analog"</option> }) } else { Either::Right(()) }}
                                    </select>
                                </div>
                            }
                        }).collect::<Vec<_>>()}
                    </div>
                </div>
            </div>
        </div>
    }
}
