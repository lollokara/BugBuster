//! Channel sparkline — renders a rolling polyline of recent channel values.
//!
//! Used inside per-channel tiles (ADC / VDAC / IDAC / IIN / HV-Digital) to show
//! a short time-series trace under the big numeric readout.
use leptos::prelude::*;
use leptos::task::spawn_local;
use wasm_bindgen::JsCast;
use web_sys::{CanvasRenderingContext2d, HtmlCanvasElement};

/// Props:
/// - `values`: signal producing a vector of recent samples (oldest -> newest)
/// - `min` / `max`: vertical range for normalization
/// - `color`: stroke color (hex, e.g. "#3b82f6")
#[component]
pub fn ChannelSparkline(
    #[prop(into)] values: Signal<Vec<f32>>,
    #[prop(into)] min: Signal<f32>,
    #[prop(into)] max: Signal<f32>,
    #[prop(into)] color: String,
) -> impl IntoView {
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();
    let color = StoredValue::new(color);

    // Re-render on values change using an Effect.
    Effect::new(move |_| {
        let vs = values.get();
        let vmin = min.get();
        let vmax = max.get();
        let col = color.get_value();

        // Defer to next tick so the canvas is mounted and sized.
        spawn_local(async move {
            let Some(canvas_el) = canvas_ref.get() else { return };
            let canvas: HtmlCanvasElement = canvas_el.into();

            let dpr = web_sys::window()
                .map(|w| w.device_pixel_ratio())
                .unwrap_or(1.0);
            let rect = canvas.get_bounding_client_rect();
            let w = rect.width();
            let h = rect.height();
            if w <= 1.0 || h <= 1.0 {
                return;
            }
            let cw = (w * dpr) as u32;
            let ch = (h * dpr) as u32;
            if canvas.width() != cw {
                canvas.set_width(cw);
            }
            if canvas.height() != ch {
                canvas.set_height(ch);
            }

            let Some(ctx_obj) = canvas.get_context("2d").ok().flatten() else { return };
            let ctx: CanvasRenderingContext2d = ctx_obj.unchecked_into();
            ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();

            // Clear
            ctx.set_fill_style_str("rgba(6,10,20,0.6)");
            ctx.fill_rect(0.0, 0.0, w, h);

            if vs.len() < 2 {
                return;
            }

            // Baseline (zero line if within range)
            let span = (vmax - vmin).max(1e-6);
            if vmin < 0.0 && vmax > 0.0 {
                let y0 = h as f64 - (((-vmin) / span) as f64) * h as f64;
                ctx.set_stroke_style_str("rgba(148,163,184,0.15)");
                ctx.set_line_width(0.5);
                ctx.begin_path();
                ctx.move_to(0.0, y0);
                ctx.line_to(w, y0);
                ctx.stroke();
            }

            // Polyline
            ctx.set_stroke_style_str(&col);
            ctx.set_line_width(1.2);
            ctx.begin_path();
            let n = vs.len();
            let step = (w as f64) / ((n - 1).max(1) as f64);
            for (i, v) in vs.iter().enumerate() {
                let x = i as f64 * step;
                let norm = ((*v - vmin) / span).clamp(0.0, 1.0) as f64;
                let y = h as f64 - norm * h as f64;
                if i == 0 {
                    ctx.move_to(x, y);
                } else {
                    ctx.line_to(x, y);
                }
            }
            ctx.stroke();
        });
    });

    view! {
        <canvas node_ref=canvas_ref class="channel-sparkline"></canvas>
    }
}
