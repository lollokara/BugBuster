#![allow(dead_code)]

use leptos::prelude::*;

/// Glass card wrapper
#[component]
pub fn Card(
    children: Children,
    #[prop(default = "")] class: &'static str,
) -> impl IntoView {
    view! {
        <div class=format!("card {}", class)>
            {children()}
        </div>
    }
}

/// Channel card with header
#[component]
pub fn ChannelCard(
    children: Children,
    #[prop(into)] channel: usize,
    #[prop(into)] func_name: Signal<String>,
    #[prop(default = "")] class: &'static str,
) -> impl IntoView {
    let ch_names = ["A", "B", "C", "D"];
    let label = if channel < 4 { ch_names[channel] } else { "?" };
    view! {
        <div class=format!("card channel-card {}", class)>
            <div class="card-header">
                <span class="channel-label">{format!("CH {}", label)}</span>
                <span class="channel-func">{move || func_name.get()}</span>
            </div>
            <div class="card-body">
                {children()}
            </div>
        </div>
    }
}

/// Tab bar navigation
#[component]
pub fn TabBar(
    #[prop(into)] active: Signal<String>,
    #[prop(into)] on_select: Callback<String>,
    tabs: Vec<(&'static str, &'static str)>, // (id, label)
) -> impl IntoView {
    view! {
        <nav class="tab-bar">
            {tabs.into_iter().map(|(id, label)| {
                let id_str = id.to_string();
                let id_click = id_str.clone();
                view! {
                    <button
                        class="tab-item"
                        class:active=move || active.get() == id_str
                        on:click=move |_| on_select.run(id_click.clone())
                    >
                        {label}
                    </button>
                }
            }).collect::<Vec<_>>()}
        </nav>
    }
}
