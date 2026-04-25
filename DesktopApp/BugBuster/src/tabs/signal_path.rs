use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;
use wasm_bindgen::JsCast;
use web_sys::{HtmlCanvasElement, CanvasRenderingContext2d};
use crate::tauri_bridge::*;

// PCA9535 control IDs (must match firmware PcaControl enum)
const PCA_VADJ1_EN: u8 = 0;
const PCA_VADJ2_EN: u8 = 1;
// const PCA_EN_15V_A: u8 = 2;
// const PCA_EN_MUX: u8 = 3;
// const PCA_EN_USB_HUB: u8 = 4;
const PCA_EFUSE1_EN: u8 = 5;
const PCA_EFUSE2_EN: u8 = 6;
const PCA_EFUSE3_EN: u8 = 7;
const PCA_EFUSE4_EN: u8 = 8;

// PCB swap: physical P3 is wired to EFUSE4, P4 to EFUSE3 (silkscreen crossed).
const PCA_EFUSE_IDS: [u8; 4] = [PCA_EFUSE1_EN, PCA_EFUSE2_EN, PCA_EFUSE4_EN, PCA_EFUSE3_EN];

#[derive(Serialize)]
struct PcaSetControlArgs { control: u8, on: bool }

fn send_pca_control(control: u8, on: bool) {
    let args = serde_wasm_bindgen::to_value(&PcaSetControlArgs { control, on }).unwrap();
    let name = match control {
        0 => "VADJ1", 1 => "VADJ2", 5 => "EFuse1", 6 => "EFuse2", 7 => "EFuse3", 8 => "EFuse4",
        _ => "PCA",
    };
    let label = format!("{} {}", if on { "Enable" } else { "Disable" }, name);
    invoke_with_feedback("pca_set_control", args, &label);
}

// EfuseState and IoExpState come from tauri_bridge::* (canonical types with all fields)

const PRESETS: &[(&str, [u8; 4])] = &[
    ("All Open", [0x00; 4]), ("GPIO Direct", [0x51; 4]),
    ("ADC Read", [0x04; 4]), ("External", [0x08; 4]),
];

const C_GPIO: &str = "#22c55e";    // Green - direct GPIO
const C_GPIO_R: &str = "#eab308";  // Yellow - GPIO via 2kΩ
const C_ADC: &str = "#3b82f6";     // Blue - ADC
const C_EXT: &str = "#f97316";     // Orange - external
const C_BG: &str = "#070d1a";
const C_CHIP: &str = "#0e1629";
const C_CHIP_BD: &str = "#1e3050";

const ACCENTS: [&str; 4] = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"];
const MUX_REF: [&str; 4] = ["U10", "U11", "U17", "U16"];

// Switch input topology:
// GPIO pairs: IO goes through level shifter, then SPLITS:
//   S1 = direct (green), S2 = via 2kΩ (yellow)  — same IO
//   S5 = direct (green), S6 = via 2kΩ (yellow)  — same IO
//   S7 = direct (green), S8 = via 2kΩ (yellow)  — same IO
// Non-GPIO: S3 = ADC channel, S4 = external connector
//
// type: p=gpio pair direct, q=gpio pair resistor, a=adc, e=ext
// GPIO label names (one per pair, shown before LS)
const GPIO_PAIR_LABELS: [[&str; 3]; 4] = [
    ["IO1", "IO2", "IO3"],     // U10: pair1=S1/S2, pair2=S5/S6, pair3=S7/S8
    ["IO4", "IO5", "IO6"],     // U11
    ["IO7", "IO8", "IO9"],      // U17
    ["IO10", "IO11", "IO12"],   // U16
];

// S3 and S4 labels
const ADC_LABELS: [&str; 4] = ["CH A", "CH B", "CH D", "CH C"];
const EXT_LABELS: [&str; 4] = ["EXT 1", "EXT 2", "EXT 3", "EXT 4"];

#[component]
pub fn SignalPathTab(state: ReadSignal<DeviceState>) -> impl IntoView {
    let (mux, set_mux) = signal([0u8; 4]);
    let (psu, set_psu) = signal([false; 2]);
    let (ef, set_ef) = signal([false; 4]);
    let (oe, set_oe) = signal(false); // Level shifter OE (UI-only, ESP32 GPIO14)
    // In-flight guards: when the user toggles a PSU/EF, the 500 ms PCA poll can
    // overwrite the optimistic update with stale state until the firmware
    // applies the new value (~50–300 ms). These flags suppress polling
    // overwrites for ~700 ms after a user action.
    let psu_inflight: [RwSignal<bool>; 2] = std::array::from_fn(|_| RwSignal::new(false));
    let ef_inflight: [RwSignal<bool>; 4] = std::array::from_fn(|_| RwSignal::new(false));
    let cr = NodeRef::<leptos::html::Canvas>::new();

    // Alive flag — flips false on tab unmount so background loops terminate.
    // Without this, the 25 Hz canvas redraw and 500 ms PCA poll keep running
    // forever after the user navigates away (one of the major contributors to
    // the "100 % CPU in idle" complaint).
    let alive: RwSignal<bool> = RwSignal::new(true);
    on_cleanup(move || alive.set(false));

    // Sync MUX state from device-state event
    Effect::new(move || {
        let d = state.get();
        if d.mux_states.len() >= 4 {
            let mut a = [0u8; 4]; a.copy_from_slice(&d.mux_states[..4]); set_mux.set(a);
        }
    });

    // Poll PCA9535 status to sync PSU and E-Fuse UI with hardware
    spawn_local(async move {
        let mut fail_count = 0u32;
        loop {
            slp(500).await;
            if !alive.get_untracked() { break; }
            let result = invoke("pca_get_status", wasm_bindgen::JsValue::NULL).await;
            if let Ok(st) = serde_wasm_bindgen::from_value::<IoExpState>(result) {
                fail_count = 0;
                if st.present {
                    // Skip overwriting fields that have a recent user action in
                    // flight — otherwise the toggle UI flickers back to the
                    // pre-write firmware state until the next poll catches up.
                    let psu_new = [st.vadj1_en, st.vadj2_en];
                    set_psu.update(|v| {
                        for i in 0..2 {
                            if !psu_inflight[i].get_untracked() {
                                v[i] = psu_new[i];
                            }
                        }
                    });
                    let mut ef_new = [false; 4];
                    for (i, e) in st.efuses.iter().enumerate().take(4) {
                        ef_new[i] = e.enabled;
                    }
                    set_ef.update(|v| {
                        for i in 0..4 {
                            if !ef_inflight[i].get_untracked() {
                                v[i] = ef_new[i];
                            }
                        }
                    });
                }
            } else {
                fail_count += 1;
                if fail_count >= 10 {
                    break; // Stop polling after 10 consecutive failures (likely disconnected)
                }
            }
        }
    });

    let tog = move |d: usize, s: usize| {
        let mut st = mux.get_untracked();
        let on = (st[d] >> s) & 1 != 0;

        if !on {
            // Closing: clear other switches in the same group first (mutual exclusion)
            let group_mask: u8 = if s < 4 { 0x0F } else if s < 6 { 0x30 } else { 0xC0 };
            st[d] &= !group_mask;  // Open all in group
            st[d] |= 1 << s;       // Close requested
        } else {
            st[d] &= !(1 << s);    // Open requested
        }

        #[derive(Serialize)] #[serde(rename_all = "camelCase")] struct A { device: u8, switch_num: u8, state: bool }
        let new_state = !on;
        let a = serde_wasm_bindgen::to_value(&A { device: d as u8, switch_num: s as u8, state: new_state }).unwrap();
        let label = format!("MUX{} S{} {}", d + 1, s + 1, if new_state { "ON" } else { "OFF" });
        web_sys::console::log_1(&format!("[mux] toggle device={} switch={} new_state={}", d, s, new_state).into());
        invoke_with_feedback("mux_set_switch", a, &label);
        // Readback after 50 ms to verify firmware applied the state
        let expected = st;
        spawn_local(async move {
            slp(50).await;
            let got = crate::tauri_bridge::try_invoke("mux_get_all", wasm_bindgen::JsValue::NULL).await;
            web_sys::console::log_1(&format!("[mux] readback after toggle: {:?}", got).into());
            if let Some(val) = got {
                if let Ok(returned) = serde_wasm_bindgen::from_value::<Vec<u8>>(val) {
                    if returned.len() >= 4 && returned[..4] != expected[..4] {
                        web_sys::console::warn_1(&format!(
                            "[mux] state mismatch! expected={:?} got={:?}",
                            &expected[..4], &returned[..4]
                        ).into());
                    }
                }
            }
        });
        set_mux.set(st);
    };

    let pre = move |s: [u8; 4]| {
        web_sys::console::log_1(&format!("[mux] preset states={:?}", s).into());
        let a = serde_wasm_bindgen::to_value(&serde_json::json!({"states": s.to_vec()})).unwrap();
        invoke_with_feedback("mux_set_all", a, "Apply MUX preset");
        set_mux.set(s);
    };

    spawn_local(async move {
        loop {
            slp(40).await;
            if !alive.get_untracked() { break; }
            let Some(cv) = cr.get() else { continue };
            let cv: HtmlCanvasElement = cv;
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
            let oe_on = oe.get();

            c.set_fill_style_str(C_BG); c.fill_rect(0.0, 0.0, w, h);

            // Layout
            let psu_h = 42.0;
            let rt = psu_h + 10.0;
            let ra = h - rt - 4.0; // No legend at bottom — it's in toolbar
            let rh = ra / 4.0;

            let gpio_x = w * 0.06;
            let ls_l = w * 0.085;
            let ls_r = w * 0.115;
            let adc_x = w * 0.155;
            let mux_l = w * 0.18;
            let mux_r = w * 0.50;
            let out_x = w * 0.52;
            let ef_x = w * 0.63;
            let cn_l = w * 0.76;
            let cn_r = w * 0.95;

            // PSU bars
            psu_bar(&c, 8.0, 4.0, w * 0.48 - 12.0, psu_h, "V_ADJ1", "→ P1, P2", ps[0]);
            psu_bar(&c, w * 0.5 + 4.0, 4.0, w * 0.48 - 12.0, psu_h, "V_ADJ2", "→ P3, P4", ps[1]);

            // ── LEVEL SHIFTERS (merged: U13 spans rows 0+1, U15 spans rows 2+3) ──
            for pair in 0..2 {
                let y1 = rt + pair as f64 * 2.0 * rh;
                let y2 = y1 + 2.0 * rh;
                let ls_name = if pair == 0 { "U13" } else { "U15" };

                // Level shifter block spanning 2 rows
                let ls_pad = 4.0;
                rrect(&c, ls_l - 1.0, y1 + ls_pad, ls_r - ls_l + 2.0, (y2 - y1) - ls_pad * 2.0, 3.0);
                c.set_fill_style_str(if oe_on { "#0c1a12" } else { "#0a0f1c" });
                c.fill();
                c.set_stroke_style_str(if oe_on { "#1a4030" } else { "#1a2a40" });
                c.set_line_width(1.0);
                c.stroke();

                // LS label
                c.set_fill_style_str(if oe_on { "#22c55e" } else { "#2a3f5f" });
                c.set_font("bold 7px monospace");
                c.set_text_align("center");
                let _ = c.fill_text(ls_name, (ls_l + ls_r) / 2.0, y1 + ls_pad - 2.0);

                // OE indicator at bottom of LS
                let oe_y = y2 - ls_pad - 6.0;
                c.set_fill_style_str(if oe_on { "#22c55e" } else { "#1e293b" });
                c.begin_path(); c.arc((ls_l + ls_r) / 2.0, oe_y, 3.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                if oe_on {
                    c.set_fill_style_str("rgba(34,197,94,0.15)");
                    c.begin_path(); c.arc((ls_l + ls_r) / 2.0, oe_y, 7.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                }
                c.set_fill_style_str("#334155");
                c.set_font("5px monospace");
                let _ = c.fill_text("OE", (ls_l + ls_r) / 2.0, oe_y + 10.0);
            }

            // ── CHANNEL ROWS ──
            for ch in 0..4usize {
                let ry = rt + ch as f64 * rh;
                let st = ms[ch];
                let ac = ACCENTS[ch];
                let pi = if ch < 2 { 0 } else { 1 };
                let psu_on = ps[pi];
                let ef_on = es[ch];

                if ch > 0 {
                    c.set_stroke_style_str("#111828"); c.set_line_width(0.5);
                    c.begin_path(); c.move_to(0.0, ry); c.line_to(w, ry); c.stroke();
                }

                // Switch Y positions
                let lh = (rh - 12.0) / 8.5;
                let g = lh * 0.4;
                let mut sy = [0.0f64; 8];
                for s in 0..8 {
                    let gap = if s >= 6 { g * 2.0 } else if s >= 4 { g } else { 0.0 };
                    sy[s] = ry + 6.0 + s as f64 * lh + gap;
                }

                // MUX chip
                let mt = ry + 2.0; let mh = rh - 4.0;
                c.set_fill_style_str(C_CHIP);
                c.fill_rect(mux_l, mt, mux_r - mux_l, mh);
                c.set_stroke_style_str(C_CHIP_BD); c.set_line_width(1.0);
                c.stroke_rect(mux_l, mt, mux_r - mux_l, mh);

                // Group separators
                c.set_stroke_style_str("#162540"); c.set_line_width(0.5);
                let sep1 = (sy[3] + sy[4]) / 2.0;
                let sep2 = (sy[5] + sy[6]) / 2.0;
                c.begin_path(); c.move_to(mux_l + 3.0, sep1); c.line_to(mux_r - 3.0, sep1); c.stroke();
                c.begin_path(); c.move_to(mux_l + 3.0, sep2); c.line_to(mux_r - 3.0, sep2); c.stroke();

                // MUX label
                c.set_fill_style_str(ac); c.set_font("bold 9px monospace"); c.set_text_align("center");
                let _ = c.fill_text(&format!("MUX {} · {}", ch + 1, MUX_REF[ch]),
                                    (mux_l + mux_r) / 2.0, mt + mh - 3.0);

                // ── GPIO PAIRS: IO → LevelShifter → split (direct green / 2kΩ yellow) ──
                let gpio_pairs: [(usize, usize, usize); 3] = [(0,1,0), (4,5,1), (6,7,2)];
                for &(sd, sr, pi2) in &gpio_pairs {
                    let yd = sy[sd];
                    let yr = sy[sr];
                    let ym = (yd + yr) / 2.0;
                    let lbl = GPIO_PAIR_LABELS[ch][pi2];
                    // IO label (white, before LS)
                    c.set_font("bold 9px monospace"); c.set_text_align("right"); c.set_fill_style_str("#e2e8f0");
                    let _ = c.fill_text(lbl, gpio_x, ym + 3.0);
                    // Trace: label → LS (gray)
                    c.set_stroke_style_str("#94a3b8"); c.set_line_width(1.0);
                    c.begin_path(); c.move_to(gpio_x + 3.0, ym); c.line_to(ls_l, ym); c.stroke();
                    c.set_fill_style_str("#94a3b8");
                    c.begin_path(); c.arc(ls_l, ym, 1.5, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                    c.begin_path(); c.arc(ls_r, ym, 1.5, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                    // After LS: split
                    let sp = ls_r + 4.0;
                    c.set_stroke_style_str("#94a3b8"); c.set_line_width(1.0);
                    c.begin_path(); c.move_to(ls_r, ym); c.line_to(sp, ym); c.stroke();
                    c.set_stroke_style_str("#475569"); c.set_line_width(0.5);
                    c.begin_path(); c.move_to(sp, yd); c.line_to(sp, yr); c.stroke();
                    // Direct branch (green)
                    c.set_stroke_style_str(C_GPIO); c.set_line_width(1.0);
                    c.begin_path(); c.move_to(sp, yd); c.line_to(mux_l, yd); c.stroke();
                    // Resistor branch (yellow + zigzag)
                    c.set_stroke_style_str(C_GPIO_R); c.set_line_width(1.0);
                    c.begin_path(); c.move_to(sp, yr); c.line_to(mux_l, yr); c.stroke();
                    let rz = (sp + mux_l) / 2.0;
                    draw_resistor(&c, rz - 8.0, yr, 16.0, C_GPIO_R);
                }
                // ── ADC (S3) ──
                c.set_font("8px monospace"); c.set_text_align("right"); c.set_fill_style_str(C_ADC);
                let _ = c.fill_text(ADC_LABELS[ch], adc_x, sy[2] + 3.0);
                c.set_stroke_style_str(C_ADC); c.set_line_width(1.0);
                c.begin_path(); c.move_to(adc_x + 3.0, sy[2]); c.line_to(mux_l, sy[2]); c.stroke();
                // ── EXT (S4) ──
                c.set_fill_style_str(C_EXT);
                let _ = c.fill_text(EXT_LABELS[ch], adc_x, sy[3] + 3.0);
                c.set_stroke_style_str(C_EXT); c.set_line_width(1.0);
                c.begin_path(); c.move_to(adc_x + 3.0, sy[3]); c.line_to(mux_l, sy[3]); c.stroke();

                // ── ALL 8 SWITCH BARS ──
                for s in 0..8usize {
                    let y = sy[s];
                    let on = (st >> s) & 1 != 0;
                    let bl = mux_l + 4.0;
                    let br = mux_r - 8.0;
                    if on {
                        c.set_fill_style_str(ac);
                        c.fill_rect(bl, y - 2.0, br - bl, 4.0);
                        let glow = format!("{}44", ac);
                        c.set_fill_style_str(&glow);
                        c.begin_path(); c.arc(br, y, 6.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                        c.set_fill_style_str(ac);
                        c.begin_path(); c.arc(br, y, 3.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                    } else {
                        c.set_fill_style_str("#0b1322");
                        c.fill_rect(bl, y - 1.0, br - bl, 2.0);
                        c.set_fill_style_str("#152030");
                        c.begin_path(); c.arc(br, y, 2.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                    }
                    let sc = match s { 0|4|6 => C_GPIO, 1|5|7 => C_GPIO_R, 2 => C_ADC, 3 => C_EXT, _ => "#fff" };
                    c.set_fill_style_str(if on { sc } else { "#1e2d40" });
                    c.set_font("bold 7px monospace"); c.set_text_align("left");
                    let _ = c.fill_text(&format!("S{}", s + 1), bl + 2.0, y + 3.0);
                }

                // Output traces — color matches the active switch's signal type
                let grps: [(usize, usize, &str); 3] = [(0, 4, "Main"), (4, 6, "Aux1"), (6, 8, "Aux2")];
                for &(s0, s1, lbl) in &grps {
                    let cy = (sy[s0] + sy[s1 - 1]) / 2.0;
                    // Find the active switch in this group and use its signal color
                    let active_sw = (s0..s1).find(|&s| (st >> s) & 1 != 0);
                    let any = active_sw.is_some();
                    let tc = if let Some(s) = active_sw {
                        match s { 0|4|6 => C_GPIO, 1|5|7 => C_GPIO_R, 2 => C_ADC, 3 => C_EXT, _ => ac }
                    } else { "#0c1525" };
                    c.set_stroke_style_str(tc);
                    c.set_line_width(if any { 1.5 } else { 0.3 });
                    c.begin_path(); c.move_to(mux_r, cy); c.line_to(ef_x - 28.0, cy); c.stroke();
                    if any {
                        let glow = format!("{}15", tc);
                        c.set_stroke_style_str(&glow); c.set_line_width(6.0);
                        c.begin_path(); c.move_to(mux_r, cy); c.line_to(ef_x - 28.0, cy); c.stroke();
                        c.set_stroke_style_str(tc); c.set_line_width(1.0);
                        c.begin_path(); c.move_to(ef_x + 28.0, cy); c.line_to(cn_l, cy); c.stroke();
                    }
                    c.set_fill_style_str(if any { "#e2e8f0" } else { "#253040" });
                    c.set_font("8px monospace"); c.set_text_align("left");
                    let _ = c.fill_text(lbl, out_x, cy + 3.0);
                }

                // E-Fuse
                let ef_w = 25.0;
                let ef_top = ry + rh * 0.1;
                let ef_h2 = rh * 0.75;
                let (ef_fill, ef_bd, ef_txt) = if !psu_on {
                    (C_CHIP, C_CHIP_BD, "#334155")
                } else if !ef_on {
                    ("#1a1508", "#8b6020", "#f59e0b")     // Orange: PSU on, EF off
                } else {
                    ("#081a10", "#20603a", "#10b981")      // Green: power flowing
                };
                rrect(&c, ef_x - ef_w, ef_top, ef_w * 2.0, ef_h2, 4.0);
                c.set_fill_style_str(ef_fill); c.fill();
                c.set_stroke_style_str(ef_bd); c.set_line_width(1.0); c.stroke();
                c.set_fill_style_str(ef_txt); c.set_font("bold 7px monospace"); c.set_text_align("center");
                let _ = c.fill_text("E-FUSE", ef_x, ef_top + 14.0);
                c.set_font("6px monospace");
                let _ = c.fill_text("TPS1641", ef_x, ef_top + 24.0);
                c.set_fill_style_str(ef_txt);
                c.begin_path(); c.arc(ef_x, ef_top + ef_h2 - 10.0, 4.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();

                // ── CONNECTOR ──
                let ct = ry + 3.0;
                let conn_h = rh - 6.0;
                let conn_w = cn_r - cn_l;
                rrect(&c, cn_l, ct, conn_w, conn_h, 5.0);
                c.set_fill_style_str("#0a1222"); c.fill();
                let bdc = format!("{}55", ac);
                c.set_stroke_style_str(&bdc); c.set_line_width(1.5); c.stroke();

                // Port name centered at top
                c.set_fill_style_str(ac);
                c.set_font("bold 13px Inter, sans-serif");
                c.set_text_align("center");
                let _ = c.fill_text(&format!("P{}", ch + 1), cn_l + conn_w / 2.0, ct + 16.0);

                // Pin labels — Y positions MATCHED to output trace centers
                let pw = psu_on && ef_on;
                let psu_lbl = if pi == 0 { "V_ADJ1" } else { "V_ADJ2" };

                // These must match the group center Ys used for output traces above
                let main_cy = (sy[0] + sy[3]) / 2.0;
                let aux1_cy = (sy[4] + sy[5]) / 2.0;
                let aux2_cy = (sy[6] + sy[7]) / 2.0;
                let main_sw = (0..4).find(|&s| (st >> s) & 1 != 0);
                let aux1_sw = (4..6).find(|&s| (st >> s) & 1 != 0);
                let aux2_sw = (6..8).find(|&s| (st >> s) & 1 != 0);
                let main_on = main_sw.is_some();
                let aux1_on = aux1_sw.is_some();
                let aux2_on = aux2_sw.is_some();
                let sw_color = |sw: Option<usize>| -> &str {
                    match sw {
                        Some(0)|Some(4)|Some(6) => C_GPIO,
                        Some(1)|Some(5)|Some(7) => C_GPIO_R,
                        Some(2) => C_ADC,
                        Some(3) => C_EXT,
                        _ => "#253040",
                    }
                };
                let main_c = sw_color(main_sw);
                let aux1_c = sw_color(aux1_sw);
                let aux2_c = sw_color(aux2_sw);

                // Pin Y positions aligned to traces
                let pin_ys = [
                    ct + 14.0,      // Pin 1: V_ADJ (at top of connector)
                    main_cy,        // Pin 2: Main — aligned with Group A trace
                    aux1_cy,        // Pin 3: Aux1 — aligned with Group B trace
                    aux2_cy,        // Pin 4: Aux2 — aligned with Group C trace
                    ct + conn_h - 8.0, // Pin 5: GND (at bottom)
                ];

                c.set_font("bold 10px monospace");
                c.set_text_align("left");
                let pin_x = cn_l + 8.0;
                let num_x = cn_r - 14.0;

                // Pin 1: V_ADJ (power) — smaller font, pushed back
                c.set_font("8px monospace"); c.set_text_align("left");
                c.set_fill_style_str(if pw { "#ef444499" } else { "#1e2d40" });
                let _ = c.fill_text(psu_lbl, pin_x, pin_ys[0] + 3.0);
                c.set_text_align("right"); c.set_fill_style_str("#253040"); c.set_font("7px monospace");
                let _ = c.fill_text("1", num_x, pin_ys[0] + 3.0);
                // PWR dot
                c.set_fill_style_str(if pw { "#ef4444" } else { "#1e293b" });
                c.begin_path(); c.arc(num_x - 10.0, pin_ys[0], 4.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                if pw {
                    c.set_fill_style_str("rgba(239,68,68,0.12)");
                    c.begin_path(); c.arc(num_x - 10.0, pin_ys[0], 8.0, 0.0, std::f64::consts::TAU).unwrap(); c.fill();
                }

                // Pin 2: Main (OUT1)
                c.set_font("bold 10px monospace"); c.set_text_align("left");
                c.set_fill_style_str(if main_on { main_c } else { "#253040" });
                let _ = c.fill_text("Main", pin_x, pin_ys[1] + 4.0);
                c.set_text_align("right"); c.set_fill_style_str("#334155"); c.set_font("7px monospace");
                let _ = c.fill_text("2", num_x, pin_ys[1] + 3.0);

                // Pin 3: Aux1 (OUT2)
                c.set_font("bold 10px monospace"); c.set_text_align("left");
                c.set_fill_style_str(if aux1_on { aux1_c } else { "#253040" });
                let _ = c.fill_text("Aux1", pin_x, pin_ys[2] + 4.0);
                c.set_text_align("right"); c.set_fill_style_str("#334155"); c.set_font("7px monospace");
                let _ = c.fill_text("3", num_x, pin_ys[2] + 3.0);

                // Pin 4: Aux2 (OUT3)
                c.set_font("bold 10px monospace"); c.set_text_align("left");
                c.set_fill_style_str(if aux2_on { aux2_c } else { "#253040" });
                let _ = c.fill_text("Aux2", pin_x, pin_ys[3] + 4.0);
                c.set_text_align("right"); c.set_fill_style_str("#334155"); c.set_font("7px monospace");
                let _ = c.fill_text("4", num_x, pin_ys[3] + 3.0);

                // Pin 5: GND — smaller font, pushed back
                c.set_font("8px monospace"); c.set_text_align("left");
                c.set_fill_style_str("#1e2d40");
                let _ = c.fill_text("GND", pin_x, pin_ys[4] + 3.0);
                c.set_text_align("right"); c.set_fill_style_str("#253040"); c.set_font("7px monospace");
                let _ = c.fill_text("5", num_x, pin_ys[4] + 3.0);
            }
        }
    });

    let on_click = move |e: leptos::ev::MouseEvent| {
        let Some(cv) = cr.get() else { return };
        let cv: HtmlCanvasElement = cv.clone();
        let r = cv.get_bounding_client_rect();
        let (x, y, w, h) = (e.client_x() as f64 - r.left(), e.client_y() as f64 - r.top(), r.width(), r.height());
        let rt2 = 52.0; let rh2 = (h - rt2) / 4.0;
        let ml = w * 0.18; let mr = w * 0.50;
        if y > rt2 && x >= ml && x <= mr {
            let ch = ((y - rt2) / rh2).floor() as usize;
            if ch < 4 {
                let ry = rt2 + ch as f64 * rh2;
                let lh = (rh2 - 12.0) / 8.5;
                let sw = ((y - ry - 6.0) / lh).clamp(0.0, 7.0) as usize;
                tog(ch, sw);
            }
        }
    };

    view! {
        <div class="tab-content signal-path-tab">
            <div class="tab-desc">"Interactive signal routing matrix. Click switches inside the MUX to connect GPIOs, ADC channels, or external inputs to the output connectors. Only one switch per group (S1-S4, S5-S6, S7-S8) can be active at a time."</div>
            <div class="sp-toolbar">
                <span class="sp-title">"Signal Path"</span>
                <div class="sp-presets">
                    {PRESETS.iter().map(|(n, s)| { let s = *s;
                        view! { <button class="scope-btn" on:click=move |_| pre(s)>{*n}</button> }
                    }).collect::<Vec<_>>()}
                </div>
                <div class="sp-psu-controls">
                    <button class="sp-oe-btn" class:sp-oe-on=move || oe.get()
                        on:click=move |_| {
                            set_oe.update(|v| *v = !*v);
                            let new_val = oe.get_untracked();
                            #[derive(serde::Serialize)]
                            struct Args { on: bool }
                            let args = serde_wasm_bindgen::to_value(&Args { on: new_val }).unwrap();
                            let label = format!("{} Level Shifter OE", if new_val { "Enable" } else { "Disable" });
                            invoke_with_feedback("set_lshift_oe", args, &label);
                        }
                    >"LShift OE"</button>
                    <button class="sp-psu-btn" class:sp-psu-on=move || psu.get()[0]
                        on:click=move |_| {
                            let new_val = !psu.get_untracked()[0];
                            psu_inflight[0].set(true);
                            send_pca_control(PCA_VADJ1_EN, new_val);
                            set_psu.update(|v| v[0] = new_val);
                            spawn_local(async move {
                                slp(700).await;
                                psu_inflight[0].set(false);
                            });
                        }>"V_ADJ1"</button>
                    <button class="sp-psu-btn" class:sp-psu-on=move || psu.get()[1]
                        on:click=move |_| {
                            let new_val = !psu.get_untracked()[1];
                            psu_inflight[1].set(true);
                            send_pca_control(PCA_VADJ2_EN, new_val);
                            set_psu.update(|v| v[1] = new_val);
                            spawn_local(async move {
                                slp(700).await;
                                psu_inflight[1].set(false);
                            });
                        }>"V_ADJ2"</button>
                    {(0..4).map(|i| view! {
                        <button class="sp-ef-btn" class:sp-ef-on=move || ef.get()[i]
                            on:click=move |_| {
                                let new_val = !ef.get_untracked()[i];
                                ef_inflight[i].set(true);
                                send_pca_control(PCA_EFUSE_IDS[i], new_val);
                                set_ef.update(|v| v[i] = new_val);
                                spawn_local(async move {
                                    slp(700).await;
                                    ef_inflight[i].set(false);
                                });
                            }>{format!("EF{}", i+1)}</button>
                    }).collect::<Vec<_>>()}
                </div>
            </div>
            // Legend inside toolbar area
            <div class="sp-legend">
                <span class="sp-leg-item" style="color: #22c55e">"● GPIO (direct)"</span>
                <span class="sp-leg-item" style="color: #eab308">"● GPIO (2kΩ)"</span>
                <span class="sp-leg-item" style="color: #3b82f6">"● ADC Channel"</span>
                <span class="sp-leg-item" style="color: #f97316">"● External"</span>
                <span class="sp-leg-item" style="color: #ef4444">"● Power"</span>
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

// Draw a small resistor zigzag symbol
fn draw_resistor(c: &CanvasRenderingContext2d, x: f64, y: f64, w: f64, color: &str) {
    c.set_stroke_style_str(color);
    c.set_line_width(1.0);
    c.begin_path();
    let steps = 4;
    let step_w = w / steps as f64;
    let amp = 3.0;
    c.move_to(x, y);
    for i in 0..steps {
        let sx = x + i as f64 * step_w;
        c.line_to(sx + step_w * 0.25, y - amp);
        c.line_to(sx + step_w * 0.75, y + amp);
        c.line_to(sx + step_w, y);
    }
    c.stroke();
    // "2k" label
    c.set_fill_style_str(color);
    c.set_font("5px monospace");
    c.set_text_align("center");
    let _ = c.fill_text("2k", x + w / 2.0, y - 5.0);
}

fn psu_bar(c: &CanvasRenderingContext2d, x: f64, y: f64, w: f64, h: f64, name: &str, feeds: &str, on: bool) {
    rrect(c, x, y, w, h, 5.0);
    c.set_fill_style_str(if on { "#180808" } else { "#0a1020" });
    c.fill();
    c.set_stroke_style_str(if on { "#5b1818" } else { "#182030" });
    c.set_line_width(1.0); c.stroke();
    c.set_fill_style_str(if on { "#ef4444" } else { "#475569" });
    c.set_font("bold 11px Inter, sans-serif"); c.set_text_align("left");
    let _ = c.fill_text(name, x + 10.0, y + 16.0);
    c.set_fill_style_str("#3b4a60"); c.set_font("8px monospace");
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
