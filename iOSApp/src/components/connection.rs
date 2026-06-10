use crate::tauri_bridge::{log, show_toast, try_invoke, DiscoveredDevice, sleep_ms};
use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::prelude::*;

#[component]
pub fn ConnectionPanel(
    devices: Signal<Vec<DiscoveredDevice>>,
    scanning: Signal<bool>,
    scan_completed: Signal<bool>,
    on_scan: Callback<ev::MouseEvent>,
) -> impl IntoView {
    let (selected_device_id, set_selected_device_id) = signal(None::<String>);
    let (token_input, set_token_input) = signal(String::new());

    let connect = move |device: DiscoveredDevice| {
        use serde::Serialize;
        #[derive(Serialize)]
        struct Args {
            #[serde(rename = "deviceId")]
            device_id: String,
        }
        
        let device_id = device.id.clone();
        let mac = device.id.replace("http://", "").replace("http:", "");

        spawn_local(async move {
            let token = token_input.get_untracked();
            if !token.is_empty() {
                #[derive(Serialize)]
                struct TokenArgs { mac: String, token: String }
                let _ = try_invoke("save_device_token", serde_wasm_bindgen::to_value(&TokenArgs { mac: mac.clone(), token }).unwrap()).await;
            }

            log(&format!("Connecting to: {}", device_id));
            let args = serde_wasm_bindgen::to_value(&Args { device_id }).unwrap();
            if try_invoke("connect_device", args).await.is_none() {
                show_toast("Connection failed — ensure token is correct", "err");
            }
        });
    };

    Effect::new(move |_| {
        // Run on mount
        spawn_local(async move {
            let mut retries = 0;
            loop {
                if let Some(window) = web_sys::window() {
                    let init_fn = js_sys::Reflect::get(&window, &"initSplash".into()).unwrap();
                    if init_fn.is_function() {
                        let _ = init_fn.unchecked_into::<js_sys::Function>().call0(&window);

                        // Set correct state in JS
                        let update_fn =
                            js_sys::Reflect::get(&window, &"updateScanStatus".into()).unwrap();
                        if update_fn.is_function() {
                            let _ = update_fn
                                .unchecked_into::<js_sys::Function>()
                                .call1(&window, &scan_completed.get().into());
                        }
                        break;
                    }
                }

                retries += 1;
                if retries > 50 {
                    web_sys::console::error_1(
                        &"ConnectionPanel: initSplash not found after 5 seconds".into(),
                    );
                    break;
                }
                sleep_ms(100).await;
            }
        });

        // Cleanup on unmount
        on_cleanup(move || {
            if let Some(window) = web_sys::window() {
                let destroy_fn = js_sys::Reflect::get(&window, &"destroySplash".into()).unwrap();
                if destroy_fn.is_function() {
                    let _ = destroy_fn
                        .unchecked_into::<js_sys::Function>()
                        .call0(&window);
                }
            }
        });
    });

    view! {
        <div class="connection-layout mobile-optimized">
            <div class="connection-ui-side">
                <div class="connection-header-group">
                    <h1 class="logo-title">"BugBuster"</h1>
                    <p class="subtitle-desc">"WiFi Debug Suite"</p>
                </div>

                <div class="card connection-card">
                    {move || if !scan_completed.get() {
                        view! {
                            <div class="scanning-loader-wrap">
                                <div class="scanning-glow-ring"></div>
                                <p class="scanning-status">"Initializing WiFi..."</p>
                            </div>
                        }.into_any()
                    } else {
                        view! {
                            <div style="display: flex; gap: 10px; margin-bottom: 20px;">
                                <button class="btn btn-primary btn-scan" style="flex: 1" on:click=move |e| on_scan.run(e) disabled=move || scanning.get()>
                                    {move || if scanning.get() { "Scanning..." } else { "Refresh" }}
                                </button>
                            </div>

                            <div class="device-list-container">
                                <div class="device-list">
                                    <For
                                        each=move || devices.get()
                                        key=|dev| dev.id.clone()
                                        children=move |dev: DiscoveredDevice| {
                                            let d_item = dev.clone();
                                            let id = dev.id.clone();
                                            let id_for_select = id.clone();
                                            let id_for_is_selected = id.clone();
                                            let is_selected = move || selected_device_id.get().as_deref() == Some(&id_for_is_selected);
                                            
                                            let dev_for_connect = d_item.clone();

                                            view! {
                                                <div class="device-item-wrap" class:selected=is_selected>
                                                    <button class="device-item" on:click=move |_| {
                                                        if is_selected() {
                                                            set_selected_device_id.set(None);
                                                        } else {
                                                            set_selected_device_id.set(Some(id_for_select.clone()));
                                                        }
                                                    }>
                                                        <span class="device-icon">"📡"</span>
                                                        <div class="device-info">
                                                            <span class="device-name">{d_item.name.clone()}</span>
                                                            <span class="device-addr">{d_item.address.clone()}</span>
                                                        </div>
                                                    </button>
                                                    
                                                    {move || {
                                                        if is_selected() {
                                                            let d = dev_for_connect.clone();
                                                            view! {
                                                                <div class="token-entry-area fade-in">
                                                                    <p style="font-size: 11px; color: #888; margin: 10px 0 5px;">"Enter Token (QR code on PCB)"</p>
                                                                    <input type="text" 
                                                                        class="token-input"
                                                                        placeholder="e.g. 123456"
                                                                        on:input=move |e| set_token_input.set(event_target_value(&e))
                                                                        prop:value=token_input
                                                                    />
                                                                    <button class="btn btn-success" style="width: 100%; margin-top: 10px; height: 44px;" 
                                                                        on:click=move |_| connect(d.clone())>
                                                                        "Authorize & Connect"
                                                                    </button>
                                                                </div>
                                                            }.into_any()
                                                        } else {
                                                            view! { <></> }.into_any()
                                                        }
                                                    }}
                                                </div>
                                            }
                                        }
                                    />
                                </div>
                            </div>
                        }.into_any()
                    }}
                </div>
            </div>
        </div>
    }
}
