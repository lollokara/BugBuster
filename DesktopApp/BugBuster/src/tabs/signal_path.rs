use crate::components::icons::Icon;
use crate::tauri_bridge::*;
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Serialize;

/// Signal Path does not claim analog slots merely for viewing the MUX map.
pub const SLOTS: &[u8] = &[];

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

// Firmware pca9535_user_arm_efuse() handles the IO_Block 3/4 PCB placement swap.
// Pass logical channel IDs (EFUSE1..4 = 5..8) unchanged — no host-side swap needed.
const PCA_EFUSE_IDS: [u8; 4] = [PCA_EFUSE1_EN, PCA_EFUSE2_EN, PCA_EFUSE3_EN, PCA_EFUSE4_EN];

#[derive(Serialize)]
struct PcaSetControlArgs {
    control: u8,
    on: bool,
}

fn send_pca_control(control: u8, on: bool) {
    let args = serde_wasm_bindgen::to_value(&PcaSetControlArgs { control, on }).unwrap();
    let name = match control {
        0 => "VADJ1",
        1 => "VADJ2",
        5 => "EFuse1",
        6 => "EFuse2",
        7 => "EFuse3",
        8 => "EFuse4",
        _ => "PCA",
    };
    let label = format!("{} {}", if on { "Enable" } else { "Disable" }, name);
    invoke_with_feedback("pca_set_control", args, &label);
}

// EfuseState and IoExpState come from tauri_bridge::* (canonical types with all fields)

const PRESETS: &[(&str, [u8; 4], &str)] = &[
    ("All Open", [0x00; 4], "Open every switch on all four MUX devices"),
    ("GPIO Direct", [0x51; 4], "S1, S5 and S7 closed: each IO drives its pin directly"),
    ("ADC Read", [0x04; 4], "S3 closed: route each port to its ADC channel"),
    ("External", [0x08; 4], "S4 closed: route each port to its external connector"),
];

const MUX_REF: [&str; 4] = ["U10", "U11", "U17", "U16"];
// IO_Block 3/4 sit on swapped ADGS2414D device indices (confirmed by hardware
// probing). SYNC: python/bugbuster/hal.py DEFAULT_ROUTING, bus_planner.cpp
// IO_ROUTES, tasks.cpp tasks_logical_to_physical, web SignalPath.tsx.
const MUX_DEVICE_BY_LOGICAL: [usize; 4] = [0, 1, 3, 2];

// Switch input topology:
// GPIO pairs: IO goes through level shifter, then SPLITS:
//   S1 = direct, S2 = via 2kΩ  — same IO
//   S5 = direct, S6 = via 2kΩ  — same IO
//   S7 = direct, S8 = via 2kΩ  — same IO
// Non-GPIO: S3 = ADC channel, S4 = external connector
const GPIO_PAIR_LABELS: [[&str; 3]; 4] = [
    ["IO3", "IO2", "IO1"],    // U10: pair1=S1/S2 (analog), pair2=S5/S6, pair3=S7/S8
    ["IO6", "IO5", "IO4"],    // U11
    ["IO9", "IO8", "IO7"],    // U17
    ["IO12", "IO11", "IO10"], // U16
];

// Keep the operator-facing connector order natural; the AD74416H C/D hardware
// swap is handled in the routing tables.
const ADC_LABELS: [&str; 4] = ["CH A", "CH B", "CH C", "CH D"];
const EXT_LABELS: [&str; 4] = ["EXT 1", "EXT 2", "EXT 3", "EXT 4"];

/// (first switch, one-past-last switch, name) of each mutually exclusive group.
const GROUPS: [(usize, usize, &str); 3] = [(0, 4, "Main"), (4, 6, "Aux1"), (6, 8, "Aux2")];
const PAIRS: [(usize, usize); 3] = [(0, 1), (4, 5), (6, 7)];

fn kind(s: usize) -> &'static str {
    match s {
        0 | 4 | 6 => "direct",
        1 | 5 | 7 => "res",
        2 => "adc",
        _ => "ext",
    }
}

fn kind_label(s: usize) -> &'static str {
    match s {
        0 | 4 | 6 => "GPIO, direct",
        1 | 5 | 7 => "GPIO, via 2 kΩ",
        2 => "ADC channel",
        _ => "External connector",
    }
}

fn group_of(s: usize) -> usize {
    if s < 4 {
        0
    } else if s < 6 {
        1
    } else {
        2
    }
}

fn source_label(ch: usize, s: usize) -> &'static str {
    match s {
        0 | 1 => GPIO_PAIR_LABELS[ch][0],
        4 | 5 => GPIO_PAIR_LABELS[ch][1],
        6 | 7 => GPIO_PAIR_LABELS[ch][2],
        2 => ADC_LABELS[ch],
        _ => EXT_LABELS[ch],
    }
}

fn active_in_group(st: u8, g: usize) -> Option<usize> {
    let (s0, s1, _) = GROUPS[g];
    (s0..s1).find(|&s| (st >> s) & 1 != 0)
}

// ---- Schematic geometry (SVG user units) ----
const ROW_H: f64 = 224.0;
const TOP: f64 = 72.0;
const IO_END: f64 = 62.0;
const IO_W0: f64 = 68.0;
const LS_L: f64 = 88.0;
const LS_R: f64 = 122.0;
const SPLIT: f64 = 138.0;
const RES_X0: f64 = 176.0;
const RES_X1: f64 = 208.0;
const PORT_END: f64 = 222.0;
const PORT_W0: f64 = 230.0;
const CHIP_L: f64 = 250.0;
const CHIP_R: f64 = 600.0;
const TERM_L: f64 = 304.0;
const TERM_R: f64 = 484.0;
const BUS: f64 = 502.0;
const STATE_X: f64 = 514.0;
const EF_L: f64 = 650.0;
const EF_R: f64 = 740.0;
const CN_L: f64 = 800.0;
const CN_R: f64 = 960.0;

fn sw_y(s: usize) -> f64 {
    36.0 + s as f64 * 20.0 + if s >= 4 { 8.0 } else { 0.0 } + if s >= 6 { 8.0 } else { 0.0 }
}

fn f(v: f64) -> String {
    format!("{:.1}", v)
}

fn wire_class(s: usize, on: bool) -> String {
    format!("sg-w sg-k-{}{}", kind(s), if on { " lit" } else { "" })
}

fn seg(class: &'static str, x1: f64, y1: f64, x2: f64, y2: f64) -> impl IntoView {
    view! { <line class=class x1=f(x1) y1=f(y1) x2=f(x2) y2=f(y2) /> }
}

fn seg_dyn<F: Fn() -> String + Send + Sync + 'static>(
    class: F,
    x1: f64,
    y1: f64,
    x2: f64,
    y2: f64,
) -> impl IntoView {
    view! { <line class=class x1=f(x1) y1=f(y1) x2=f(x2) y2=f(y2) /> }
}

fn txt(class: &'static str, x: f64, y: f64, s: String) -> impl IntoView {
    view! { <text class=class x=f(x) y=f(y)>{s}</text> }
}

type Sel = Option<(usize, usize)>;

/// Everything the schematic needs; all members are Copy handles.
#[derive(Clone, Copy)]
struct Ctx {
    mux: ReadSignal<[u8; 4]>,
    psu: ReadSignal<[bool; 2]>,
    ef: ReadSignal<[bool; 4]>,
    oe: ReadSignal<bool>,
    selected: RwSignal<Sel>,
    hover: RwSignal<Sel>,
    /// Logical channel, switch index.
    toggle: Callback<(usize, usize)>,
}

fn psu_block(c: Ctx, i: usize) -> impl IntoView {
    let x = if i == 0 { 8.0 } else { 508.0 };
    let on = move || c.psu.get()[i];
    view! {
        <g class=move || if on() { "sg-psu on" } else { "sg-psu" }>
            <rect class="sg-psu-box" x=f(x) y="8" width="484" height="50" rx="8" />
            {txt("sg-psu-t", x + 14.0, 30.0, format!("V_ADJ{}", i + 1))}
            {txt("sg-psu-s", x + 14.0, 47.0,
                format!("LTM8063 · DS4424 · 3–15 V · feeds P{}, P{}", 2 * i + 1, 2 * i + 2))}
            <circle class="sg-psu-led" cx=f(x + 410.0) cy="33" r="4" />
            <text class="sg-psu-st sg-end" x=f(x + 470.0) y="37">
                {move || if on() { "ON" } else { "OFF" }}
            </text>
        </g>
    }
}

fn ls_block(c: Ctx, pair: usize) -> impl IntoView {
    let y0 = TOP + pair as f64 * 2.0 * ROW_H + 8.0;
    let h = 2.0 * ROW_H - 16.0;
    let cx = (LS_L + LS_R) / 2.0;
    let name = if pair == 0 { "U13" } else { "U15" };
    let mid = y0 + h / 2.0;
    view! {
        <g class=move || if c.oe.get() { "sg-ls on" } else { "sg-ls" }>
            <rect class="sg-ls-box" x=f(LS_L) y=f(y0) width=f(LS_R - LS_L) height=f(h) rx="5" />
            <text class="sg-ls-t sg-mid" x=f(cx) y=f(y0 + 16.0)>{name}</text>
            <text class="sg-ls-s sg-mid" x=f(cx) y=f(y0 + 29.0)>"LS"</text>
            <circle class="sg-ls-led" cx=f(cx) cy=f(mid - 6.0) r="4" />
            <text class="sg-ls-s sg-mid" x=f(cx) y=f(mid + 12.0)>"OE"</text>
            <text class="sg-ls-s sg-mid" x=f(cx) y=f(mid + 24.0)>
                {move || if c.oe.get() { "ON" } else { "OFF" }}
            </text>
        </g>
    }
}

fn channel_row(c: Ctx, ch: usize) -> impl IntoView {
    let dev = MUX_DEVICE_BY_LOGICAL[ch];
    let ry = TOP + ch as f64 * ROW_H;
    let pi = ch / 2;
    let ch_class = ["sg-ch-a", "sg-ch-b", "sg-ch-c", "sg-ch-d"][ch];
    let on_fn = move |s: usize| (c.mux.get()[dev] >> s) & 1 != 0;

    // --- Input network: IO -> level shifter -> split into direct / 2 kΩ ---
    let pair_views = PAIRS
        .iter()
        .enumerate()
        .map(|(pidx, &(sd, sr))| {
            let yd = ry + sw_y(sd);
            let yr = ry + sw_y(sr);
            let ym = (yd + yr) / 2.0;
            let zig = format!(
                "M{} {} L{} {} L{} {} L{} {} L{} {} L{} {}",
                f(RES_X0),
                f(yr),
                f(RES_X0 + 4.0),
                f(yr - 4.0),
                f(RES_X0 + 12.0),
                f(yr + 4.0),
                f(RES_X0 + 20.0),
                f(yr - 4.0),
                f(RES_X0 + 28.0),
                f(yr + 4.0),
                f(RES_X1),
                f(yr),
            );
            view! {
                <g>
                    {txt("sg-io sg-end", IO_END, ym + 4.0, GPIO_PAIR_LABELS[ch][pidx].to_string())}
                    {seg("sg-w", IO_W0, ym, LS_L, ym)}
                    {seg("sg-w", LS_R, ym, SPLIT, ym)}
                    {seg("sg-w", SPLIT, yd, SPLIT, yr)}
                    {seg_dyn(move || wire_class(sd, on_fn(sd)), SPLIT, yd, TERM_L, yd)}
                    {seg_dyn(move || wire_class(sr, on_fn(sr)), SPLIT, yr, RES_X0, yr)}
                    <path class=move || wire_class(sr, on_fn(sr)) d=zig />
                    {seg_dyn(move || wire_class(sr, on_fn(sr)), RES_X1, yr, TERM_L, yr)}
                    {txt("sg-2k sg-mid", (RES_X0 + RES_X1) / 2.0, yr - 8.0, "2k".to_string())}
                </g>
            }
        })
        .collect::<Vec<_>>();

    // --- ADC (S3) and external (S4) ports ---
    let port_views = [(2usize, ADC_LABELS[ch]), (3usize, EXT_LABELS[ch])]
        .into_iter()
        .map(|(s, label)| {
            let y = ry + sw_y(s);
            let tcls = if s == 2 { "sg-port sg-end sg-k-adc" } else { "sg-port sg-end sg-k-ext" };
            view! {
                <g>
                    {txt(tcls, PORT_END, y + 4.0, label.to_string())}
                    {seg_dyn(move || wire_class(s, on_fn(s)), PORT_W0, y, TERM_L, y)}
                </g>
            }
        })
        .collect::<Vec<_>>();

    // --- The eight switches ---
    let switch_views = (0..8usize)
        .map(|s| {
            let y = ry + sw_y(s);
            let src = source_label(ch, s);
            let toggle_this = move || {
                c.selected.set(Some((ch, s)));
                c.toggle.run((ch, s));
            };
            view! {
                <g
                    class=move || format!(
                        "sg-sw sg-k-{}{}{}",
                        kind(s),
                        if on_fn(s) { " on" } else { " off" },
                        if c.selected.get() == Some((ch, s)) { " sel" } else { "" }
                    )
                    role="button"
                    tabindex="0"
                    aria-label=move || format!(
                        "MUX {} switch S{}, {} {}: {}. Activate to toggle.",
                        dev + 1, s + 1, src, kind_label(s),
                        if on_fn(s) { "closed" } else { "open" }
                    )
                    aria-pressed=move || if on_fn(s) { "true" } else { "false" }
                    on:click=move |_| toggle_this()
                    on:keydown=move |e: leptos::ev::KeyboardEvent| {
                        let k = e.key();
                        if k == "Enter" || k == " " {
                            e.prevent_default();
                            toggle_this();
                        }
                    }
                    on:mouseenter=move |_| c.hover.set(Some((ch, s)))
                    on:mouseleave=move |_| c.hover.set(None)
                    on:focus=move |_| c.hover.set(Some((ch, s)))
                    on:blur=move |_| c.hover.set(None)
                >
                    <rect class="sg-hit" x=f(CHIP_L + 4.0) y=f(y - 9.0)
                        width=f(CHIP_R - CHIP_L - 8.0) height="18" rx="4" />
                    {txt("sg-lbl", CHIP_L + 12.0, y + 4.0, format!("S{}", s + 1))}
                    <circle class="sg-term" cx=f(TERM_L) cy=f(y) r="3.2" />
                    <line class="sg-blade" x1=f(TERM_L) y1=f(y)
                        x2=move || f(if on_fn(s) { TERM_R } else { TERM_R - 22.0 })
                        y2=move || f(if on_fn(s) { y } else { y - 9.0 }) />
                    <circle class="sg-term" cx=f(TERM_R) cy=f(y) r="3.2" />
                    <text class="sg-state" x=f(STATE_X) y=f(y + 4.0)>
                        {move || if on_fn(s) { "ON" } else { "OFF" }}
                    </text>
                </g>
            }
        })
        .collect::<Vec<_>>();

    // --- Group buses and outputs ---
    let group_views = GROUPS
        .iter()
        .enumerate()
        .map(|(g, &(s0, s1, name))| {
            let y0 = ry + sw_y(s0);
            let y1 = ry + sw_y(s1 - 1);
            let cy = (y0 + y1) / 2.0;
            let active = move || active_in_group(c.mux.get()[dev], g);
            let pin_y = cy;
            let pin_no = 4 - g;
            let pin_label = GPIO_PAIR_LABELS[ch][g];
            let stubs = (s0..s1)
                .map(|s| seg_dyn(move || wire_class(s, on_fn(s)), TERM_R, ry + sw_y(s), BUS, ry + sw_y(s)))
                .collect::<Vec<_>>();
            let pin_cls = move || match active() {
                Some(s) => format!("sg-pin-lbl lit sg-k-{}", kind(s)),
                None => "sg-pin-lbl".to_string(),
            };
            view! {
                <g>
                    {seg("sg-bus", BUS, y0, BUS, y1)}
                    {stubs}
                    {seg_dyn(
                        move || match active() {
                            Some(s) => format!("sg-out lit sg-k-{}", kind(s)),
                            None => "sg-out".to_string(),
                        },
                        BUS, cy, CN_L, cy,
                    )}
                    {txt("sg-grp", CHIP_R + 6.0, cy - 6.0, name.to_string())}
                    <rect class="sg-pin" x=f(CN_L - 4.0) y=f(pin_y - 3.0) width="8" height="6" />
                    <text
                        class=pin_cls
                        x=f(CN_L + 14.0) y=f(pin_y + 4.0)
                    >{pin_label}</text>
                    {txt("sg-pin-num sg-end", CN_R - 10.0, pin_y + 4.0, pin_no.to_string())}
                </g>
            }
        })
        .collect::<Vec<_>>();

    // --- E-fuse ---
    let ef_state = move || {
        let psu_on = c.psu.get()[pi];
        let ef_on = c.ef.get()[ch];
        if !psu_on {
            ("sg-ef off", "NO VADJ")
        } else if !ef_on {
            ("sg-ef armed", "EF OFF")
        } else {
            ("sg-ef live", "LIVE")
        }
    };

    // --- Connector power/GND pins ---
    let pw_live = move || c.psu.get()[pi] && c.ef.get()[ch];

    view! {
        <g class=ch_class>
            {(ch > 0).then(|| seg("sg-sep", 0.0, ry, 1000.0, ry))}

            // MUX chip
            <rect class="sg-chip" x=f(CHIP_L) y=f(ry + 4.0)
                width=f(CHIP_R - CHIP_L) height=f(ROW_H - 12.0) rx="8" />
            {txt("sg-chip-t", CHIP_L + 12.0, ry + 22.0,
                format!("P{} · MUX {} · {}", ch + 1, dev + 1, MUX_REF[ch]))}
            <text class="sg-reg sg-end" x=f(CHIP_R - 12.0) y=f(ry + 22.0)>
                {move || format!("0x{:02X}", c.mux.get()[dev])}
            </text>
            {seg("sg-gsep", CHIP_L, ry + (sw_y(3) + sw_y(4)) / 2.0, CHIP_R, ry + (sw_y(3) + sw_y(4)) / 2.0)}
            {seg("sg-gsep", CHIP_L, ry + (sw_y(5) + sw_y(6)) / 2.0, CHIP_R, ry + (sw_y(5) + sw_y(6)) / 2.0)}

            // E-fuse block (behind the signal wires)
            <g class=move || ef_state().0>
                <rect class="sg-ef-box" x=f(EF_L) y=f(ry + 6.0) width=f(EF_R - EF_L)
                    height=f(ROW_H - 14.0) rx="6" />
                {txt("sg-ef-t sg-mid", (EF_L + EF_R) / 2.0, ry + 24.0, "E-FUSE".to_string())}
                {txt("sg-ef-s sg-mid", (EF_L + EF_R) / 2.0, ry + 38.0, "TPS1641".to_string())}
                <circle class="sg-ef-led" cx=f(EF_L + 14.0) cy=f(ry + 203.0) r="4" />
                <text class="sg-ef-st" x=f(EF_L + 24.0) y=f(ry + 207.0)>{move || ef_state().1}</text>
            </g>

            // Connector
            <rect class="sg-cn" x=f(CN_L) y=f(ry + 6.0) width=f(CN_R - CN_L)
                height=f(ROW_H - 14.0) rx="8" />
            {txt("sg-cn-t", CN_L + 14.0, ry + 28.0, format!("P{}", ch + 1))}
            <rect class="sg-pin" x=f(CN_L - 4.0) y=f(ry + 41.0) width="8" height="6" />
            {txt("sg-pin-lbl", CN_L + 14.0, ry + 48.0, "GND".to_string())}
            {txt("sg-pin-num sg-end", CN_R - 10.0, ry + 48.0, "1".to_string())}
            <rect class="sg-pin" x=f(CN_L - 4.0) y=f(ry + 201.0) width="8" height="6" />
            <text class=move || if pw_live() { "sg-pin-lbl sg-pwr lit" } else { "sg-pin-lbl sg-pwr" }
                x=f(CN_L + 14.0) y=f(ry + 208.0)>
                {format!("V_ADJ{}", pi + 1)}
            </text>
            <text class=move || if pw_live() { "sg-pin-st sg-end lit" } else { "sg-pin-st sg-end" }
                x=f(CN_R - 26.0) y=f(ry + 208.0)>
                {move || if pw_live() { "LIVE" } else { "OFF" }}
            </text>
            {txt("sg-pin-num sg-end", CN_R - 10.0, ry + 208.0, "5".to_string())}

            {pair_views}
            {port_views}
            {switch_views}
            {group_views}
        </g>
    }
}

#[component]
fn SgToggle(
    label: String,
    #[prop(into)] on: Signal<bool>,
    on_click: Callback<()>,
    title: String,
) -> impl IntoView {
    view! {
        <button
            type="button"
            class="sg-tog"
            class:on=move || on.get()
            aria-pressed=move || if on.get() { "true" } else { "false" }
            title=title
            on:click=move |_| on_click.run(())
        >
            <span class="sg-tog-mark" aria-hidden="true"></span>
            <span>{label}</span>
            <span class="sg-tog-state">{move || if on.get() { "On" } else { "Off" }}</span>
        </button>
    }
}

fn state_badge(on: bool, on_text: &'static str, off_text: &'static str) -> impl IntoView {
    view! {
        <span class=if on { "badge tone-green" } else { "badge" }>
            <Icon name=if on { "check" } else { "minus" } size=12 />
            {if on { on_text } else { off_text }}
        </span>
    }
}

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
    let selected: RwSignal<Sel> = RwSignal::new(None);
    let hover: RwSignal<Sel> = RwSignal::new(None);

    // Alive flag — flips false on tab unmount so the background poll terminates.
    let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
    let alive_clean = alive.clone();
    on_cleanup(move || alive_clean.store(false, std::sync::atomic::Ordering::Relaxed));

    // Sync MUX state from device-state event
    Effect::new(move || {
        let d = state.get();
        if d.mux_states.len() >= 4 {
            let mut a = [0u8; 4];
            a.copy_from_slice(&d.mux_states[..4]);
            set_mux.set(a);
        }
    });

    // Poll PCA9535 status to sync PSU and E-Fuse UI with hardware
    let alive_poll = alive.clone();
    spawn_local(async move {
        let mut fail_count = 0u32;
        loop {
            slp(500).await;
            if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                break;
            }
            let result = try_invoke("pca_get_status", wasm_bindgen::JsValue::NULL).await;
            if let Some(st) =
                result.and_then(|r| serde_wasm_bindgen::from_value::<IoExpState>(r).ok())
            {
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
            let group_mask: u8 = if s < 4 {
                0x0F
            } else if s < 6 {
                0x30
            } else {
                0xC0
            };
            st[d] &= !group_mask; // Open all in group
            st[d] |= 1 << s; // Close requested
        } else {
            st[d] &= !(1 << s); // Open requested
        }

        #[derive(Serialize)]
        #[serde(rename_all = "camelCase")]
        struct A {
            device: u8,
            switch_num: u8,
            state: bool,
        }
        let new_state = !on;
        let a = serde_wasm_bindgen::to_value(&A {
            device: d as u8,
            switch_num: s as u8,
            state: new_state,
        })
        .unwrap();
        let label = format!(
            "MUX{} S{} {}",
            d + 1,
            s + 1,
            if new_state { "ON" } else { "OFF" }
        );
        web_sys::console::log_1(
            &format!(
                "[mux] toggle device={} switch={} new_state={}",
                d, s, new_state
            )
            .into(),
        );
        invoke_with_feedback("mux_set_switch", a, &label);
        // Readback after 50 ms to verify firmware applied the state
        let expected = st;
        spawn_local(async move {
            slp(50).await;
            let got =
                crate::tauri_bridge::try_invoke("mux_get_all", wasm_bindgen::JsValue::NULL).await;
            web_sys::console::log_1(&format!("[mux] readback after toggle: {:?}", got).into());
            if let Some(val) = got {
                if let Ok(returned) = serde_wasm_bindgen::from_value::<Vec<u8>>(val) {
                    if returned.len() >= 4 && returned[..4] != expected[..4] {
                        web_sys::console::warn_1(
                            &format!(
                                "[mux] state mismatch! expected={:?} got={:?}",
                                &expected[..4],
                                &returned[..4]
                            )
                            .into(),
                        );
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

    let toggle = Callback::new(move |(ch, s): (usize, usize)| tog(MUX_DEVICE_BY_LOGICAL[ch], s));
    let ctx = Ctx { mux, psu, ef, oe, selected, hover, toggle };
    let svg_h = TOP + 4.0 * ROW_H;

    let focus = move || hover.get().or(selected.get());

    view! {
        <div class="sg-root">
            // ============ TOOLBAR ============
            <div class="sg-bar">
                <div class="sg-group" role="group" aria-label="Presets">
                    <span class="sg-cap">"Presets"</span>
                    {PRESETS.iter().map(|(n, s, tip)| { let s = *s;
                        view! { <button type="button" class="btn btn-sm" title=*tip on:click=move |_| pre(s)>{*n}</button> }
                    }).collect::<Vec<_>>()}
                </div>
                <div class="sg-divider"></div>
                <div class="sg-group" role="group" aria-label="Level shifter">
                    <span class="sg-cap">"Level shifter"</span>
                    <SgToggle
                        label="OE".to_string()
                        title="Level shifter output enable (U13, U15)".to_string()
                        on=oe
                        on_click=Callback::new(move |_| {
                            set_oe.update(|v| *v = !*v);
                            let new_val = oe.get_untracked();
                            #[derive(serde::Serialize)]
                            struct Args { on: bool }
                            let args = serde_wasm_bindgen::to_value(&Args { on: new_val }).unwrap();
                            let label = format!("{} Level Shifter OE", if new_val { "Enable" } else { "Disable" });
                            invoke_with_feedback("set_lshift_oe", args, &label);
                        })
                    />
                </div>
                <div class="sg-divider"></div>
                <div class="sg-group" role="group" aria-label="Supplies">
                    <span class="sg-cap">"Supply"</span>
                    {[(0usize, PCA_VADJ1_EN), (1usize, PCA_VADJ2_EN)].into_iter().map(|(i, id)| view! {
                        <SgToggle
                            label=format!("V_ADJ{}", i + 1)
                            title=format!("Enable the V_ADJ{} supply rail", i + 1)
                            on=Signal::derive(move || psu.get()[i])
                            on_click=Callback::new(move |_| {
                                let new_val = !psu.get_untracked()[i];
                                psu_inflight[i].set(true);
                                send_pca_control(id, new_val);
                                set_psu.update(|v| v[i] = new_val);
                                spawn_local(async move {
                                    slp(700).await;
                                    psu_inflight[i].set(false);
                                });
                            })
                        />
                    }).collect::<Vec<_>>()}
                </div>
                <div class="sg-divider"></div>
                <div class="sg-group" role="group" aria-label="E-fuses">
                    <span class="sg-cap">"E-fuse"</span>
                    {(0..4usize).map(|i| view! {
                        <SgToggle
                            label=format!("EF{}", i + 1)
                            title=format!("Arm the E-fuse of port P{}", i + 1)
                            on=Signal::derive(move || ef.get()[i])
                            on_click=Callback::new(move |_| {
                                let new_val = !ef.get_untracked()[i];
                                ef_inflight[i].set(true);
                                send_pca_control(PCA_EFUSE_IDS[i], new_val);
                                set_ef.update(|v| v[i] = new_val);
                                spawn_local(async move {
                                    slp(700).await;
                                    ef_inflight[i].set(false);
                                });
                            })
                        />
                    }).collect::<Vec<_>>()}
                </div>
            </div>

            // ============ LEGEND ============
            <div class="sg-legend">
                <span class="sg-leg sg-k-direct"><i class="sg-leg-sw"></i>"GPIO direct"</span>
                <span class="sg-leg sg-k-res"><i class="sg-leg-sw"></i>"GPIO via 2 kΩ"</span>
                <span class="sg-leg sg-k-adc"><i class="sg-leg-sw"></i>"ADC channel"</span>
                <span class="sg-leg sg-k-ext"><i class="sg-leg-sw"></i>"External"</span>
                <span class="sg-leg sg-k-pwr"><i class="sg-leg-sw"></i>"Power"</span>
                <span class="sg-leg-note">
                    <span class="sg-key"><i class="sg-key-closed"></i>"Closed"</span>
                    <span class="sg-key"><i class="sg-key-open"></i>"Open"</span>
                </span>
                <span class="sg-spacer"></span>
                <span class="sg-help">"Click a switch to close or open it. One switch per group (S1-S4, S5-S6, S7-S8) can be closed."</span>
            </div>

            // ============ SCHEMATIC + INSPECTOR ============
            <div class="sg-main">
                <div class="sg-canvas">
                    <svg class="sg-svg" viewBox=format!("0 0 1000 {}", svg_h) role="group"
                        aria-label="MUX signal routing schematic">
                        {psu_block(ctx, 0)}
                        {psu_block(ctx, 1)}
                        {ls_block(ctx, 0)}
                        {ls_block(ctx, 1)}
                        {(0..4usize).map(|ch| channel_row(ctx, ch)).collect::<Vec<_>>()}
                    </svg>
                </div>

                <aside class="inspector sg-inspector" aria-label="Switch inspector">
                    <div class="inspector-header">
                        <span>"Inspector"</span>
                        <span class="subtle text-caption">"ADGS2414D"</span>
                    </div>
                    {move || match focus() {
                        None => view! {
                            <div class="inspector-section">
                                <p class="sg-note">"Hover or focus a switch to inspect it. Click a switch to close or open it."</p>
                            </div>
                        }.into_any(),
                        Some((ch, s)) => {
                            let dev = MUX_DEVICE_BY_LOGICAL[ch];
                            let st = mux.get()[dev];
                            let on = (st >> s) & 1 != 0;
                            let g = group_of(s);
                            let (g0, g1, gname) = GROUPS[g];
                            let others = (g0..g1)
                                .filter(|&o| o != s)
                                .map(|o| format!("S{}", o + 1))
                                .collect::<Vec<_>>()
                                .join(", ");
                            let pi = ch / 2;
                            let psu_on = psu.get()[pi];
                            let ef_on = ef.get()[ch];
                            view! {
                                <div class="inspector-section">
                                    <div class="sg-sel-head">
                                        <span class="sg-sel-title">{format!("S{} · {}", s + 1, source_label(ch, s))}</span>
                                        {state_badge(on, "Closed", "Open")}
                                    </div>
                                    <dl class="kv sg-kv">
                                        <dt>"Port"</dt><dd>{format!("P{}", ch + 1)}</dd>
                                        <dt>"Device"</dt><dd>{format!("MUX {} ({})", dev + 1, MUX_REF[ch])}</dd>
                                        <dt>"Signal"</dt><dd>{kind_label(s)}</dd>
                                        <dt>"Group"</dt><dd>{format!("{} (S{}-S{})", gname, g0 + 1, g1)}</dd>
                                        <dt>"Output pin"</dt><dd>{format!("{} · pin {}", GPIO_PAIR_LABELS[ch][g], 4 - g)}</dd>
                                        <dt>"Register"</dt><dd class="sg-mono">{format!("0x{:02X} · bit {}", st, s)}</dd>
                                    </dl>
                                    <button type="button"
                                        class=if on { "btn btn-sm btn-block" } else { "btn btn-sm btn-primary btn-block" }
                                        on:click=move |_| {
                                            selected.set(Some((ch, s)));
                                            toggle.run((ch, s));
                                        }
                                    >{if on { format!("Open S{}", s + 1) } else { format!("Close S{}", s + 1) }}</button>
                                    <p class="sg-note">
                                        {if on {
                                            format!("S{} is the active path of the {} group.", s + 1, gname)
                                        } else {
                                            format!("Closing S{} first opens {} (one switch per group).", s + 1, others)
                                        }}
                                    </p>
                                </div>
                                <div class="inspector-section">
                                    <h4>{format!("Port P{} power", ch + 1)}</h4>
                                    <div class="row"><span class="row-label">{format!("V_ADJ{} supply", pi + 1)}</span>{state_badge(psu_on, "On", "Off")}</div>
                                    <div class="row"><span class="row-label">{format!("E-fuse EF{}", ch + 1)}</span>{state_badge(ef_on, "Armed", "Off")}</div>
                                </div>
                            }.into_any()
                        }
                    }}
                    <div class="inspector-section">
                        <h4>"Switch matrix"</h4>
                        {move || { let st = mux.get();
                            (0..4usize).map(|ch| { let d = MUX_DEVICE_BY_LOGICAL[ch]; view! {
                                <div class="sg-mx">
                                    <span class="sg-mx-label">{format!("{} · MUX {} ({})", ADC_LABELS[ch], d + 1, MUX_REF[ch])}</span>
                                    <div class="sg-mx-row">
                                        {(0..8usize).map(|s| { let on = (st[d] >> s) & 1 != 0;
                                            view! {
                                                <button type="button" class="sg-mx-btn" class:on=on
                                                    aria-pressed=if on { "true" } else { "false" }
                                                    title=format!("MUX {} S{}: {}", d + 1, s + 1, if on { "closed" } else { "open" })
                                                    on:mouseenter=move |_| hover.set(Some((ch, s)))
                                                    on:mouseleave=move |_| hover.set(None)
                                                    on:click=move |_| { selected.set(Some((ch, s))); toggle.run((ch, s)); }
                                                >{format!("{}", s + 1)}</button>
                                            }
                                        }).collect::<Vec<_>>()}
                                    </div>
                                </div>
                            }}).collect::<Vec<_>>()
                        }}
                        <p class="sg-note">"Numbers are switch indices; filled = closed."</p>
                    </div>
                </aside>
            </div>
        </div>
    }
}

async fn slp(ms: u32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        web_sys::window()
            .unwrap()
            .set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms as i32)
            .unwrap();
    });
    wasm_bindgen_futures::JsFuture::from(p).await.unwrap();
}
