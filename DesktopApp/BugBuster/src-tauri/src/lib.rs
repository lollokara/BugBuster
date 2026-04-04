// =============================================================================
// lib.rs - Tauri application entry point
// =============================================================================

mod bbp;
mod commands;
mod connection_manager;
mod discovery;
mod http_transport;
mod la_usb;
mod la_store;
mod la_decoders;
mod la_commands;
mod state;
mod transport;
mod usb_transport;
mod wavegen;

use connection_manager::ConnectionManager;
use la_commands::LaState;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .manage(ConnectionManager::new())
        .manage(LaState::new())
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
            commands::set_rtd_config,
            commands::set_din_config,
            commands::set_do_config,
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
            commands::start_scope_stream,
            commands::stop_scope_stream,
            // Waveform Generator
            commands::start_wavegen,
            commands::stop_wavegen,
            // Level Shifter
            commands::set_lshift_oe,
            // WiFi Management
            commands::wifi_get_status,
            commands::wifi_connect,
            commands::wifi_scan,
            // Firmware / OTA
            commands::get_firmware_info,
            commands::ota_upload_firmware,
            // DS4424 IDAC
            commands::idac_get_status,
            commands::idac_set_code,
            commands::idac_set_voltage,
            commands::idac_cal_add_point,
            commands::idac_cal_clear,
            commands::idac_cal_save,
            // Self-Test / Auto-Calibration
            commands::selftest_status,
            commands::selftest_measure_supply,
            commands::selftest_efuse_currents,
            commands::selftest_auto_calibrate,
            // PCA9535 GPIO Expander
            commands::pca_get_status,
            commands::pca_set_control,
            // HAT Expansion Board
            commands::hat_get_status,
            commands::hat_get_power,
            commands::hat_set_pin,
            commands::hat_set_all_pins,
            commands::hat_reset,
            commands::hat_detect,
            commands::hat_set_power,
            commands::hat_set_io_voltage,
            commands::hat_setup_swd,
            // HUSB238 USB PD
            commands::usbpd_get_status,
            commands::usbpd_select_pdo,
            // File Dialog
            commands::pick_save_file,
            // Recording (BBSC binary + CSV legacy)
            commands::start_recording,
            commands::stop_recording,
            commands::append_recording_data,
            commands::export_bbsc_to_csv,
            commands::start_csv_recording,
            commands::stop_csv_recording,
            commands::append_csv_data,
            // Config Export/Import
            commands::export_config,
            commands::import_config,
            commands::pick_config_save_file,
            commands::pick_config_open_file,
            // Logic Analyzer
            la_commands::la_check_usb,
            la_commands::la_connect_usb,
            la_commands::la_configure,
            la_commands::la_set_trigger,
            la_commands::la_arm,
            la_commands::la_force,
            la_commands::la_stop,
            la_commands::la_get_status,
            la_commands::la_read_capture,
            la_commands::la_get_cached_capture,
            la_commands::la_get_view,
            la_commands::la_load_raw,
            la_commands::la_export_vcd,
            la_commands::la_get_capture_info,
            la_commands::la_decode,
            la_commands::la_delete_range,
            la_commands::la_read_append,
            la_commands::la_read_append_usb,
            la_commands::la_read_append_fast,
            la_commands::la_stream_cycle,
            la_commands::la_stream_usb,
            la_commands::la_stream_usb_stop,
            la_commands::la_stream_usb_active,
            la_commands::la_read_uart_chunks,
            la_commands::la_export_vcd_file,
            la_commands::la_export_json,
            la_commands::la_import_json,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
