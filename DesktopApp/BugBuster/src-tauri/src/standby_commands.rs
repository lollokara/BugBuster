// =============================================================================
// standby_commands.rs - Tauri commands for system standby.
//
// Wire: BBP `STANDBY` 0x78 over USB, `/api/standby/*` over HTTP (BBP is never
// tunnelled over HTTP, and there is no hidden Wi-Fi fallback for a USB session).
// Every reply is the same 28-byte status record (schema 1); the HTTP JSON form
// carries the same fields with `state` as a string.
//
//   sub-op  len  payload
//   0 status    1  [op]
//   1 presence  6  [op][u32 id LE][u8 present]
//   2 policy    3  [op][u16 seconds LE]   (0 = off, 60, 300, 900; persisted on the S3)
//   3 wake      1  [op]
//
// Logical presence: the app is a control client only while a connection is
// actually established. `standby_presence(true)` registers a random non-zero id
// for the current connection (a new id per connection epoch) and must be called
// again at least every `PRESENCE_REFRESH_SECS` (the device expires an id after
// 45 s and keeps at most 8). `standby_presence(false)` releases it. A refresh is
// not activity. Discovery, an attached cable or an associated Wi-Fi link are not
// presence.
//
// Selected-transport mapping reuses `ConnectionManager` (USB -> send_command,
// HTTP -> its base URL and admin token); nothing here opens a second handle.
// =============================================================================

use crate::connection_manager::ConnectionManager;
use crate::state::ConnectionMode;
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;
use tauri::State;

type CmdResult<T> = Result<T, String>;

/// BBP_CMD_STANDBY (bbp.h). Mirrored here until bbp.rs carries the constant.
pub const CMD_STANDBY: u8 = 0x78;
pub const SUBOP_STATUS: u8 = 0;
pub const SUBOP_PRESENCE: u8 = 1;
pub const SUBOP_POLICY: u8 = 2;
pub const SUBOP_WAKE: u8 = 3;

pub const STATUS_LEN: usize = 28;
pub const STATUS_SCHEMA: u8 = 1;
/// Allowed persisted timeouts in seconds (0 = automatic standby off).
pub const TIMEOUT_CHOICES: [u16; 4] = [0, 60, 300, 900];
/// The caller refreshes presence at this period (device TTL is 45 s).
pub const PRESENCE_REFRESH_SECS: u64 = 10;

const HTTP_TIMEOUT: Duration = Duration::from_secs(4);
const ADMIN_TOKEN_HEADER: &str = "X-BugBuster-Admin-Token";

/// The status record every standby reply carries (JSON field names are the
/// firmware's camelCase; `state` is active|preparing|asleep|waking|fault_safe).
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StandbyStatus {
    pub schema: u8,
    pub state: String,
    pub ready: bool,
    pub stage: u8,
    pub generation: u32,
    pub timeout_seconds: u16,
    pub clients: u8,
    pub failed_stage: u8,
    pub inhibitors: u32,
    pub completed: u16,
    pub failed: u16,
    pub skipped: u16,
    pub idle_remaining_ms: u32,
}

fn state_name(code: u8) -> String {
    match code {
        0 => "active".to_string(),
        1 => "preparing".to_string(),
        2 => "asleep".to_string(),
        3 => "waking".to_string(),
        4 => "fault_safe".to_string(),
        other => format!("unknown({other})"),
    }
}

/// Decode the 28-byte binary status record.
pub fn parse_status(data: &[u8]) -> Result<StandbyStatus, String> {
    if data.len() < STATUS_LEN {
        return Err(format!("STANDBY status is {} bytes, need {STATUS_LEN}", data.len()));
    }
    if data[0] != STATUS_SCHEMA {
        return Err(format!(
            "unsupported STANDBY status schema {} (app knows {STATUS_SCHEMA})",
            data[0]
        ));
    }
    let u16_at = |o: usize| u16::from_le_bytes([data[o], data[o + 1]]);
    let u32_at = |o: usize| u32::from_le_bytes([data[o], data[o + 1], data[o + 2], data[o + 3]]);
    Ok(StandbyStatus {
        schema: data[0],
        state: state_name(data[1]),
        ready: data[2] != 0,
        stage: data[3],
        generation: u32_at(4),
        timeout_seconds: u16_at(8),
        clients: data[10],
        failed_stage: data[11],
        inhibitors: u32_at(12),
        completed: u16_at(16),
        failed: u16_at(18),
        skipped: u16_at(20),
        idle_remaining_ms: u32_at(24),
    })
}

/// Decode the HTTP JSON form of the status record.
pub fn parse_status_json(v: &Value) -> Result<StandbyStatus, String> {
    let state = v
        .get("state")
        .ok_or_else(|| "standby JSON status needs 'state'".to_string())?;
    let state = match state {
        Value::String(s) => s.trim().to_ascii_lowercase().replace('-', "_"),
        Value::Number(n) => state_name(n.as_u64().unwrap_or(255) as u8),
        _ => return Err("standby JSON 'state' must be a string".to_string()),
    };
    let ready = v
        .get("ready")
        .and_then(Value::as_bool)
        .ok_or_else(|| "standby JSON status needs boolean 'ready'".to_string())?;
    let num = |keys: &[&str]| -> u64 {
        keys.iter()
            .find_map(|k| v.get(*k).and_then(Value::as_u64))
            .unwrap_or(0)
    };
    Ok(StandbyStatus {
        schema: num(&["schema"]).max(1) as u8,
        state,
        ready,
        stage: num(&["stage"]) as u8,
        generation: num(&["generation", "gen"]) as u32,
        timeout_seconds: num(&["timeoutSeconds"]) as u16,
        clients: num(&["clients"]) as u8,
        failed_stage: num(&["failedStage"]) as u8,
        inhibitors: num(&["inhibitors"]) as u32,
        completed: num(&["completed"]) as u16,
        failed: num(&["failed"]) as u16,
        skipped: num(&["skipped"]) as u16,
        idle_remaining_ms: num(&["idleRemainingMs"]) as u32,
    })
}

pub fn presence_payload(client_id: u32, present: bool) -> [u8; 6] {
    let id = client_id.to_le_bytes();
    [SUBOP_PRESENCE, id[0], id[1], id[2], id[3], present as u8]
}

pub fn policy_payload(seconds: u16) -> Result<[u8; 3], String> {
    if !TIMEOUT_CHOICES.contains(&seconds) {
        return Err(format!(
            "standby timeout must be one of {TIMEOUT_CHOICES:?} seconds (0 = off), got {seconds}"
        ));
    }
    let s = seconds.to_le_bytes();
    Ok([SUBOP_POLICY, s[0], s[1]])
}

/// A random non-zero u32 (std's OS-seeded hasher; no extra dependency).
pub fn new_client_id() -> u32 {
    let mut h = RandomState::new().build_hasher();
    h.write_u64(
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0),
    );
    let x = h.finish();
    let id = (x as u32) ^ ((x >> 32) as u32);
    if id == 0 {
        1
    } else {
        id
    }
}

// -----------------------------------------------------------------------------
// Presence epoch (one per connection; a new random id for every epoch)
// -----------------------------------------------------------------------------

#[derive(Default)]
struct PresenceEpoch {
    identity: String,
    id: u32,
    unsupported: bool,
}

impl PresenceEpoch {
    /// The id to register for `identity`, or None when this connection's
    /// firmware is already known to have no standby.
    fn acquire(&mut self, identity: &str, mut next_id: impl FnMut() -> u32) -> Option<u32> {
        if self.identity != identity {
            *self = PresenceEpoch {
                identity: identity.to_string(),
                id: 0,
                unsupported: false,
            };
        }
        if self.unsupported {
            return None;
        }
        if self.id == 0 {
            self.id = next_id();
        }
        Some(self.id)
    }

    /// The id to release for `identity` (clears it so the next acquire is a new epoch).
    fn release(&mut self, identity: &str) -> Option<u32> {
        if self.identity != identity || self.id == 0 || self.unsupported {
            return None;
        }
        Some(std::mem::take(&mut self.id))
    }

    fn mark_unsupported(&mut self, identity: &str) {
        if self.identity == identity {
            self.unsupported = true;
            self.id = 0;
        }
    }

    fn forget(&mut self) {
        *self = PresenceEpoch::default();
    }
}

fn epoch() -> &'static Mutex<PresenceEpoch> {
    static EPOCH: OnceLock<Mutex<PresenceEpoch>> = OnceLock::new();
    EPOCH.get_or_init(|| Mutex::new(PresenceEpoch::default()))
}

fn with_epoch<T>(f: impl FnOnce(&mut PresenceEpoch) -> T) -> T {
    let mut guard = epoch().lock().unwrap_or_else(|e| e.into_inner());
    f(&mut guard)
}

fn connection_identity(mode: &ConnectionMode, port_or_url: &str) -> String {
    format!("{mode:?}:{port_or_url}")
}

// -----------------------------------------------------------------------------
// Request execution over the selected transport
// -----------------------------------------------------------------------------

enum Op {
    Status,
    Presence { id: u32, present: bool },
    Policy(u16),
    Wake,
}

impl Op {
    fn payload(&self) -> Vec<u8> {
        match self {
            Op::Status => vec![SUBOP_STATUS],
            Op::Presence { id, present } => presence_payload(*id, *present).to_vec(),
            Op::Policy(s) => policy_payload(*s).map(|p| p.to_vec()).unwrap_or_default(),
            Op::Wake => vec![SUBOP_WAKE],
        }
    }

    /// (is_get, path, body)
    fn http(&self) -> (bool, &'static str, Value) {
        match self {
            Op::Status => (true, "/api/standby/status", Value::Null),
            Op::Presence { id, present } => (
                false,
                "/api/standby/presence",
                json!({ "clientId": id, "present": present }),
            ),
            Op::Policy(s) => (false, "/api/standby/policy", json!({ "timeoutSeconds": s })),
            Op::Wake => (false, "/api/standby/wake", json!({})),
        }
    }

    /// Policy changes need the admin token; presence, status and wake are anonymous.
    fn needs_admin(&self) -> bool {
        matches!(self, Op::Policy(_))
    }
}

#[derive(Debug, PartialEq)]
enum CallError {
    /// Firmware without system standby.
    Unsupported,
    Failed(String),
}

/// A BBP ERR for 0x78 reads "... (error 0x01, cmd 0x78)": INVALID_CMD on an
/// opcode this firmware does not know.
fn classify_usb_error(msg: &str) -> CallError {
    if msg.contains("error 0x01, cmd 0x78") {
        CallError::Unsupported
    } else {
        CallError::Failed(msg.to_string())
    }
}

fn classify_http_status(code: u16) -> Option<CallError> {
    matches!(code, 404 | 405).then_some(CallError::Unsupported)
}

async fn http_call(base: &str, token: Option<&str>, op: &Op) -> Result<StandbyStatus, CallError> {
    static CLIENT: OnceLock<reqwest::Client> = OnceLock::new();
    let client = CLIENT.get_or_init(reqwest::Client::new);
    let (is_get, path, body) = op.http();
    let url = format!("{}{}", base.trim_end_matches('/'), path);
    let mut req = if is_get {
        client.get(&url)
    } else {
        client.post(&url).json(&body)
    };
    if op.needs_admin() {
        if let Some(t) = token {
            req = req.header(ADMIN_TOKEN_HEADER, t);
        }
    }
    let resp = req
        .timeout(HTTP_TIMEOUT)
        .send()
        .await
        .map_err(|e| CallError::Failed(e.to_string()))?;
    let code = resp.status();
    if let Some(e) = classify_http_status(code.as_u16()) {
        return Err(e);
    }
    let text = resp
        .text()
        .await
        .map_err(|e| CallError::Failed(e.to_string()))?;
    let parsed: Result<Value, _> = serde_json::from_str(&text);
    if !code.is_success() {
        let detail = parsed
            .ok()
            .and_then(|v| v.get("error").and_then(Value::as_str).map(str::to_owned))
            .unwrap_or_else(|| format!("HTTP {code}"));
        return Err(CallError::Failed(detail));
    }
    let v = parsed.map_err(|e| CallError::Failed(format!("bad standby reply: {e}")))?;
    parse_status_json(&v).map_err(CallError::Failed)
}

async fn execute(mgr: &ConnectionManager, op: &Op) -> Result<StandbyStatus, CallError> {
    let cs = mgr.get_connection_status();
    match cs.mode {
        ConnectionMode::Disconnected => Err(CallError::Failed("Not connected".to_string())),
        ConnectionMode::Usb => match mgr.send_command(CMD_STANDBY, &op.payload()).await {
            Ok(rsp) => parse_status(&rsp).map_err(CallError::Failed),
            Err(e) => Err(classify_usb_error(&e.to_string())),
        },
        ConnectionMode::Http => {
            let base = mgr
                .get_base_url()
                .await
                .ok_or_else(|| CallError::Failed("HTTP base URL unavailable".to_string()))?;
            http_call(&base, cs.admin_token.as_deref(), op).await
        }
    }
}

fn into_cmd_error(e: CallError) -> String {
    match e {
        CallError::Unsupported => {
            "standby unsupported: this firmware has no system standby (BBP 0x78 / /api/standby)"
                .to_string()
        }
        CallError::Failed(m) => m,
    }
}

// -----------------------------------------------------------------------------
// Tauri commands
// -----------------------------------------------------------------------------

/// Current standby state, readiness, timeout, client and inhibitor counts.
/// Errors with a "standby unsupported" message on older firmware.
#[tauri::command]
pub async fn standby_status(mgr: State<'_, ConnectionManager>) -> CmdResult<StandbyStatus> {
    execute(&mgr, &Op::Status).await.map_err(into_cmd_error)
}

/// Persist the automatic standby timeout on the mainboard: 0 (off), 60, 300 or 900.
#[tauri::command]
pub async fn standby_set_timeout(
    seconds: u16,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<StandbyStatus> {
    policy_payload(seconds)?;
    execute(&mgr, &Op::Policy(seconds))
        .await
        .map_err(into_cmd_error)
}

/// Wake the device. Hardware commands answer BUSY until `ready`; nothing is
/// replayed and outputs stay off.
#[tauri::command]
pub async fn standby_wake(mgr: State<'_, ConnectionManager>) -> CmdResult<StandbyStatus> {
    execute(&mgr, &Op::Wake).await.map_err(into_cmd_error)
}

/// Register (`present = true`, call at least every `PRESENCE_REFRESH_SECS`) or
/// release (`false`) this app as a control client of the current connection.
///
/// Returns `Ok(Some(status))` when registered, `Ok(None)` when the firmware has
/// no standby (remembered per connection, so a heartbeat is a no-op afterwards)
/// and for every release. A release never errors: an expired id is answered
/// NOT_FOUND by the device and the TTL covers a lost link.
#[tauri::command]
pub async fn standby_presence(
    present: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<Option<StandbyStatus>> {
    let cs = mgr.get_connection_status();
    if cs.mode == ConnectionMode::Disconnected {
        with_epoch(PresenceEpoch::forget);
        return if present {
            Err("Not connected".to_string())
        } else {
            Ok(None)
        };
    }
    let identity = connection_identity(&cs.mode, &cs.port_or_url);

    if !present {
        if let Some(id) = with_epoch(|e| e.release(&identity)) {
            let _ = execute(&mgr, &Op::Presence { id, present: false }).await;
        }
        return Ok(None);
    }

    let Some(id) = with_epoch(|e| e.acquire(&identity, new_client_id)) else {
        return Ok(None);
    };
    match execute(&mgr, &Op::Presence { id, present: true }).await {
        Ok(status) => Ok(Some(status)),
        Err(CallError::Unsupported) => {
            with_epoch(|e| e.mark_unsupported(&identity));
            Ok(None)
        }
        Err(CallError::Failed(m)) => Err(m),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record(state: u8, ready: u8, stage: u8, next_id: u32, timeout: u16, clients: u8) -> Vec<u8> {
        let mut v = vec![1, state, ready, stage];
        v.extend_from_slice(&next_id.to_le_bytes());
        v.extend_from_slice(&timeout.to_le_bytes());
        v.push(clients);
        v.push(6); // failedStage
        v.extend_from_slice(&0x1122_3344u32.to_le_bytes()); // inhibitors
        v.extend_from_slice(&1u16.to_le_bytes());
        v.extend_from_slice(&2u16.to_le_bytes());
        v.extend_from_slice(&3u16.to_le_bytes());
        v.extend_from_slice(&0u16.to_le_bytes()); // reserved
        v.extend_from_slice(&272_000u32.to_le_bytes());
        v
    }

    #[test]
    fn status_record_decodes_every_field_at_the_published_offsets() {
        let raw = record(2, 0, 5, 0xAABB_CCDD, 900, 3);
        assert_eq!(raw.len(), STATUS_LEN);
        let s = parse_status(&raw).unwrap();
        assert_eq!(s.state, "asleep");
        assert!(!s.ready);
        assert_eq!((s.stage, s.generation, s.timeout_seconds, s.clients), (5, 0xAABB_CCDD, 900, 3));
        assert_eq!((s.failed_stage, s.inhibitors), (6, 0x1122_3344));
        assert_eq!((s.completed, s.failed, s.skipped, s.idle_remaining_ms), (1, 2, 3, 272_000));
    }

    #[test]
    fn short_or_foreign_schema_records_are_rejected() {
        assert!(parse_status(&[0u8; 27]).is_err());
        let mut raw = record(0, 1, 0, 1, 300, 0);
        raw[0] = 2;
        assert!(parse_status(&raw).unwrap_err().contains("schema"));
    }

    #[test]
    fn unknown_state_code_is_named_not_dropped() {
        assert_eq!(parse_status(&record(9, 0, 0, 0, 300, 0)).unwrap().state, "unknown(9)");
    }

    #[test]
    fn json_status_matches_the_binary_decode() {
        let v = json!({
            "schema": 1, "state": "fault_safe", "ready": false, "stage": 3, "generation": 11,
            "timeoutSeconds": 60, "clients": 2, "failedStage": 4, "inhibitors": 5,
            "completed": 6, "failed": 7, "skipped": 8, "idleRemainingMs": 9
        });
        let s = parse_status_json(&v).unwrap();
        assert_eq!(s.state, "fault_safe");
        assert_eq!((s.generation, s.timeout_seconds, s.clients, s.failed_stage), (11, 60, 2, 4));
        assert_eq!((s.completed, s.failed, s.skipped, s.idle_remaining_ms), (6, 7, 8, 9));
        assert!(parse_status_json(&json!({ "ok": true })).is_err());
    }

    #[test]
    fn status_serialises_camel_case_for_the_frontend() {
        let s = parse_status(&record(0, 1, 0, 1, 300, 1)).unwrap();
        let v = serde_json::to_value(&s).unwrap();
        for key in ["timeoutSeconds", "failedStage", "idleRemainingMs", "generation"] {
            assert!(v.get(key).is_some(), "missing {key}");
        }
    }

    #[test]
    fn presence_request_is_six_bytes() {
        assert_eq!(presence_payload(0x0102_0304, true), [1, 4, 3, 2, 1, 1]);
        assert_eq!(presence_payload(7, false)[5], 0);
    }

    #[test]
    fn policy_request_validates_the_persisted_choices() {
        assert_eq!(policy_payload(900).unwrap(), [2, 0x84, 0x03]);
        for ok in TIMEOUT_CHOICES {
            assert!(policy_payload(ok).is_ok());
        }
        for bad in [1u16, 30, 120, 600, 3600] {
            assert!(policy_payload(bad).is_err());
        }
    }

    #[test]
    fn client_ids_are_never_zero_and_vary() {
        let ids: std::collections::HashSet<u32> = (0..200).map(|_| new_client_id()).collect();
        assert!(!ids.contains(&0));
        assert!(ids.len() > 150);
    }

    #[test]
    fn epoch_keeps_one_id_until_released_then_issues_a_new_one() {
        let mut e = PresenceEpoch::default();
        let mut next = 0u32;
        let mut next_id = || {
            next += 1;
            next
        };
        assert_eq!(e.acquire("Usb:COM6", &mut next_id), Some(1));
        assert_eq!(e.acquire("Usb:COM6", &mut next_id), Some(1));
        assert_eq!(e.release("Usb:COM6"), Some(1));
        assert_eq!(e.release("Usb:COM6"), None);
        assert_eq!(e.acquire("Usb:COM6", &mut next_id), Some(2));
    }

    #[test]
    fn a_different_connection_starts_a_new_epoch() {
        let mut e = PresenceEpoch::default();
        let mut next = 10u32;
        let mut next_id = || {
            next += 1;
            next
        };
        assert_eq!(e.acquire("Usb:COM6", &mut next_id), Some(11));
        assert_eq!(e.acquire("Http:http://10.0.0.5", &mut next_id), Some(12));
        assert_eq!(e.release("Usb:COM6"), None, "old connection id is not releasable");
    }

    #[test]
    fn unsupported_firmware_is_remembered_for_the_connection_only() {
        let mut e = PresenceEpoch::default();
        let mut next_id = || 5u32;
        assert_eq!(e.acquire("Usb:COM6", &mut next_id), Some(5));
        e.mark_unsupported("Usb:COM6");
        assert_eq!(e.acquire("Usb:COM6", &mut next_id), None);
        assert_eq!(e.release("Usb:COM6"), None);
        assert_eq!(e.acquire("Usb:COM7", &mut next_id), Some(5));
    }

    #[test]
    fn forget_clears_the_epoch() {
        let mut e = PresenceEpoch::default();
        let mut next_id = || 5u32;
        e.acquire("Usb:COM6", &mut next_id);
        e.forget();
        assert_eq!(e.release("Usb:COM6"), None);
    }

    #[test]
    fn usb_invalid_cmd_for_0x78_means_unsupported() {
        let msg = "The command is not recognized by the device (error 0x01, cmd 0x78)";
        assert_eq!(classify_usb_error(msg), CallError::Unsupported);
        let busy = "Device is busy processing another operation (error 0x06, cmd 0x78)";
        assert_eq!(classify_usb_error(busy), CallError::Failed(busy.to_string()));
        let other = "The command is not recognized by the device (error 0x01, cmd 0x10)";
        assert!(matches!(classify_usb_error(other), CallError::Failed(_)));
    }

    #[test]
    fn http_404_and_405_mean_unsupported_503_does_not() {
        assert_eq!(classify_http_status(404), Some(CallError::Unsupported));
        assert_eq!(classify_http_status(405), Some(CallError::Unsupported));
        assert_eq!(classify_http_status(503), None);
        assert_eq!(classify_http_status(409), None);
    }

    #[test]
    fn http_routes_and_auth_requirements() {
        let (get, path, _) = Op::Status.http();
        assert!(get && path == "/api/standby/status");
        let (get, path, body) = Op::Presence { id: 9, present: true }.http();
        assert!(!get && path == "/api/standby/presence");
        assert_eq!(body, json!({ "clientId": 9, "present": true }));
        let (_, path, body) = Op::Policy(60).http();
        assert_eq!(path, "/api/standby/policy");
        assert_eq!(body, json!({ "timeoutSeconds": 60 }));
        assert_eq!(Op::Wake.http().1, "/api/standby/wake");
        assert!(Op::Policy(60).needs_admin());
        assert!(!Op::Presence { id: 1, present: true }.needs_admin());
        assert!(!Op::Wake.needs_admin());
        assert!(!Op::Status.needs_admin());
    }

    #[test]
    fn usb_payloads_match_the_wire_layout() {
        assert_eq!(Op::Status.payload(), vec![0]);
        assert_eq!(Op::Wake.payload(), vec![3]);
        assert_eq!(Op::Policy(300).payload(), vec![2, 0x2C, 0x01]);
        assert_eq!(
            Op::Presence { id: 0xDEAD_BEEF, present: true }.payload(),
            vec![1, 0xEF, 0xBE, 0xAD, 0xDE, 1]
        );
    }

    #[test]
    fn unsupported_command_error_is_explicit() {
        let m = into_cmd_error(CallError::Unsupported);
        assert!(m.starts_with("standby unsupported"));
    }
}
