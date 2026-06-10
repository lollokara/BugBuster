use serde::de::DeserializeOwned;
use serde::Serialize;
use tauri::{
    plugin::{PluginApi, PluginHandle},
    AppHandle, Runtime,
};

use crate::models::*;

#[cfg(target_os = "ios")]
tauri::ios_plugin_binding!(init_plugin_ios_glass_tabbar);

/// Registers the native plugin. iOS-only: the native glass tab bar is a UITabBar
/// (Liquid Glass on the iOS 26 SDK). On Android (and any other mobile target) the
/// commands are no-ops so cross-platform apps still compile + run.
pub fn init<R: Runtime, C: DeserializeOwned>(
    _app: &AppHandle<R>,
    #[allow(unused_variables)] api: PluginApi<R, C>,
) -> crate::Result<IosGlassTabbar<R>> {
    #[cfg(target_os = "ios")]
    let handle = Some(api.register_ios_plugin(init_plugin_ios_glass_tabbar)?);
    #[cfg(not(target_os = "ios"))]
    let handle: Option<PluginHandle<R>> = None;
    Ok(IosGlassTabbar(handle))
}

/// Access to the ios-glass-tabbar APIs.
pub struct IosGlassTabbar<R: Runtime>(Option<PluginHandle<R>>);

impl<R: Runtime> IosGlassTabbar<R> {
    fn call<T: Serialize>(&self, cmd: &str, payload: T) -> crate::Result<()> {
        match &self.0 {
            Some(handle) => handle
                .run_mobile_plugin::<serde_json::Value>(cmd, payload)
                .map(|_| ())
                .map_err(Into::into),
            None => Ok(()),
        }
    }

    pub fn set_items(&self, payload: SetItemsRequest) -> crate::Result<()> {
        self.call("setItems", payload)
    }
    pub fn set_active_tab(&self, payload: SetActiveTabRequest) -> crate::Result<()> {
        self.call("setActiveTab", payload)
    }
    pub fn set_hidden(&self, payload: SetHiddenRequest) -> crate::Result<()> {
        self.call("setHidden", payload)
    }
    pub fn set_badge(&self, payload: SetBadgeRequest) -> crate::Result<()> {
        self.call("setBadge", payload)
    }
    pub fn register_listener<T: Serialize>(&self, payload: T) -> crate::Result<()> {
        self.call("registerListener", payload)
    }
}
