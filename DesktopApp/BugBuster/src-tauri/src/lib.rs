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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .manage(ConnectionManager::new())
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
            // UART
            commands::set_uart_config,
            // Diagnostics
            commands::set_diag_config,
            // Faults
            commands::clear_all_alerts,
            commands::clear_channel_alert,
            // System
            commands::device_reset,
            // MUX Switch Matrix
            commands::mux_set_all,
            commands::mux_get_all,
            commands::mux_set_switch,
            // Streaming
            commands::start_adc_stream,
            commands::stop_adc_stream,
            // Waveform Generator
            commands::start_wavegen,
            commands::stop_wavegen,
            // File Dialog
            commands::pick_save_file,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
