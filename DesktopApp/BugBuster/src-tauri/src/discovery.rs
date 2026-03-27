// =============================================================================
// discovery.rs - Device discovery (USB enumeration + network scanning)
// =============================================================================

use crate::state::DiscoveredDevice;

/// Espressif USB VID (default for ESP32-S3 TinyUSB)
const ESPRESSIF_VID: u16 = 0x303A;

/// Enumerate USB serial ports that could be BugBuster devices.
pub fn discover_usb() -> Vec<DiscoveredDevice> {
    let mut devices = Vec::new();

    let ports = match serialport::available_ports() {
        Ok(ports) => ports,
        Err(e) => {
            log::warn!("Failed to enumerate serial ports: {}", e);
            return devices;
        }
    };

    for port in ports {
        let is_candidate = match &port.port_type {
            serialport::SerialPortType::UsbPort(usb) => {
                // Match Espressif VID, or any CDC/ACM device
                usb.vid == ESPRESSIF_VID
                    || usb.manufacturer.as_deref().map_or(false, |m| {
                        m.to_lowercase().contains("espressif")
                            || m.to_lowercase().contains("bugbuster")
                    })
                    || usb.product.as_deref().map_or(false, |p| {
                        p.to_lowercase().contains("bugbuster")
                    })
            }
            // On macOS, CDC devices may appear under other types
            _ => {
                port.port_name.contains("usbmodem")
            }
        };

        if is_candidate {
            let name = match &port.port_type {
                serialport::SerialPortType::UsbPort(usb) => {
                    format!(
                        "{} ({})",
                        usb.product.as_deref().unwrap_or("ESP32-S3"),
                        port.port_name
                    )
                }
                _ => port.port_name.clone(),
            };

            devices.push(DiscoveredDevice {
                id: format!("usb:{}", port.port_name),
                name,
                transport: "usb".to_string(),
                address: port.port_name.clone(),
            });
        }
    }

    devices
}

/// Scan known network addresses for BugBuster HTTP API.
pub async fn discover_http() -> Vec<DiscoveredDevice> {
    let mut devices = Vec::new();

    // Try the default BugBuster AP address first
    let candidates = [
        "http://192.168.4.1",
    ];

    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(2))
        .build()
        .unwrap();

    for &addr in &candidates {
        let url = format!("{}/api/device/info", addr);
        match client.get(&url).send().await {
            Ok(resp) if resp.status().is_success() => {
                if let Ok(json) = resp.json::<serde_json::Value>().await {
                    if json.get("spiOk").is_some() {
                        devices.push(DiscoveredDevice {
                            id: format!("http:{}", addr),
                            name: format!("BugBuster ({})", addr),
                            transport: "http".to_string(),
                            address: addr.to_string(),
                        });
                    }
                }
            }
            _ => {}
        }
    }

    devices
}

/// Discover all available devices (USB + HTTP).
pub async fn discover_all() -> Vec<DiscoveredDevice> {
    let mut devices = discover_usb();
    let http_devices = discover_http().await;
    devices.extend(http_devices);
    devices
}
