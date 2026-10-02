//! Ctrl/Cmd-K command palette: jump to any view or run a global action.

use leptos::ev;
use leptos::html;
use leptos::prelude::*;

use crate::components::icons::Icon;

#[derive(Clone, Debug, PartialEq)]
pub struct PaletteEntry {
    pub id: String,
    pub label: String,
    pub group: &'static str,
    pub icon: &'static str,
    pub hint: String,
}

/// Case-insensitive subsequence match; lower score is better.
fn score(query: &str, text: &str) -> Option<usize> {
    if query.is_empty() {
        return Some(0);
    }
    let text = text.to_lowercase();
    if let Some(pos) = text.find(query) {
        return Some(pos);
    }
    let mut it = text.chars().enumerate();
    let mut last = 0;
    for q in query.chars() {
        let (i, _) = it.find(|(_, c)| *c == q)?;
        last = i;
    }
    Some(100 + last)
}

#[component]
pub fn CommandPalette(
    open: RwSignal<bool>,
    #[prop(into)] entries: Signal<Vec<PaletteEntry>>,
    #[prop(into)] on_run: Callback<String>,
) -> impl IntoView {
    let query = RwSignal::new(String::new());
    let cursor = RwSignal::new(0usize);
    let input_ref = NodeRef::<html::Input>::new();

    let results = Memo::new(move |_| {
        let q = query.get().trim().to_lowercase();
        let mut v: Vec<(usize, usize, PaletteEntry)> = entries
            .get()
            .into_iter()
            .enumerate()
            .filter_map(|(i, e)| {
                let s = score(&q, &e.label).or_else(|| score(&q, e.group).map(|s| s + 200))?;
                Some((s, i, e))
            })
            .collect();
        v.sort_by_key(|(s, i, _)| (*s, *i));
        v.into_iter().map(|(_, _, e)| e).collect::<Vec<_>>()
    });

    Effect::new(move |_| {
        if open.get() {
            query.set(String::new());
            cursor.set(0);
            request_animation_frame(move || {
                if let Some(el) = input_ref.get_untracked() {
                    let _ = el.focus();
                }
            });
        }
    });

    let run = move |id: String| {
        open.set(false);
        on_run.run(id);
    };

    let on_key = move |e: ev::KeyboardEvent| {
        let n = results.with_untracked(|r| r.len());
        match e.key().as_str() {
            "ArrowDown" => {
                e.prevent_default();
                if n > 0 {
                    cursor.update(|c| *c = (*c + 1) % n);
                }
            }
            "ArrowUp" => {
                e.prevent_default();
                if n > 0 {
                    cursor.update(|c| *c = (*c + n - 1) % n);
                }
            }
            "Enter" => {
                e.prevent_default();
                let id = results.with_untracked(|r| r.get(cursor.get_untracked()).map(|e| e.id.clone()));
                if let Some(id) = id {
                    run(id);
                }
            }
            "Escape" => {
                e.prevent_default();
                open.set(false);
            }
            _ => {}
        }
    };

    view! {
        <Show when=move || open.get()>
            <div class="scrim" on:click=move |_| open.set(false)></div>
            <div class="dialog popover palette" role="dialog" aria-modal="true" aria-label="Command palette">
                <div class="palette-input">
                    <Icon name="search" size=18 />
                    <input
                        node_ref=input_ref
                        type="text"
                        placeholder="Go to view or run a command"
                        aria-label="Search views and commands"
                        prop:value=move || query.get()
                        on:input=move |e| { query.set(event_target_value(&e)); cursor.set(0); }
                        on:keydown=on_key
                    />
                </div>
                <div class="palette-list" role="listbox">
                    {move || {
                        let items = results.get();
                        if items.is_empty() {
                            return view! { <div class="palette-empty">"No matches"</div> }.into_any();
                        }
                        let mut last_group = "";
                        items.into_iter().enumerate().map(|(i, e)| {
                            let header = (e.group != last_group).then(|| {
                                last_group = e.group;
                                view! { <div class="palette-group">{e.group}</div> }
                            });
                            let id = e.id.clone();
                            view! {
                                {header}
                                <button
                                    type="button"
                                    role="option"
                                    class="palette-item"
                                    class:active=move || cursor.get() == i
                                    aria-selected=move || if cursor.get() == i { "true" } else { "false" }
                                    on:mousemove=move |_| if cursor.get_untracked() != i { cursor.set(i) }
                                    on:click=move |_| run(id.clone())
                                >
                                    <Icon name=e.icon size=16 />
                                    <span>{e.label.clone()}</span>
                                    <span class="palette-hint">{e.hint.clone()}</span>
                                </button>
                            }
                        }).collect::<Vec<_>>().into_any()
                    }}
                </div>
                <div class="palette-footer">
                    <span><kbd>"↑"</kbd><kbd>"↓"</kbd>" Navigate"</span>
                    <span><kbd>"↵"</kbd>" Open"</span>
                    <span><kbd>"Esc"</kbd>" Close"</span>
                </div>
            </div>
        </Show>
    }
}
