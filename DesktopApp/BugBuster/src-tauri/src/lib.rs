// =============================================================================
// lib.rs - Tauri application entry point
// =============================================================================

mod bbp;
mod commands;
mod connection_manager;
mod discovery;
mod http_transport;
mod state;
mod transport;
mod usb_transport;
mod wavegen;

use connection_manager::ConnectionManager;
use wavegen::WavegenState;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .manage(ConnectionManager::new())
        .manage(WavegenState::new())
        .invoke_handler(tauri::generate_handler![
            // Discovery & Connection
            commands::discover_devices,
            commands::connect_device,
            commands::disconnect_device,
            commands::get_connection_status,
            // Device State
            commands::get_device_state,
            // Channel Configuration
            commands::set_channel_function,
            commands::set_dac_code,
            commands::set_dac_voltage,
            commands::set_dac_current,
            commands::set_adc_config,
            commands::set_do_state,
            commands::set_vout_range,
            commands::set_current_limit,
            commands::set_gpio_config,
            commands::set_gpio_value,
            // Faults
            commands::clear_all_alerts,
            commands::clear_channel_alert,
            // System
            commands::device_reset,
            // Streaming
            commands::start_adc_stream,
            commands::stop_adc_stream,
            // Waveform Generator
            commands::start_wavegen,
            commands::stop_wavegen,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
