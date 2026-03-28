use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

const PRESETS: &[(&str, [u8; 4])] = &[
    ("All Open", [0x00; 4]), ("GPIO Direct", [0x51; 4]),
    ("ADC Read", [0x04; 4]), ("External", [0x08; 4]),
];

const C_GPIO: &str = "#22c55e";
const C_GPIO_R: &str = "#eab308";
const C_ADC: &str = "#3b82f6";
const C_EXT: &str = "#f97316";
const C_TEXT: &str = "#94a3b8";
const C_DIM: &str = "#334155";
const C_BG: &str = "#070d1a";
const C_CHIP: &str = "#0e1629";
const C_CHIP_BD: &str = "#1e3050";

const ACCENTS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];
const MUX_REF: [&str; 4] = ["U10", "U11", "U16", "U17"];

// [device][switch] = (label, type: g=gpio, r=gpio+resistor, a=adc, e=ext)
const INP: [[[&str; 2]; 8]; 4] = [
    [["IO1","g"],["IO1","r"],["CH A","a"],["J4","e"],["IO2","g"],["IO2","r"],["IO3","g"],["IO3","r"]],
    [["IO5","g"],["IO5","r"],["CH B","a"],["J4","e"],["IO6","g"],["IO6","r"],["IO7","g"],["IO7","r"]],
    [["IO13","g"],["IO13","r"],["CH C","a"],["J4","e"],["IO12","g"],["IO12","r"],["IO11","g"],["IO11","r"]],
    [["IO10","g"],["IO10","r"],["CH D","a"],["J4","e"],["IO9","g"],["IO9","r"],["IO8","g"],["IO8","r"]],
];

fn ic(t: &str) -> &'static str {
    match t { "g" => C_GPIO, "r" => C_GPIO_R, "a" => C_ADC, "e" => C_EXT, _ => C_TEXT }
}

#[component]
pub fn SignalPathTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (mux, set_mux) = signal([0u8; 4]);
    let (psu, set_psu) = signal([false; 2]);
    let (ef, set_ef) = signal([false; 4]);
    let cr = NodeRef::<leptos::html::Canvas>::new();

    Effect::new(move || {
        let d = state.get();
        if d.mux_states.len() >= 4 {
            let mut a = [0u8; 4]; a.copy_from_slice(&d.mux_states[..4]); set_mux.set(a);
        }
    });

    let tog = move |d: usize, s: usize| {
        let mut st = mux.get_untracked();
        let on = (st[d] >> s) & 1 != 0;
        #[derive(Serialize)] struct A { device: u8, switch_num: u8, state: bool }
        let a = serde_wasm_bindgen::to_value(&A { device: d as u8, switch_num: s as u8, state: !on }).unwrap();
        invoke_void("mux_set_switch", a);
        if on { st[d] &= !(1 << s); } else { st[d] |= 1 << s; }
        set_mux.set(st);
    };

    let pre = move |s: [u8; 4]| {
        let a = serde_wasm_bindgen::to_value(&serde_json::json!({"states": s.to_vec()})).unwrap();
        invoke_void("mux_set_all", a);
        set_mux.set(s);
    };

    spawn_local(async move {
        loop {
            slp(40).await;
            let Some(cv) = cr.get() else { continue };
            let cv: HtmlCanvasElement = cv.into();
            let dp = web_sys::window().unwrap().device_pixel_ratio();
            let rc = cv.get_bounding_client_rect();
            let (w, h) = (rc.width(), rc.height());
            if w < 300.0 || h < 200.0 { continue; }
            cv.set_width((w * dp) as u32); cv.set_height((h * dp) as u32);
            let c: CanvasRenderingContext2d = cv.get_context("2d").unwrap().unwrap().dyn_into().unwrap();
            c.scale(dp, dp).unwrap();

            let ms = mux.get();
            let ps = psu.get();
            let es = ef.get();

            c.set_fill_style_str(C_BG);
            c.fill_rect(0.0, 0.0, w, h);

            // Layout
            let psu_h = 42.0;
            let leg_h = 22.0;
            let rt = psu_h + 10.0;
            let ra = h - rt - leg_h;
            let rh = ra / 4.0;
            let lbl_x = w * 0.14;   // Input labels right-align here
            let mux_l = w * 0.17;   // MUX left
            let mux_r = w * 0.50;   // MUX right
            let out_x = w * 0.52;   // Output labels
            let ef_x = w * 0.63;    // E-Fuse center
            let cn_l = w * 0.76;    // Connector left
            let cn_r = w * 0.95;    // Connector right

            // PSU bars
            psu_bar(&c, 8.0, 4.0, w * 0.48 - 12.0, psu_h, "V_ADJ1", "→ P1, P2", ps[0]);
            psu_bar(&c, w * 0.5 + 4.0, 4.0, w * 0.48 - 12.0, psu_h, "V_ADJ2", "→ P3, P4", ps[1]);

            // Channel rows
            for ch in 0..4usize {
                let ry = rt + ch as f64 * rh;
                let st = ms[ch];
                let ac = ACCENTS[ch];
                let pi = if ch < 2 { 0 } else { 1 };
                let psu_on = ps[pi];
                let ef_on = es[ch];

                // Row divider
                if ch > 0 {
                    c.set_stroke_style_str("#111828");
                    c.set_line_width(0.5);
                    c.begin_path(); c.move_to(0.0, ry); c.line_to(w, ry); c.stroke();
                }

                // Switch Y positions (8 switches, gaps between groups)
                let lh = (rh - 14.0) / 8.5;
                let g1 = lh * 0.4; // gap after S4
                let g2 = lh * 0.4; // gap after S6
                let mut sy = [0.0f64; 8];
                for s in 0..8 {
                    let gap = if s >= 6 { g1 + g2 } else if s >= 4 { g1 } else { 0.0 };
                    sy[s] = ry + 7.0 + s as f64 * lh + gap;
                }

                // ── MUX CHIP (solid, covers everything behind) ──
                let mt = ry + 2.0;
                let mh = rh - 4.0;
                c.set_fill_style_str(C_CHIP);
                c.fill_rect(mux_l, mt, mux_r - mux_l, mh);
                c.set_stroke_style_str(C_CHIP_BD);
                c.set_line_width(1.0);
                c.stroke_rect(mux_l, mt, mux_r - mux_l, mh);

                // Group separators
                c.set_stroke_style_str("#162540");
                c.set_line_width(0.5);
                let sep1 = (sy[3] + sy[4]) / 2.0;
                let sep2 = (sy[5] + sy[6]) / 2.0;
                c.begin_path(); c.move_to(mux_l + 3.0, sep1); c.line_to(mux_r - 3.0, sep1); c.stroke();
                c.begin_path(); c.move_to(mux_l + 3.0, sep2); c.line_to(mux_r - 3.0, sep2); c.stroke();

                // MUX label at bottom
                c.set_fill_style_str(ac);
                c.set_font("bold 9px monospace");
                c.set_text_align("center");
                let _ = c.fill_text(&format!("MUX {} · {}", ch + 1, MUX_REF[ch]),
                                    (mux_l + mux_r) / 2.0, mt + mh - 3.0);

                // ── SWITCHES ──
                for s in 0..8usize {
                    let y = sy[s];
                    let on = (st >> s) & 1 != 0;
                    let col = ic(INP[ch][s][1]);
                    let lbl = INP[ch][s][0];

                    // Input label — ALWAYS visible in its color
                    c.set_font("9px monospace");
                    c.set_text_align("right");
                    c.set_fill_style_str(col);
                    let _ = c.fill_text(lbl, lbl_x, y + 4.0);

                    // Input trace: label → MUX edge (ALWAYS drawn, colored)
                    c.set_stroke_style_str(col);
                    c.set_line_width(1.0);
                    c.begin_path();
                    c.move_to(lbl_x + 4.0, y);
                    c.line_to(mux_l, y);
                    c.stroke();

                    // ── Inside MUX: switch bar (only lights up when ON) ──
                    let bl = mux_l + 4.0;
                    let br = mux_r - 8.0;

                    if on {
                        // Active bar
                        c.set_fill_style_str(ac);
                        c.fill_rect(bl, y - 2.0, br - bl, 4.0);
                        // Glow dot at right end
                        let glow = format!("{}44", ac);
                        c.set_fill_style_str(&glow);
                        c.begin_path();
                        c.arc(br, y, 6.0, 0.0, std::f64::consts::TAU).unwrap();
                        c.fill();
                        c.set_fill_style_str(ac);
                        c.begin_path();
                        c.arc(br, y, 3.0, 0.0, std::f64::consts::TAU).unwrap();
                        c.fill();
                    } else {
                        // Inactive: thin dim line
                        c.set_fill_style_str("#0b1322");
                        c.fill_rect(bl, y - 1.0, br - bl, 2.0);
                        // Dim dot
                        c.set_fill_style_str("#152030");
                        c.begin_path();
                        c.arc(br, y, 2.0, 0.0, std::f64::consts::TAU).unwrap();
                        c.fill();
                    }

                    // Switch number
                    c.set_fill_style_str(if on { "#ffffff" } else { "#1e2d40" });
                    c.set_font("bold 7px monospace");
                    c.set_text_align("left");
                    let _ = c.fill_text(&format!("S{}", s + 1), bl + 2.0, y + 3.0);
                }

                // ── OUTPUT TRACES ──
                let grps: [(usize, usize, &str); 3] = [(0, 4, "Main"), (4, 6, "Aux1"), (6, 8, "Aux2")];
                for &(s0, s1, lbl) in &grps {
                    let cy = (sy[s0] + sy[s1 - 1]) / 2.0;
                    let any = (s0..s1).any(|s| (st >> s) & 1 != 0);

                    // Trace MUX → output area
                    c.set_stroke_style_str(if any { ac } else { "#0c1525" });
                    c.set_line_width(if any { 1.5 } else { 0.3 });
                    c.begin_path();
                    c.move_to(mux_r, cy);
                    c.line_to(ef_x - 30.0, cy);
                    c.stroke();

                    // If active, continue past E-Fuse to connector
                    if any {
                        let glow = format!("{}15", ac);
                        c.set_stroke_style_str(&glow);
                        c.set_line_width(6.0);
                        c.begin_path(); c.move_to(mux_r, cy); c.line_to(ef_x - 30.0, cy); c.stroke();

                        c.set_stroke_style_str(ac);
                        c.set_line_width(1.0);
                        c.begin_path(); c.move_to(ef_x + 28.0, cy); c.line_to(cn_l, cy); c.stroke();
                    }

                    // Label
                    c.set_fill_style_str(if any { "#e2e8f0" } else { C_DIM });
                    c.set_font("8px monospace");
                    c.set_text_align("left");
                    let _ = c.fill_text(lbl, out_x, cy + 3.0);
                }

                // ── E-FUSE ──
                // States: off=dim, psu_on=orange, psu_on+ef_on=green, fault=red
                let ef_w = 25.0;
                let ef_top = ry + rh * 0.1;
                let ef_h = rh * 0.75;
                let (ef_fill, ef_bd, ef_txt) = if !psu_on {
                    (C_CHIP, C_CHIP_BD, C_DIM)          // Off
                } else if !ef_on {
                    ("#1a1508", "#8b6020", "#f59e0b")    // PSU on, E-Fuse off = orange
                } else {
                    ("#081a10", "#20603a", "#10b981")    // Both on = green (power fed)
                };

                rrect(&c, ef_x - ef_w, ef_top, ef_w * 2.0, ef_h, 4.0);
                c.set_fill_style_str(ef_fill);
                c.fill();
                c.set_stroke_style_str(ef_bd);
                c.set_line_width(1.0);
                c.stroke();

                c.set_fill_style_str(ef_txt);
                c.set_font("bold 7px monospace");
                c.set_text_align("center");
                let _ = c.fill_text("E-FUSE", ef_x, ef_top + 14.0);
                c.set_font("6px monospace");
                let _ = c.fill_text("TPS1641", ef_x, ef_top + 24.0);

                // Status dot
                c.set_fill_style_str(ef_txt);
                c.begin_path();
                c.arc(ef_x, ef_top + ef_h - 12.0, 4.0, 0.0, std::f64::consts::TAU).unwrap();
                c.fill();

                // ── CONNECTOR ──
                let ct = ry + 3.0;
                let ch2 = rh - 6.0;
                rrect(&c, cn_l, ct, cn_r - cn_l, ch2, 5.0);
                c.set_fill_style_str("#0a1222");
                c.fill();
                let bdc = format!("{}55", ac);
                c.set_stroke_style_str(&bdc);
                c.set_line_width(1.5);
                c.stroke();

                // Port label
                c.set_fill_style_str(ac);
                c.set_font("bold 16px Inter, sans-serif");
                c.set_text_align("center");
                let cx = (cn_l + cn_r) / 2.0;
                let _ = c.fill_text(&format!("P{}", ch + 1), cx, ct + 22.0);

                // Pin list
                c.set_fill_style_str(C_TEXT);
                c.set_font("8px monospace");
                c.set_text_align("left");
                let _ = c.fill_text("Main", cn_l + 8.0, ct + 38.0);
                let _ = c.fill_text("Aux1", cn_l + 8.0, ct + 50.0);
                let _ = c.fill_text("Aux2", cn_l + 8.0, ct + 62.0);
                let psu_lbl = if pi == 0 { "V_ADJ1" } else { "V_ADJ2" };
                c.set_fill_style_str(if psu_on && ef_on { "#ef4444" } else { C_DIM });
                let _ = c.fill_text(psu_lbl, cn_l + 8.0, ct + 76.0);
                c.set_fill_style_str("#475569");
                let _ = c.fill_text("GND", cn_l + 8.0, ct + ch2 - 8.0);

                // PWR indicator
                let pw = psu_on && ef_on;
                c.set_fill_style_str(if pw { "#ef4444" } else { "#1e293b" });
                c.begin_path();
                c.arc(cn_r - 14.0, ct + 14.0, 5.0, 0.0, std::f64::consts::TAU).unwrap();
                c.fill();
                if pw {
                    c.set_fill_style_str("rgba(239,68,68,0.12)");
                    c.begin_path();
                    c.arc(cn_r - 14.0, ct + 14.0, 10.0, 0.0, std::f64::consts::TAU).unwrap();
                    c.fill();
                }
            }

            // Legend
            let ly = h - 10.0;
            c.set_font("9px monospace");
            c.set_text_align("left");
            let leg: [(&str, &str); 5] = [
                (C_GPIO, "GPIO (direct)"), (C_GPIO_R, "GPIO (2kΩ)"),
                (C_ADC, "ADC Channel"), (C_EXT, "External"), ("#ef4444", "Power"),
            ];
            let mut lx = 10.0;
            for (col, txt) in &leg {
                c.set_fill_style_str(col);
                c.begin_path(); c.arc(lx, ly, 3.5, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                let _ = c.fill_text(txt, lx + 7.0, ly + 3.0);
                lx += 105.0;
            }
        }
    });

    let on_click = move |e: leptos::ev::MouseEvent| {
        let Some(cv) = cr.get() else { return };
        let cv: HtmlCanvasElement = cv.clone().into();
        let r = cv.get_bounding_client_rect();
        let (x, y, w, h) = (e.client_x() as f64 - r.left(), e.client_y() as f64 - r.top(), r.width(), r.height());
        let rt2 = 52.0;
        let rh2 = (h - rt2 - 22.0) / 4.0;
        let ml = w * 0.17;
        let mr = w * 0.50;
        if y > rt2 && x >= ml && x <= mr {
            let ch = ((y - rt2) / rh2).floor() as usize;
            if ch < 4 {
                let ry = rt2 + ch as f64 * rh2;
                let lh = (rh2 - 14.0) / 8.5;
                let sw = ((y - ry - 7.0) / lh).floor().clamp(0.0, 7.0) as usize;
                tog(ch, sw);
            }
        }
    };

    view! {
        <div class="tab-content signal-path-tab">
            <div class="sp-toolbar">
                <span class="sp-title">"Signal Path"</span>
                <div class="sp-presets">
                    {PRESETS.iter().map(|(n, s)| { let s = *s;
                        view! { <button class="scope-btn" on:click=move |_| pre(s)>{*n}</button> }
                    }).collect::<Vec<_>>()}
                </div>
                <div class="sp-psu-controls">
                    <button class="sp-psu-btn" class:sp-psu-on=move || psu.get()[0]
                        on:click=move |_| set_psu.update(|v| v[0] = !v[0])>"V_ADJ1"</button>
                    <button class="sp-psu-btn" class:sp-psu-on=move || psu.get()[1]
                        on:click=move |_| set_psu.update(|v| v[1] = !v[1])>"V_ADJ2"</button>
                    {(0..4).map(|i| view! {
                        <button class="sp-ef-btn" class:sp-ef-on=move || ef.get()[i]
                            on:click=move |_| set_ef.update(|v| v[i] = !v[i])>{format!("EF{}", i+1)}</button>
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            <div class="sp-canvas-wrap">
                <canvas node_ref=cr class="sp-canvas" on:click=on_click></canvas>
            </div>
            <div class="sp-summary">
                {move || { let st = mux.get();
                    (0..4).map(|d| view! {
                        <div class="sp-dev-summary">
                            <span class="sp-dev-label">{format!("MUX {} ({})", d+1, MUX_REF[d])}</span>
                            <div class="sp-sw-row">
                                {(0..8).map(|s| { let on = (st[d] >> s) & 1 != 0;
                                    view! { <button class="sp-sw-btn" class:sp-sw-on=on
                                        on:click=move |_| tog(d, s)>{format!("S{}", s+1)}</button> }
                                }).collect::<Vec<_>>()}
                            </div>
                        </div>
                    }).collect::<Vec<_>>()
                }}
            </div>
        </div>
    }
}

fn psu_bar(c: &CanvasRenderingContext2d, x: f64, y: f64, w: f64, h: f64, name: &str, feeds: &str, on: bool) {
    rrect(c, x, y, w, h, 5.0);
    c.set_fill_style_str(if on { "#180808" } else { "#0a1020" });
    c.fill();
    c.set_stroke_style_str(if on { "#5b1818" } else { "#182030" });
    c.set_line_width(1.0);
    c.stroke();
    c.set_fill_style_str(if on { "#ef4444" } else { "#475569" });
    c.set_font("bold 11px Inter, sans-serif");
    c.set_text_align("left");
    let _ = c.fill_text(name, x + 10.0, y + 16.0);
    c.set_fill_style_str("#3b4a60");
    c.set_font("8px monospace");
    let _ = c.fill_text(&format!("LTM8063 · DS4424 · 3–15V  {}", feeds), x + 10.0, y + 30.0);
    c.set_fill_style_str(if on { "#10b981" } else { "#1e293b" });
    c.begin_path(); c.arc(x + w - 20.0, y + h / 2.0, 4.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
    c.set_fill_style_str(if on { "#f59e0b" } else { "#1e293b" });
    c.begin_path(); c.arc(x + w - 6.0, y + h / 2.0, 4.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
}

fn rrect(c: &CanvasRenderingContext2d, x: f64, y: f64, w: f64, h: f64, r: f64) {
    c.begin_path();
    c.move_to(x + r, y); c.line_to(x + w - r, y);
    c.arc_to(x + w, y, x + w, y + r, r).unwrap();
    c.line_to(x + w, y + h - r);
    c.arc_to(x + w, y + h, x + w - r, y + h, r).unwrap();
    c.line_to(x + r, y + h);
    c.arc_to(x, y + h, x, y + h - r, r).unwrap();
    c.line_to(x, y + r);
    c.arc_to(x, y, x + r, y, r).unwrap();
    c.close_path();
}

async fn slp(ms: u32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        web_sys::window().unwrap().set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms as i32).unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(p).await.unwrap();
}
