use serde::de::DeserializeOwned;
use tauri::{plugin::PluginApi, AppHandle, Runtime};

use crate::models::*;

/// Desktop is a no-op — this plugin only does anything on iOS.
pub fn init<R: Runtime, C: DeserializeOwned>(
    app: &AppHandle<R>,
    _api: PluginApi<R, C>,
) -> crate::Result<IosGlassTabbar<R>> {
    Ok(IosGlassTabbar(app.clone()))
}

/// Access to the ios-glass-tabbar APIs.
pub struct IosGlassTabbar<R: Runtime>(AppHandle<R>);

impl<R: Runtime> IosGlassTabbar<R> {
    pub fn set_items(&self, _payload: SetItemsRequest) -> crate::Result<()> {
        Ok(())
    }
    pub fn set_active_tab(&self, _payload: SetActiveTabRequest) -> crate::Result<()> {
        Ok(())
    }
    pub fn set_hidden(&self, _payload: SetHiddenRequest) -> crate::Result<()> {
        Ok(())
    }
    pub fn set_badge(&self, _payload: SetBadgeRequest) -> crate::Result<()> {
        Ok(())
    }
    pub fn register_listener<T: serde::Serialize>(&self, _payload: T) -> crate::Result<()> {
        Ok(())
    }
}
