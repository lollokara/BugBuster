#![allow(dead_code)]

use leptos::ev;
use leptos::prelude::*;

/// Range slider with value display
#[component]
pub fn Slider(
    #[prop(into)] value: Signal<f64>,
    #[prop(into)] on_input: Callback<f64>,
    #[prop(into)] on_change: Callback<f64>,
    #[prop(default = 0.0)] min: f64,
    #[prop(default = 100.0)] max: f64,
    #[prop(default = 1.0)] step: f64,
    #[prop(default = "")] class: &'static str,
) -> impl IntoView {
    view! {
        <input
            type="range"
            class=format!("slider {}", class)
            min=min
            max=max
            step=step
            prop:value=move || value.get()
            on:input=move |e: ev::Event| {
                if let Ok(v) = event_target_value(&e).parse::<f64>() {
                    on_input.run(v);
                }
            }
            on:change=move |e: ev::Event| {
                if let Ok(v) = event_target_value(&e).parse::<f64>() {
                    on_change.run(v);
                }
            }
        />
    }
}

/// Toggle switch with label
#[component]
pub fn Toggle(
    #[prop(into)] checked: Signal<bool>,
    #[prop(into)] on_change: Callback<bool>,
    #[prop(default = "")] label: &'static str,
    #[prop(default = "")] label_off: &'static str,
    #[prop(default = "")] label_on: &'static str,
) -> impl IntoView {
    let off = if label_off.is_empty() { "Off" } else { label_off };
    let on = if label_on.is_empty() { "On" } else { label_on };
    view! {
        <label class="toggle-wrap">
            {if !label.is_empty() { Some(view! { <span class="toggle-label">{label}</span> }) } else { None }}
            <span class="toggle-off-label">{off}</span>
            <div class="toggle" class:active=move || checked.get()
                on:click=move |_| on_change.run(!checked.get_untracked())
            >
                <div class="toggle-thumb"></div>
            </div>
            <span class="toggle-on-label">{on}</span>
        </label>
    }
}

/// Dropdown select
#[component]
pub fn Dropdown(
    #[prop(into)] value: Signal<String>,
    #[prop(into)] on_change: Callback<String>,
    options: Vec<(String, String)>, // (value, label)
    #[prop(default = "")] class: &'static str,
) -> impl IntoView {
    let opts = options.clone();
    view! {
        <select
            class=format!("dropdown {}", class)
            prop:value=move || value.get()
            on:change=move |e: ev::Event| {
                on_change.run(event_target_value(&e));
            }
        >
            {opts.into_iter().map(|(val, label)| {
                view! { <option value=val>{label}</option> }
            }).collect::<Vec<_>>()}
        </select>
    }
}

/// Number input with optional Set button
#[component]
pub fn NumberInput(
    #[prop(into)] value: Signal<f64>,
    #[prop(into)] on_change: Callback<f64>,
    #[prop(default = 0.0)] min: f64,
    #[prop(default = 100.0)] max: f64,
    #[prop(default = 0.001)] step: f64,
    #[prop(default = "")] unit: &'static str,
    #[prop(default = true)] show_button: bool,
) -> impl IntoView {
    let (local_val, set_local_val) = signal(0.0f64);

    // Sync local from prop on mount
    Effect::new(move || {
        set_local_val.set(value.get());
    });

    view! {
        <div class="number-input-wrap">
            <input
                type="number"
                class="number-input"
                min=min max=max step=step
                prop:value=move || format!("{:.3}", local_val.get())
                on:input=move |e: ev::Event| {
                    if let Ok(v) = event_target_value(&e).parse::<f64>() {
                        set_local_val.set(v);
                    }
                }
            />
            {if !unit.is_empty() {
                Some(view! { <span class="number-unit">{unit}</span> })
            } else { None }}
            {if show_button {
                Some(view! {
                    <button class="btn btn-sm btn-primary"
                        on:click=move |_| on_change.run(local_val.get_untracked())
                    >"Set"</button>
                })
            } else { None }}
        </div>
    }
}
