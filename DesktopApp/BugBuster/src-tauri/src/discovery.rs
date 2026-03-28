// =============================================================================
// discovery.rs - Device discovery (USB enumeration + network scanning)
//
// Probes each candidate port with a BBP handshake to identify the CLI port.
// Filters out tty.* duplicates, bridge ports, and non-BugBuster devices.
// =============================================================================

use std::time::Duration;
use crate::bbp;
use crate::state::DiscoveredDevice;

/// Espressif USB VID (default for ESP32-S3 TinyUSB)
const ESPRESSIF_VID: u16 = 0x303A;

/// Try a BBP handshake on a port. Returns firmware info on success.
fn probe_bbp(port_name: &str) -> Option<(u8, u8, u8, u8)> {
    let mut port = serialport::new(port_name, 115200)
        .timeout(Duration::from_millis(200))
        .open()
        .ok()?;

    // Drain any pending data
    let mut drain = [0u8; 512];
    loop {
        match port.read(&mut drain) {
            Ok(n) if n > 0 => continue,
            _ => break,
        }
    }

    // Send handshake magic
    port.write_all(&bbp::MAGIC).ok()?;
    port.flush().ok()?;

    // Read response, scanning for magic pattern
    let mut buf = Vec::with_capacity(64);
    let deadline = std::time::Instant::now() + Duration::from_millis(1000);

    while std::time::Instant::now() < deadline {
        let mut tmp = [0u8; 32];
        match port.read(&mut tmp) {
            Ok(n) if n > 0 => {
                buf.extend_from_slice(&tmp[..n]);
                // Look for magic in response
                if buf.len() >= bbp::HANDSHAKE_RSP_LEN {
                    for i in 0..=(buf.len() - bbp::HANDSHAKE_RSP_LEN) {
                        if buf[i..i + 4] == bbp::MAGIC {
                            let rsp = &buf[i..i + bbp::HANDSHAKE_RSP_LEN];
                            if let Some(info) = bbp::HandshakeInfo::parse(rsp) {
                                // Send DISCONNECT to return to CLI mode
                                let disconnect_frame = bbp::Message::build_frame(1, bbp::CMD_DISCONNECT, &[]);
                                let _ = port.write_all(&disconnect_frame);
                                let _ = port.flush();
                                return Some((info.proto_version, info.fw_major, info.fw_minor, info.fw_patch));
                            }
                        }
                    }
                }
            }
            Ok(_) => {}
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(_) => break,
        }
    }
    None
}

/// Enumerate USB serial ports, probe for BugBuster, return only verified CLI ports.
pub fn discover_usb() -> Vec<DiscoveredDevice> {
    let mut devices = Vec::new();

    let ports = match serialport::available_ports() {
        Ok(ports) => ports,
        Err(e) => {
            log::warn!("Failed to enumerate serial ports: {}", e);
            return devices;
        }
    };

    // Collect candidate ports, filtering platform-specific duplicates
    let candidates: Vec<_> = ports.into_iter().filter(|port| {
        // macOS: skip tty.* duplicates (cu.* and tty.* are the same device)
        #[cfg(target_os = "macos")]
        if port.port_name.contains("/tty.") {
            return false;
        }

        match &port.port_type {
            serialport::SerialPortType::UsbPort(usb) => {
                usb.vid == ESPRESSIF_VID
                    || usb.manufacturer.as_deref().map_or(false, |m| {
                        let ml = m.to_lowercase();
                        ml.contains("espressif") || ml.contains("bugbuster")
                    })
            }
            // macOS: CDC devices may appear as generic types
            #[cfg(target_os = "macos")]
            _ => port.port_name.contains("usbmodem"),
            // Linux: ttyACM devices are CDC/ACM
            #[cfg(target_os = "linux")]
            _ => port.port_name.contains("ttyACM"),
            // Windows: all COM ports with USB info are candidates
            #[cfg(target_os = "windows")]
            _ => false, // Only match on UsbPort type
            #[cfg(not(any(target_os = "macos", target_os = "linux", target_os = "windows")))]
            _ => false,
        }
    }).collect();

    log::info!("Found {} USB candidates, probing...", candidates.len());

    for port in &candidates {
        log::info!("Probing {}...", port.port_name);
        match probe_bbp(&port.port_name) {
            Some((proto, fw_maj, fw_min, fw_pat)) => {
                log::info!("  ✓ BugBuster detected: proto v{}, fw v{}.{}.{}", proto, fw_maj, fw_min, fw_pat);
                devices.push(DiscoveredDevice {
                    id: format!("usb:{}", port.port_name),
                    name: format!("BugBuster (fw {}.{}.{})", fw_maj, fw_min, fw_pat),
                    transport: "usb".to_string(),
                    address: port.port_name.clone(),
                });
            }
            None => {
                log::info!("  ✗ Not a BugBuster CLI port");
            }
        }
    }

    devices
}

/// Scan known network addresses for BugBuster HTTP API.
pub async fn discover_http() -> Vec<DiscoveredDevice> {
    let mut devices = Vec::new();

    let candidates = [
        "http://192.168.4.1",
    ];

    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(2))
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
                            name: format!("BugBuster (WiFi: {})", addr),
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

/// Discover all available devices (USB probe + HTTP scan).
pub async fn discover_all() -> Vec<DiscoveredDevice> {
    // Run USB probe on blocking thread (serial I/O)
    let usb_devices = tokio::task::spawn_blocking(discover_usb)
        .await
        .unwrap_or_default();

    let mut devices = usb_devices;
    let http_devices = discover_http().await;
    devices.extend(http_devices);
    devices
}
