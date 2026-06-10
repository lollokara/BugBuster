const COMMANDS: &[&str] = &["set_items", "set_active_tab", "set_hidden", "set_badge"];

fn main() {
    tauri_plugin::Builder::new(COMMANDS)
        .android_path("android")
        .ios_path("ios")
        .build();
}
