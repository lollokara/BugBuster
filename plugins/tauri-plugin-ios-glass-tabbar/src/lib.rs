use tauri::{
    plugin::{Builder, TauriPlugin},
    Manager, Runtime,
};

pub use models::*;

#[cfg(desktop)]
mod desktop;
#[cfg(mobile)]
mod mobile;

mod commands;
mod error;
mod models;

pub use error::{Error, Result};

#[cfg(desktop)]
use desktop::IosGlassTabbar;
#[cfg(mobile)]
use mobile::IosGlassTabbar;

/// Extensions to [`tauri::App`], [`tauri::AppHandle`] and [`tauri::Window`] to access the ios-glass-tabbar APIs.
pub trait IosGlassTabbarExt<R: Runtime> {
    fn ios_glass_tabbar(&self) -> &IosGlassTabbar<R>;
}

impl<R: Runtime, T: Manager<R>> crate::IosGlassTabbarExt<R> for T {
    fn ios_glass_tabbar(&self) -> &IosGlassTabbar<R> {
        self.state::<IosGlassTabbar<R>>().inner()
    }
}

/// Initializes the plugin.
pub fn init<R: Runtime>() -> TauriPlugin<R> {
    Builder::new("ios-glass-tabbar")
        .invoke_handler(tauri::generate_handler![
            commands::set_items,
            commands::set_active_tab,
            commands::set_hidden,
            commands::set_badge,
            commands::register_listener,
            commands::registerListener
        ])
        .setup(|app, api| {
            #[cfg(mobile)]
            let ios_glass_tabbar = mobile::init(app, api)?;
            #[cfg(desktop)]
            let ios_glass_tabbar = desktop::init(app, api)?;
            app.manage(ios_glass_tabbar);
            Ok(())
        })
        .build()
}
