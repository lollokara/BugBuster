use tauri::{command, AppHandle, Runtime};

use crate::models::*;
use crate::IosGlassTabbarExt;
use crate::Result;

#[command]
pub(crate) async fn set_items<R: Runtime>(
    app: AppHandle<R>,
    payload: SetItemsRequest,
) -> Result<()> {
    app.ios_glass_tabbar().set_items(payload)
}

#[command]
pub(crate) async fn set_active_tab<R: Runtime>(
    app: AppHandle<R>,
    payload: SetActiveTabRequest,
) -> Result<()> {
    app.ios_glass_tabbar().set_active_tab(payload)
}

#[command]
pub(crate) async fn set_badge<R: Runtime>(
    app: AppHandle<R>,
    payload: SetBadgeRequest,
) -> Result<()> {
    app.ios_glass_tabbar().set_badge(payload)
}

#[command]
pub(crate) async fn set_hidden<R: Runtime>(
    app: AppHandle<R>,
    payload: SetHiddenRequest,
) -> Result<()> {
    app.ios_glass_tabbar().set_hidden(payload)
}

#[derive(serde::Serialize)]
struct RegisterListenerPayload {
    event: String,
    handler: tauri::ipc::Channel,
}

#[command]
pub(crate) async fn register_listener<R: Runtime>(
    app: AppHandle<R>,
    event: String,
    handler: tauri::ipc::Channel,
) -> Result<()> {
    let payload = RegisterListenerPayload { event, handler };
    app.ios_glass_tabbar().register_listener(payload)
}

#[command]
pub(crate) async fn registerListener<R: Runtime>(
    app: AppHandle<R>,
    event: String,
    handler: tauri::ipc::Channel,
) -> Result<()> {
    register_listener(app, event, handler).await
}
