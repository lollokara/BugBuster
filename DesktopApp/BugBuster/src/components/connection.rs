use crate::components::icons::Icon;
use crate::tauri_bridge::{log, show_toast, try_invoke, DiscoveredDevice};
use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::prelude::*;

/// Reads a CSS custom property from :root (theme-aware canvas colours).
fn css_var(name: &str, fallback: &str) -> String {
    web_sys::window()
        .and_then(|w| {
            let root = w.document()?.document_element()?;
            let cs = w.get_computed_style(&root).ok().flatten()?;
            let v = cs.get_property_value(name).ok()?;
            let v = v.trim().to_string();
            (!v.is_empty()).then_some(v)
        })
        .unwrap_or_else(|| fallback.to_string())
}

/// Quiet particle network behind the welcome screen; colours follow the theme tokens.
#[component]
fn ParticleBackground() -> impl IntoView {
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    spawn_local(async move {
        use wasm_bindgen::JsCast;
        use web_sys::{CanvasRenderingContext2d, HtmlCanvasElement};

        // Wait for canvas to mount
        slp(50).await;

        let Some(el) = canvas_ref.get_untracked() else { return };
        let canvas: HtmlCanvasElement = el;
        let ctx: CanvasRenderingContext2d = match canvas
            .get_context("2d")
            .ok()
            .flatten()
            .and_then(|o| o.dyn_into().ok())
        {
            Some(c) => c,
            None => {
                web_sys::console::warn_1(
                    &"ParticleBackground: failed to get 2D canvas context".into(),
                );
                return;
            }
        };

        let window = match web_sys::window() {
            Some(w) => w,
            None => {
                web_sys::console::warn_1(&"ParticleBackground: window unavailable".into());
                return;
            }
        };

        // Particle state
        const NUM: usize = 56;
        const CONNECT_DIST: f64 = 140.0;
        const SPEED: f64 = 0.25;

        struct P {
            x: f64,
            y: f64,
            vx: f64,
            vy: f64,
            r: f64,
        }

        let reduced_motion = window
            .match_media("(prefers-reduced-motion: reduce)")
            .ok()
            .flatten()
            .map(|m| m.matches())
            .unwrap_or(false);

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
                r: 1.0 + rng() * 1.4,
            });
        }

        let mut frame: u32 = 0;
        let mut line_col = css_var("--label-4", "#b4b4ba");
        let mut dot_col = css_var("--label-3", "#8a8a90");

        loop {
            // Check if the canvas has been unmounted to prevent leaks
            if !canvas.is_connected() {
                break;
            }

            slp(if reduced_motion { 250 } else { 16 }).await; // ~60fps

            // Pick up theme switches without querying styles every frame.
            frame = frame.wrapping_add(1);
            if frame % 30 == 0 {
                line_col = css_var("--label-4", "#b4b4ba");
                dot_col = css_var("--label-3", "#8a8a90");
            }

            let dp = window.device_pixel_ratio();
            let w = window.inner_width().unwrap().as_f64().unwrap();
            let h = window.inner_height().unwrap().as_f64().unwrap();
            canvas.set_width((w * dp) as u32);
            canvas.set_height((h * dp) as u32);
            let _ = ctx.scale(dp, dp);

            ctx.clear_rect(0.0, 0.0, w, h);

            // Update positions
            if !reduced_motion {
                for p in particles.iter_mut() {
                    p.x += p.vx;
                    p.y += p.vy;
                    if p.x < 0.0 {
                        p.x = w;
                    }
                    if p.x > w {
                        p.x = 0.0;
                    }
                    if p.y < 0.0 {
                        p.y = h;
                    }
                    if p.y > h {
                        p.y = 0.0;
                    }
                }
            }

            // Draw connections
            ctx.set_stroke_style_str(&line_col);
            ctx.set_line_width(0.6);
            for i in 0..particles.len() {
                for j in (i + 1)..particles.len() {
                    let dx = particles[i].x - particles[j].x;
                    let dy = particles[i].y - particles[j].y;
                    let dist = (dx * dx + dy * dy).sqrt();
                    if dist < CONNECT_DIST {
                        ctx.set_global_alpha((1.0 - dist / CONNECT_DIST) * 0.5);
                        ctx.begin_path();
                        ctx.move_to(particles[i].x, particles[i].y);
                        ctx.line_to(particles[j].x, particles[j].y);
                        ctx.stroke();
                    }
                }
            }

            // Draw particles
            ctx.set_global_alpha(0.45);
            ctx.set_fill_style_str(&dot_col);
            for p in &particles {
                ctx.begin_path();
                let _ = ctx.arc(p.x, p.y, p.r, 0.0, std::f64::consts::TAU);
                ctx.fill();
            }
            ctx.set_global_alpha(1.0);

            // Reset transform for next frame
            ctx.set_transform(1.0, 0.0, 0.0, 1.0, 0.0, 0.0).ok();
        }
    });

    view! {
        <canvas node_ref=canvas_ref class="wl-particles" aria-hidden="true"
            style="position: fixed; inset: 0; width: 100vw; height: 100vh; z-index: 0; pointer-events: none;"
        />
    }
}

#[component]
pub fn ConnectionPanel(
    devices: Signal<Vec<DiscoveredDevice>>,
    scanning: Signal<bool>,
    scan_completed: Signal<bool>,
    on_scan: Callback<ev::MouseEvent>,
    on_mock: Callback<ev::MouseEvent>,
) -> impl IntoView {
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

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
                slp(100).await;
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

    let connect = move |device_id: String| {
        use serde::Serialize;
        #[derive(Serialize)]
        struct Args {
            #[serde(rename = "deviceId")]
            device_id: String,
        }
        spawn_local(async move {
            log(&format!("Connecting to: {}", device_id));
            let args = serde_wasm_bindgen::to_value(&Args { device_id }).unwrap();
            if try_invoke("connect_device", args).await.is_none() {
                show_toast("Connection failed — check logs for details", "err");
            }
        });
    };

    view! {
        <div class="connection-layout"
            class:split=move || scan_completed.get()
            class:centered=move || !scan_completed.get()
        >
            // 2D Particle Background at z-index 0
            <ParticleBackground />

            // 3D Canvas at z-index 1
            <canvas id="board-canvas" node_ref=canvas_ref />

            // UI controls at z-index 2
            <div class="connection-ui-side fade-in-center">
                <div class="connection-header-group">
                    <div class="wl-mark" aria-hidden="true"><Icon name="bug" size=30 /></div>
                    <h1 class="logo-title">"BugBuster"</h1>
                    <p class="subtitle-desc">"CMSIS-DAP Probe & Debug Suite"</p>
                </div>

                <div class="card connection-card">
                    {move || if !scan_completed.get() {
                        view! {
                            <div class="scanning-loader-wrap" role="status">
                                <div class="spinner" aria-hidden="true"></div>
                                <p class="scanning-status">"Initializing..."</p>
                            </div>
                        }.into_any()
                    } else {
                        view! {
                            <div class="wl-toolbar">
                                <span class="wl-toolbar-title">"Devices"</span>
                                <span class="wl-toolbar-count">
                                    {move || {
                                        let n = devices.get().len();
                                        if scanning.get() { "Scanning…".to_string() }
                                        else if n == 1 { "1 found".to_string() }
                                        else { format!("{n} found") }
                                    }}
                                </span>
                            </div>

                            <div class="device-list-container">
                                <div class="device-list">
                                    <For
                                        each=move || devices.get()
                                        key=|dev| dev.id.clone()
                                        children=move |dev: DiscoveredDevice| {
                                            let id = dev.id.clone();
                                            let is_usb = dev.transport == "usb";
                                            let name = dev.name.clone();
                                            let addr = dev.address.clone();
                                            let tbadge = dev.transport.to_uppercase();
                                            let id_click = id.clone();
                                            let aria = format!("Connect to {} ({})", dev.name, dev.address);
                                            view! {
                                                <button class="device-item" aria-label=aria on:click=move |_| {
                                                    connect(id_click.clone());
                                                }>
                                                    <span class="device-icon">
                                                        <Icon name=if is_usb { "usb" } else { "wifi" } size=18 />
                                                    </span>
                                                    <div class="device-info">
                                                        <span class="device-name">{name}</span>
                                                        <span class="device-addr">{addr}</span>
                                                    </div>
                                                    <span class="device-transport">{tbadge}</span>
                                                    <span class="device-connect">"Connect"<Icon name="chevron-right" size=13 /></span>
                                                </button>
                                            }
                                        }
                                    />
                                    <Show when=move || devices.get().is_empty()>
                                        <div class="wl-empty">
                                            {move || if scanning.get() {
                                                "Looking for BugBuster devices…"
                                            } else {
                                                "No devices found. Plug in a BugBuster over USB or join its network, then scan again."
                                            }}
                                        </div>
                                    </Show>
                                    // Synthetic device — runs the app with no hardware.
                                    <div class="wl-section">"No hardware?"</div>
                                    <button class="device-item" aria-label="Connect to the demo device" on:click=move |e| on_mock.run(e)>
                                        <span class="device-icon"><Icon name="activity" size=18 /></span>
                                        <div class="device-info">
                                            <span class="device-name">"Demo / Mock device (DAQ)"</span>
                                            <span class="device-addr">"Synthetic power-analyzer stream"</span>
                                        </div>
                                        <span class="device-transport">"DEMO"</span>
                                        <span class="device-connect">"Connect"<Icon name="chevron-right" size=13 /></span>
                                    </button>
                                </div>
                            </div>

                            <button class="btn btn-primary btn-scan"
                                class:wl-quiet=move || !devices.get().is_empty()
                                class:is-scanning=move || scanning.get()
                                on:click=move |e| on_scan.run(e) disabled=move || scanning.get()>
                                <Icon name="refresh-cw" size=14 />
                                {move || if scanning.get() { "Scanning..." } else { "Scan for Devices" }}
                            </button>
                            <p class="hint">"USB and Wi-Fi (mDNS) devices are discovered automatically."</p>
                        }.into_any()
                    }}
                </div>
            </div>
        </div>
    }
}

async fn slp(ms: u32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        if let Some(w) = web_sys::window() {
            w.set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms as i32)
                .ok();
        } else {
            web_sys::console::warn_1(&"slp: window unavailable, timeout will not fire".into());
        }
    });
    wasm_bindgen_futures::JsFuture::from(p).await.ok();
}
