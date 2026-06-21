use crate::tauri_bridge::{log, show_toast, try_invoke, DiscoveredDevice};
use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::prelude::*;

/// Floating particle network background for the connection screen.
#[component]
fn ParticleBackground() -> impl IntoView {
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();

    spawn_local(async move {
        use wasm_bindgen::JsCast;
        use web_sys::{CanvasRenderingContext2d, HtmlCanvasElement};

        // Wait for canvas to mount
        slp(50).await;

        let Some(el) = canvas_ref.get() else { return };
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
        const NUM: usize = 80;
        const CONNECT_DIST: f64 = 140.0;
        const SPEED: f64 = 0.3;

        struct P {
            x: f64,
            y: f64,
            vx: f64,
            vy: f64,
            r: f64,
        }

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
            // Check if the canvas has been unmounted to prevent leaks
            if !canvas.is_connected() {
                break;
            }

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
                    <h1 class="logo-title">"BugBuster"</h1>
                    <p class="subtitle-desc">"CMSIS-DAP Probe & Debug Suite"</p>
                </div>

                <div class="card connection-card">
                    {move || if !scan_completed.get() {
                        view! {
                            <div class="scanning-loader-wrap">
                                <div class="scanning-glow-ring"></div>
                                <p class="scanning-status">"Initializing..."</p>
                            </div>
                        }.into_any()
                    } else {
                        view! {
                            <button class="btn btn-primary btn-scan" on:click=move |e| on_scan.run(e) disabled=move || scanning.get()>
                                {move || if scanning.get() { "Scanning..." } else { "Scan for Devices" }}
                            </button>

                            <div class="device-list-container">
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
                                            let id_click = id.clone();
                                            view! {
                                                <button class="device-item" on:click=move |_| {
                                                    connect(id_click.clone());
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
                                    // Synthetic device — runs the app with no hardware.
                                    <button class="device-item" on:click=move |e| on_mock.run(e)>
                                        <span class="device-icon">"🧪"</span>
                                        <div class="device-info">
                                            <span class="device-name">"Demo / Mock device (DAQ)"</span>
                                            <span class="device-addr">"Synthetic power-analyzer stream"</span>
                                        </div>
                                        <span class="device-transport">"DEMO"</span>
                                    </button>
                                </div>
                            </div>
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
