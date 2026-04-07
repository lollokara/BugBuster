use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::prelude::*;

use crate::tauri_bridge::*;
use crate::tabs::{overview::*, adc::*, diag::*, vdac::*, idac::*, iin::*, din::*, dout::*, faults::*, gpio::*, uart::*, scope::*, wavegen::*, signal_path::*, voltages::*, calibration::*, usbpd::*, ioexp::*, hat::*, la::*};

const TABS: &[(&str, &str)] = &[
    ("overview", "Overview"),
    ("adc", "ADC"),
    ("diag", "Diagnostics"),
    ("vdac", "VDAC"),
    ("idac", "IDAC"),
    ("iin", "IIN"),
    ("din", "DIN"),
    ("dout", "DOUT"),
    ("faults", "Faults"),
    ("gpio", "GPIO"),
    ("uart", "UART"),
    ("scope", "Scope"),
    ("wavegen", "WaveGen"),
    ("sigpath", "Signal Path"),
    ("voltages", "Voltages"),
    ("calibration", "Calibration"),
    ("usbpd", "USB PD"),
    ("ioexp", "IO Expander"),
    ("hat", "HAT"),
    ("la", "Logic Analyzer"),
];

#[component]
pub fn App() -> impl IntoView {
    let (devices, set_devices) = signal(Vec::<DiscoveredDevice>::new());
    let (conn_mode, set_conn_mode) = signal("Disconnected".to_string());
    let (conn_addr, set_conn_addr) = signal(String::new());
    let (scanning, set_scanning) = signal(false);
    let (device_state, set_device_state) = signal(DeviceState::default());
    let (active_tab, set_active_tab) = signal("overview".to_string());
    let uart_config = RwSignal::new(UartConfigState::new());

    // Toast notification system
    let (toasts, set_toasts) = signal(Vec::<(String, String, f64)>::new()); // (msg, kind, timestamp)

    // Listen for toast events from invoke_with_feedback
    spawn_local(async move {
        let closure: Closure<dyn FnMut(JsValue)> = Closure::new(move |event: JsValue| {
            let event: web_sys::CustomEvent = event.unchecked_into();
            if let Some(detail) = event.detail().dyn_ref::<js_sys::Object>() {
                let msg = js_sys::Reflect::get(detail, &"msg".into())
                    .ok().and_then(|v| v.as_string()).unwrap_or_default();
                let kind = js_sys::Reflect::get(detail, &"kind".into())
                    .ok().and_then(|v| v.as_string()).unwrap_or_else(|| "info".into());
                let now = js_sys::Date::now();
                set_toasts.update(|t| {
                    t.push((msg, kind, now));
                    // Keep max 5 toasts
                    if t.len() > 5 { t.remove(0); }
                });
                // Auto-remove after 3 seconds
                let set_t = set_toasts;
                spawn_local(async move {
                    let promise = js_sys::Promise::new(&mut |resolve, _| {
                        web_sys::window().unwrap()
                            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, 3000).unwrap();
                    });
                    wasm_bindgen_futures::JsFuture::from(promise).await.unwrap();
                    set_t.update(|t| {
                        t.retain(|(_, _, ts)| js_sys::Date::now() - ts < 3000.0);
                    });
                });
            }
        });
        if let Some(window) = web_sys::window() {
            window.add_event_listener_with_callback("bb-toast", closure.as_ref().unchecked_ref()).ok();
        }
        closure.forget();
    });

    // Scan for devices
    let scan = move |_: ev::MouseEvent| {
        set_scanning.set(true);
        spawn_local(async move {
            let result = invoke("discover_devices", JsValue::NULL).await;
            if let Ok(devs) = serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(result) {
                set_devices.set(devs);
            }
            set_scanning.set(false);
        });
    };

    let disconnect = move |_: ev::MouseEvent| {
        spawn_local(async move { invoke("disconnect_device", JsValue::NULL).await; });
        set_conn_mode.set("Disconnected".to_string());
        set_conn_addr.set(String::new());
        set_device_state.set(DeviceState::default());
    };

    // Event listeners
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<DeviceState>>(event) {
                set_device_state.set(evt.payload);
            }
        });
        listen("device-state", &closure).await;
        closure.forget();
    });

    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<ConnectionStatus>>(event) {
                set_conn_mode.set(evt.payload.mode.clone());
                set_conn_addr.set(evt.payload.port_or_url.clone());
            }
        });
        listen("connection-status", &closure).await;
        closure.forget();
    });

    // Listen for protocol version mismatch
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<serde_json::Value>>(event) {
                let dev_ver = evt.payload.get("device_version").and_then(|v| v.as_u64()).unwrap_or(0);
                let exp_ver = evt.payload.get("expected_version").and_then(|v| v.as_u64()).unwrap_or(0);
                let msg = format!("Protocol mismatch: device v{}, app v{}. Some features may not work. Consider updating firmware.", dev_ver, exp_ver);
                // Dispatch toast event
                if let Some(window) = web_sys::window() {
                    let detail = js_sys::Object::new();
                    let _ = js_sys::Reflect::set(&detail, &"msg".into(), &msg.into());
                    let _ = js_sys::Reflect::set(&detail, &"kind".into(), &"err".into());
                    let init = web_sys::CustomEventInit::new();
                    init.set_detail(&detail);
                    if let Ok(evt) = web_sys::CustomEvent::new_with_event_init_dict(
                        "bb-toast",
                        &init,
                    ) {
                        let _ = window.dispatch_event(&evt);
                    }
                }
            }
        });
        listen("version-mismatch", &closure).await;
        closure.forget();
    });

    // Listen for PCA9535 fault events (e-fuse trips, power-good changes)
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<TauriEvent<Vec<u8>>>(event) {
                let payload = evt.payload;
                if payload.len() >= 6 {
                    let fault_type = payload[0];
                    let channel = payload[1];
                    let msg = match fault_type {
                        0 => format!("E-Fuse {} tripped — output disabled!", channel + 1),
                        1 => format!("E-Fuse {} fault cleared", channel + 1),
                        2 => {
                            let name = match channel { 0 => "Logic", 1 => "VADJ1", _ => "VADJ2" };
                            format!("{} power-good LOST!", name)
                        }
                        3 => {
                            let name = match channel { 0 => "Logic", 1 => "VADJ1", _ => "VADJ2" };
                            format!("{} power-good restored", name)
                        }
                        _ => format!("PCA fault type={} ch={}", fault_type, channel),
                    };
                    let kind = if fault_type == 0 || fault_type == 2 { "err" } else { "ok" };
                    show_toast(&msg, kind);
                }
            }
        });
        listen("pca-fault", &closure).await;
        closure.forget();
    });

    // Auto-scan
    spawn_local(async move {
        let result = invoke("discover_devices", JsValue::NULL).await;
        if let Ok(devs) = serde_wasm_bindgen::from_value::<Vec<DiscoveredDevice>>(result) {
            set_devices.set(devs);
        }
    });

    view! {
        <div class="app">
            // Header
            <header class="header">
                <div class="header-left">
                    <span class="logo-text">"BugBuster"</span>
                    <span class="subtitle">"AD74416H Controller"</span>
                </div>
                <div class="header-right">
                    {move || {
                        let m = conn_mode.get();
                        if m == "Disconnected" {
                            view! {
                                <div class="status-bar">
                                    <span class="status-dot disconnected"></span>
                                    <span class="status-text">"Disconnected"</span>
                                </div>
                            }.into_any()
                        } else {
                            let badge = if m == "Usb" { "USB" } else { "HTTP" };
                            view! {
                                <div class="status-bar">
                                    <span class="status-dot connected"></span>
                                    <span class="status-badge">{badge}</span>
                                    <span class="status-text">{move || conn_addr.get()}</span>
                                    <span class="status-separator">"|"</span>
                                    <span class={move || if device_state.get().spi_ok { "spi-ok" } else { "spi-err" }}>
                                        {move || if device_state.get().spi_ok { "SPI OK" } else { "SPI ERR" }}
                                    </span>
                                    <span class="status-separator">"|"</span>
                                    <span class="temp-value">
                                        {move || format!("{:.1} °C", device_state.get().die_temperature)}
                                    </span>
                                    <span class="status-separator">"|"</span>
                                    <button class="btn btn-ghost btn-xs" on:click=move |_| {
                                        spawn_local(async move {
                                            let result = invoke("pick_config_save_file", JsValue::NULL).await;
                                            if let Ok(Some(path)) = serde_wasm_bindgen::from_value::<Option<String>>(result) {
                                                if !path.is_empty() {
                                                    #[derive(serde::Serialize)]
                                                    struct Args { path: String }
                                                    let args = serde_wasm_bindgen::to_value(&Args { path }).unwrap();
                                                    let _ = invoke("export_config", args).await;
                                                }
                                            }
                                        });
                                    }>"Export"</button>
                                    <button class="btn btn-ghost btn-xs" on:click=move |_| {
                                        spawn_local(async move {
                                            let result = invoke("pick_config_open_file", JsValue::NULL).await;
                                            if let Ok(Some(path)) = serde_wasm_bindgen::from_value::<Option<String>>(result) {
                                                if !path.is_empty() {
                                                    #[derive(serde::Serialize)]
                                                    struct Args { path: String }
                                                    let args = serde_wasm_bindgen::to_value(&Args { path }).unwrap();
                                                    let _ = invoke("import_config", args).await;
                                                }
                                            }
                                        });
                                    }>"Import"</button>
                                    <span class="status-separator">"|"</span>
                                    <button class="btn btn-danger btn-xs" on:click=disconnect>"Disconnect"</button>
                                </div>
                            }.into_any()
                        }
                    }}
                </div>
            </header>

            // Connection panel (when disconnected)
            <Show when=move || conn_mode.get() == "Disconnected">
                <ParticleBackground />
                <div class="connection-panel">
                    <div class="card" style="max-width: 500px; width: 100%; text-align: center; z-index: 10; position: relative;">
                        <h2>"Connect to BugBuster"</h2>
                        <p class="hint">"Scanning for devices on USB and network..."</p>
                        <button class="btn btn-primary" on:click=scan disabled=move || scanning.get()>
                            {move || if scanning.get() { "Scanning..." } else { "Scan for Devices" }}
                        </button>
                        <div class="device-list">
                            <For
                                each=move || devices.get()
                                key=|dev| dev.id.clone()
                                children=move |dev: DiscoveredDevice| {
                                    let id = dev.id.clone();
                                    let icon = if dev.transport == "usb" { "🔌" } else { "📡" };
                                    let name = dev.name.clone();
                                    let addr = dev.address.clone();
                                    let tbadge = dev.transport.to_uppercase();
                                    view! {
                                        <button class="device-item" on:click=move |_| {
                                            do_connect(id.clone());
                                        }>
                                            <span class="device-icon">{icon}</span>
                                            <div class="device-info">
                                                <span class="device-name">{name}</span>
                                                <span class="device-addr">{addr}</span>
                                            </div>
                                            <span class="device-transport">{tbadge}</span>
                                        </button>
                                    }
                                }
                            />
                        </div>
                    </div>
                </div>
            </Show>

            // Main content (when connected)
            <Show when=move || conn_mode.get() != "Disconnected">
                // Tab bar
                <nav class="tab-bar">
                    {TABS.iter().map(|(id, label)| {
                        let id_str = id.to_string();
                        let id_click = id_str.clone();
                        view! {
                            <button class="tab-item"
                                class:active=move || active_tab.get() == id_str
                                on:click=move |_| set_active_tab.set(id_click.clone())
                            >{*label}</button>
                        }
                    }).collect::<Vec<_>>()}
                </nav>

                // Tab content
                <div class="tab-container">
                    {move || match active_tab.get().as_str() {
                        "overview" => view! { <OverviewTab state=device_state /> }.into_any(),
                        "adc" => view! { <AdcTab state=device_state /> }.into_any(),
                        "diag" => view! { <DiagTab state=device_state /> }.into_any(),
                        "vdac" => view! { <VdacTab state=device_state /> }.into_any(),
                        "idac" => view! { <IdacTab state=device_state /> }.into_any(),
                        "iin" => view! { <IinTab state=device_state /> }.into_any(),
                        "din" => view! { <DinTab state=device_state /> }.into_any(),
                        "dout" => view! { <DoutTab state=device_state /> }.into_any(),
                        "faults" => view! { <FaultsTab state=device_state /> }.into_any(),
                        "gpio" => view! { <GpioTab state=device_state /> }.into_any(),
                        "uart" => view! { <UartTab uart_config=uart_config /> }.into_any(),
                        "scope" => view! { <ScopeTab state=device_state /> }.into_any(),
                        "wavegen" => view! { <WavegenTab /> }.into_any(),
                        "sigpath" => view! { <SignalPathTab state=device_state /> }.into_any(),
                        "voltages" => view! { <VoltagesTab state=device_state /> }.into_any(),
                        "calibration" => view! { <CalibrationTab state=device_state /> }.into_any(),
                        "usbpd" => view! { <UsbPdTab state=device_state /> }.into_any(),
                        "ioexp" => view! { <IoExpTab state=device_state /> }.into_any(),
                        "hat" => view! { <HatTab state=device_state /> }.into_any(),
                        "la" => view! { <LaTab state=device_state /> }.into_any(),
                        _ => view! { <div>"Unknown tab"</div> }.into_any(),
                    }}
                </div>
            </Show>

            // Toast notifications
            <div class="toast-container">
                {move || toasts.get().into_iter().map(|(msg, kind, _ts)| {
                    let class = match kind.as_str() {
                        "ok" => "toast toast-ok",
                        "err" => "toast toast-err",
                        _ => "toast toast-info",
                    };
                    view! { <div class=class>{msg}</div> }
                }).collect::<Vec<_>>()}
            </div>
        </div>
    }
}

/// Floating particle network background for the connection screen.
#[component]
fn ParticleBackground() -> impl IntoView {
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    spawn_local(async move {
        use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
        use wasm_bindgen::JsCast;

        // Wait for canvas to mount
        slp(50).await;

        let Some(el) = canvas_ref.get() else { return };
        let canvas: HtmlCanvasElement = el.into();
        let ctx: CanvasRenderingContext2d = canvas
            .get_context("2d").unwrap().unwrap()
            .dyn_into().unwrap();

        let window = web_sys::window().unwrap();

        // Particle state
        const NUM: usize = 80;
        const CONNECT_DIST: f64 = 140.0;
        const SPEED: f64 = 0.3;

        struct P { x: f64, y: f64, vx: f64, vy: f64, r: f64 }

        let mut particles: Vec<P> = Vec::with_capacity(NUM);
        // Seed with pseudo-random using simple LCG
        let mut seed: u64 = 42;
        let mut rng = || -> f64 {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
            ((seed >> 33) as f64) / (u32::MAX as f64)
        };

        let w0 = window.inner_width().unwrap().as_f64().unwrap();
        let h0 = window.inner_height().unwrap().as_f64().unwrap();
        for _ in 0..NUM {
            particles.push(P {
                x: rng() * w0,
                y: rng() * h0,
                vx: (rng() - 0.5) * SPEED * 2.0,
                vy: (rng() - 0.5) * SPEED * 2.0,
                r: 1.2 + rng() * 1.8,
            });
        }

        loop {
            slp(16).await; // ~60fps

            let dp = window.device_pixel_ratio();
            let w = window.inner_width().unwrap().as_f64().unwrap();
            let h = window.inner_height().unwrap().as_f64().unwrap();
            canvas.set_width((w * dp) as u32);
            canvas.set_height((h * dp) as u32);
            let _ = ctx.scale(dp, dp);

            // Clear
            ctx.set_fill_style_str("rgba(6,10,20,0.85)");
            ctx.fill_rect(0.0, 0.0, w, h);

            // Update positions
            for p in particles.iter_mut() {
                p.x += p.vx;
                p.y += p.vy;
                if p.x < 0.0 { p.x = w; }
                if p.x > w { p.x = 0.0; }
                if p.y < 0.0 { p.y = h; }
                if p.y > h { p.y = 0.0; }
            }

            // Draw connections
            for i in 0..particles.len() {
                for j in (i + 1)..particles.len() {
                    let dx = particles[i].x - particles[j].x;
                    let dy = particles[i].y - particles[j].y;
                    let dist = (dx * dx + dy * dy).sqrt();
                    if dist < CONNECT_DIST {
                        let alpha = (1.0 - dist / CONNECT_DIST) * 0.35;
                        ctx.set_stroke_style_str(&format!("rgba(59,130,246,{:.3})", alpha));
                        ctx.set_line_width(0.6);
                        ctx.begin_path();
                        ctx.move_to(particles[i].x, particles[i].y);
                        ctx.line_to(particles[j].x, particles[j].y);
                        ctx.stroke();
                    }
                }
            }

            // Draw particles
            for p in &particles {
                // Glow
                ctx.set_fill_style_str("rgba(59,130,246,0.15)");
                ctx.begin_path();
                let _ = ctx.arc(p.x, p.y, p.r * 3.0, 0.0, std::f64::consts::TAU);
                ctx.fill();
                // Core
                ctx.set_fill_style_str("rgba(139,170,220,0.6)");
                ctx.begin_path();
                let _ = ctx.arc(p.x, p.y, p.r, 0.0, std::f64::consts::TAU);
                ctx.fill();
            }

            // Reset transform for next frame
            ctx.set_transform(1.0, 0.0, 0.0, 1.0, 0.0, 0.0).ok();
        }
    });

    view! {
        <canvas node_ref=canvas_ref
            style="position: fixed; inset: 0; width: 100vw; height: 100vh; z-index: 0; pointer-events: none;"
        />
    }
}

fn do_connect(device_id: String) {
    use serde::Serialize;
    #[derive(Serialize)]
    struct Args { #[serde(rename = "deviceId")] device_id: String }
    spawn_local(async move {
        log(&format!("Connecting to: {}", device_id));
        let args = serde_wasm_bindgen::to_value(&Args { device_id }).unwrap();
        let _result = invoke("connect_device", args).await;
    });
}

async fn slp(ms: u32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        web_sys::window().unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms as i32).unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(p).await.unwrap();
}
