// battsim_chart.rs - canvas renderer and human-readable formatting for the
// battery-simulator history view (4 lanes sharing an elapsed-time axis).

use wasm_bindgen::JsCast;
use web_sys::{CanvasRenderingContext2d, HtmlCanvasElement};

use super::battsim::{BsEvent, BsView};

pub const LANE_GAP: f64 = 10.0;
pub const AXIS_H: f64 = 22.0;
pub const LEFT_W: f64 = 64.0;
pub const RIGHT_W: f64 = 12.0;

// --------------------------------------------------------------------------
// Formatting
// --------------------------------------------------------------------------
pub fn fmt_si(v: f64, unit: &str) -> String {
    if !v.is_finite() {
        return "-".into();
    }
    let v = if v.abs() < 1e-12 { 0.0 } else { v };
    let a = v.abs();
    let (scale, p) = if a == 0.0 {
        (1.0, "")
    } else if a < 1e-6 {
        (1e9, "n")
    } else if a < 1e-3 {
        (1e6, "\u{b5}")
    } else if a < 1.0 {
        (1e3, "m")
    } else if a < 1e3 {
        (1.0, "")
    } else {
        (1e-3, "k")
    };
    let x = v * scale;
    let digits = if x.abs() >= 100.0 { 1 } else if x.abs() >= 10.0 { 2 } else { 3 };
    format!("{:.*} {p}{unit}", digits, x)
}

pub fn fmt_duration(s: f64) -> String {
    if !s.is_finite() || s < 0.0 {
        return "-".into();
    }
    let s = s.round() as u64;
    let (d, h, m, sec) = (s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
    if d > 0 {
        format!("{d}d {h}h {m:02}m")
    } else if h > 0 {
        format!("{h}h {m:02}m")
    } else if m > 0 {
        format!("{m}m {sec:02}s")
    } else {
        format!("{sec}s")
    }
}

pub fn fmt_energy_wh(j: f64) -> String {
    fmt_si(j / 3600.0, "Wh")
}

pub fn fmt_charge_ah(c: f64) -> String {
    fmt_si(c / 3600.0, "Ah")
}

/// Wall-clock label for an absolute Unix time (local timezone).
pub fn fmt_wall(unix_s: f64, with_date: bool) -> String {
    let d = js_sys::Date::new(&wasm_bindgen::JsValue::from_f64(unix_s * 1000.0));
    let hm = format!("{:02}:{:02}", d.get_hours(), d.get_minutes());
    if with_date {
        format!("{:02}/{:02} {hm}", d.get_date(), d.get_month() + 1)
    } else {
        hm
    }
}

const TIME_STEPS: &[f64] = &[
    1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0, 7200.0,
    10800.0, 21600.0, 43200.0, 86400.0, 172800.0, 604800.0, 1209600.0, 2592000.0,
];

/// Tick step for an elapsed-time axis so that ~`target` ticks fit.
pub fn time_step(span: f64, target: f64) -> f64 {
    let raw = span / target.max(1.0);
    *TIME_STEPS.iter().find(|s| **s >= raw).unwrap_or(&2592000.0)
}

/// Elapsed label whose precision follows the tick step ("3d", "2d 06h", "14:30", "05:20").
pub fn fmt_elapsed_tick(t: f64, step: f64) -> String {
    let t = t.max(0.0).round() as u64;
    let (d, h, m, s) = (t / 86400, (t / 3600) % 24, (t / 60) % 60, t % 60);
    if step >= 86400.0 {
        format!("{d}d")
    } else if step >= 3600.0 {
        if d > 0 { format!("{d}d {h:02}h") } else { format!("{h}h") }
    } else if step >= 60.0 {
        if d > 0 { format!("{d}d {h:02}:{m:02}") } else { format!("{h:02}:{m:02}") }
    } else if d > 0 || h > 0 {
        format!("{}:{m:02}:{s:02}", d * 24 + h)
    } else {
        format!("{m:02}:{s:02}")
    }
}

fn nice_step(span: f64, target: f64) -> f64 {
    let raw = (span / target).max(1e-15);
    let mag = 10f64.powf(raw.log10().floor());
    let n = raw / mag;
    let f = if n <= 1.0 { 1.0 } else if n <= 2.0 { 2.0 } else if n <= 5.0 { 5.0 } else { 10.0 };
    f * mag
}

// --------------------------------------------------------------------------
// Rendering
// --------------------------------------------------------------------------
#[derive(Clone, Copy, PartialEq)]
pub enum Lane {
    Voltage,
    Current,
    Power,
    Soc,
}

impl Lane {
    fn label(self) -> &'static str {
        match self {
            Lane::Voltage => "Voltage",
            Lane::Current => "Current",
            Lane::Power => "Power",
            Lane::Soc => "SOC",
        }
    }
    fn unit(self) -> &'static str {
        match self {
            Lane::Voltage => "V",
            Lane::Current => "A",
            Lane::Power => "W",
            Lane::Soc => "%",
        }
    }
    fn color_var(self) -> &'static str {
        match self {
            Lane::Voltage => "--series-voltage",
            Lane::Current => "--series-current",
            Lane::Power => "--series-power",
            Lane::Soc => "--accent",
        }
    }
}

pub struct ChartOpts {
    pub t0: f64,
    pub t1: f64,
    pub lanes: Vec<Lane>,
    pub log_current: bool,
    pub wall_epoch: Option<f64>,
    pub hover_x: Option<f64>,
    pub box_sel: Option<(f64, f64)>,
}

pub struct Layout {
    pub plot_x: f64,
    pub plot_w: f64,
    pub lanes: Vec<(Lane, f64, f64)>, // lane, y top, height
}

pub fn layout(w: f64, h: f64, lanes: &[Lane]) -> Layout {
    let n = lanes.len().max(1) as f64;
    let avail = (h - AXIS_H - LANE_GAP * (n - 1.0)).max(40.0);
    let lh = avail / n;
    Layout {
        plot_x: LEFT_W,
        plot_w: (w - LEFT_W - RIGHT_W).max(10.0),
        lanes: lanes.iter().enumerate().map(|(i, l)| (*l, i as f64 * (lh + LANE_GAP), lh)).collect(),
    }
}

fn css(var: &str) -> String {
    web_sys::window()
        .and_then(|w| w.document())
        .and_then(|d| d.document_element())
        .and_then(|e| web_sys::window()?.get_computed_style(&e).ok().flatten())
        .and_then(|s| s.get_property_value(var).ok())
        .map(|v| v.trim().to_string())
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| "#888".into())
}

fn series(view: &BsView, lane: Lane) -> (&[f64], &[f64], &[f64]) {
    match lane {
        Lane::Voltage => (&view.v_min, &view.v_avg, &view.v_max),
        Lane::Current => (&view.i_min, &view.i_avg, &view.i_max),
        Lane::Power => (&view.p_avg, &view.p_avg, &view.p_avg),
        Lane::Soc => (&view.soc, &view.soc, &view.soc),
    }
}

fn y_range(lo: &[f64], hi: &[f64], log: bool) -> (f64, f64) {
    let f = |x: f64| if log { x.max(1e-10).log10() } else { x };
    // Log axis: zero / negative readings (output off, offset) would stretch it to 1e-10.
    let ok = |x: f64| x.is_finite() && (!log || x > 0.0);
    let mut mn = f64::INFINITY;
    let mut mx = f64::NEG_INFINITY;
    for (&a, &b) in lo.iter().zip(hi) {
        if ok(a) { mn = mn.min(f(a)); }
        if ok(b) { mx = mx.max(f(b)); }
    }
    if log && !mn.is_finite() && mx.is_finite() {
        mn = mx - 3.0;
    }
    if !mn.is_finite() || !mx.is_finite() {
        return if log { (-9.0, 0.0) } else { (0.0, 1.0) };
    }
    if (mx - mn).abs() < 1e-12 {
        let pad = if log { 0.5 } else { mx.abs().max(1e-9) * 0.05 };
        return (mn - pad, mx + pad);
    }
    let pad = (mx - mn) * 0.06;
    (mn - pad, mx + pad)
}

pub fn draw(canvas: &HtmlCanvasElement, view: Option<&BsView>, o: &ChartOpts) {
    let dpr = web_sys::window().map(|w| w.device_pixel_ratio()).unwrap_or(1.0);
    let (cw, ch) = (canvas.client_width() as f64, canvas.client_height() as f64);
    if cw < 10.0 || ch < 10.0 {
        return;
    }
    let (pw, ph) = ((cw * dpr) as u32, (ch * dpr) as u32);
    if canvas.width() != pw || canvas.height() != ph {
        canvas.set_width(pw);
        canvas.set_height(ph);
    }
    let Some(ctx) = canvas
        .get_context("2d")
        .ok()
        .flatten()
        .and_then(|c| c.dyn_into::<CanvasRenderingContext2d>().ok())
    else {
        return;
    };
    let _ = ctx.set_transform(dpr, 0.0, 0.0, dpr, 0.0, 0.0);
    ctx.clear_rect(0.0, 0.0, cw, ch);

    let lab2 = css("--label-2");
    let lab3 = css("--label-3");
    let sep = css("--sep");
    let marker = css("--series-marker");
    ctx.set_font("11px 'JetBrains Mono', monospace");

    let lay = layout(cw, ch, &o.lanes);
    let span = (o.t1 - o.t0).max(1e-9);
    let xof = |t: f64| lay.plot_x + (t - o.t0) / span * lay.plot_w;

    // Time grid + axis labels.
    let step = time_step(span, (lay.plot_w / 110.0).max(2.0));
    let mut t = (o.t0 / step).ceil() * step;
    let axis_y = ch - AXIS_H + 14.0;
    ctx.set_text_align("center");
    while t <= o.t1 {
        let x = xof(t);
        ctx.set_stroke_style_str(&sep);
        ctx.set_line_width(1.0);
        ctx.begin_path();
        ctx.move_to(x.round() + 0.5, 0.0);
        ctx.line_to(x.round() + 0.5, ch - AXIS_H);
        ctx.stroke();
        let label = match o.wall_epoch {
            Some(e) => fmt_wall(e + t, step >= 21600.0),
            None => fmt_elapsed_tick(t, step),
        };
        ctx.set_fill_style_str(&lab2);
        let _ = ctx.fill_text(&label, x, axis_y);
        t += step;
    }

    for &(lane, top, lh) in &lay.lanes {
        let color = css(lane.color_var());
        let log = lane == Lane::Current && o.log_current;
        ctx.set_stroke_style_str(&sep);
        ctx.stroke_rect(lay.plot_x + 0.5, top + 0.5, lay.plot_w - 1.0, lh - 1.0);
        ctx.set_text_align("left");
        let Some(v) = view else {
            ctx.set_fill_style_str(&lab3);
            let _ = ctx.fill_text(lane.label(), lay.plot_x + 6.0, top + 13.0);
            continue;
        };
        if v.t.is_empty() {
            continue;
        }
        let (lo, avg, hi) = series(v, lane);
        let (y0, y1) = if lane == Lane::Soc && !log { let (a, b) = y_range(lo, hi, false); (a.max(-2.0), b.min(102.0)) } else { y_range(lo, hi, log) };
        let fy = |x: f64| {
            // Log: readings at or below the floor sit on the bottom edge instead of vanishing.
            let x = if log { x.max(10f64.powf(y0)).log10() } else { x };
            top + lh - (x - y0) / (y1 - y0) * lh
        };

        // Y grid + labels.
        ctx.set_text_align("right");
        if log {
            let every = ((y1 - y0) / (lh / 24.0).max(1.0)).ceil().max(1.0) as i64;
            let mut d = y0.ceil();
            while d <= y1 {
                if (d as i64).rem_euclid(every) != 0 {
                    d += 1.0;
                    continue;
                }
                let y = fy(10f64.powf(d));
                ctx.set_stroke_style_str(&sep);
                ctx.begin_path();
                ctx.move_to(lay.plot_x, y.round() + 0.5);
                ctx.line_to(lay.plot_x + lay.plot_w, y.round() + 0.5);
                ctx.stroke();
                ctx.set_fill_style_str(&lab2);
                let _ = ctx.fill_text(&fmt_si(10f64.powf(d), lane.unit()), lay.plot_x - 6.0, y + 4.0);
                d += 1.0;
            }
        } else {
            let st = nice_step(y1 - y0, (lh / 28.0).clamp(2.0, 6.0));
            let mut y = (y0 / st).ceil() * st;
            while y <= y1 {
                let py = fy(y);
                ctx.set_stroke_style_str(&sep);
                ctx.begin_path();
                ctx.move_to(lay.plot_x, py.round() + 0.5);
                ctx.line_to(lay.plot_x + lay.plot_w, py.round() + 0.5);
                ctx.stroke();
                ctx.set_fill_style_str(&lab2);
                let label = if lane == Lane::Soc { format!("{y:.0} %") } else { fmt_si(y, lane.unit()) };
                let _ = ctx.fill_text(&label, lay.plot_x - 6.0, py + 4.0);
                y += st;
            }
        }

        ctx.save();
        ctx.begin_path();
        ctx.rect(lay.plot_x, top, lay.plot_w, lh);
        ctx.clip();
        let segs = step_segments(&v.t, &v.dt);
        // Min/max envelope: one column per interval, so no diagonal artefacts.
        if !std::ptr::eq(lo, avg) {
            ctx.set_global_alpha(0.22);
            ctx.set_fill_style_str(&color);
            for (i, &(a, b, _)) in segs.iter().enumerate() {
                let (xa, xb) = (xof(a), xof(b));
                let (ya, yb) = (fy(hi[i]), fy(lo[i]));
                ctx.fill_rect(xa, ya.min(yb), (xb - xa).max(1.0), (yb - ya).abs().max(1.0));
            }
            ctx.set_global_alpha(1.0);
        }
        ctx.set_stroke_style_str(&color);
        ctx.set_line_width(1.5);
        ctx.begin_path();
        for (i, &(a, b, joined)) in segs.iter().enumerate() {
            let y = fy(avg[i]);
            if joined { ctx.line_to(xof(a), y) } else { ctx.move_to(xof(a), y) }
            ctx.line_to(xof(b), y);
        }
        ctx.stroke();
        ctx.restore();
        // Lane title on a backdrop so traces never run through it.
        let tw = ctx.measure_text(lane.label()).map(|m| m.width()).unwrap_or(40.0);
        ctx.set_fill_style_str(&css("--surface-plot"));
        ctx.fill_rect(lay.plot_x + 1.0, top + 1.0, tw + 12.0, 17.0);
        ctx.set_text_align("left");
        ctx.set_fill_style_str(&lab2);
        let _ = ctx.fill_text(lane.label(), lay.plot_x + 6.0, top + 13.0);
    }

    // Run events across every lane.
    if let Some(v) = view {
        ctx.set_stroke_style_str(&marker);
        ctx.set_fill_style_str(&marker);
        ctx.set_text_align("left");
        let mut last_label = f64::NEG_INFINITY;
        for e in &v.events {
            let x = xof(e.t_s as f64).round() + 0.5;
            let _ = ctx.set_line_dash(&js_sys::Array::of2(&3.0.into(), &3.0.into()));
            ctx.begin_path();
            ctx.move_to(x, 0.0);
            ctx.line_to(x, ch - AXIS_H);
            ctx.stroke();
            let _ = ctx.set_line_dash(&js_sys::Array::new());
            if x - last_label < 48.0 {
                continue;
            }
            let label = event_name(e);
            let wl = ctx.measure_text(label).map(|m| m.width()).unwrap_or(40.0);
            let lx = if x + 3.0 + wl > lay.plot_x + lay.plot_w { x - 3.0 - wl } else { x + 3.0 };
            let _ = ctx.fill_text(label, lx, ch - AXIS_H - 4.0);
            last_label = x;
        }
    }

    if let Some((a, b)) = o.box_sel {
        ctx.set_fill_style_str(&css("--accent-tint-strong"));
        ctx.fill_rect(a.min(b), 0.0, (b - a).abs(), ch - AXIS_H);
    }
    if let Some(hx) = o.hover_x {
        if hx >= lay.plot_x && hx <= lay.plot_x + lay.plot_w {
            ctx.set_stroke_style_str(&lab3);
            ctx.set_line_width(1.0);
            ctx.begin_path();
            ctx.move_to(hx.round() + 0.5, 0.0);
            ctx.line_to(hx.round() + 0.5, ch - AXIS_H);
            ctx.stroke();
        }
    }
}

/// Interval [start, end] of each point and whether it joins the previous one; a hole
/// longer than half an interval (missing data) breaks the trace.
pub fn step_segments(t: &[f64], dt: &[f64]) -> Vec<(f64, f64, bool)> {
    let mut prev_end = f64::NEG_INFINITY;
    t.iter()
        .enumerate()
        .map(|(i, &c)| {
            let w = dt.get(i).copied().filter(|w| *w > 0.0).unwrap_or(60.0);
            let (a, b) = (c - w / 2.0, c + w / 2.0);
            let joined = (a - prev_end).abs() <= w * 0.5;
            prev_end = b;
            (a, b, joined)
        })
        .collect()
}

pub fn event_name(e: &BsEvent) -> &'static str {
    match e.code {
        1 => "created",
        2 => "start",
        3 => "pause",
        4 => "stop",
        5 => "depleted",
        6 => "reboot",
        7 => "param",
        8 => "stall",
        9 => "output off",
        10 => "PD lost",
        11 => "store error",
        _ => "event",
    }
}
