use leptos::html;
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::{Deserialize, Serialize};
use wasm_bindgen::prelude::*;

use crate::tauri_bridge::*;

// -----------------------------------------------------------------------------
// Mirror types from scripts_commands.rs
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ScriptFileInfo {
    pub name: String,
    pub size: u32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ScriptStorageInfo {
    pub total_bytes: u64,
    pub used_bytes: u64,
    pub free_bytes: u64,
    pub script_count: u32,
    pub max_script_bytes: u32,
    pub max_scripts: u32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ScriptStatus {
    pub running: bool,
    pub current_script_id: u32,
    pub total_runs: u32,
    pub total_errors: u32,
    pub last_error: Option<String>,
    pub mode: String,
    pub idle_for_ms: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ScriptLogsResult {
    pub text: String,
    pub next_cursor: u64,
}

// -----------------------------------------------------------------------------
// Invoke helpers
// -----------------------------------------------------------------------------

async fn scripts_list_files_invoke() -> Vec<ScriptFileInfo> {
    try_invoke("scripts_list_files", JsValue::NULL)
        .await
        .and_then(|v| serde_wasm_bindgen::from_value(v).ok())
        .unwrap_or_default()
}

async fn scripts_get_file_invoke(name: &str) -> Option<String> {
    #[derive(Serialize)]
    struct Args<'a> {
        name: &'a str,
    }
    let args = serde_wasm_bindgen::to_value(&Args { name }).ok()?;
    try_invoke("scripts_get_file", args)
        .await
        .and_then(|v| v.as_string())
}

async fn scripts_save_file_invoke(name: &str, source: &str) -> bool {
    #[derive(Serialize)]
    struct Args<'a> {
        name: &'a str,
        source: &'a str,
    }
    let args = match serde_wasm_bindgen::to_value(&Args { name, source }) {
        Ok(v) => v,
        Err(_) => return false,
    };
    try_invoke("scripts_save_file", args).await.is_some()
}

async fn scripts_delete_file_invoke(name: &str) -> bool {
    #[derive(Serialize)]
    struct Args<'a> {
        name: &'a str,
    }
    let args = match serde_wasm_bindgen::to_value(&Args { name }) {
        Ok(v) => v,
        Err(_) => return false,
    };
    try_invoke("scripts_delete_file", args).await.is_some()
}

async fn scripts_get_storage_invoke() -> Option<ScriptStorageInfo> {
    try_invoke("scripts_get_storage", JsValue::NULL)
        .await
        .and_then(|v| serde_wasm_bindgen::from_value(v).ok())
}

async fn scripts_run_file_invoke(name: &str) -> bool {
    #[derive(Serialize)]
    struct Args<'a> {
        name: &'a str,
    }
    let args = match serde_wasm_bindgen::to_value(&Args { name }) {
        Ok(v) => v,
        Err(_) => return false,
    };
    try_invoke("scripts_run_file", args).await.is_some()
}

async fn scripts_stop_invoke() {
    try_invoke("scripts_stop", JsValue::NULL).await;
}

async fn scripts_reset_vm_invoke() {
    try_invoke("scripts_reset_vm", JsValue::NULL).await;
}

async fn scripts_get_status_invoke() -> Option<ScriptStatus> {
    try_invoke("scripts_get_status", JsValue::NULL)
        .await
        .and_then(|v| serde_wasm_bindgen::from_value(v).ok())
}

async fn scripts_get_logs_invoke(since: Option<u64>) -> Option<ScriptLogsResult> {
    #[derive(Serialize)]
    struct Args {
        since: Option<u64>,
    }
    let args = serde_wasm_bindgen::to_value(&Args { since }).ok()?;
    try_invoke("scripts_get_logs", args)
        .await
        .and_then(|v| serde_wasm_bindgen::from_value(v).ok())
}

async fn scripts_repl_connect_invoke() -> bool {
    try_invoke("scripts_repl_connect", JsValue::NULL)
        .await
        .is_some()
}

async fn scripts_repl_send_invoke(text: &str) -> bool {
    #[derive(Serialize)]
    struct Args<'a> {
        text: &'a str,
    }
    let args = match serde_wasm_bindgen::to_value(&Args { text }) {
        Ok(v) => v,
        Err(_) => return false,
    };
    try_invoke("scripts_repl_send", args).await.is_some()
}

async fn scripts_repl_disconnect_invoke() {
    try_invoke("scripts_repl_disconnect", JsValue::NULL).await;
}

// -----------------------------------------------------------------------------
// Panel enum
// -----------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Debug)]
enum Panel {
    Files,
    Editor,
    Repl,
}

// -----------------------------------------------------------------------------
// Main component
// -----------------------------------------------------------------------------

#[component]
pub fn ScriptsTab() -> impl IntoView {
    let panel = RwSignal::new(Panel::Files);

    let files: RwSignal<Vec<ScriptFileInfo>> = RwSignal::new(vec![]);
    let storage: RwSignal<Option<ScriptStorageInfo>> = RwSignal::new(None);
    let editor_name: RwSignal<String> = RwSignal::new("untitled.py".to_string());
    let editor_content: RwSignal<String> = RwSignal::new(String::new());
    let editor_dirty: RwSignal<bool> = RwSignal::new(false);
    let script_status: RwSignal<Option<ScriptStatus>> = RwSignal::new(None);
    let repl_output: RwSignal<Vec<String>> = RwSignal::new(vec![]);
    let repl_connected: RwSignal<bool> = RwSignal::new(false);
    let repl_input: RwSignal<String> = RwSignal::new(String::new());
    let new_file_name: RwSignal<String> = RwSignal::new(String::new());
    let show_new_dialog: RwSignal<bool> = RwSignal::new(false);
    let logs_cursor: RwSignal<Option<u64>> = RwSignal::new(None);

    // NodeRefs for iOS autocorrect/autocapitalize suppression
    let editor_ref = NodeRef::<html::Textarea>::new();
    let repl_input_ref = NodeRef::<html::Input>::new();

    Effect::new(move |_| {
        if let Some(el) = editor_ref.get() {
            let _ = el.set_attribute("autocorrect", "off");
            let _ = el.set_attribute("autocapitalize", "none");
            let _ = el.set_attribute("autocomplete", "off");
        }
    });
    Effect::new(move |_| {
        if let Some(el) = repl_input_ref.get() {
            let _ = el.set_attribute("autocorrect", "off");
            let _ = el.set_attribute("autocapitalize", "none");
            let _ = el.set_attribute("autocomplete", "off");
        }
    });

    // Load files on mount
    spawn_local(async move {
        let f = scripts_list_files_invoke().await;
        files.set(f);
        let s = scripts_get_storage_invoke().await;
        storage.set(s);
    });

    // Listen for REPL output and disconnect events
    spawn_local(async move {
        let closure_out = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
            if let Some(text) = js_sys::Reflect::get(&event, &"payload".into())
                .ok()
                .and_then(|v| v.as_string())
            {
                repl_output.update(|lines| {
                    for line in text.split('\n') {
                        lines.push(line.to_string());
                    }
                    if lines.len() > 500 {
                        *lines = lines.split_off(lines.len() - 500);
                    }
                });
                // Auto-scroll: set a flag via a DOM call
                if let Some(el) = web_sys::window()
                    .and_then(|w| w.document())
                    .and_then(|d| d.get_element_by_id("repl-output-scroll"))
                {
                    el.set_scroll_top(el.scroll_height());
                }
            }
        });
        listen("repl-output", &closure_out).await;
        closure_out.forget();

        let closure_dc = Closure::<dyn FnMut(JsValue)>::new(move |_: JsValue| {
            repl_connected.set(false);
        });
        listen("repl-disconnected", &closure_dc).await;
        closure_dc.forget();
    });

    // Status + log polling loop (1 s)
    {
        let alive = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
        let alive_clean = alive.clone();
        on_cleanup(move || {
            alive_clean.store(false, std::sync::atomic::Ordering::Relaxed);
            spawn_local(async move { scripts_repl_disconnect_invoke().await; });
        });
        let alive_poll = alive.clone();
        spawn_local(async move {
            loop {
                if !alive_poll.load(std::sync::atomic::Ordering::Relaxed) {
                    break;
                }
                let p = panel.get_untracked();
                if p == Panel::Editor || p == Panel::Repl {
                    if let Some(s) = scripts_get_status_invoke().await {
                        script_status.set(Some(s));
                    }
                }
                if p == Panel::Repl && !repl_connected.get_untracked() {
                    let cursor = logs_cursor.get_untracked();
                    if let Some(result) = scripts_get_logs_invoke(cursor).await {
                        if !result.text.is_empty() {
                            let nc = result.next_cursor;
                            repl_output.update(|lines| {
                                for line in result.text.lines() {
                                    lines.push(line.to_string());
                                }
                                if lines.len() > 500 {
                                    *lines = lines.split_off(lines.len() - 500);
                                }
                            });
                            if nc > 0 {
                                logs_cursor.set(Some(nc));
                            }
                        }
                    }
                }
                sleep_ms(1000).await;
            }
        });
    }

    let do_refresh = move || {
        spawn_local(async move {
            files.set(scripts_list_files_invoke().await);
            storage.set(scripts_get_storage_invoke().await);
        });
    };

    // -------------------------------------------------------------------------
    // View
    // -------------------------------------------------------------------------
    view! {
        <div class="scripts-tab">
            // Sub-tab switcher
            <div class="scripts-sub-tabs">
                <button class="scripts-sub-tab" class:active=move || panel.get() == Panel::Files
                    on:click=move |_| panel.set(Panel::Files)>"Files"</button>
                <button class="scripts-sub-tab" class:active=move || panel.get() == Panel::Editor
                    on:click=move |_| panel.set(Panel::Editor)>"Editor"</button>
                <button class="scripts-sub-tab" class:active=move || panel.get() == Panel::Repl
                    on:click=move |_| panel.set(Panel::Repl)>"REPL"</button>
            </div>

            {move || match panel.get() {

                // ---- FILES ----
                Panel::Files => {
                    let running = script_status.get().map(|s| s.running).unwrap_or(false);
                    view! {
                        <div class="scripts-panel">
                            {move || storage.get().map(|s| {
                                let pct = if s.total_bytes > 0 {
                                    (s.used_bytes as f64 / s.total_bytes as f64 * 100.0) as u32
                                } else { 0 };
                                view! {
                                    <div class="scripts-storage">
                                        <div class="scripts-storage-label">
                                            <span>"Storage"</span>
                                            <span class="scripts-storage-nums">
                                                {format!("{} / {} KB · {} files",
                                                    s.used_bytes / 1024,
                                                    s.total_bytes / 1024,
                                                    s.script_count)}
                                            </span>
                                        </div>
                                        <div class="storage-bar-track">
                                            <div class="storage-bar-fill"
                                                style=format!("width: {}%", pct.min(100))>
                                            </div>
                                        </div>
                                    </div>
                                }
                            })}

                            <div class="scripts-file-list">
                                {move || {
                                    let list = files.get();
                                    if list.is_empty() {
                                        view! {
                                            <div class="scripts-empty">
                                                "No scripts yet. Tap + to create one."
                                            </div>
                                        }.into_any()
                                    } else {
                                        list.into_iter().map(|f| {
                                            let fname = f.name.clone();
                                            let fn_load = fname.clone();
                                            let fn_run  = fname.clone();
                                            let fn_del  = fname.clone();
                                            view! {
                                                <div class="scripts-file-card">
                                                    <div class="scripts-file-info">
                                                        <span class="scripts-file-name">{fname}</span>
                                                        <span class="scripts-file-size">
                                                            {format!("{} B", f.size)}
                                                        </span>
                                                    </div>
                                                    <div class="scripts-file-actions">
                                                        <button class="btn btn-xs btn-primary"
                                                            on:click=move |_| {
                                                                let n = fn_load.clone();
                                                                spawn_local(async move {
                                                                    if let Some(src) = scripts_get_file_invoke(&n).await {
                                                                        editor_name.set(n.clone());
                                                                        editor_content.set(src);
                                                                        editor_dirty.set(false);
                                                                        panel.set(Panel::Editor);
                                                                    }
                                                                });
                                                            }
                                                        >"Edit"</button>
                                                        <button class="btn btn-xs btn-success"
                                                            prop:disabled=move || running
                                                            on:click=move |_| {
                                                                let n = fn_run.clone();
                                                                spawn_local(async move {
                                                                    scripts_run_file_invoke(&n).await;
                                                                    panel.set(Panel::Repl);
                                                                });
                                                            }
                                                        >"▶"</button>
                                                        <button class="btn btn-xs btn-danger"
                                                            on:click=move |_| {
                                                                let n = fn_del.clone();
                                                                spawn_local(async move {
                                                                    scripts_delete_file_invoke(&n).await;
                                                                    do_refresh();
                                                                });
                                                            }
                                                        >"✕"</button>
                                                    </div>
                                                </div>
                                            }
                                        }).collect::<Vec<_>>().into_any()
                                    }
                                }}
                            </div>

                            <button class="scripts-fab"
                                on:click=move |_| {
                                    new_file_name.set("script.py".to_string());
                                    show_new_dialog.set(true);
                                }
                            >"+"</button>

                            <Show when=move || show_new_dialog.get()>
                                <div class="scripts-modal-backdrop"
                                    on:click=move |_| show_new_dialog.set(false)>
                                </div>
                                <div class="scripts-modal">
                                    <div class="scripts-modal-title">"New Script"</div>
                                    <input class="scripts-input" placeholder="filename.py"
                                        prop:value=move || new_file_name.get()
                                        on:input=move |e| new_file_name.set(event_target_value(&e))
                                    />
                                    <div class="scripts-modal-actions">
                                        <button class="btn btn-primary" on:click=move |_| {
                                            let name = new_file_name.get();
                                            if name.is_empty() { return; }
                                            spawn_local(async move {
                                                scripts_save_file_invoke(&name, "# New script\n").await;
                                                do_refresh();
                                                editor_name.set(name.clone());
                                                editor_content.set("# New script\n".to_string());
                                                editor_dirty.set(false);
                                                show_new_dialog.set(false);
                                                panel.set(Panel::Editor);
                                            });
                                        }>"Create"</button>
                                        <button class="btn"
                                            on:click=move |_| show_new_dialog.set(false)
                                        >"Cancel"</button>
                                    </div>
                                </div>
                            </Show>
                        </div>
                    }.into_any()
                }

                // ---- EDITOR ----
                Panel::Editor => {
                    let running = script_status.get().map(|s| s.running).unwrap_or(false);
                    let (status_color, status_text) = if running {
                        ("#4ade80", "Running")
                    } else {
                        ("#94a3b8", "Idle")
                    };
                    view! {
                        <div class="scripts-panel scripts-editor-panel">
                            <div class="scripts-editor-header">
                                <div class="scripts-editor-title-row">
                                    <input class="scripts-name-input"
                                        prop:value=move || editor_name.get()
                                        on:input=move |e| editor_name.set(event_target_value(&e))
                                    />
                                    <Show when=move || editor_dirty.get()>
                                        <span class="dirty-dot" title="Unsaved changes"></span>
                                    </Show>
                                </div>
                                <div class="scripts-status-badge"
                                    style=format!("color: {}", status_color)>
                                    <span class="scripts-status-dot"
                                        style=format!("background: {}", status_color)>
                                    </span>
                                    {status_text}
                                    {move || script_status.get()
                                        .and_then(|s| s.last_error)
                                        .map(|e| view! {
                                            <span class="scripts-err-hint" title=e>"⚠"</span>
                                        })}
                                </div>
                            </div>

                            <textarea class="scripts-editor"
                                node_ref=editor_ref
                                spellcheck="false"
                                prop:value=move || editor_content.get()
                                on:input=move |e| {
                                    editor_content.set(event_target_value(&e));
                                    editor_dirty.set(true);
                                }
                            ></textarea>

                            <div class="scripts-editor-actions">
                                <button class="btn btn-primary btn-sm" on:click=move |_| {
                                    let name = editor_name.get();
                                    let src  = editor_content.get();
                                    spawn_local(async move {
                                        if scripts_save_file_invoke(&name, &src).await {
                                            editor_dirty.set(false);
                                            show_toast("Saved", "ok");
                                            do_refresh();
                                        } else {
                                            show_toast("Save failed", "err");
                                        }
                                    });
                                }>"Save"</button>
                                <button class="btn btn-success btn-sm"
                                    prop:disabled=move || running
                                    on:click=move |_| {
                                        let name = editor_name.get();
                                        let src  = editor_content.get();
                                        spawn_local(async move {
                                            scripts_save_file_invoke(&name, &src).await;
                                            editor_dirty.set(false);
                                            do_refresh();
                                            scripts_run_file_invoke(&name).await;
                                            panel.set(Panel::Repl);
                                        });
                                    }
                                >"▶ Run"</button>
                                <button class="btn btn-danger btn-sm"
                                    prop:disabled=move || !running
                                    on:click=move |_| {
                                        spawn_local(async move { scripts_stop_invoke().await; });
                                    }
                                >"■ Stop"</button>
                                <button class="btn btn-sm" on:click=move |_| {
                                    spawn_local(async move { scripts_reset_vm_invoke().await; });
                                }>"Reset VM"</button>
                            </div>
                        </div>
                    }.into_any()
                }

                // ---- REPL ----
                Panel::Repl => view! {
                    <div class="scripts-panel scripts-repl-panel">
                        <div class="repl-header">
                            <span class="repl-title">
                                "MicroPython REPL"
                                {move || if repl_connected.get() {
                                    view! { <span class="repl-conn-dot repl-conn-on"></span> }.into_any()
                                } else {
                                    view! { <span class="repl-conn-dot repl-conn-off"></span> }.into_any()
                                }}
                            </span>
                            <div class="repl-header-actions">
                                <button class="btn btn-xs"
                                    on:click=move |_| {
                                        repl_output.set(vec![]);
                                        logs_cursor.set(None);
                                    }
                                >"Clear"</button>
                                {move || if repl_connected.get() {
                                    view! {
                                        <button class="btn btn-xs btn-danger" on:click=move |_| {
                                            spawn_local(async move {
                                                scripts_repl_disconnect_invoke().await;
                                                repl_connected.set(false);
                                            });
                                        }>"Disconnect"</button>
                                    }.into_any()
                                } else {
                                    view! {
                                        <button class="btn btn-xs btn-success" on:click=move |_| {
                                            spawn_local(async move {
                                                if scripts_repl_connect_invoke().await {
                                                    repl_connected.set(true);
                                                    repl_output.set(vec![]);
                                                    logs_cursor.set(None);
                                                } else {
                                                    show_toast("REPL connect failed", "err");
                                                }
                                            });
                                        }>"Connect"</button>
                                    }.into_any()
                                }}
                            </div>
                        </div>

                        <div class="repl-output" id="repl-output-scroll">
                            {move || repl_output.get().into_iter().map(|line| {
                                view! { <div class="repl-line">{line}</div> }
                            }).collect::<Vec<_>>()}
                        </div>

                        <div class="repl-input-row">
                            <span class="repl-prompt">">>>"</span>
                            <input class="repl-input"
                                node_ref=repl_input_ref
                                placeholder="Type Python expression…"
                                prop:value=move || repl_input.get()
                                prop:disabled=move || !repl_connected.get()
                                on:input=move |e| repl_input.set(event_target_value(&e))
                                on:keydown=move |e| {
                                    if e.key() == "Enter" {
                                        let text = repl_input.get();
                                        if text.is_empty() { return; }
                                        repl_input.set(String::new());
                                        let send = format!("{}\r\n", text);
                                        spawn_local(async move {
                                            scripts_repl_send_invoke(&send).await;
                                        });
                                    }
                                }
                            />
                            <button class="btn btn-primary btn-xs"
                                prop:disabled=move || !repl_connected.get()
                                on:click=move |_| {
                                    let text = repl_input.get();
                                    if text.is_empty() { return; }
                                    repl_input.set(String::new());
                                    let send = format!("{}\r\n", text);
                                    spawn_local(async move {
                                        scripts_repl_send_invoke(&send).await;
                                    });
                                }
                            >"Send"</button>
                        </div>
                    </div>
                }.into_any(),
            }}
        </div>
    }
}
