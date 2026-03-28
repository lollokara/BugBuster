use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

const MUX_NAMES: [&str; 4] = ["MUX 1 (U10)", "MUX 2 (U11)", "MUX 3 (U16)", "MUX 4 (U17)"];
const CH_LABELS: [&str; 4] = ["CH A", "CH B", "CH C", "CH D"];
const PORT_LABELS: [&str; 4] = ["P1", "P2", "P3", "P4"];
const SW_LABELS: [&str; 8] = ["S1 GPIO", "S2 GPIO~", "S3 ADC", "S4 EXT", "S5 GPIO", "S6 GPIO~", "S7 GPIO", "S8 GPIO~"];
const GROUP_LABELS: [&str; 3] = ["Main Bus", "Aux 1", "Aux 2"];

const TRACE_ACTIVE: &str = "#10b981";
const TRACE_INACTIVE: &str = "#1e293b";
const BG_PCB: &str = "#0a1628";
const CHIP_BG: &str = "#162035";
const CHIP_BORDER: &str = "#2d4a6f";
const TEXT_DIM: &str = "#64748b";
const TEXT_BRIGHT: &str = "#e2e8f0";

const PRESETS: &[(&str, [u8; 4])] = &[
    ("All Open", [0x00, 0x00, 0x00, 0x00]),
    ("Direct GPIO", [0x51, 0x51, 0x51, 0x51]),       // SW1+SW5+SW7 on all
    ("ADC Measurement", [0x04, 0x04, 0x04, 0x04]),    // SW3 on all (AD74416H)
    ("External Interface", [0x08, 0x08, 0x08, 0x08]), // SW4 on all
    ("GPIO + ADC", [0x05, 0x05, 0x05, 0x05]),         // SW1+SW3 (loopback)
];

#[component]
pub fn SignalPathTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (mux_states, set_mux_states) = signal([0u8; 4]);
    let canvas_ref = NodeRef::<leptos::html::Canvas>::new();
    let (hovered_sw, set_hovered_sw) = signal(Option::<(usize, usize)>::None);

    // Sync mux states from device state
    Effect::new(move || {
        let ds = state.get();
        if ds.mux_states.len() >= 4 {
            let mut arr = [0u8; 4];
            arr.copy_from_slice(&ds.mux_states[..4]);
            set_mux_states.set(arr);
        }
    });

    // Toggle a switch
    let toggle_switch = move |dev: usize, sw: usize| {
        let mut states = mux_states.get_untracked();
        let is_on = (states[dev] >> sw) & 1 != 0;
        #[derive(Serialize)]
        struct Args { device: u8, switch_num: u8, state: bool }
        let args = serde_wasm_bindgen::to_value(&Args {
            device: dev as u8, switch_num: sw as u8, state: !is_on
        }).unwrap();
        invoke_void("mux_set_switch", args);
        // Optimistic update
        if is_on { states[dev] &= !(1 << sw); } else { states[dev] |= 1 << sw; }
        set_mux_states.set(states);
    };

    // Apply preset
    let apply_preset = move |states: [u8; 4]| {
        let args = serde_wasm_bindgen::to_value(&serde_json::json!({
            "states": states.to_vec()
        })).unwrap();
        invoke_void("mux_set_all", args);
        set_mux_states.set(states);
    };

    // Render loop
    let render = move || {
        let Some(canvas) = canvas_ref.get() else { return };
        let canvas: HtmlCanvasElement = canvas.into();
        let dpr = web_sys::window().unwrap().device_pixel_ratio();
        let rect = canvas.get_bounding_client_rect();
        let w = rect.width();
        let h = rect.height();
        if w < 100.0 || h < 100.0 { return; }
        canvas.set_width((w * dpr) as u32);
        canvas.set_height((h * dpr) as u32);

        let ctx: CanvasRenderingContext2d = canvas
            .get_context("2d").unwrap().unwrap()
            .dyn_into().unwrap();
        ctx.scale(dpr, dpr).unwrap();

        let states = mux_states.get();

        // Background
        ctx.set_fill_style_str(BG_PCB);
        ctx.fill_rect(0.0, 0.0, w, h);

        // Layout: 4 mux rows
        let row_h = (h - 40.0) / 4.0;
        let margin = 20.0;

        for dev in 0..4 {
            let y_base = margin + dev as f64 * row_h;
            let state = states[dev];

            draw_mux_row(&ctx, w, y_base, row_h - 8.0, dev, state);
        }
    };

    // Animation frame
    spawn_local(async move {
        loop {
            sleep_ms(50).await;
            render();
        }
    });

    // Handle clicks on canvas
    let on_click = move |e: leptos::ev::MouseEvent| {
        let Some(canvas) = canvas_ref.get() else { return };
        let canvas: HtmlCanvasElement = canvas.clone().into();
        let rect = canvas.get_bounding_client_rect();
        let x = e.client_x() as f64 - rect.left();
        let y = e.client_y() as f64 - rect.top();
        let w = rect.width();
        let h = rect.height();
        let row_h = (h - 40.0) / 4.0;

        // Determine which device row was clicked
        let dev = ((y - 20.0) / row_h).floor() as usize;
        if dev >= 4 { return; }

        // Determine which switch was clicked
        // Switches are in the MUX chip area (center of row)
        let chip_x = w * 0.35;
        let chip_w = w * 0.2;
        let sw_h = (row_h - 16.0) / 8.0;
        let y_in_row = y - 20.0 - dev as f64 * row_h;
        let sw = ((y_in_row - 4.0) / sw_h).floor() as usize;
        if sw >= 8 { return; }

        if x >= chip_x && x <= chip_x + chip_w {
            toggle_switch(dev, sw);
        }
    };

    view! {
        <div class="tab-content signal-path-tab">
            <div class="sp-toolbar">
                <span class="sp-title">"Signal Path Configuration"</span>
                <div class="sp-presets">
                    {PRESETS.iter().map(|(name, states)| {
                        let s = *states;
                        view! {
                            <button class="scope-btn" on:click=move |_| apply_preset(s)>{*name}</button>
                        }
                    }).collect::<Vec<_>>()}
                </div>
            </div>

            <div class="sp-canvas-wrap">
                <canvas node_ref=canvas_ref class="sp-canvas" on:click=on_click></canvas>
            </div>

            // Switch state summary below canvas
            <div class="sp-summary">
                {move || {
                    let states = mux_states.get();
                    (0..4).map(|dev| {
                        view! {
                            <div class="sp-dev-summary">
                                <span class="sp-dev-label">{MUX_NAMES[dev]}</span>
                                <div class="sp-sw-row">
                                    {(0..8).map(|sw| {
                                        let is_on = (states[dev] >> sw) & 1 != 0;
                                        view! {
                                            <button
                                                class="sp-sw-btn"
                                                class:sp-sw-on=is_on
                                                on:click=move |_| toggle_switch(dev, sw)
                                            >
                                                {format!("S{}", sw + 1)}
                                            </button>
                                        }
                                    }).collect::<Vec<_>>()}
                                </div>
                            </div>
                        }
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}

fn draw_mux_row(ctx: &CanvasRenderingContext2d, w: f64, y: f64, h: f64, dev: usize, state: u8) {
    let margin = 20.0;

    // Column positions
    let input_x = margin;
    let lshift_x = w * 0.18;
    let chip_x = w * 0.35;
    let chip_w = w * 0.2;
    let output_x = w * 0.65;
    let port_x = w * 0.82;

    // Draw input labels (ESP + ADC + EXT)
    ctx.set_font("11px monospace");
    ctx.set_text_align("right");

    let sw_h = h / 8.0;
    let input_labels = [
        ("ESP GPIO", "direct"),
        ("ESP GPIO~", "2kΩ"),
        (CH_LABELS[dev], "AD74416H"),
        ("EXT J4", "connector"),
        ("ESP GPIO", "direct"),
        ("ESP GPIO~", "2kΩ"),
        ("ESP GPIO", "direct"),
        ("ESP GPIO~", "2kΩ"),
    ];

    for sw in 0..8 {
        let sy = y + sw as f64 * sw_h + sw_h / 2.0;
        let is_on = (state >> sw) & 1 != 0;
        let trace_color = if is_on { TRACE_ACTIVE } else { TRACE_INACTIVE };

        // Input label
        ctx.set_fill_style_str(if is_on { TEXT_BRIGHT } else { TEXT_DIM });
        let _ = ctx.fill_text(input_labels[sw].0, lshift_x - 8.0, sy + 3.0);

        // Trace: input → level shifter → chip
        ctx.set_stroke_style_str(trace_color);
        ctx.set_line_width(if is_on { 2.0 } else { 1.0 });
        ctx.begin_path();
        ctx.move_to(lshift_x, sy);
        ctx.line_to(chip_x, sy);
        ctx.stroke();

        if is_on {
            // Glow effect
            ctx.set_stroke_style_str(&format!("{}44", TRACE_ACTIVE));
            ctx.set_line_width(6.0);
            ctx.begin_path();
            ctx.move_to(lshift_x, sy);
            ctx.line_to(chip_x, sy);
            ctx.stroke();
        }

        // Switch indicator (dot)
        let dot_x = chip_x + chip_w / 2.0;
        ctx.set_fill_style_str(if is_on { TRACE_ACTIVE } else { "#1e293b" });
        ctx.begin_path();
        ctx.arc(dot_x, sy, 4.0, 0.0, std::f64::consts::TAU).unwrap();
        ctx.fill();
        if is_on {
            ctx.set_fill_style_str(&format!("{}66", TRACE_ACTIVE));
            ctx.begin_path();
            ctx.arc(dot_x, sy, 8.0, 0.0, std::f64::consts::TAU).unwrap();
            ctx.fill();
        }

        // Switch label inside chip
        ctx.set_fill_style_str(if is_on { TEXT_BRIGHT } else { TEXT_DIM });
        ctx.set_font("9px monospace");
        ctx.set_text_align("left");
        let _ = ctx.fill_text(&format!("S{}", sw + 1), chip_x + 4.0, sy + 3.0);
    }

    // Draw output traces (grouped)
    let groups = [(0..4, 0), (4..6, 1), (6..8, 2)];
    for (range, group_idx) in &groups {
        let group_y = y + (range.start as f64 + range.end as f64) / 2.0 * sw_h;
        let any_on = (range.start..range.end).any(|sw| (state >> sw) & 1 != 0);
        let trace_color = if any_on { TRACE_ACTIVE } else { TRACE_INACTIVE };

        // Output trace
        ctx.set_stroke_style_str(trace_color);
        ctx.set_line_width(if any_on { 2.5 } else { 1.0 });
        ctx.begin_path();
        ctx.move_to(chip_x + chip_w, group_y);
        ctx.line_to(port_x, group_y);
        ctx.stroke();

        if any_on {
            ctx.set_stroke_style_str(&format!("{}44", TRACE_ACTIVE));
            ctx.set_line_width(8.0);
            ctx.begin_path();
            ctx.move_to(chip_x + chip_w, group_y);
            ctx.line_to(port_x, group_y);
            ctx.stroke();
        }

        // Output label
        ctx.set_fill_style_str(if any_on { TEXT_BRIGHT } else { TEXT_DIM });
        ctx.set_font("10px monospace");
        ctx.set_text_align("left");
        let _ = ctx.fill_text(&format!("{} {}", PORT_LABELS[dev], GROUP_LABELS[*group_idx]),
                              port_x + 8.0, group_y + 3.0);
    }

    // Draw MUX chip outline
    ctx.set_stroke_style_str(CHIP_BORDER);
    ctx.set_line_width(1.5);
    ctx.set_fill_style_str(&format!("{}88", CHIP_BG));
    ctx.fill_rect(chip_x, y, chip_w, h);
    ctx.stroke_rect(chip_x, y, chip_w, h);

    // Chip label
    ctx.set_fill_style_str(TEXT_DIM);
    ctx.set_font("10px monospace");
    ctx.set_text_align("center");
    let _ = ctx.fill_text(MUX_NAMES[dev], chip_x + chip_w / 2.0, y - 4.0);

    // Level shifter box
    let ls_w = 30.0;
    ctx.set_stroke_style_str("#2d4a6f55");
    ctx.set_fill_style_str("#16203544");
    ctx.fill_rect(lshift_x - ls_w / 2.0, y, ls_w, h);
    ctx.stroke_rect(lshift_x - ls_w / 2.0, y, ls_w, h);
    ctx.set_fill_style_str(TEXT_DIM);
    ctx.set_font("8px monospace");
    let _ = ctx.fill_text(if dev < 2 { "U13" } else { "U15" }, lshift_x, y - 4.0);
}

async fn sleep_ms(ms: u32) {
    let promise = js_sys::Promise::new(&mut |resolve, _| {
        web_sys::window().unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&resolve, ms as i32)
            .unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(promise).await.unwrap();
}
