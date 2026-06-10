use tauri::{AppHandle, Manager};

#[tauri::command]
pub fn get_device_token(app: AppHandle, mac: String) -> Option<String> {
    let mgr = app.state::<crate::connection_manager::ConnectionManager>();
    mgr.get_token(&mac, &app)
}

#[tauri::command]
pub fn save_device_token(app: AppHandle, mac: String, token: String) {
    let mgr = app.state::<crate::connection_manager::ConnectionManager>();
    mgr.save_token(mac, token, &app);
}
