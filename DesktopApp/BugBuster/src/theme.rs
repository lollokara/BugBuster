//! Appearance: System / Light / Dark preference, persisted in localStorage and
//! applied as `data-theme` on <html>. Canvas renderers read colours through
//! `css_var` and subscribe to `ThemeCtx::resolved` to repaint on change.

use leptos::prelude::*;
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;

const STORAGE_KEY: &str = "bb-theme";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ThemePref {
    System,
    Light,
    Dark,
}

impl ThemePref {
    pub fn as_str(self) -> &'static str {
        match self {
            ThemePref::System => "system",
            ThemePref::Light => "light",
            ThemePref::Dark => "dark",
        }
    }
    fn parse(s: &str) -> Self {
        match s {
            "light" => ThemePref::Light,
            "dark" => ThemePref::Dark,
            _ => ThemePref::System,
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            ThemePref::System => "System",
            ThemePref::Light => "Light",
            ThemePref::Dark => "Dark",
        }
    }
}

#[derive(Clone, Copy)]
pub struct ThemeCtx {
    pub pref: RwSignal<ThemePref>,
    /// Effective theme: "light" or "dark".
    pub resolved: Signal<&'static str>,
}

fn storage() -> Option<web_sys::Storage> {
    web_sys::window()?.local_storage().ok().flatten()
}

fn system_query() -> Option<web_sys::MediaQueryList> {
    web_sys::window()?
        .match_media("(prefers-color-scheme: dark)")
        .ok()
        .flatten()
}

/// Install the theme context. Call once from `App`.
pub fn provide_theme() -> ThemeCtx {
    let initial = storage()
        .and_then(|s| s.get_item(STORAGE_KEY).ok().flatten())
        .map(|s| ThemePref::parse(&s))
        .unwrap_or(ThemePref::System);
    let pref = RwSignal::new(initial);
    let system_dark = RwSignal::new(system_query().map(|q| q.matches()).unwrap_or(true));

    if let Some(q) = system_query() {
        let cb = Closure::<dyn FnMut(web_sys::MediaQueryListEvent)>::new(
            move |e: web_sys::MediaQueryListEvent| system_dark.set(e.matches()),
        );
        let _ = q.add_event_listener_with_callback("change", cb.as_ref().unchecked_ref());
        // INTENTIONAL: app-lifetime listener
        cb.forget();
    }

    let resolved = Signal::derive(move || match pref.get() {
        ThemePref::Light => "light",
        ThemePref::Dark => "dark",
        ThemePref::System => {
            if system_dark.get() {
                "dark"
            } else {
                "light"
            }
        }
    });

    Effect::new(move |_| {
        let theme = resolved.get();
        if let Some(root) = web_sys::window()
            .and_then(|w| w.document())
            .and_then(|d| d.document_element())
        {
            let _ = root.set_attribute("data-theme", theme);
        }
    });
    Effect::new(move |_| {
        let p = pref.get();
        if let Some(s) = storage() {
            let _ = s.set_item(STORAGE_KEY, p.as_str());
        }
    });

    let ctx = ThemeCtx { pref, resolved };
    provide_context(ctx);
    ctx
}

pub fn use_theme() -> ThemeCtx {
    expect_context::<ThemeCtx>()
}

/// Current value of a CSS custom property on <html>, e.g. `css_var("--ch-a")`.
/// Read inside a draw call so the colour follows the active theme.
pub fn css_var(name: &str) -> String {
    web_sys::window()
        .and_then(|w| {
            let el = w.document()?.document_element()?;
            w.get_computed_style(&el).ok().flatten()
        })
        .and_then(|s| s.get_property_value(name).ok())
        .map(|v| v.trim().to_string())
        .unwrap_or_default()
}
