//! Shared parts of the per-channel instrument cards (ADC / VDAC / IDAC / IIN /
//! DIN / DOUT / HV IO): the rolling sparkline, the card header, the compact
//! "wrong function" body and the channel-function setter.
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::JsCast;
use web_sys::{CanvasRenderingContext2d, HtmlCanvasElement};

use crate::components::icons::Icon;
use crate::tauri_bridge::{func_name, invoke_with_feedback, CH_NAMES};

/// CSS custom property carrying the identity colour of channel `i`.
pub fn ch_var(i: usize) -> &'static str {
    ["--ch-a", "--ch-b", "--ch-c", "--ch-d"][i.min(3)]
}

/// Value for the `data-ch` attribute on a channel card (see views/io.css).
pub fn ch_key(i: usize) -> &'static str {
    ["a", "b", "c", "d"][i.min(3)]
}

/// Switch a channel to a different AD74416H function.
pub fn set_channel_function(ch: u8, func: u8) {
    #[derive(Serialize)]
    struct Args {
        channel: u8,
        function: u8,
    }
    let args = serde_wasm_bindgen::to_value(&Args {
        channel: ch,
        function: func,
    })
    .unwrap();
    let label = format!("Set CH {} to {}", CH_NAMES[ch as usize], func_name(func));
    invoke_with_feedback("set_channel_function", args, &label);
}

/// Card header: channel dot + "Channel A", function chip, optional extra controls.
#[component]
pub fn ChannelHead(
    idx: usize,
    #[prop(into)] func: String,
    #[prop(optional)] children: Option<Children>,
) -> impl IntoView {
    view! {
        <div class="group-header io-head">
            <div class="group-title">
                <span class="io-dot" aria-hidden="true"></span>
                {format!("Channel {}", CH_NAMES[idx.min(3)])}
            </div>
            <div class="group-actions">
                <span class="chip">{func}</span>
                {children.map(|c| c())}
            </div>
        </div>
    }
}

/// Compact in-card body for a channel whose function makes the view inapplicable.
#[component]
pub fn ChannelEmpty(
    icon: &'static str,
    message: &'static str,
    #[prop(optional)] children: Option<Children>,
) -> impl IntoView {
    view! {
        <div class="io-empty">
            <Icon name=icon size=24 />
            <p>{message}</p>
            {children.map(|c| view! { <div class="io-empty-actions">{c()}</div> })}
        </div>
    }
}

/// Rolling polyline of recent channel values.
///
/// - `values`: recent samples (oldest -> newest)
/// - `min` / `max`: vertical range for normalization
/// - `color_var`: CSS custom property name for the stroke, e.g. `"--ch-a"`
///
/// Colours are read from CSS at draw time and the plot repaints when the theme flips.
#[component]
pub fn ChannelSparkline(
    #[prop(into)] values: Signal<Vec<f32>>,
    #[prop(into)] min: Signal<f32>,
    #[prop(into)] max: Signal<f32>,
    #[prop(into)] color_var: String,
) -> impl IntoView {
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();
    let color_var = StoredValue::new(color_var);
    let theme = crate::theme::use_theme().resolved;

    Effect::new(move |_| {
        let vs = values.get();
        let vmin = min.get();
        let vmax = max.get();
        let _ = theme.get();
        let var = color_var.get_value();

        // Defer to next tick so the canvas is mounted and sized.
        spawn_local(async move {
            let Some(canvas_el) = canvas_ref.get_untracked() else {
                return;
            };
            let canvas: HtmlCanvasElement = canvas_el;

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

            let Some(ctx_obj) = canvas.get_context("2d").ok().flatten() else {
                return;
            };
            let ctx: CanvasRenderingContext2d = ctx_obj.unchecked_into();
            ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0).ok();

            ctx.set_fill_style_str(&crate::theme::css_var("--surface-plot"));
            ctx.fill_rect(0.0, 0.0, w, h);

            let grid = crate::theme::css_var("--grid-line");
            ctx.set_stroke_style_str(&grid);
            ctx.set_line_width(1.0);
            for k in 1..4 {
                let y = h * (k as f64) / 4.0;
                ctx.begin_path();
                ctx.move_to(0.0, y);
                ctx.line_to(w, y);
                ctx.stroke();
            }

            if vs.len() < 2 {
                return;
            }

            // Zero line if within range
            let span = (vmax - vmin).max(1e-6);
            if vmin < 0.0 && vmax > 0.0 {
                let y0 = h - (((-vmin) / span) as f64) * h;
                ctx.set_stroke_style_str(&crate::theme::css_var("--label-3"));
                ctx.set_line_width(0.5);
                ctx.begin_path();
                ctx.move_to(0.0, y0);
                ctx.line_to(w, y0);
                ctx.stroke();
            }

            ctx.set_stroke_style_str(&crate::theme::css_var(&var));
            ctx.set_line_width(1.5);
            ctx.set_line_join("round");
            ctx.begin_path();
            let n = vs.len();
            let step = w / ((n - 1).max(1) as f64);
            for (i, v) in vs.iter().enumerate() {
                let x = i as f64 * step;
                let norm = ((*v - vmin) / span).clamp(0.0, 1.0) as f64;
                let y = h - norm * h;
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
        <canvas node_ref=canvas_ref class="io-spark" role="img" aria-label="Recent values"></canvas>
    }
}
