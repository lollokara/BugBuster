// =============================================================================
// discovery.rs - Device discovery (WiFi scanning)
//
// Probes network addresses for BugBuster HTTP API.
// USB discovery is disabled for iOS port.
// =============================================================================

use crate::state::DiscoveredDevice;
use std::time::Duration;

/// Get local IP addresses to derive subnet scan ranges.
/// Hardened for iOS sandbox constraints.
fn get_local_subnets() -> Vec<String> {
    let mut subnets = Vec::new();
    
    // Attempt to list interfaces, but catch potential panics/errors from crate
    let interfaces = match std::panic::catch_unwind(|| {
        local_ip_address::list_afinet_netifas()
    }) {
        Ok(Ok(ifaces)) => ifaces,
        _ => {
            log::warn!("discovery: could not list network interfaces (iOS sandbox?)");
            return vec!["192.168.4".to_string()]; // Fallback to BugBuster default AP subnet
        }
    };

    for (_name, ip) in interfaces {
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

    // Always ensure the default BugBuster AP subnet is checked
    if !subnets.contains(&"192.168.4".to_string()) {
        subnets.push("192.168.4".to_string());
    }

    subnets
}

/// Probe a single HTTP address for BugBuster device.
async fn probe_http(client: &reqwest::Client, addr: &str) -> Option<DiscoveredDevice> {
    let url = format!("{}/api/device/info", addr);
    let resp = client.get(&url).send().await.ok()?;

    if resp.status() == reqwest::StatusCode::FORBIDDEN {
        // addr is the full URL (http://x.x.x.x); store just the IP for address
        let ip = addr
            .trim_start_matches("http://")
            .trim_start_matches("https://")
            .to_string();
        return Some(DiscoveredDevice {
            id: addr.to_string(),
            name: "BugBuster (WiFi)".to_string(),
            transport: "http".to_string(),
            address: ip,
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
    let ip = addr
        .trim_start_matches("http://")
        .trim_start_matches("https://")
        .to_string();
    Some(DiscoveredDevice {
        id: addr.to_string(),
        name: "BugBuster (WiFi)".to_string(),
        transport: "http".to_string(),
        address: ip,
        serial_number: None,
    })
}

/// Scan known network addresses for BugBuster HTTP API.
pub async fn discover_http() -> Vec<DiscoveredDevice> {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_millis(300))
        .build()
        .unwrap();

    let subnets = get_local_subnets();
    let mut candidates = Vec::new();

    for subnet in subnets {
        for host in 1..=254u8 {
            candidates.push(format!("http://{}.{}", subnet, host));
        }
    }

    let mut devices = Vec::new();
    // Scan in chunks of 64; BugBuster responds in <50 ms so 300 ms timeout is ample
    for chunk in candidates.chunks(64) {
        let futs: Vec<_> = chunk.iter().map(|addr| probe_http(&client, addr)).collect();
        let results = futures::future::join_all(futs).await;
        for dev in results.into_iter().flatten() {
            devices.push(dev);
        }
        if !devices.is_empty() {
            break;
        }
    }

    devices
}

pub async fn discover_all() -> Vec<DiscoveredDevice> {
    discover_http().await
}
