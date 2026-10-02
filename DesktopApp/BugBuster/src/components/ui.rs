//! Design-system primitives with behaviour. Pure-markup patterns (groups,
//! rows, badges, buttons) are plain CSS classes; see styles/components.css.

use leptos::prelude::*;

use crate::components::icons::Icon;

/// macOS-style segmented control.
#[component]
pub fn SegmentedControl<T>(
    options: Vec<(T, &'static str)>,
    #[prop(into)] value: Signal<T>,
    #[prop(into)] on_change: Callback<T>,
    #[prop(optional)] block: bool,
    #[prop(optional)] small: bool,
    #[prop(optional)] aria_label: &'static str,
) -> impl IntoView
where
    T: Clone + PartialEq + Send + Sync + 'static,
{
    let class = format!(
        "seg{}{}",
        if block { " seg-block" } else { "" },
        if small { " seg-sm" } else { "" }
    );
    view! {
        <div class=class role="radiogroup" aria-label=aria_label>
            {options.into_iter().map(|(v, label)| {
                let v_active = v.clone();
                let v_click = v.clone();
                let is_active = move || value.get() == v_active;
                view! {
                    <button
                        type="button"
                        role="radio"
                        class:active=is_active.clone()
                        aria-checked=move || if is_active() { "true" } else { "false" }
                        on:click=move |_| on_change.run(v_click.clone())
                    >{label}</button>
                }
            }).collect::<Vec<_>>()}
        </div>
    }
}

/// On/off switch (role="switch").
#[component]
pub fn Switch(
    #[prop(into)] checked: Signal<bool>,
    #[prop(into)] on_change: Callback<bool>,
    #[prop(optional)] aria_label: &'static str,
    #[prop(optional, into)] disabled: Signal<bool>,
) -> impl IntoView {
    view! {
        <button
            type="button"
            role="switch"
            class="switch"
            class:on=move || checked.get()
            aria-checked=move || if checked.get() { "true" } else { "false" }
            aria-label=aria_label
            disabled=move || disabled.get()
            on:click=move |_| on_change.run(!checked.get_untracked())
        ></button>
    }
}

/// Label + large tabular value + unit.
#[component]
pub fn Readout(
    label: &'static str,
    #[prop(into)] value: Signal<String>,
    #[prop(optional)] unit: &'static str,
    /// "", "sm" or "lg"
    #[prop(optional)] size: &'static str,
    #[prop(optional, into)] tone: Signal<&'static str>,
) -> impl IntoView {
    let class = if size.is_empty() {
        "readout".to_string()
    } else {
        format!("readout readout-{size}")
    };
    view! {
        <div class=class>
            <span class="readout-label">{label}</span>
            <span class=move || {
                let t = tone.get();
                if t.is_empty() { "readout-value".to_string() } else { format!("readout-value tone-text-{t}") }
            }>
                {move || value.get()}
                {(!unit.is_empty()).then(|| view! { <span class="unit">{unit}</span> })}
            </span>
        </div>
    }
}

/// Inline banner. `tone`: "", "blue", "orange", "red", "green".
#[component]
pub fn Callout(
    #[prop(optional)] tone: &'static str,
    #[prop(optional)] icon: &'static str,
    children: Children,
) -> impl IntoView {
    let icon = if icon.is_empty() {
        match tone {
            "orange" | "red" => "triangle-alert",
            "green" => "circle-check",
            _ => "info",
        }
    } else {
        icon
    };
    view! {
        <div class=format!("callout tone-{tone}") role=if tone == "red" { "alert" } else { "note" }>
            <Icon name=icon size=15 />
            <div>{children()}</div>
        </div>
    }
}

#[component]
pub fn EmptyState(
    icon: &'static str,
    title: &'static str,
    #[prop(optional)] message: &'static str,
    #[prop(optional)] children: Option<Children>,
) -> impl IntoView {
    view! {
        <div class="empty-state">
            <Icon name=icon size=32 />
            <h3>{title}</h3>
            {(!message.is_empty()).then(|| view! { <p>{message}</p> })}
            {children.map(|c| c())}
        </div>
    }
}
