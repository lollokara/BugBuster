//! Scripts workspace: stored-script list, CodeMirror editor, run/log console, REPL, autorun and
//! API docs. All state lives in `ScriptStore` (see `script_state.rs`), so leaving this view never
//! stops a run, drops unsaved buffers or resets the log cursor.

use leptos::ev;
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::Deserialize;
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;

use crate::components::icons::Icon;
use crate::script_state::*;
use crate::tauri_bridge::show_toast;

// =============================================================================
// JavaScript interop (window.bbScriptEditor / window.bbScriptCatalogue)
// =============================================================================

mod js {
    use wasm_bindgen::prelude::*;
    use wasm_bindgen::JsCast;

    pub fn global(name: &str) -> Option<JsValue> {
        let w = web_sys::window()?;
        let v = js_sys::Reflect::get(&w, &name.into()).ok()?;
        (!v.is_undefined() && !v.is_null()).then_some(v)
    }

    pub fn get(obj: &JsValue, key: &str) -> JsValue {
        js_sys::Reflect::get(obj, &key.into()).unwrap_or(JsValue::UNDEFINED)
    }

    pub fn call(obj: &JsValue, method: &str, args: &[JsValue]) -> Result<JsValue, JsValue> {
        let f = js_sys::Reflect::get(obj, &method.into())?;
        let f: js_sys::Function = f
            .dyn_into()
            .map_err(|_| JsValue::from_str("not a function"))?;
        let arr = js_sys::Array::new();
        for a in args {
            arr.push(a);
        }
        f.apply(obj, &arr)
    }

    pub fn object(pairs: &[(&str, JsValue)]) -> JsValue {
        let o = js_sys::Object::new();
        for (k, v) in pairs {
            let _ = js_sys::Reflect::set(&o, &(*k).into(), v);
        }
        o.into()
    }
}

fn mod_key() -> &'static str {
    let mac = web_sys::window()
        .and_then(|w| w.navigator().platform().ok())
        .is_some_and(|p| p.starts_with("Mac"));
    if mac {
        "⌘"
    } else {
        "Ctrl"
    }
}

fn copy_text(text: &str, what: &str) {
    if let Some(win) = web_sys::window() {
        let _ = win.navigator().clipboard().write_text(text);
        show_toast(&format!("Copied {what}"), "ok");
    }
}

fn active_html_element() -> Option<web_sys::HtmlElement> {
    let el = web_sys::window()?.document()?.active_element()?;
    let el = el.dyn_into::<web_sys::HtmlElement>().ok()?;
    (el.tag_name() != "BODY").then_some(el)
}

fn open_dialog_element() -> Option<web_sys::Element> {
    let doc = web_sys::window()?.document()?;
    doc.query_selector(".sc-dialog").ok().flatten()
}

fn within(root: &web_sys::Element, el: &web_sys::Element) -> bool {
    let node: &web_sys::Node = el;
    root.contains(Some(node))
}

/// Move focus into the open dialog: the name field, else its first (safe) action.
fn focus_modal() {
    let Some(dlg) = open_dialog_element() else {
        return;
    };
    let active = web_sys::window()
        .and_then(|w| w.document())
        .and_then(|d| d.active_element());
    if active.is_some_and(|a| within(&dlg, &a)) {
        return;
    }
    let target = dlg
        .query_selector("input:not([disabled])")
        .ok()
        .flatten()
        .or_else(|| dlg.query_selector("button:not([disabled])").ok().flatten());
    if let Some(el) = target.and_then(|t| t.dyn_into::<web_sys::HtmlElement>().ok()) {
        let _ = el.focus();
    }
}

fn trap_tab(e: &ev::KeyboardEvent) {
    let Some(dlg) = open_dialog_element() else {
        return;
    };
    let Some(list) = js::call(
        &dlg,
        "querySelectorAll",
        &[JsValue::from_str(
            "button:not([disabled]), input:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex='-1'])",
        )],
    )
    .ok() else {
        return;
    };
    let items: Vec<web_sys::HtmlElement> = js_sys::Array::from(&list)
        .iter()
        .filter_map(|v| v.dyn_into::<web_sys::HtmlElement>().ok())
        .collect();
    let (Some(first), Some(last)) = (items.first(), items.last()) else {
        e.prevent_default();
        return;
    };
    let active = web_sys::window()
        .and_then(|w| w.document())
        .and_then(|d| d.active_element());
    let same = |a: &web_sys::Element, b: &web_sys::HtmlElement| {
        let (x, y): (&JsValue, &JsValue) = (a.as_ref(), b.as_ref());
        x == y
    };
    match active {
        Some(a) if within(&dlg, &a) => {
            if e.shift_key() && same(&a, first) {
                e.prevent_default();
                let _ = last.focus();
            } else if !e.shift_key() && same(&a, last) {
                e.prevent_default();
                let _ = first.focus();
            }
        }
        _ => {
            e.prevent_default();
            let _ = first.focus();
        }
    }
}

/// Save `text` through a temporary object URL and an anchor click.
fn download_text(filename: &str, text: &str) -> bool {
    let go = || -> Option<()> {
        let parts = js_sys::Array::of1(&JsValue::from_str(text));
        let blob = web_sys::Blob::new_with_str_sequence(&parts).ok()?;
        let url_api = js::global("URL")?;
        let url = js::call(&url_api, "createObjectURL", &[blob.into()])
            .ok()?
            .as_string()?;
        let doc = web_sys::window()?.document()?;
        let a = doc.create_element("a").ok()?;
        a.set_attribute("href", &url).ok()?;
        a.set_attribute("download", filename).ok()?;
        a.unchecked_ref::<web_sys::HtmlElement>().click();
        let _ = js::call(&url_api, "revokeObjectURL", &[JsValue::from_str(&url)]);
        Some(())
    };
    go().is_some()
}

fn timestamp_for_file() -> String {
    let d = js_sys::Date::new_0();
    format!(
        "{}{:02}{:02}-{:02}{:02}{:02}",
        d.get_full_year(),
        d.get_month() + 1,
        d.get_date(),
        d.get_hours(),
        d.get_minutes(),
        d.get_seconds()
    )
}

// =============================================================================
// Editor control
// =============================================================================

#[derive(Clone, Copy)]
pub struct EditorCtl {
    handle: StoredValue<Option<JsValue>, LocalStorage>,
    pub ready: RwSignal<bool>,
    pub failed: RwSignal<Option<String>>,
}

impl EditorCtl {
    fn from_store(store: ScriptStore) -> Self {
        Self {
            handle: store.editor_handle,
            ready: store.editor_ready,
            failed: store.editor_failed,
        }
    }

    fn call(&self, method: &str, args: &[JsValue]) -> Option<JsValue> {
        self.handle
            .with_value(|h| h.as_ref().and_then(|h| js::call(h, method, args).ok()))
    }

    fn set_source(&self, text: &str) {
        self.call("setSource", &[text.into()]);
    }

    fn focus(&self) {
        self.call("focus", &[]);
    }

    fn find(&self) {
        self.call("search", &[]);
    }

    fn selection(&self) -> Option<(u32, u32)> {
        let s = self.call("getSelection", &[])?;
        let a = js::get(&s, "anchor").as_f64()?;
        let h = js::get(&s, "head").as_f64()?;
        Some((a as u32, h as u32))
    }

    /// Selection restore goes through the CodeMirror view the editor module exposes for tests.
    fn restore_selection(&self, anchor: u32, head: u32, doc_len: u32) {
        let clamp = |n: u32| JsValue::from_f64(n.min(doc_len) as f64);
        self.handle.with_value(|h| {
            let Some(h) = h else { return };
            let view = js::get(h, "_view");
            if view.is_undefined() {
                return;
            }
            let sel = js::object(&[("anchor", clamp(anchor)), ("head", clamp(head))]);
            let _ = js::call(&view, "dispatch", &[js::object(&[("selection", sel)])]);
        });
    }

    fn set_diagnostics(&self, diag: Option<(String, Option<u32>)>) {
        let arr = js_sys::Array::new();
        if let Some((message, line)) = diag {
            let mut pairs = vec![
                ("severity", JsValue::from_str("error")),
                ("message", JsValue::from_str(&message)),
                ("source", JsValue::from_str("device syntax")),
            ];
            if let Some(l) = line {
                pairs.push(("line", JsValue::from_f64(l as f64)));
            }
            arr.push(&js::object(&pairs));
        }
        self.call("setDiagnostics", &[arr.into()]);
    }

    /// Insert a catalogue entry (id/path string or a synthetic entry object); adds missing imports.
    fn insert_doc(&self, entry: &JsValue) -> bool {
        self.call("insertDoc", std::slice::from_ref(entry))
            .is_some()
    }

    fn destroy(&self) {
        self.call("destroy", &[]);
        self.handle.set_value(None);
        self.ready.set(false);
    }
}

#[component]
fn EditorHost(ctl: EditorCtl) -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let host = NodeRef::<leptos::html::Div>::new();
    let current: StoredValue<String> = StoredValue::new(String::new());
    let keep: StoredValue<Option<Closure<dyn FnMut(JsValue)>>, LocalStorage> =
        StoredValue::new_local(None);

    // Mount once the host element exists.
    Effect::new(move |_| {
        let Some(el) = host.get() else { return };
        if ctl.handle.with_value(|h| h.is_some()) {
            return;
        }
        let Some(api) = js::global("bbScriptEditor") else {
            ctl.failed.set(Some(
                "The editor bundle (public/scripts-editor.js) did not load.".into(),
            ));
            return;
        };
        let (name, text, sel) = store.model.with_untracked(|m| {
            m.active_buffer()
                .map(|(n, b)| (n, b.text.clone(), (b.sel_anchor, b.sel_head)))
                .unwrap_or_default()
        });
        current.set_value(name);
        let cb = Closure::<dyn FnMut(JsValue)>::new(move |s: JsValue| {
            if let Some(text) = s.as_string() {
                store.edit(&current.get_value(), text);
            }
        });
        let opts = js::object(&[
            ("source", JsValue::from_str(&text)),
            ("theme", JsValue::from_str("inherit")),
        ]);
        match js::call(
            &api,
            "mount",
            &[JsValue::from(el), cb.as_ref().clone(), opts],
        ) {
            Ok(h) => {
                ctl.handle.set_value(Some(h));
                keep.set_value(Some(cb));
                ctl.failed.set(None);
                ctl.restore_selection(sel.0, sel.1, text.encode_utf16().count() as u32);
                ctl.ready.set(true);
            }
            Err(e) => ctl.failed.set(Some(format!(
                "Editor failed to start: {}",
                e.as_string().unwrap_or_default()
            ))),
        }
    });

    // Reload when the active document changes (open, switch, revert).
    Effect::new(move |prev: Option<u64>| {
        let n = store.doc_load.get();
        if !ctl.ready.get() {
            return n;
        }
        let (name, text, sel) = store.model.with_untracked(|m| {
            m.active_buffer()
                .map(|(n, b)| (n, b.text.clone(), (b.sel_anchor, b.sel_head)))
                .unwrap_or_default()
        });
        if let Some((a, h)) = ctl.selection() {
            store.store_selection(&current.get_value(), a, h);
        }
        current.set_value(name);
        ctl.set_source(&text);
        ctl.restore_selection(sel.0, sel.1, text.encode_utf16().count() as u32);
        if prev.is_some() {
            ctl.focus();
        }
        n
    });

    // Device syntax errors are shown on the editor only while they describe the current revision.
    let diag = Memo::new(move |_| {
        store.model.with(|m| {
            m.active_buffer().and_then(|(_, b)| {
                b.lint
                    .as_ref()
                    .filter(|r| r.rev == b.rev)
                    .and_then(|r| match &r.banner {
                        LintBanner::Syntax { message, line } => Some((message.clone(), *line)),
                        _ => None,
                    })
            })
        })
    });
    Effect::new(move |_| {
        let _ = store.doc_load.get();
        let d = diag.get();
        if ctl.ready.get() {
            ctl.set_diagnostics(d);
        }
    });

    on_cleanup(move || {
        if let Some((a, h)) = ctl.selection() {
            store.store_selection(&current.get_value(), a, h);
        }
        ctl.destroy();
        keep.set_value(None);
    });

    view! { <div class="sc-editor-host" node_ref=host data-testid="sc-editor-host"></div> }
}

// =============================================================================
// API catalogue access
// =============================================================================

fn nullable<'de, D>(d: D) -> Result<String, D::Error>
where
    D: serde::Deserializer<'de>,
{
    Ok(Option::<String>::deserialize(d)?.unwrap_or_default())
}

#[derive(Deserialize, Clone, PartialEq, Default)]
#[serde(default)]
struct DocParam {
    #[serde(deserialize_with = "nullable")]
    name: String,
    #[serde(deserialize_with = "nullable")]
    kind: String,
    annotation: Option<String>,
    #[serde(rename = "default")]
    default_value: Option<String>,
    required: bool,
    #[serde(deserialize_with = "nullable")]
    doc: String,
}

#[derive(Deserialize, Clone, PartialEq, Default)]
#[serde(default)]
struct DocKey {
    #[serde(deserialize_with = "nullable")]
    name: String,
    #[serde(deserialize_with = "nullable")]
    doc: String,
}

#[derive(Deserialize, Clone, PartialEq, Default)]
#[serde(default)]
struct DocRaise {
    #[serde(rename = "type", deserialize_with = "nullable")]
    kind: String,
    #[serde(deserialize_with = "nullable")]
    doc: String,
}

#[derive(Deserialize, Clone, PartialEq, Default)]
#[serde(default, rename_all = "camelCase")]
struct DocEntry {
    #[serde(deserialize_with = "nullable")]
    id: String,
    #[serde(deserialize_with = "nullable")]
    kind: String,
    #[serde(deserialize_with = "nullable")]
    path: String,
    #[serde(deserialize_with = "nullable")]
    name: String,
    #[serde(deserialize_with = "nullable")]
    signature: String,
    #[serde(deserialize_with = "nullable")]
    summary: String,
    #[serde(deserialize_with = "nullable")]
    description: String,
    #[serde(deserialize_with = "nullable")]
    doc: String,
    params: Vec<DocParam>,
    returns: Option<String>,
    #[serde(deserialize_with = "nullable")]
    returns_doc: String,
    return_keys: Vec<DocKey>,
    raises: Vec<DocRaise>,
    #[serde(deserialize_with = "nullable")]
    notes: String,
    examples: Vec<String>,
    imports: Vec<String>,
    #[serde(deserialize_with = "nullable")]
    insert_text: String,
    value: Option<String>,
    source: Option<String>,
}

impl DocEntry {
    fn kind_tag(&self) -> &'static str {
        match self.kind.as_str() {
            "module" => "mod",
            "namespace" => "ns",
            "class" => "class",
            "function" => "fn",
            "method" => "meth",
            "constant" => "const",
            "example" => "ex",
            _ => "api",
        }
    }
}

/// Docstring markup shown as plain text: ``code`` and :func:`name` lose their markers.
fn plain_doc(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut rest = text.replace("``", "");
    for role in [":func:`", ":class:`", ":meth:`", ":attr:`"] {
        while let Some(i) = rest.find(role) {
            let after = &rest[i + role.len()..];
            match after.find('`') {
                Some(j) => {
                    let inner = after[..j].to_string();
                    rest = format!("{}{}{}", &rest[..i], inner, &after[j + 1..]);
                }
                None => break,
            }
        }
    }
    out.push_str(&rest.replace('`', ""));
    out
}

fn param_name(p: &DocParam) -> String {
    match p.kind.as_str() {
        "var_positional" => format!("*{}", p.name),
        "var_keyword" => format!("**{}", p.name),
        "keyword_only" => format!("{}=", p.name),
        _ => p.name.clone(),
    }
}

fn param_requirement(p: &DocParam) -> String {
    if let Some(d) = &p.default_value {
        format!("default {d}")
    } else if p.required {
        "required".into()
    } else {
        "optional".into()
    }
}

fn catalogue() -> Option<JsValue> {
    js::global("bbScriptCatalogue")
}

/// (state, message) of the shared catalogue: empty | loading | ready | failed.
fn catalogue_status() -> (String, String) {
    let Some(c) = catalogue() else {
        return (
            "missing".into(),
            "The catalogue bundle did not load.".into(),
        );
    };
    let Ok(s) = js::call(&c, "status", &[]) else {
        return ("failed".into(), "Catalogue status unavailable.".into());
    };
    (
        js::get(&s, "state").as_string().unwrap_or_default(),
        js::get(&s, "message").as_string().unwrap_or_default(),
    )
}

fn catalogue_load() {
    if let Some(c) = catalogue() {
        if let Ok(p) = js::call(&c, "load", &[]) {
            if let Ok(p) = p.dyn_into::<js_sys::Promise>() {
                spawn_local(async move {
                    let _ = wasm_bindgen_futures::JsFuture::from(p).await;
                });
            }
        }
    }
}

fn kinds_for(filter: &str) -> Option<Vec<&'static str>> {
    match filter {
        "modules" => Some(vec!["module", "namespace"]),
        "functions" => Some(vec!["function", "method"]),
        "classes" => Some(vec!["class"]),
        "constants" => Some(vec!["constant"]),
        "examples" => Some(vec!["example"]),
        _ => None,
    }
}

fn catalogue_search(query: &str, filter: &str) -> Vec<DocEntry> {
    let Some(c) = catalogue() else { return vec![] };
    let mut pairs = vec![("limit", JsValue::from_f64(120.0))];
    if let Some(kinds) = kinds_for(filter) {
        let arr = js_sys::Array::new();
        for k in kinds {
            arr.push(&JsValue::from_str(k));
        }
        pairs.push(("kinds", arr.into()));
    }
    js::call(
        &c,
        "search",
        &[JsValue::from_str(query), js::object(&pairs)],
    )
    .ok()
    .and_then(|v| serde_wasm_bindgen::from_value::<Vec<DocEntry>>(v).ok())
    .unwrap_or_default()
}

fn catalogue_entry(id: &str) -> Option<DocEntry> {
    let c = catalogue()?;
    let v = js::call(&c, "entry", &[JsValue::from_str(id)]).ok()?;
    if v.is_null() || v.is_undefined() {
        return None;
    }
    serde_wasm_bindgen::from_value(v).ok()
}

// =============================================================================
// Run chip (every view)
// =============================================================================

/// Toolbar chip shown on every view while a script is active; opens the log, stops the script.
#[component]
pub fn ScriptRunChip(on_open: Callback<()>) -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let show = move || store.connected() && store.is_active();
    view! {
        <Show when=show>
            <div class="sc-chip" data-testid="script-run-chip" role="group" aria-label="Running script">
                <button class="sc-chip-main" title="Show script log"
                    on:click=move |_| on_open.run(())>
                    <span class=move || {
                        let tone = store.status.with(|s| s.as_ref().map(RunStatus::tone).unwrap_or("gray"));
                        if store.stale.get() { "dot tone-orange".to_string() } else { format!("dot tone-{tone} live") }
                    }></span>
                    <span class="sc-chip-name">
                        {move || store.status.with(|s| s.as_ref().map(RunStatus::display_name).unwrap_or_default())}
                    </span>
                    <span class="sc-chip-time" data-testid="script-run-chip-time">
                        {move || {
                            let stopping = store.status.with(|s| s.as_ref().is_some_and(|s| s.state == RunState::Stopping));
                            if stopping { "stopping".to_string() }
                            else { store.elapsed().map(|e| e.text()).unwrap_or_default() }
                        }}
                    </span>
                </button>
                <button class="sc-chip-stop" title="Stop script" aria-label="Stop script"
                    data-testid="script-run-chip-stop"
                    disabled=move || store.status.with(|s| s.as_ref().is_some_and(|s| s.state == RunState::Stopping))
                    on:click=move |_| store.stop()>
                    <Icon name="square" size=11 />
                </button>
            </div>
        </Show>
    }
}

// =============================================================================
// Scripts view
// =============================================================================

#[derive(Clone, PartialEq, Default)]
struct DocVm {
    name: String,
    dirty: bool,
    lossy: bool,
    has: bool,
}

#[derive(Clone, PartialEq)]
enum LintVm {
    None,
    Checking,
    Stale,
    Ok {
        saved: bool,
    },
    Syntax {
        message: String,
        line: Option<u32>,
        saved: bool,
    },
    Skipped {
        text: String,
        saved: bool,
    },
}

#[derive(Clone, PartialEq)]
struct FileRow {
    name: String,
    dirty: bool,
    active: bool,
    running: bool,
    local: bool,
}

#[component]
pub fn ScriptsTab() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let ctl = EditorCtl::from_store(store);
    provide_context(ctl);

    Effect::new(move |_| store.scripts_visible.set(true));
    on_cleanup(move || store.scripts_visible.set(false));

    // Narrow windows start with the side panes collapsed.
    let width = web_sys::window()
        .and_then(|w| w.inner_width().ok())
        .and_then(|w| w.as_f64())
        .unwrap_or(1400.0);
    if width < 1100.0 {
        store.docs_open.set(false);
        store.list_open.set(false);
    }
    if catalogue_status().0 == "empty" {
        catalogue_load();
    }
    if !store.connected() && store.model.with_untracked(|m| m.open_name().is_none()) {
        store.open_scratch();
    }

    let has_open = Memo::new(move |_| store.model.with(|m| m.open_name().is_some()));

    // The view measures itself (the sidebar and the window both change its width), so the side
    // panes collapse into drawers on the view's width rather than the viewport's.
    let root = NodeRef::<leptos::html::Div>::new();
    let narrow = RwSignal::new(false);
    let compact = RwSignal::new(false);
    let measure = move || {
        if let Some(el) = root.get_untracked() {
            let w = el.client_width();
            if w <= 0 {
                return;
            }
            let (n, c) = (w < 980, w < 640);
            if narrow.get_untracked() != n {
                narrow.set(n);
            }
            if compact.get_untracked() != c {
                compact.set(c);
            }
        }
    };
    Effect::new(move |_| {
        let _ = root.get();
        measure();
    });
    // Picking a document or the REPL from the list drawer reveals it.
    Effect::new(move |prev: Option<(Option<String>, bool)>| {
        let now = (
            store.model.with(|m| m.open_name()),
            store.repl_open.get(),
        );
        if prev.is_some_and(|p| p != now) && narrow.get_untracked() {
            store.list_open.set(false);
        }
        now
    });
    // As drawers, the list and the docs never cover each other: the one opened last wins.
    Effect::new(move |prev: Option<(bool, bool)>| {
        let (l, d, n) = (store.list_open.get(), store.docs_open.get(), narrow.get());
        if n && l && d {
            match prev {
                Some((prev_list, _)) if !prev_list => store.docs_open.set(false),
                _ => store.list_open.set(false),
            }
        }
        (l, d)
    });
    let tick =
        leptos::prelude::set_interval_with_handle(measure, std::time::Duration::from_millis(400))
            .ok();
    on_cleanup(move || {
        if let Some(h) = tick {
            h.clear();
        }
    });
    let on_key = move |e: ev::KeyboardEvent| {
        let ctrl = e.ctrl_key() || e.meta_key();
        let key = e.key();
        let open = store.model.with_untracked(|m| m.open_name());
        // Document shortcuts need a document on screen, not the REPL.
        let doc_keys = !store.repl_open.get_untracked();
        if ctrl && key.eq_ignore_ascii_case("s") {
            e.prevent_default();
            if doc_keys {
                store.save_active();
            }
        } else if key == "F5" {
            e.prevent_default();
            if e.shift_key() {
                store.stop();
            } else if let Some(n) = open.filter(|n| !n.is_empty() && doc_keys) {
                store.run(n, ctrl);
            }
        } else if key == "F7" {
            e.prevent_default();
            if doc_keys {
                store.check_syntax();
            }
        } else if ctrl && key.eq_ignore_ascii_case("j") {
            e.prevent_default();
            store.console_open.update(|o| *o = !*o);
        }
    };

    view! {
        <div class="sc-root" tabindex="-1" data-testid="scripts-root" on:keydown=on_key node_ref=root
            data-narrow=move || narrow.get().to_string()
            data-compact=move || compact.get().to_string()>
            <StatusBar />
            <NoticeBar />
            <div class="sc-body"
                data-list=move || if store.list_open.get() { "open" } else { "closed" }
                data-docs=move || if store.docs_open.get() { "open" } else { "closed" }>
                <FileList />
                <main class="sc-main">
                    {move || {
                        if store.repl_open.get() {
                            view! { <ReplPane /> }.into_any()
                        } else if has_open.get() {
                            view! { <EditorPane ctl=ctl /> }.into_any()
                        } else {
                            view! { <EmptyMain /> }.into_any()
                        }
                    }}
                </main>
                <DocsPanel />
                <div class="sc-drawer-scrim" on:click=move |_| {
                    store.list_open.set(false);
                    store.docs_open.set(false);
                }></div>
            </div>
            <Show when=move || store.console_open.get()>
                <Console />
            </Show>
            <Dialogs />
        </div>
    }
}

#[component]
fn NoticeBar() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    view! {
        {move || store.notice.get().map(|n| {
            let icon = if n.tone == "red" { "circle-alert" } else { "info" };
            view! {
                <div class=format!("sc-banner tone-{}", n.tone) role="alert" data-testid="sc-notice">
                    <Icon name=icon size=14 />
                    <span class="sc-banner-text">{n.text}</span>
                    <button class="btn btn-plain btn-icon btn-sm" title="Dismiss" aria-label="Dismiss"
                        on:click=move |_| store.notice.set(None)>
                        <Icon name="x" size=13 />
                    </button>
                </div>
            }
        })}
    }
}

#[component]
fn StatusBar() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let key = mod_key();
    let tone = move || {
        let c = store.conn.get();
        if !c.connected() {
            return "gray";
        }
        if store.stale.get() {
            return "orange";
        }
        store
            .status
            .with(|s| s.as_ref().map(RunStatus::tone).unwrap_or("gray"))
    };
    let name = move || {
        let connected = store.connected();
        store.status.with(|s| match s {
            Some(s) if s.is_active() => s.display_name(),
            _ if !connected => "Offline".to_string(),
            Some(_) => "No script running".to_string(),
            None => "Connecting…".to_string(),
        })
    };
    let sub = move || {
        let c = store.conn.get();
        if c.mode == "Mock" {
            return "The demo device has no script engine".to_string();
        }
        if !c.connected() {
            return match store.status.get() {
                Some(s) if s.is_active() => format!(
                    "Not connected - last seen: {} '{}'",
                    s.state_label(),
                    s.display_name()
                ),
                _ => "Not connected".to_string(),
            };
        }
        if let Some(e) = store.status_error.get() {
            return e;
        }
        let Some(s) = store.status.get() else {
            return if c.verified {
                "Reading status…".into()
            } else {
                "Identifying device…".into()
            };
        };
        let mut parts = vec![s.state_label().to_string()];
        if s.is_active() {
            parts.push(s.source.label().to_string());
            if let Some(e) = store.elapsed() {
                parts.push(e.text());
            }
        } else if s.state == RunState::Error && !s.last_error.is_empty() {
            parts.push(s.last_error.lines().next().unwrap_or("").to_string());
        }
        if store.stale.get() {
            parts.push("status stale".into());
        }
        parts.join(" · ")
    };
    view! {
        <div class="sc-status" data-testid="script-status">
            <span class=move || format!("dot tone-{}", tone())></span>
            <div class="sc-status-main">
                <b class="sc-status-name" data-testid="script-status-name">{name}</b>
                <span class="sc-status-sub" data-testid="script-status-sub">{sub}</span>
            </div>
            <span class="spacer"></span>
            {move || {
                let c = store.conn.get();
                let label = c.transport_label();
                (!label.is_empty()).then(|| view! {
                    <span class="badge tone-blue" title=format!("Script requests use the {label} link") data-testid="script-transport">{label}</span>
                })
            }}
            <button class="btn btn-plain btn-icon btn-sm"
                title=format!("Show or hide the log ({key} J)") aria-label="Toggle log"
                aria-pressed=move || store.console_open.get().to_string()
                class:active=move || store.console_open.get()
                data-testid="sc-log-toggle"
                on:click=move |_| store.console_open.update(|o| *o = !*o)>
                <Icon name="list" size=15 />
            </button>
            <Show when=move || store.connected() && store.is_active()>
                <button class="btn btn-sm btn-tinted tone-red btn-icon" title="Stop (Shift F5)" aria-label="Stop script"
                    data-testid="sc-stop"
                    disabled=move || store.status.with(|s| s.as_ref().is_some_and(|s| s.state == RunState::Stopping))
                    on:click=move |_| store.stop()>
                    <Icon name="square" size=13 />
                </button>
            </Show>
        </div>
    }
}

#[component]
fn EmptyMain() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    view! {
        <div class="sc-empty" data-testid="sc-empty">
            <Icon name="terminal" size=30 />
            <h3>"No script open"</h3>
            <p>{move || if store.ready() {
                "Pick a stored script, create one, or open the REPL."
            } else {
                "Docs and a local scratch buffer work without a device. Connect one to open stored scripts."
            }}</p>
            <div class="hstack">
                <button class="btn btn-sm btn-primary" on:click=move |_| store.dialog.set(Some(Dialog::New { save_as: false }))
                    disabled=move || !store.ready()>
                    <Icon name="plus" size=14 />"New script"
                </button>
                <button class="btn btn-sm" on:click=move |_| store.open_scratch()>"Open scratch buffer"</button>
                {move || (!store.list_open.get()).then(|| view! {
                    <button class="btn btn-sm sc-list-toggle" data-testid="sc-empty-files"
                        on:click=move |_| store.list_open.set(true)>
                        <Icon name="panel-left" size=14 />"Stored scripts"
                    </button>
                })}
                <button class="btn btn-sm" on:click=move |_| store.repl_open.set(true)>
                    <Icon name="terminal" size=14 />"REPL"
                </button>
            </div>
        </div>
    }
}

// -----------------------------------------------------------------------------
// File list and autorun
// -----------------------------------------------------------------------------

#[component]
fn FileList() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let rows = Memo::new(move |_| {
        store.model.with(|m| {
            let Some(ws) = m.ws() else {
                return Vec::<FileRow>::new();
            };
            let open = ws.open.clone();
            let mut v: Vec<FileRow> = Vec::new();
            if ws.buffers.get(SCRATCH).is_some_and(|b| !b.text.is_empty())
                || open.as_deref() == Some(SCRATCH)
            {
                v.push(FileRow {
                    name: String::new(),
                    dirty: ws.buffers.get(SCRATCH).is_some_and(Buffer::dirty),
                    active: open.as_deref() == Some(SCRATCH),
                    running: false,
                    local: true,
                });
            }
            for f in &ws.files {
                v.push(FileRow {
                    name: f.clone(),
                    dirty: ws.buffers.get(f).is_some_and(Buffer::dirty),
                    active: open.as_deref() == Some(f.as_str()),
                    running: false,
                    local: ws.buffers.contains_key(f),
                });
            }
            v
        })
    });
    let running_name = Memo::new(move |_| {
        store.status.with(|s| {
            s.as_ref()
                .filter(|s| s.holds_file_slot())
                .map(|s| s.file_slot_name.clone())
        })
    });
    let storage = Memo::new(move |_| {
        store.model.with(|m| {
            m.ws()
                .and_then(|w| w.storage.as_ref().map(StorageInfo::text))
        })
    });
    let elsewhere = Memo::new(move |_| store.model.with(Scripts::dirty_elsewhere));
    let autorun_pick = RwSignal::new(false);

    view! {
        <aside class="sc-list" data-testid="sc-list" aria-label="Stored scripts">
            <div class="sc-list-head">
                <div class="sc-list-title">
                    <b>"Stored scripts"</b>
                    <span class="subtle" data-testid="sc-storage">{move || storage.get().unwrap_or_default()}</span>
                </div>
                <button class="btn btn-plain btn-icon btn-sm" title="New script" aria-label="New script"
                    data-testid="sc-new"
                    disabled=move || !store.ready()
                    on:click=move |_| store.dialog.set(Some(Dialog::New { save_as: false }))>
                    <Icon name="plus" size=15 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title="Refresh list and autorun" aria-label="Refresh"
                    data-testid="sc-refresh"
                    disabled=move || !store.ready() || store.files_loading.get()
                    on:click=move |_| store.refresh()>
                    <Icon name="refresh-cw" size=14 />
                </button>
            </div>
            <div class="sc-list-scroll">
                <button class="sc-file-main sc-repl-link" class:active=move || store.repl_open.get()
                    title="Open the REPL" data-testid="sc-open-repl"
                    on:click=move |_| store.repl_open.update(|o| *o = !*o)>
                    <Icon name="terminal" size=14 />
                    <span>"REPL"</span>
                </button>
                {move || (!store.connected()).then(|| view! {
                    <div class="sc-list-note" data-testid="sc-list-offline">
                        {if store.conn.with(|c| c.mode == "Mock") {
                            "The demo device has no script engine."
                        } else {
                            "Not connected. Unsaved buffers are kept until a device is available."
                        }}
                    </div>
                })}
                {move || store.connected().then(|| view! {
                    <details class="disclosure sc-autorun" data-testid="sc-autorun">
                        <summary>
                            "Autorun on boot"
                            <span class=move || {
                                let on = store.autorun.with(|a| a.as_ref().is_some_and(|a| a.enabled));
                                format!("badge {}", if on { "tone-green" } else { "" })
                            } data-testid="sc-autorun-state">
                                {move || store.autorun.with(|a| a.as_ref().map(|a| a.state_text()).unwrap_or("…"))}
                            </span>
                        </summary>
                        <div class="sc-autorun-body">
                            {move || store.autorun.get().map(|a| view! {
                                <dl class="kv sc-kv">
                                    {a.rows().into_iter().map(|(k, v)| view! { <dt>{k}</dt><dd>{v}</dd> }).collect_view()}
                                </dl>
                            })}
                            <div class="hstack">
                                <div class="menu-anchor">
                                    <button class="btn btn-sm" data-testid="sc-autorun-pick"
                                        disabled=move || !store.ready() || rows.with(|r| r.iter().all(|f| f.local && f.name.is_empty()))
                                        on:click=move |_| autorun_pick.update(|o| *o = !*o)>
                                        {move || if store.autorun.with(|a| a.as_ref().is_some_and(|a| a.enabled)) { "Change…" } else { "Enable…" }}
                                    </button>
                                    <Show when=move || autorun_pick.get()>
                                        <div class="scrim" style="background: transparent" on:click=move |_| autorun_pick.set(false)></div>
                                        <div class="popover sc-pick" role="menu" style="z-index: 950">
                                            {move || rows.get().into_iter().filter(|f| !f.name.is_empty()).map(|f| {
                                                let n = f.name.clone();
                                                view! {
                                                    <button class="menu-item" role="menuitem" on:click=move |_| {
                                                        autorun_pick.set(false);
                                                        store.dialog.set(Some(Dialog::AutorunEnable(n.clone())));
                                                    }>{f.name}</button>
                                                }
                                            }).collect_view()}
                                        </div>
                                    </Show>
                                </div>
                                <button class="btn btn-sm" data-testid="sc-autorun-disable"
                                    disabled=move || !store.ready() || !store.autorun.with(|a| a.as_ref().is_some_and(|a| a.enabled))
                                    on:click=move |_| store.dialog.set(Some(Dialog::AutorunDisable))>
                                    "Disable…"
                                </button>
                            </div>
                        </div>
                    </details>
                })}
                <div class="sc-files" data-testid="sc-files">
                    {move || {
                        let list = rows.get();
                        if list.is_empty() {
                            let msg = if store.files_loading.get() {
                                "Loading…"
                            } else if store.ready() {
                                "No stored scripts"
                            } else {
                                ""
                            };
                            return view! { <div class="sc-list-note">{msg}</div> }.into_any();
                        }
                        list.into_iter().map(|f| {
                            let name = f.name.clone();
                            let label = if f.name.is_empty() { "Untitled (local)".to_string() } else { f.name.clone() };
                            let n_open = name.clone();
                            let n_run = name.clone();
                            let n_del = name.clone();
                            let n_run_title = name.clone();
                            let is_scratch = f.name.is_empty();
                            let open_label = label.clone();
                            let row_name = name.clone();
                            view! {
                                <div class="sc-file" class:active=f.active data-file=row_name data-testid="sc-file">
                                    <button class="sc-file-main" title=format!("Open {open_label}")
                                        on:click=move |_| {
                                            if n_open.is_empty() { store.open_scratch() } else { store.open_file(n_open.clone()) }
                                        }>
                                        <span class="sc-file-name">{label}</span>
                                        {f.dirty.then(|| view! { <span class="sc-dirty" title="Unsaved changes" aria-label="Unsaved changes"></span> })}
                                        {move || (!is_scratch && running_name.get().as_deref() == Some(name.as_str()))
                                            .then(|| view! { <span class="badge tone-green">"running"</span> })}
                                    </button>
                                    {(!is_scratch).then(|| view! {
                                        <button class="btn btn-plain btn-icon btn-sm" title=format!("Run {n_run_title}")
                                            aria-label=format!("Run {n_run_title}") data-testid="sc-file-run"
                                            disabled=move || !store.ready() || store.launching.get()
                                            on:click=move |_| store.run(n_run.clone(), false)>
                                            <Icon name="play" size=13 />
                                        </button>
                                        <button class="btn btn-plain btn-icon btn-sm" title="Delete from device"
                                            aria-label="Delete from device" data-testid="sc-file-delete"
                                            disabled=move || !store.ready()
                                            on:click=move |_| store.dialog.set(Some(Dialog::Delete(n_del.clone())))>
                                            <Icon name="trash-2" size=13 />
                                        </button>
                                    })}
                                </div>
                            }
                        }).collect_view().into_any()
                    }}
                </div>
                {move || {
                    let other = elsewhere.get();
                    let names = other.iter().map(|(_, n)| n.clone()).collect::<Vec<_>>().join(", ");
                    (!other.is_empty()).then(|| view! {
                        <div class="sc-list-note" data-testid="sc-elsewhere" title=names>
                            {format!("{} unsaved buffer(s) kept for another device", other.len())}
                        </div>
                    })
                }}
            </div>
        </aside>
    }
}

// -----------------------------------------------------------------------------
// Editor pane
// -----------------------------------------------------------------------------

#[component]
fn EditorPane(ctl: EditorCtl) -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let key = mod_key();
    let run_menu = RwSignal::new(false);
    let doc = Memo::new(move |_| {
        store.model.with(|m| match m.active_buffer() {
            Some((n, b)) => DocVm {
                name: n,
                dirty: b.dirty(),
                lossy: b.lossy,
                has: true,
            },
            None => DocVm::default(),
        })
    });
    let lint = Memo::new(move |_| {
        if store.linting.get() {
            return LintVm::Checking;
        }
        store.model.with(|m| {
            let Some((_, b)) = m.active_buffer() else {
                return LintVm::None;
            };
            match &b.lint {
                None => LintVm::None,
                Some(r) if r.rev != b.rev => LintVm::Stale,
                Some(r) => match &r.banner {
                    LintBanner::Ok => LintVm::Ok { saved: r.saved },
                    LintBanner::Syntax { message, line } => LintVm::Syntax {
                        message: message.clone(),
                        line: *line,
                        saved: r.saved,
                    },
                    LintBanner::Skipped { text, .. } => LintVm::Skipped {
                        text: text.clone(),
                        saved: r.saved,
                    },
                },
            }
        })
    });
    let named = move || !doc.with(|d| d.name.is_empty());
    let can_run = move || store.ready() && !store.launching.get() && named();
    let progress = move || {
        store.upload.with(|u| {
            u.as_ref().map(|u| {
                if u.total == 0 {
                    0.0
                } else {
                    (u.sent as f64 / u.total as f64 * 100.0).min(100.0)
                }
            })
        })
    };

    view! {
        <div class="sc-editor-pane" data-testid="sc-editor">
            <div class="sc-toolbar" role="toolbar" aria-label="Editor">
                <button class="btn btn-plain btn-icon btn-sm sc-list-toggle" title="Show or hide the script list" aria-label="Toggle script list"
                    aria-pressed=move || store.list_open.get().to_string()
                    on:click=move |_| store.list_open.update(|o| *o = !*o)>
                    <Icon name="panel-left" size=15 />
                </button>
                <span class="sc-doc-name" data-testid="sc-doc-name">
                    {move || doc.with(|d| if d.name.is_empty() { "Untitled (local)".to_string() } else { d.name.clone() })}
                </span>
                {move || doc.with(|d| d.dirty).then(|| view! {
                    <span class="sc-dirty" title="Unsaved changes" aria-label="Unsaved changes" data-testid="sc-dirty"></span>
                })}
                <span class="spacer"></span>
                <button class="btn btn-sm btn-icon" data-testid="sc-save" aria-label="Save"
                    title=move || if !store.ready() { format!("Save ({key} S): connect a device first") } else { format!("Save and check syntax ({key} S)") }
                    disabled=move || !store.ready() || store.saving.get()
                    on:click=move |_| store.save_active()>
                    <Icon name="save" size=15 />
                </button>
                <button class="btn btn-sm btn-icon" data-testid="sc-check" aria-label="Check syntax"
                    title="Check syntax on the device (F7). Compile only, nothing runs."
                    disabled=move || !store.ready() || store.linting.get()
                    on:click=move |_| store.check_syntax()>
                    <Icon name="circle-check" size=15 />
                </button>
                <div class="toolbar-sep"></div>
                <div class="btn-group sc-run-group">
                    <button class="btn btn-sm btn-icon btn-tinted tone-green" data-testid="sc-run" aria-label="Run"
                        title="Run (F5). Saves unsaved changes first."
                        disabled=move || !can_run()
                        on:click=move |_| {
                            let n = doc.with_untracked(|d| d.name.clone());
                            store.run(n, false);
                        }>
                        <Icon name="play" size=15 />
                    </button>
                    <div class="menu-anchor">
                        <button class="btn btn-sm btn-icon btn-tinted tone-green sc-run-more" aria-label="More run options"
                            title="More run options" aria-haspopup="menu" data-testid="sc-run-menu"
                            aria-expanded=move || run_menu.get().to_string()
                            disabled=move || !can_run()
                            on:click=move |_| run_menu.update(|o| *o = !*o)>
                            <Icon name="chevron-down" size=13 />
                        </button>
                        <Show when=move || run_menu.get()>
                            <div class="scrim" style="background: transparent" on:click=move |_| run_menu.set(false)></div>
                            <div class="popover" role="menu" style="z-index: 950">
                                <button class="menu-item" role="menuitem" on:click=move |_| {
                                    run_menu.set(false);
                                    store.run(doc.with_untracked(|d| d.name.clone()), false);
                                }><Icon name="play" size=14 /><span class="spacer">"Run"</span><span class="kbd">"F5"</span></button>
                                <button class="menu-item" role="menuitem" data-testid="sc-run-bg" on:click=move |_| {
                                    run_menu.set(false);
                                    store.run(doc.with_untracked(|d| d.name.clone()), true);
                                }><Icon name="play" size=14 /><span class="spacer">"Run in background"</span><span class="kbd">{format!("{key} F5")}</span></button>
                            </div>
                        </Show>
                    </div>
                </div>
                <Show when=move || store.connected() && store.is_active()>
                    <button class="btn btn-sm btn-icon btn-tinted tone-red" title="Stop (Shift F5)" aria-label="Stop script"
                        data-testid="sc-stop-editor"
                        disabled=move || store.status.with(|s| s.as_ref().is_some_and(|s| s.state == RunState::Stopping))
                        on:click=move |_| store.stop()>
                        <Icon name="square" size=13 />
                    </button>
                </Show>
                <div class="toolbar-sep"></div>
                <button class="btn btn-plain btn-icon btn-sm sc-secondary" title=format!("Find and replace ({key} F)") aria-label="Find and replace"
                    on:click=move |_| ctl.find()>
                    <Icon name="search" size=15 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm sc-secondary" title="Discard unsaved changes and reload from the device" aria-label="Revert"
                    data-testid="sc-revert"
                    disabled=move || !store.ready() || !doc.with(|d| d.dirty && !d.name.is_empty())
                    on:click=move |_| store.dialog.set(Some(Dialog::Revert(doc.with_untracked(|d| d.name.clone()))))>
                    <Icon name="rotate-ccw" size=15 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title="Close the editor (unsaved changes are kept)" aria-label="Close editor"
                    data-testid="sc-close" on:click=move |_| store.close_open()>
                    <Icon name="x" size=15 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title="API docs" aria-label="Toggle API docs"
                    data-testid="sc-docs-toggle"
                    aria-pressed=move || store.docs_open.get().to_string()
                    class:active=move || store.docs_open.get()
                    on:click=move |_| store.docs_open.update(|o| *o = !*o)>
                    <Icon name="panel-right" size=15 />
                </button>
            </div>
            {move || progress().map(|p| view! {
                <div class="progress sc-upload" role="progressbar" aria-label="Upload progress" data-testid="sc-upload">
                    <span style=format!("width: {p:.0}%")></span>
                </div>
            })}
            {move || (!store.connected()).then(|| view! {
                <div class="sc-banner tone-blue" data-testid="sc-offline-banner">
                    <Icon name="info" size=14 />
                    <span class="sc-banner-text">"Offline: edits stay in this buffer. Connect a device to save, check and run."</span>
                </div>
            })}
            {move || (store.connected() && !store.ready()).then(|| view! {
                <div class="sc-banner tone-blue">
                    <Icon name="info" size=14 />
                    <span class="sc-banner-text">"Identifying the device… save, check and run unlock when it answers."</span>
                </div>
            })}
            {move || doc.with(|d| d.lossy).then(|| view! {
                <div class="sc-banner tone-orange" data-testid="sc-lossy">
                    <Icon name="triangle-alert" size=14 />
                    <span class="sc-banner-text">"This file contains bytes that are not valid UTF-8. Saving replaces them."</span>
                </div>
            })}
            {move || match lint.get() {
                LintVm::None => ().into_any(),
                LintVm::Checking => view! {
                    <div class="sc-banner tone-blue" data-testid="sc-lint-banner" data-lint="checking">
                        <span class="spinner sc-spin" aria-hidden="true"></span>
                        <span class="sc-banner-text">"Checking syntax…"</span>
                    </div>
                }.into_any(),
                LintVm::Stale => view! {
                    <div class="sc-banner tone-gray" data-testid="sc-lint-banner" data-lint="stale">
                        <Icon name="info" size=14 />
                        <span class="sc-banner-text">"Edited since the last syntax check."</span>
                    </div>
                }.into_any(),
                LintVm::Ok { saved } => view! {
                    <div class="sc-banner tone-green" data-testid="sc-lint-banner" data-lint="ok">
                        <Icon name="circle-check" size=14 />
                        <span class="sc-banner-text">{if saved { "Saved · syntax OK" } else { "Syntax OK (not saved)" }}</span>
                    </div>
                }.into_any(),
                LintVm::Syntax { message, line, saved } => view! {
                    <div class="sc-banner tone-red" data-testid="sc-lint-banner" data-lint="syntax" role="alert">
                        <Icon name="triangle-alert" size=14 />
                        <div class="sc-banner-text">
                            <b>{format!("{}Syntax error{}", if saved { "Saved, but " } else { "" }, line.map(|l| format!(" at line {l}")).unwrap_or_default())}</b>
                            <pre class="sc-banner-pre">{message}</pre>
                        </div>
                    </div>
                }.into_any(),
                LintVm::Skipped { text, saved } => view! {
                    <div class="sc-banner tone-orange" data-testid="sc-lint-banner" data-lint="skipped">
                        <Icon name="info" size=14 />
                        <span class="sc-banner-text">{if saved { format!("Saved. {text}") } else { text }}</span>
                    </div>
                }.into_any(),
            }}
            <div class="sc-editor-wrap">
                {move || ctl.failed.get().map(|m| view! {
                    <div class="sc-banner tone-red" role="alert" data-testid="sc-editor-failed">
                        <Icon name="circle-alert" size=14 />
                        <span class="sc-banner-text">{m}</span>
                    </div>
                })}
                <EditorHost ctl=ctl />
            </div>
        </div>
    }
}

// -----------------------------------------------------------------------------
// REPL
// -----------------------------------------------------------------------------

#[component]
fn ReplPane() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let input = RwSignal::new(String::new());
    let out_ref = NodeRef::<leptos::html::Div>::new();
    let key = mod_key();

    let readonly = move || store.holds_file_slot();
    let ws_mode = move || store.conn.with(|c| c.mode == "Http");
    let ws_phase = move || store.repl_ws.with(|w| w.phase);
    let enabled = move || {
        store.ready()
            && !readonly()
            && !store.repl_sending.get()
            && (!ws_mode() || ws_phase() == WsPhase::Connected)
    };
    let label = move || store.conn.with(Conn::repl_label);

    Effect::new(move |_| {
        let _ = store
            .repl
            .with(|r| r.entries.iter().map(|e| e.output.len() + 1).sum::<usize>());
        let _ = store.repl_ws.with(|w| w.term.text.len());
        request_animation_frame(move || {
            if let Some(el) = out_ref.try_get_untracked().flatten() {
                el.set_scroll_top(el.scroll_height());
            }
        });
    });

    let submit = move || {
        let text = input.get_untracked();
        if text.trim().is_empty() || !enabled() {
            return;
        }
        store.repl_submit(text);
        input.set(String::new());
    };

    let on_key = move |e: ev::KeyboardEvent| {
        let target: Option<JsValue> = e.target().map(JsValue::from);
        let caret = target
            .as_ref()
            .and_then(|t| js::get(t, "selectionStart").as_f64())
            .unwrap_or(0.0) as usize;
        let text = input.get_untracked();
        match e.key().as_str() {
            "Enter" if e.ctrl_key() || e.meta_key() => {
                e.prevent_default();
                submit();
            }
            "Enter" if !e.shift_key() => {
                e.prevent_default();
                if repl_needs_more(&text) {
                    input.set(format!("{text}\n{}", repl_next_indent(&text)));
                } else {
                    submit();
                }
            }
            "ArrowUp" if !text.get(..caret).unwrap_or("").contains('\n') => {
                if let Some(h) = store.repl.try_update(|r| r.history_prev(&text)).flatten() {
                    e.prevent_default();
                    input.set(h);
                }
            }
            "ArrowDown" if !text.get(caret..).unwrap_or("").contains('\n') => {
                if let Some(h) = store.repl.try_update(|r| r.history_next()).flatten() {
                    e.prevent_default();
                    input.set(h);
                }
            }
            "Escape" => input.set(String::new()),
            _ => {}
        }
    };

    view! {
        <div class="sc-repl" data-testid="sc-repl">
            <div class="sc-toolbar" role="toolbar" aria-label="REPL">
                <button class="btn btn-plain btn-icon btn-sm sc-list-toggle" title="Show or hide the script list" aria-label="Toggle script list"
                    on:click=move |_| store.list_open.update(|o| *o = !*o)>
                    <Icon name="panel-left" size=15 />
                </button>
                <b class="sc-doc-name">"MicroPython REPL"</b>
                <span class="badge tone-blue" data-testid="sc-repl-transport"
                    title="Wi-Fi: the device's authenticated WebSocket REPL (one line per Enter; multi-line input runs as an explicit REST eval). USB: persistent eval through the USB tunnel.">
                    {label}
                </span>
                {move || ws_mode().then(|| view! {
                    <span class=move || format!("badge tone-{}", store.repl_ws.with(|w| w.tone()))
                        data-testid="sc-repl-state" title="State of the device REPL socket">
                        {move || store.repl_ws.with(|w| w.status_text())}
                    </span>
                })}
                <span class="spacer"></span>
                {move || ws_mode().then(|| view! {
                    <button class="btn btn-sm" data-testid="sc-repl-reconnect"
                        title="Open a new device REPL session (nothing typed earlier is resent)"
                        disabled=move || !store.ready() || ws_phase().active()
                        on:click=move |_| store.ws_reconnect()>
                        <Icon name="refresh-cw" size=14 />"Reconnect"
                    </button>
                    <button class="btn btn-sm" data-testid="sc-repl-ctrlc"
                        title="Ctrl-C: interrupt the running REPL line (KeyboardInterrupt)"
                        disabled=move || ws_phase() != WsPhase::Connected || readonly()
                        on:click=move |_| store.ws_interrupt()>
                        <Icon name="square" size=14 />"Ctrl-C"
                    </button>
                    <button class="btn btn-sm" data-testid="sc-repl-ctrld"
                        title="Ctrl-D: soft reset of the MicroPython interpreter (clears variables; the device is not reset)"
                        disabled=move || !store.ready() || readonly()
                        on:click=move |_| store.dialog.set(Some(Dialog::ResetVm))>
                        "Ctrl-D"
                    </button>
                })}
                <button class="btn btn-plain btn-icon btn-sm" title="Copy output" aria-label="Copy output"
                    data-testid="sc-repl-copy"
                    on:click=move |_| {
                        let text = if ws_mode() {
                            store.repl_ws.with(|w| w.term.text.clone())
                        } else {
                            store.repl.with(ReplState::transcript)
                        };
                        copy_text(&text, "REPL output")
                    }>
                    <Icon name="copy" size=14 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title="Clear transcript (the interpreter keeps its state)" aria-label="Clear transcript"
                    on:click=move |_| {
                        if ws_mode() {
                            store.repl_ws.update(|w| w.term.clear())
                        } else {
                            store.repl.update(|r| r.entries.clear())
                        }
                    }>
                    <Icon name="trash-2" size=14 />
                </button>
                <button class="btn btn-sm" title="Reset the MicroPython interpreter (clears variables; the device is not reset)"
                    data-testid="sc-repl-reset"
                    disabled=move || !store.ready() || readonly()
                    on:click=move |_| store.dialog.set(Some(Dialog::ResetVm))>
                    <Icon name="rotate-ccw" size=14 />"Reset VM"
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title="Close the REPL" aria-label="Close REPL"
                    data-testid="sc-repl-close"
                    on:click=move |_| store.repl_open.set(false)>
                    <Icon name="x" size=15 />
                </button>
            </div>
            {move || (!store.connected()).then(|| view! {
                <div class="sc-banner tone-blue"><Icon name="info" size=14 />
                    <span class="sc-banner-text">"The REPL needs a connected device."</span></div>
            })}
            {move || store.status.get().filter(|s| s.holds_file_slot() && store.connected()).map(|s| view! {
                <div class="sc-banner tone-orange" data-testid="sc-repl-banner">
                    <Icon name="lock" size=14 />
                    <span class="sc-banner-text">{format!("'{}' is running: the REPL is read-only until it stops.", s.file_slot_name)}</span>
                    <button class="btn btn-sm btn-danger" data-testid="sc-repl-stop" on:click=move |_| store.stop()>"Stop"</button>
                </div>
            })}
            {move || store.repl_ws.with(|w| w.fail.clone()).filter(|_| ws_mode() && store.connected()).map(|f| view! {
                <div class=format!("sc-banner tone-{}", if f.reason == "busy" { "orange" } else { "red" }) data-testid="sc-repl-fail" role="alert">
                    <Icon name="info" size=14 />
                    <span class="sc-banner-text">{format!("{} Use Reconnect to try again.", f.message)}</span>
                </div>
            })}
            <div class="sc-repl-out" node_ref=out_ref data-testid="sc-repl-out">
                {move || {
                    if ws_mode() {
                        let text = store.repl_ws.with(|w| w.term.text.clone());
                        if text.is_empty() {
                            let status = store.repl_ws.with(|w| w.status_text());
                            return view! { <div class="sc-list-note">{format!("Device REPL: {status}. One line per Enter; the device echoes and prompts.")}</div> }.into_any();
                        }
                        return view! {
                            <div class="sc-repl-entry"><pre class="sc-repl-result" data-testid="sc-repl-term">{text}</pre></div>
                        }.into_any();
                    }
                    let entries = store.repl.with(|r| r.entries.clone());
                    if entries.is_empty() {
                        return view! { <div class="sc-list-note">"Enter MicroPython. Shift+Enter adds a line; Up/Down recalls history."</div> }.into_any();
                    }
                    entries.into_iter().map(|e| view! {
                        <div class="sc-repl-entry">
                            <pre class="sc-repl-echo">{e.echo.split('\n').enumerate()
                                .map(|(i, l)| format!("{}{l}", if i == 0 { ">>> " } else { "... " })).collect::<Vec<_>>().join("\n")}</pre>
                            {(!e.output.is_empty()).then(|| view! { <pre class="sc-repl-result">{e.output.join("\n")}</pre> })}
                            {e.error.map(|m| view! { <pre class="sc-repl-error">{m}</pre> })}
                        </div>
                    }).collect_view().into_any()
                }}
            </div>
            <div class="sc-repl-in">
                <span class="sc-prompt">">>>"</span>
                <textarea class="sc-repl-input" data-testid="sc-repl-input" spellcheck="false" autocomplete="off"
                    placeholder=move || if readonly() { "Read-only while a script runs" } else if ws_mode() && ws_phase() != WsPhase::Connected { "Device REPL not connected" } else { "MicroPython" }
                    rows=move || input.with(|t| t.matches('\n').count().min(7) + 1)
                    prop:value=move || input.get()
                    readonly=move || !enabled()
                    on:input=move |e| input.set(event_target_value(&e))
                    on:keydown=on_key></textarea>
                <button class="btn btn-sm btn-primary btn-icon" title=format!("Run (Enter, or {key} Enter)") aria-label="Send"
                    data-testid="sc-repl-send"
                    disabled=move || !enabled() || input.with(|t| t.trim().is_empty())
                    on:click=move |_| submit()>
                    <Icon name="play" size=14 />
                </button>
            </div>
        </div>
    }
}

// -----------------------------------------------------------------------------
// Log console
// -----------------------------------------------------------------------------

#[component]
fn Console() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let key = mod_key();
    let body = NodeRef::<leptos::html::Div>::new();
    let levels = RwSignal::new([true; 4]);
    let query = RwSignal::new(String::new());
    let search_open = RwSignal::new(false);
    let paused = RwSignal::new(false);
    let at_bottom = RwSignal::new(true);
    let follow = move || !paused.get() && at_bottom.get();

    let filter = Memo::new(move |_| LogFilter {
        levels: levels.get(),
        query: query.get(),
    });
    let visible = Memo::new(move |_| {
        let f = filter.get();
        store.log.with(|l| {
            let all: Vec<LogLine> = l.lines.iter().filter(|x| f.matches(x)).cloned().collect();
            let skip = all.len().saturating_sub(1500);
            all[skip..].to_vec()
        })
    });
    let total = Memo::new(move |_| store.log.with(|l| l.lines.len()));
    let last_id = Memo::new(move |_| visible.with(|v| v.last().map(|l| l.id)));

    let scroll_bottom = move || {
        request_animation_frame(move || {
            if let Some(el) = body.try_get_untracked().flatten() {
                el.set_scroll_top(el.scroll_height());
            }
        });
    };
    Effect::new(move |_| {
        let _ = last_id.get();
        if follow() {
            scroll_bottom();
        }
    });
    Effect::new(move |_| {
        let _ = body.get();
        scroll_bottom();
    });

    let on_scroll = move |_| {
        if let Some(el) = body.get_untracked() {
            at_bottom.set(el.scroll_top() + el.client_height() >= el.scroll_height() - 8);
        }
    };

    // Drag-resize with window-level listeners (removed on cleanup).
    let dragging: StoredValue<Option<(f64, f64)>> = StoredValue::new(None);
    let listeners: StoredValue<
        Option<(
            Closure<dyn FnMut(web_sys::MouseEvent)>,
            Closure<dyn FnMut(web_sys::MouseEvent)>,
        )>,
        LocalStorage,
    > = StoredValue::new_local(None);
    {
        let mv = Closure::<dyn FnMut(web_sys::MouseEvent)>::new(move |e: web_sys::MouseEvent| {
            if let Some((y0, h0)) = dragging.get_value() {
                let max = web_sys::window()
                    .and_then(|w| w.inner_height().ok())
                    .and_then(|h| h.as_f64())
                    .unwrap_or(900.0)
                    * 0.7;
                store
                    .console_height
                    .set((h0 + (y0 - e.client_y() as f64)).clamp(96.0, max));
            }
        });
        let up = Closure::<dyn FnMut(web_sys::MouseEvent)>::new(move |_| dragging.set_value(None));
        if let Some(w) = web_sys::window() {
            let _ = w.add_event_listener_with_callback("mousemove", mv.as_ref().unchecked_ref());
            let _ = w.add_event_listener_with_callback("mouseup", up.as_ref().unchecked_ref());
        }
        listeners.set_value(Some((mv, up)));
    }
    on_cleanup(move || {
        if let Some(w) = web_sys::window() {
            listeners.with_value(|l| {
                if let Some((mv, up)) = l {
                    let _ = w.remove_event_listener_with_callback(
                        "mousemove",
                        mv.as_ref().unchecked_ref(),
                    );
                    let _ = w.remove_event_listener_with_callback(
                        "mouseup",
                        up.as_ref().unchecked_ref(),
                    );
                }
            });
        }
        listeners.set_value(None);
    });

    let filtered_text = move || {
        let f = filter.get_untracked();
        store.log.with_untracked(|l| l.filtered_text(&f))
    };

    view! {
        <section class="sc-console" data-testid="sc-console" style=move || format!("height: {:.0}px", store.console_height.get())>
            <div class="sc-resize" role="separator" aria-orientation="horizontal" aria-label="Resize log"
                tabindex="0" title="Drag to resize"
                data-testid="sc-resize"
                on:mousedown=move |e| {
                    e.prevent_default();
                    dragging.set_value(Some((e.client_y() as f64, store.console_height.get_untracked())));
                }
                on:keydown=move |e: ev::KeyboardEvent| match e.key().as_str() {
                    "ArrowUp" => { e.prevent_default(); store.console_height.update(|h| *h = (*h + 24.0).min(700.0)); }
                    "ArrowDown" => { e.prevent_default(); store.console_height.update(|h| *h = (*h - 24.0).max(96.0)); }
                    _ => {}
                }></div>
            <div class="sc-console-head">
                <b>"Script log"</b>
                <span class="subtle sc-console-count" data-testid="sc-log-count">{move || format!("{} lines", total.get())}</span>
                <span class="spacer"></span>
                <div class="btn-group sc-levels" role="group" aria-label="Levels">
                    {LogLevel::ALL.into_iter().map(|lv| view! {
                        <button class="btn btn-xs" title=format!("Show {} lines", lv.label())
                            data-testid=format!("sc-level-{}", lv.letter())
                            aria-pressed=move || levels.with(|l| l[lv.index()]).to_string()
                            class:active=move || levels.with(|l| l[lv.index()])
                            on:click=move |_| levels.update(|l| l[lv.index()] = !l[lv.index()])>
                            {lv.letter()}
                        </button>
                    }).collect_view()}
                </div>
                <button class="btn btn-plain btn-icon btn-sm" title="Filter text" aria-label="Filter text"
                    data-testid="sc-log-search-toggle"
                    class:active=move || search_open.get()
                    on:click=move |_| search_open.update(|o| { *o = !*o; if !*o { query.set(String::new()); } })>
                    <Icon name="search" size=14 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm"
                    title=move || if paused.get() { "Resume following the log" } else { "Pause following the log" }
                    aria-label="Pause or resume" data-testid="sc-log-pause"
                    class:active=move || paused.get()
                    on:click=move |_| { paused.update(|p| *p = !*p); if !paused.get_untracked() { scroll_bottom(); } }>
                    {move || if paused.get() { view! { <Icon name="play" size=14 /> }.into_any() } else { view! { <Icon name="pause" size=14 /> }.into_any() }}
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title="Copy the visible log" aria-label="Copy log" data-testid="sc-log-copy"
                    on:click=move |_| copy_text(&filtered_text(), "log")>
                    <Icon name="copy" size=14 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title="Export the log to a text file" aria-label="Export log" data-testid="sc-log-export"
                    on:click=move |_| {
                        let name = format!("bugbuster-script-log-{}.txt", timestamp_for_file());
                        if !download_text(&name, &filtered_text()) {
                            show_toast("Could not export the log", "err");
                        }
                    }>
                    <Icon name="download" size=14 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title="Clear this view. Device history is not cleared and cleared lines do not come back."
                    aria-label="Clear local log" data-testid="sc-log-clear"
                    on:click=move |_| store.log.update(LogStore::clear)>
                    <Icon name="trash-2" size=14 />
                </button>
                <button class="btn btn-plain btn-icon btn-sm" title=format!("Hide the log ({key} J)") aria-label="Hide log" data-testid="sc-log-close"
                    on:click=move |_| store.console_open.set(false)>
                    <Icon name="chevron-down" size=15 />
                </button>
            </div>
            {move || search_open.get().then(|| view! {
                <div class="sc-console-search">
                    <input type="search" placeholder="Filter text or source" aria-label="Filter log text" data-testid="sc-log-query"
                        prop:value=move || query.get()
                        on:input=move |e| query.set(event_target_value(&e)) />
                </div>
            })}
            <div class="sc-console-body" node_ref=body on:scroll=on_scroll data-testid="sc-log-body">
                <For each=move || visible.get() key=|l| l.id children=move |l| {
                    if l.is_marker {
                        view! { <div class="sc-log-marker" data-testid="sc-log-marker">{format!("--- {} ---", l.text)}</div> }.into_any()
                    } else {
                        let lvl = l.level.letter();
                        let ts = l.timestamp();
                        let has_ts = l.ts_ms.is_some();
                        view! {
                            <div class=format!("sc-log-line lv-{lvl}") data-testid="sc-log-line">
                                {has_ts.then(|| view! { <span class="sc-log-ts">{ts}</span><span class="sc-log-lv">{lvl}</span> })}
                                <span class="sc-log-text">{l.text}</span>
                            </div>
                        }.into_any()
                    }
                } />
                {move || (visible.with(Vec::is_empty)).then(|| view! {
                    <div class="sc-log-empty">{if total.get() == 0 { "No output yet" } else { "No lines match the filter" }}</div>
                })}
            </div>
            {move || (!follow()).then(|| view! {
                <button class="btn btn-sm btn-primary sc-latest" data-testid="sc-log-latest"
                    on:click=move |_| { paused.set(false); at_bottom.set(true); scroll_bottom(); }>
                    <Icon name="chevron-down" size=13 />"Latest"
                </button>
            })}
        </section>
    }
}

// -----------------------------------------------------------------------------
// Docs inspector
// -----------------------------------------------------------------------------

#[component]
fn DocsPanel() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let ctl = expect_context::<EditorCtl>();
    let query = RwSignal::new(String::new());
    let filter = RwSignal::new("all".to_string());
    let selected = RwSignal::new(Option::<String>::None);
    let cat_rev = RwSignal::new(0u32);

    // Re-run searches when the shared catalogue (re)loads.
    let sub: StoredValue<Option<(Closure<dyn FnMut()>, JsValue)>, LocalStorage> =
        StoredValue::new_local(None);
    if let Some(c) = catalogue() {
        let cb = Closure::<dyn FnMut()>::new(move || cat_rev.update(|n| *n += 1));
        if let Ok(unsub) = js::call(&c, "onChange", &[cb.as_ref().clone()]) {
            sub.set_value(Some((cb, unsub)));
        }
    }
    on_cleanup(move || {
        sub.with_value(|s| {
            if let Some((_, unsub)) = s {
                if let Some(f) = unsub.dyn_ref::<js_sys::Function>() {
                    let _ = f.call0(&JsValue::NULL);
                }
            }
        });
        sub.set_value(None);
    });

    let status = Memo::new(move |_| {
        let _ = cat_rev.get();
        catalogue_status()
    });
    let results = Memo::new(move |_| {
        let _ = cat_rev.get();
        if status.get().0 != "ready" {
            return Vec::<DocEntry>::new();
        }
        catalogue_search(&query.get(), &filter.get())
    });
    let detail = Memo::new(move |_| {
        let _ = cat_rev.get();
        selected.get().and_then(|id| catalogue_entry(&id))
    });
    let can_insert = move || {
        ctl.ready.get() && store.model.with(|m| m.open_name().is_some()) && !store.repl_open.get()
    };

    let insert_id = move |id: String| {
        ctl.insert_doc(&JsValue::from_str(&id));
    };
    // Function examples are plain code: insert with the entry's imports.
    let insert_code = move |code: String, imports: Vec<String>| {
        let arr = js_sys::Array::new();
        for i in &imports {
            arr.push(&JsValue::from_str(i));
        }
        let entry = js::object(&[
            ("kind", JsValue::from_str("snippet")),
            ("imports", arr.into()),
            ("insertText", JsValue::from_str(&code)),
        ]);
        ctl.insert_doc(&entry);
    };

    view! {
        <aside class="sc-docs" data-testid="sc-docs" aria-label="API docs">
            <div class="inspector-header">
                <span>"API docs"</span>
                <button class="btn btn-plain btn-icon btn-sm" title="Close docs" aria-label="Close docs"
                    on:click=move |_| store.docs_open.set(false)><Icon name="x" size=14 /></button>
            </div>
            <div class="sc-docs-controls">
                <input type="search" placeholder="Functions, classes, examples" aria-label="Search API docs"
                    data-testid="sc-docs-search"
                    prop:value=move || query.get()
                    on:input=move |e| { query.set(event_target_value(&e)); selected.set(None); } />
                <select class="dropdown dropdown-sm" aria-label="Kind" data-testid="sc-docs-kind"
                    on:change=move |e| { filter.set(event_target_value(&e)); selected.set(None); }>
                    <option value="all">"All"</option>
                    <option value="modules">"Modules"</option>
                    <option value="functions">"Functions"</option>
                    <option value="classes">"Classes"</option>
                    <option value="constants">"Constants"</option>
                    <option value="examples">"Examples"</option>
                </select>
            </div>
            <div class="sc-docs-body">
                {move || {
                    let (state, message) = status.get();
                    if state != "ready" {
                        let text = match state.as_str() {
                            "loading" => "Loading the API catalogue…".to_string(),
                            "failed" => format!("API catalogue unavailable: {message}"),
                            "missing" => "API catalogue unavailable: the bundle did not load.".to_string(),
                            _ => "API catalogue not loaded yet.".to_string(),
                        };
                        return view! {
                            <div class="sc-banner tone-orange" data-testid="sc-docs-state">
                                <Icon name="info" size=14 />
                                <span class="sc-banner-text">{text}</span>
                                <button class="btn btn-sm" on:click=move |_| catalogue_load()>"Retry"</button>
                            </div>
                        }.into_any();
                    }
                    if let Some(e) = detail.get() {
                        let id = e.id.clone();
                        let id_ins = e.id.clone();
                        let imports = e.imports.clone();
                        let is_example = e.kind == "example";
                        let sig = e.signature.clone();
                        let show_sig = !sig.is_empty() && !is_example;
                        let src = e.source.clone().unwrap_or_default();
                        let src_copy = src.clone();
                        let call_text = e.insert_text.clone();
                        let call_copy = call_text.clone();
                        let call_imports = imports.clone();
                        return view! {
                            <div class="sc-doc" data-testid="sc-doc-detail">
                                <button class="btn btn-sm btn-plain" on:click=move |_| selected.set(None)>
                                    <Icon name="chevron-left" size=14 />"Back"
                                </button>
                                <h3 class="sc-doc-title">{e.path.clone()}</h3>
                                {show_sig.then(|| view! { <pre class="sc-code sc-code-sig">{sig}</pre> })}
                                {(!e.summary.is_empty()).then(|| view! { <p class="sc-doc-summary">{plain_doc(&e.summary)}</p> })}
                                {(!e.description.is_empty()).then(|| view! { <p>{plain_doc(&e.description)}</p> })}
                                {(e.description.is_empty() && e.summary.is_empty() && !e.doc.is_empty())
                                    .then(|| view! { <p>{plain_doc(&e.doc)}</p> })}
                                {e.value.clone().filter(|_| e.kind == "constant").map(|v| view! {
                                    <div class="sc-doc-sec"><h4>"Value"</h4><pre class="sc-code">{v}</pre></div>
                                })}
                                {(!e.params.is_empty()).then(|| view! {
                                    <div class="sc-doc-sec" data-testid="sc-doc-params"><h4>"Parameters"</h4>
                                        {e.params.iter().map(|p| view! {
                                            <div class="sc-param">
                                                <div class="sc-param-head">
                                                    <code>{param_name(p)}</code>
                                                    {p.annotation.clone().map(|a| view! { <code class="sc-param-type">{a}</code> })}
                                                    <span class="spacer"></span>
                                                    <span class="subtle">{param_requirement(p)}</span>
                                                </div>
                                                {(!p.doc.is_empty()).then(|| view! { <div class="sc-param-doc">{plain_doc(&p.doc)}</div> })}
                                            </div>
                                        }).collect_view()}
                                    </div>
                                })}
                                {(e.returns.as_deref().is_some_and(|r| r != "None") || !e.returns_doc.is_empty()).then(|| view! {
                                    <div class="sc-doc-sec" data-testid="sc-doc-returns"><h4>"Returns"</h4>
                                        {e.returns.clone().map(|r| view! { <code class="sc-param-type">{format!("-> {r}")}</code> })}
                                        {(!e.returns_doc.is_empty()).then(|| view! { <p>{plain_doc(&e.returns_doc)}</p> })}
                                        {e.return_keys.iter().map(|k| view! {
                                            <div class="sc-param"><code>{format!("\"{}\"", k.name)}</code>
                                                <div class="sc-param-doc">{plain_doc(&k.doc)}</div></div>
                                        }).collect_view()}
                                    </div>
                                })}
                                {(!e.raises.is_empty()).then(|| view! {
                                    <div class="sc-doc-sec" data-testid="sc-doc-raises"><h4>"Raises"</h4>
                                        {e.raises.iter().map(|r| view! {
                                            <div class="sc-param"><code>{r.kind.clone()}</code>
                                                <div class="sc-param-doc">{plain_doc(&r.doc)}</div></div>
                                        }).collect_view()}
                                    </div>
                                })}
                                {(!e.notes.is_empty()).then(|| view! {
                                    <div class="sc-banner tone-orange" data-testid="sc-doc-safety">
                                        <Icon name="triangle-alert" size=14 />
                                        <div class="sc-banner-text"><b>"Safety"</b><div>{plain_doc(&e.notes)}</div></div>
                                    </div>
                                })}
                                {is_example.then(|| view! {
                                    <div class="sc-doc-sec"><h4>"Example"</h4>
                                        <pre class="sc-code">{src.clone()}</pre>
                                        <div class="hstack">
                                            <button class="btn btn-sm btn-primary" data-testid="sc-doc-insert"
                                                disabled=move || !can_insert()
                                                title=move || if can_insert() { "Insert at the cursor" } else { "Open a script to insert" }
                                                on:click={let id = id_ins.clone(); move |_| insert_id(id.clone())}>"Insert example"</button>
                                            <button class="btn btn-sm" on:click={let c = src_copy.clone(); move |_| copy_text(&c, "example")}>"Copy"</button>
                                        </div>
                                    </div>
                                })}
                                {(!is_example).then(|| {
                                    let examples = e.examples.clone();
                                    let many = examples.len() > 1;
                                    let imports_for = imports.clone();
                                    view! {
                                        <div class="sc-doc-sec" data-testid="sc-doc-examples">
                                            {examples.into_iter().enumerate().map(|(i, code)| {
                                                let c1 = code.clone();
                                                let c2 = code.clone();
                                                let imps = imports_for.clone();
                                                view! {
                                                    <h4>{if many { format!("Example {}", i + 1) } else { "Example".to_string() }}</h4>
                                                    <pre class="sc-code">{code}</pre>
                                                    <div class="hstack">
                                                        <button class="btn btn-sm btn-primary"
                                                            disabled=move || !can_insert()
                                                            on:click=move |_| insert_code(c1.clone(), imps.clone())>"Insert"</button>
                                                        <button class="btn btn-sm" on:click=move |_| copy_text(&c2, "example")>"Copy"</button>
                                                    </div>
                                                }
                                            }).collect_view()}
                                            <h4>"Call"</h4>
                                            <pre class="sc-code">{call_text.clone()}</pre>
                                            <div class="hstack">
                                                <button class="btn btn-sm btn-primary" data-testid="sc-doc-insert"
                                                    disabled=move || !can_insert()
                                                    title=move || if can_insert() { "Insert at the cursor" } else { "Open a script to insert" }
                                                    on:click={let id = id.clone(); move |_| insert_id(id.clone())}>"Insert call"</button>
                                                <button class="btn btn-sm" on:click={let c = call_copy.clone(); move |_| copy_text(&c, "call")}>"Copy"</button>
                                            </div>
                                            {(!call_imports.is_empty()).then(|| view! {
                                                <div class="subtle sc-doc-imports">{format!("Adds if missing: {}", call_imports.join("; "))}</div>
                                            })}
                                        </div>
                                    }
                                })}
                            </div>
                        }.into_any();
                    }
                    let list = results.get();
                    if list.is_empty() {
                        return view! { <div class="sc-list-note" data-testid="sc-docs-empty">"No matches"</div> }.into_any();
                    }
                    view! {
                        <div class="sc-doc-list" data-testid="sc-doc-list">
                            {list.into_iter().map(|e| {
                                let id = e.id.clone();
                                view! {
                                    <button class="sc-doc-row" data-doc=e.id.clone() on:click=move |_| selected.set(Some(id.clone()))>
                                        <span class="sc-kind">{e.kind_tag()}</span>
                                        <span class="sc-doc-row-main">
                                            <span class="sc-doc-row-title">{e.path.clone()}</span>
                                            {(!e.summary.is_empty()).then(|| view! { <span class="sc-doc-row-sum">{plain_doc(&e.summary)}</span> })}
                                        </span>
                                    </button>
                                }
                            }).collect_view()}
                        </div>
                    }.into_any()
                }}
            </div>
        </aside>
    }
}

// -----------------------------------------------------------------------------
// Dialogs
// -----------------------------------------------------------------------------

#[component]
fn Dialogs() -> impl IntoView {
    let store = expect_context::<ScriptStore>();
    let name_input = RwSignal::new(String::new());
    let input_ref = NodeRef::<leptos::html::Input>::new();

    Effect::new(move |_| {
        if matches!(store.dialog.get(), Some(Dialog::New { .. })) {
            name_input.set(String::new());
            request_animation_frame(move || {
                if let Some(el) = input_ref.try_get_untracked().flatten() {
                    let _ = el.focus();
                }
            });
        }
    });

    // Modal behaviour: take focus on open, hand it back on close, Escape cancels, Tab stays inside.
    let restore: StoredValue<Option<web_sys::HtmlElement>, LocalStorage> =
        StoredValue::new_local(None);
    Effect::new(move |was_open: Option<bool>| {
        let open = store.dialog.with(Option::is_some) || store.replace.with(Option::is_some);
        if open {
            if was_open != Some(true) {
                restore.set_value(active_html_element());
            }
            request_animation_frame(focus_modal);
        } else if was_open == Some(true) {
            request_animation_frame(move || {
                if let Some(Some(el)) = restore.try_update_value(Option::take) {
                    let _ = el.focus();
                }
            });
        }
        open
    });
    let key_handle = window_event_listener(ev::keydown, move |e: ev::KeyboardEvent| {
        let replace_open = store.replace.with_untracked(Option::is_some);
        let dialog_open = store.dialog.with_untracked(Option::is_some);
        if !replace_open && !dialog_open {
            return;
        }
        match e.key().as_str() {
            "Escape" => {
                e.prevent_default();
                if replace_open {
                    store.cancel_replace();
                } else {
                    store.dialog.set(None);
                }
            }
            "Tab" => trap_tab(&e),
            _ => {}
        }
    });
    on_cleanup(move || key_handle.remove());

    let close = move || store.dialog.set(None);
    let dirty = move |n: &str| store.model.with_untracked(|m| m.is_dirty(n));

    view! {
        {move || store.replace.get().map(|p| view! {
            <div class="scrim" on:click=move |_| store.cancel_replace()></div>
            <div class="dialog popover sc-dialog" role="alertdialog" aria-modal="true" aria-labelledby="sc-replace-title" data-testid="sc-replace">
                <div class="group-title" id="sc-replace-title">{p.message()}</div>
                <p class="sc-muted">"Only one script runs at a time. The running script is asked to stop (up to 3 s), then the interpreter resets. Output stays in the log."</p>
                <div class="sc-dialog-actions">
                    <button class="btn btn-sm" on:click=move |_| store.cancel_replace()>"Cancel"</button>
                    <button class="btn btn-sm btn-danger" data-testid="sc-replace-confirm"
                        on:click=move |_| store.confirm_replace()>"Stop and run"</button>
                </div>
            </div>
        })}
        {move || store.dialog.get().map(|d| {
            match d {
                Dialog::New { save_as } => {
                    let title = if save_as { "Save script as" } else { "New script" };
                    let action = if save_as { "Save" } else { "Create" };
                    let submit = move || {
                        let raw = name_input.get_untracked();
                        if normalize_name(&raw).is_none() { return; }
                        let content = save_as.then(|| {
                            store.model.with_untracked(|m| m.buffer(SCRATCH).map(|b| b.text.clone()).unwrap_or_default())
                        });
                        store.dialog.set(None);
                        store.create_file(raw, content);
                    };
                    let valid = move || normalize_name(&name_input.get()).is_some();
                    view! {
                        <div class="scrim" on:click=move |_| close()></div>
                        <div class="dialog popover sc-dialog" role="dialog" aria-modal="true" aria-labelledby="sc-dlg-title" data-testid="sc-dialog-new">
                            <div class="group-title" id="sc-dlg-title">{title}</div>
                            <input type="text" class="input sc-name-input" placeholder="script_name.py" aria-label="Script name"
                                data-testid="sc-name-input" node_ref=input_ref autocomplete="off" spellcheck="false"
                                prop:value=move || name_input.get()
                                on:input=move |e| name_input.set(event_target_value(&e))
                                on:keydown=move |e: ev::KeyboardEvent| if e.key() == "Enter" { e.prevent_default(); submit(); } />
                            <p class=move || if name_input.with(|n| n.trim().is_empty()) || valid() { "sc-muted" } else { "sc-muted sc-bad" }
                                data-testid="sc-name-hint">
                                {move || if name_input.with(|n| n.trim().is_empty()) || valid() {
                                    name_rules().to_string()
                                } else {
                                    format!("Not a valid name. {}", name_rules())
                                }}
                            </p>
                            <div class="sc-dialog-actions">
                                <button class="btn btn-sm" on:click=move |_| close()>"Cancel"</button>
                                <button class="btn btn-sm btn-primary" data-testid="sc-dialog-confirm"
                                    disabled=move || !valid() on:click=move |_| submit()>{action}</button>
                            </div>
                        </div>
                    }.into_any()
                }
                Dialog::Delete(name) => {
                    let n = name.clone();
                    let warn = dirty(&name);
                    view! {
                        <div class="scrim" on:click=move |_| close()></div>
                        <div class="dialog popover sc-dialog" role="alertdialog" aria-modal="true" data-testid="sc-dialog-delete">
                            <div class="group-title">{format!("Delete '{name}' from the device?")}</div>
                            <p class="sc-muted">{if warn { "Its unsaved edits in this app are discarded too. This cannot be undone." } else { "This cannot be undone." }}</p>
                            <div class="sc-dialog-actions">
                                <button class="btn btn-sm" on:click=move |_| close()>"Cancel"</button>
                                <button class="btn btn-sm btn-danger" data-testid="sc-dialog-confirm"
                                    on:click=move |_| { close(); store.delete_file(n.clone()); }>"Delete"</button>
                            </div>
                        </div>
                    }.into_any()
                }
                Dialog::Revert(name) => {
                    let n = name.clone();
                    view! {
                        <div class="scrim" on:click=move |_| close()></div>
                        <div class="dialog popover sc-dialog" role="alertdialog" aria-modal="true" data-testid="sc-dialog-revert">
                            <div class="group-title">{format!("Discard unsaved changes to '{name}'?")}</div>
                            <p class="sc-muted">"The editor reloads the copy stored on the device."</p>
                            <div class="sc-dialog-actions">
                                <button class="btn btn-sm" on:click=move |_| close()>"Keep editing"</button>
                                <button class="btn btn-sm btn-danger" data-testid="sc-dialog-confirm"
                                    on:click=move |_| { close(); store.revert_file(n.clone()); }>"Discard and reload"</button>
                            </div>
                        </div>
                    }.into_any()
                }
                Dialog::AutorunEnable(name) => {
                    let n = name.clone();
                    let changing = store.autorun.with_untracked(|a| a.as_ref().is_some_and(|a| a.enabled));
                    view! {
                        <div class="scrim" on:click=move |_| close()></div>
                        <div class="dialog popover sc-dialog" role="alertdialog" aria-modal="true" data-testid="sc-dialog-autorun">
                            <div class="group-title">{format!("Run '{name}' automatically at every boot?")}</div>
                            <p class="sc-muted">{if changing { "This replaces the current autorun script. " } else { "" }}
                                "It starts on its own after each power-up unless IO12 is held low. Nothing else changes."</p>
                            <div class="sc-dialog-actions">
                                <button class="btn btn-sm" on:click=move |_| close()>"Cancel"</button>
                                <button class="btn btn-sm btn-primary" data-testid="sc-dialog-confirm"
                                    on:click=move |_| { close(); store.set_autorun(Some(n.clone())); }>"Enable autorun"</button>
                            </div>
                        </div>
                    }.into_any()
                }
                Dialog::AutorunDisable => view! {
                    <div class="scrim" on:click=move |_| close()></div>
                    <div class="dialog popover sc-dialog" role="alertdialog" aria-modal="true" data-testid="sc-dialog-autorun">
                        <div class="group-title">"Disable autorun on boot?"</div>
                        <p class="sc-muted">"The stored script is kept; it just stops starting at boot."</p>
                        <div class="sc-dialog-actions">
                            <button class="btn btn-sm" on:click=move |_| close()>"Cancel"</button>
                            <button class="btn btn-sm btn-danger" data-testid="sc-dialog-confirm"
                                on:click=move |_| { close(); store.set_autorun(None); }>"Disable autorun"</button>
                        </div>
                    </div>
                }.into_any(),
                Dialog::ResetVm => view! {
                    <div class="scrim" on:click=move |_| close()></div>
                    <div class="dialog popover sc-dialog" role="alertdialog" aria-modal="true" data-testid="sc-dialog-reset">
                        <div class="group-title">"Reset the MicroPython interpreter?"</div>
                        <p class="sc-muted">"Clears REPL variables and imports. The device itself is not reset."</p>
                        <div class="sc-dialog-actions">
                            <button class="btn btn-sm" on:click=move |_| close()>"Cancel"</button>
                            <button class="btn btn-sm btn-danger" data-testid="sc-dialog-confirm"
                                on:click=move |_| { close(); store.reset_vm(); }>"Reset interpreter"</button>
                        </div>
                    </div>
                }.into_any(),
                Dialog::SaveLossy(name) => {
                    let n = name.clone();
                    view! {
                        <div class="scrim" on:click=move |_| close()></div>
                        <div class="dialog popover sc-dialog" role="alertdialog" aria-modal="true" data-testid="sc-dialog-lossy">
                            <div class="group-title">{format!("Overwrite '{name}'?")}</div>
                            <p class="sc-muted">"The stored file has bytes that are not valid UTF-8. Saving writes the text shown in the editor instead."</p>
                            <div class="sc-dialog-actions">
                                <button class="btn btn-sm" on:click=move |_| close()>"Cancel"</button>
                                <button class="btn btn-sm btn-danger" data-testid="sc-dialog-confirm"
                                    on:click=move |_| { close(); store.save_confirmed(n.clone()); }>"Save anyway"</button>
                            </div>
                        </div>
                    }.into_any()
                }
            }
        })}
    }
}
