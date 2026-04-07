#![allow(dead_code)]

use leptos::prelude::*;

/// Large value display with unit
#[component]
pub fn BigValue(
    #[prop(into)] value: Signal<String>,
    #[prop(default = "")] unit: &'static str,
    #[prop(default = "")] class: &'static str,
) -> impl IntoView {
    view! {
        <div class=format!("big-value {}", class)>
            {move || value.get()}
            {if !unit.is_empty() {
                Some(view! { <span class="unit">{unit}</span> })
            } else { None }}
        </div>
    }
}

/// Horizontal bar gauge
#[component]
pub fn BarGauge(
    #[prop(into)] percent: Signal<f64>,
    #[prop(default = "blue")] color: &'static str,
) -> impl IntoView {
    let color_class = format!("bar-gauge bar-{}", color);
    view! {
        <div class=color_class>
            <div class="bar-fill" style=move || format!("width: {}%", percent.get().clamp(0.0, 100.0))></div>
        </div>
    }
}

/// Status LED indicator
#[component]
pub fn Led(
    #[prop(into)] on: Signal<bool>,
    #[prop(default = "green")] color: &'static str,
    #[prop(default = "")] label: &'static str,
    #[prop(default = false)] large: bool,
) -> impl IntoView {
    let size_class = if large { "led led-large" } else { "led" };
    view! {
        <div class="led-wrap">
            <div class=size_class
                class:led-on=move || on.get()
                style=move || if on.get() {
                    format!("background: {}; box-shadow: 0 0 8px {}", color, color)
                } else {
                    String::new()
                }
            ></div>
            {if !label.is_empty() {
                Some(view! { <span class="led-label">{label}</span> })
            } else { None }}
        </div>
    }
}

/// Temperature gauge (vertical bar)
#[component]
pub fn TempGauge(
    #[prop(into)] temp: Signal<f32>,
) -> impl IntoView {
    let percent = move || ((temp.get() + 25.0) / 150.0 * 100.0).clamp(0.0, 100.0);
    let color = move || {
        let t = temp.get();
        if t > 100.0 { "var(--rose)" }
        else if t > 70.0 { "var(--amber)" }
        else { "var(--green)" }
    };
    let status = move || {
        let t = temp.get();
        if t > 100.0 { "HOT" }
        else if t > 70.0 { "Warm" }
        else { "Normal" }
    };

    view! {
        <div class="temp-gauge">
            <div class="temp-value-large">{move || format!("{:.1}", temp.get())}<span class="unit">"°C"</span></div>
            <div class="temp-bar-container">
                <div class="temp-bar-fill" style=move || format!("height: {}%; background: {}", percent(), color())></div>
            </div>
            <div class="temp-status" style=move || format!("color: {}", color())>{status}</div>
        </div>
    }
}
