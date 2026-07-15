// =============================================================================
// discovery.rs - Device discovery (USB enumeration + network scanning)
//
// Probes each candidate port with a BBP handshake to identify the CLI port.
// Filters out tty.* duplicates, bridge ports, and non-BugBuster devices.
// =============================================================================

use crate::bbp;
use crate::state::DiscoveredDevice;
use std::time::Duration;

/// Espressif USB VID (default for ESP32-S3 TinyUSB)
const ESPRESSIF_VID: u16 = 0x303A;

/// Try a BBP handshake on a port. Returns firmware info on success.
fn probe_bbp(port_name: &str) -> Option<(u8, u8, u8, u8)> {
    let mut port = serialport::new(port_name, 115200)
        .timeout(Duration::from_millis(200))
        .open()
        .ok()?;

    // If the device was left in BBP mode from a previous session (app killed
    // before CMD_DISCONNECT was sent), the firmware's COBS framer may be mid-
    // frame (s_rxLen > 0). In that state the firmware's re-handshake guard
    // (s_rxLen == 0 check) prevents the magic bytes from being recognised.
    // Sending a proper COBS-encoded CMD_DISCONNECT frame fixes this: the
    // firmware dispatches it via dispatchMessage → BBP_CMD_DISCONNECT, exits
    // binary mode cleanly, and restores the CLI regardless of framer state.
    // In normal CLI mode the frame bytes are just garbage that gets echoed /
    // discarded; the subsequent handshake magic is then handled by the CLI.
    let disconnect_frame = bbp::Message::build_frame(1, bbp::CMD_DISCONNECT, &[]);
    let _ = port.write_all(&disconnect_frame);
    let _ = port.flush();
    std::thread::sleep(Duration::from_millis(150));

    // Drain any pending data (CLI restoration message, prompt, etc.)
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
                                let disconnect_frame =
                                    bbp::Message::build_frame(1, bbp::CMD_DISCONNECT, &[]);
                                let _ = port.write_all(&disconnect_frame);
                                let _ = port.flush();
                                return Some((
                                    info.proto_version,
                                    info.fw_major,
                                    info.fw_minor,
                                    info.fw_patch,
                                ));
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

    // Failed probes can still leave the device in BBP mode if the magic
    // was accepted but the response was not fully parsed. Explicitly
    // disconnect so the CLI resumes accepting plain text input.
    let disconnect_frame = bbp::Message::build_frame(1, bbp::CMD_DISCONNECT, &[]);
    let _ = port.write_all(&disconnect_frame);
    let _ = port.flush();
    None
}

/// Return (port_name, serial_number) for all Espressif USB candidates without probing.
/// Used by both the active scan and the background USB watcher.
pub fn espressif_port_candidates() -> Vec<(String, Option<String>)> {
    let ports = match serialport::available_ports() {
        Ok(p) => p,
        Err(e) => {
            log::warn!("Failed to enumerate serial ports: {}", e);
            return Vec::new();
        }
    };
    ports
        .into_iter()
        .filter(|port| {
            #[cfg(target_os = "macos")]
            if port.port_name.contains("/tty.") {
                return false;
            }
            match &port.port_type {
                serialport::SerialPortType::UsbPort(usb) => {
                    if usb.vid == 0x2E8A && usb.pid == 0x000C {
                        return false;
                    }
                    usb.vid == ESPRESSIF_VID
                        || usb.manufacturer.as_deref().map_or(false, |m| {
                            let ml = m.to_lowercase();
                            ml.contains("espressif") || ml.contains("bugbuster")
                        })
                }
                #[cfg(target_os = "macos")]
                _ => port.port_name.contains("usbmodem"),
                #[cfg(target_os = "linux")]
                _ => port.port_name.contains("ttyACM"),
                #[cfg(target_os = "windows")]
                _ => false,
                #[cfg(not(any(target_os = "macos", target_os = "linux", target_os = "windows")))]
                _ => false,
            }
        })
        .map(|p| {
            let sn = match &p.port_type {
                serialport::SerialPortType::UsbPort(usb) => usb.serial_number.clone(),
                _ => None,
            };
            (p.port_name, sn)
        })
        .collect()
}

/// Probe a single port by name. Returns a DiscoveredDevice if it is a BugBuster CLI port.
/// Used by the background USB watcher to confirm newly-appeared ports.
pub fn probe_usb_port(port_name: &str, serial_number: Option<String>) -> Option<DiscoveredDevice> {
    probe_bbp(port_name).map(|(_, fw_maj, fw_min, fw_pat)| DiscoveredDevice {
        id: format!("usb:{}", port_name),
        name: format!("BugBuster (fw {}.{}.{})", fw_maj, fw_min, fw_pat),
        transport: "usb".to_string(),
        address: port_name.to_string(),
        serial_number,
    })
}

/// Like discover_usb but calls emit_fn immediately for each confirmed device
/// so results can be streamed to the UI as they arrive.
pub fn discover_usb_streaming(emit_fn: impl Fn(DiscoveredDevice)) -> Vec<DiscoveredDevice> {
    let candidates = espressif_port_candidates();
    log::info!("Found {} USB candidates, probing...", candidates.len());
    let mut devices = Vec::new();
    for (port_name, serial_number) in candidates {
        log::info!("Probing {}...", port_name);
        match probe_usb_port(&port_name, serial_number) {
            Some(dev) => {
                log::info!("  ✓ BugBuster detected: {}", dev.name);
                emit_fn(dev.clone());
                devices.push(dev);
            }
            None => {
                log::info!("  ✗ Not a BugBuster CLI port");
            }
        }
    }
    devices
}

/// Enumerate USB serial ports, probe for BugBuster, return only verified CLI ports.
pub fn discover_usb() -> Vec<DiscoveredDevice> {
    discover_usb_streaming(|_| {})
}

/// Get local IP addresses to derive subnet scan ranges.
fn get_local_subnets() -> Vec<String> {
    let mut subnets = Vec::new();
    if let Ok(ifaces) = local_ip_address::list_afinet_netifas() {
        for (_name, ip) in ifaces {
            if let std::net::IpAddr::V4(v4) = ip {
                if v4.is_loopback() {
                    continue;
                }
                let octets = v4.octets();
                let subnet = format!("{}.{}.{}", octets[0], octets[1], octets[2]);
                if !subnets.contains(&subnet) {
                    subnets.push(subnet);
                }
            }
        }
    }
    subnets
}

/// Probe a single HTTP address for BugBuster device.
async fn probe_http(client: &reqwest::Client, addr: &str) -> Option<DiscoveredDevice> {
    let url = format!("{}/api/device/info", addr);
    let resp = client.get(&url).send().await.ok()?;

    // 403 means the device exists but requires an admin token — it's still a
    // BugBuster; surface it so the connection path can retry with stored tokens.
    if resp.status() == reqwest::StatusCode::FORBIDDEN {
        return Some(DiscoveredDevice {
            id: format!("http:{}", addr),
            name: format!("BugBuster (WiFi: {})", addr),
            transport: "http".to_string(),
            address: addr.to_string(),
            serial_number: None,
        });
    }

    if !resp.status().is_success() {
        return None;
    }
    let json: serde_json::Value = resp.json().await.ok()?;
    if json.get("spiOk").is_none() {
        return None;
    }
    Some(DiscoveredDevice {
        id: format!("http:{}", addr),
        name: format!("BugBuster (WiFi: {})", addr),
        transport: "http".to_string(),
        address: addr.to_string(),
        serial_number: None,
    })
}

/// Like discover_http but calls emit_fn immediately for each found device.
pub async fn discover_http_streaming(
    emit_fn: impl Fn(DiscoveredDevice) + Send,
) -> Vec<DiscoveredDevice> {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_millis(800))
        .build()
        .unwrap();

    let mut candidates = vec!["http://192.168.4.1".to_string()];
    for subnet in get_local_subnets() {
        for host in 1..=254u8 {
            let addr = format!("http://{}.{}", subnet, host);
            if !candidates.contains(&addr) {
                candidates.push(addr);
            }
        }
    }

    log::info!("HTTP discovery: scanning {} addresses...", candidates.len());

    let mut devices = Vec::new();
    for chunk in candidates.chunks(50) {
        let futs: Vec<_> = chunk.iter().map(|addr| probe_http(&client, addr)).collect();
        let results = futures::future::join_all(futs).await;
        for dev in results.into_iter().flatten() {
            log::info!("  ✓ Found BugBuster at {}", dev.address);
            emit_fn(dev.clone());
            devices.push(dev);
        }
        if !devices.is_empty() {
            break;
        }
    }

    devices
}

/// Scan known network addresses for BugBuster HTTP API.
/// Checks the AP address (192.168.4.1) plus scans local subnets.
pub async fn discover_http() -> Vec<DiscoveredDevice> {
    discover_http_streaming(|_| {}).await
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
