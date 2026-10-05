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
// actually established. `ConnectionManager` starts one presence SESSION per
// connect (a new random non-zero id, bound to the connection epoch even when the
// port or URL is the same as before). The session refreshes every
// `PRESENCE_REFRESH_SECS` (the device expires an id after 45 s and keeps at most
// 8) and is released best-effort on disconnect. A refresh is not activity.
// Discovery, an attached cable, an associated Wi-Fi link and the offline
// docs/editor are not presence. Every exchange is bound to the epoch that issued
// it: a heartbeat, status or close that outlives its connection is discarded and
// can never touch the replacement session.
//
// Selected-transport mapping reuses `ConnectionManager` (USB -> epoch_command,
// HTTP -> epoch_http_exchange on the selected transport); nothing here opens a
// second handle and a USB session never falls back to Wi-Fi.
// =============================================================================

use crate::bbp::{self, CMD_STANDBY};
use crate::connection_manager::{ConnectionManager, EpochChanged};
use crate::state::ConnectionMode;
use crate::transport::HttpExchange;
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::time::Duration;
use tauri::State;

type CmdResult<T> = Result<T, String>;

pub const SUBOP_STATUS: u8 = 0;
pub const SUBOP_PRESENCE: u8 = 1;
pub const SUBOP_POLICY: u8 = 2;
pub const SUBOP_WAKE: u8 = 3;

pub const STATUS_LEN: usize = 28;
pub const STATUS_SCHEMA: u8 = 1;
/// Allowed persisted timeouts in seconds (0 = automatic standby off).
pub const TIMEOUT_CHOICES: [u16; 4] = [0, 60, 300, 900];
/// The presence session refreshes at this period (device TTL is 45 s).
pub const PRESENCE_REFRESH_SECS: u64 = 10;
/// Bound on the best-effort release at disconnect.
const CLOSE_TIMEOUT: Duration = Duration::from_millis(1500);

const HTTP_TIMEOUT: Duration = Duration::from_secs(4);

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
// Presence sessions (one per connect, keyed by connection epoch + serial)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PresencePhase {
    /// Registered (or being registered); the heartbeat keeps it alive.
    Open,
    /// This firmware has no standby; nothing is sent for the connection.
    Unsupported,
    /// Released (explicitly or at disconnect); no further traffic.
    Closed,
}

#[derive(Debug, Clone, PartialEq)]
pub struct PresenceSession {
    /// Distinguishes sessions inside one epoch (explicit re-registration).
    pub serial: u64,
    /// The `ConnectionManager` connection epoch this session belongs to.
    pub epoch: u64,
    pub client_id: u32,
    pub phase: PresencePhase,
    pub status: Option<StandbyStatus>,
    pub last_error: Option<String>,
}

/// The one live presence session. Every mutator names the (epoch, serial) it acts
/// for and does nothing, returning false/None, if that is not the current session:
/// a late reply of a replaced connection cannot update or release the new one.
#[derive(Debug, Default)]
pub struct PresenceSlot {
    next_serial: u64,
    current: Option<PresenceSession>,
}

impl PresenceSlot {
    /// Start a new session, replacing any earlier one (which is NOT released here:
    /// `close_current` is the release path and runs before the epoch is retired).
    pub fn begin(&mut self, epoch: u64, client_id: u32) -> PresenceSession {
        self.next_serial += 1;
        let s = PresenceSession {
            serial: self.next_serial,
            epoch,
            client_id,
            phase: PresencePhase::Open,
            status: None,
            last_error: None,
        };
        self.current = Some(s.clone());
        s
    }

    pub fn current(&self) -> Option<&PresenceSession> {
        self.current.as_ref()
    }

    fn owned(&mut self, epoch: u64, serial: u64) -> Option<&mut PresenceSession> {
        self.current
            .as_mut()
            .filter(|s| s.epoch == epoch && s.serial == serial)
    }

    /// The id to refresh, only while this exact session is Open.
    pub fn open_id(&self, epoch: u64, serial: u64) -> Option<u32> {
        self.current
            .as_ref()
            .filter(|s| s.epoch == epoch && s.serial == serial && s.phase == PresencePhase::Open)
            .map(|s| s.client_id)
    }

    pub fn record_status(&mut self, epoch: u64, serial: u64, st: StandbyStatus) -> bool {
        match self.owned(epoch, serial) {
            Some(s) if s.phase == PresencePhase::Open => {
                s.status = Some(st);
                s.last_error = None;
                true
            }
            _ => false,
        }
    }

    pub fn record_error(&mut self, epoch: u64, serial: u64, msg: String) -> bool {
        match self.owned(epoch, serial) {
            Some(s) if s.phase == PresencePhase::Open => {
                s.last_error = Some(msg);
                true
            }
            _ => false,
        }
    }

    pub fn mark_unsupported(&mut self, epoch: u64, serial: u64) -> bool {
        match self.owned(epoch, serial) {
            Some(s) if s.phase == PresencePhase::Open => {
                s.phase = PresencePhase::Unsupported;
                s.status = None;
                true
            }
            _ => false,
        }
    }

    /// Close the current session. Returns its (epoch, id) when a release must go on
    /// the wire, i.e. it was Open; None when there is nothing to release.
    pub fn close_current(&mut self) -> Option<(u64, u32)> {
        let s = self.current.as_mut()?;
        let release = (s.phase == PresencePhase::Open).then_some((s.epoch, s.client_id));
        s.phase = PresencePhase::Closed;
        release
    }
}

/// True for a BBP BUSY reply (error 0x06): the link answered and the device refused
/// the command because it is not ready (standby, waking). Not a link failure.
pub fn is_busy_refusal(msg: &str) -> bool {
    crate::scripts::bbp_error_code(msg) == Some(bbp::ERR_BUSY)
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

    /// The REST request. The selected HTTP transport attaches the admin token; the
    /// firmware only requires it for the policy route.
    fn exchange(&self) -> HttpExchange {
        let (is_get, path, body) = self.http();
        HttpExchange {
            method: if is_get { "GET" } else { "POST" },
            path: path.to_string(),
            query: Vec::new(),
            body: (!is_get)
                .then(|| (serde_json::to_vec(&body).unwrap_or_default(), "application/json")),
            timeout: HTTP_TIMEOUT,
        }
    }
}

#[derive(Debug, PartialEq)]
enum CallError {
    /// Firmware without system standby (an old build), and nothing else.
    Unsupported,
    /// The connection this call was issued for is gone; the result must be dropped.
    Stale,
    /// Everything else: BUSY, a full client table, a malformed request, a link error.
    Failed(String),
}

/// A BBP ERR for 0x78 reads "... (error 0x01, cmd 0x78)": INVALID_CMD on an
/// opcode this firmware does not know. Malformed (0x03), BUSY/full (0x06) and
/// every other code mean the firmware HAS standby.
fn classify_usb_error(msg: &str) -> CallError {
    if msg.contains("error 0x01, cmd 0x78") {
        CallError::Unsupported
    } else {
        CallError::Failed(msg.to_string())
    }
}

/// Classify a REST reply. Old firmware answers an unknown route with a plain-text
/// 404/405; new firmware always answers a standby route with a JSON object, so a
/// JSON 404 is a refusal, not "unsupported".
fn parse_http_reply(status: u16, body: &[u8]) -> Result<StandbyStatus, CallError> {
    let json: Option<Value> = serde_json::from_slice(body).ok();
    let firmware_json = json.as_ref().is_some_and(|v| {
        v.is_object() && (v.get("ok").is_some() || v.get("error").is_some() || v.get("state").is_some())
    });
    if matches!(status, 404 | 405) && !firmware_json {
        return Err(CallError::Unsupported);
    }
    if !(200..300).contains(&status) {
        let detail = json.as_ref().and_then(|v| {
            let error = v.get("error").and_then(Value::as_str)?;
            Some(match v.get("reason").and_then(Value::as_str) {
                Some(r) => format!("{error}: {r}"),
                None => error.to_string(),
            })
        });
        return Err(CallError::Failed(
            detail.unwrap_or_else(|| format!("HTTP {status}")),
        ));
    }
    let v = json.ok_or_else(|| CallError::Failed("bad standby reply: not JSON".to_string()))?;
    parse_status_json(&v).map_err(CallError::Failed)
}

fn epoch_call_error(e: anyhow::Error) -> CallError {
    if e.is::<EpochChanged>() {
        CallError::Stale
    } else {
        CallError::Failed(e.to_string())
    }
}

/// Run `op` on the selected link for connection epoch `epoch`.
async fn execute_for(
    mgr: &ConnectionManager,
    epoch: u64,
    op: &Op,
) -> Result<StandbyStatus, CallError> {
    if mgr.connection_epoch() != epoch {
        return Err(CallError::Stale);
    }
    match mgr.get_connection_status().mode {
        ConnectionMode::Disconnected => Err(CallError::Failed("Not connected".to_string())),
        ConnectionMode::Usb => match mgr.epoch_command(epoch, CMD_STANDBY, &op.payload()).await {
            Ok(rsp) => parse_status(&rsp).map_err(CallError::Failed),
            Err(e) if e.is::<EpochChanged>() => Err(CallError::Stale),
            Err(e) => Err(classify_usb_error(&e.to_string())),
        },
        ConnectionMode::Http => match mgr.epoch_http_exchange(epoch, op.exchange()).await {
            Ok(reply) => parse_http_reply(reply.status, &reply.body),
            Err(e) => Err(epoch_call_error(e)),
        },
    }
}

/// Run `op` for the connection that is current right now.
async fn execute(mgr: &ConnectionManager, op: &Op) -> Result<StandbyStatus, CallError> {
    execute_for(mgr, mgr.connection_epoch(), op).await
}

fn into_cmd_error(e: CallError) -> String {
    match e {
        CallError::Unsupported => {
            "standby unsupported: this firmware has no system standby (BBP 0x78 / /api/standby)"
                .to_string()
        }
        CallError::Stale => "connection changed".to_string(),
        CallError::Failed(m) => m,
    }
}

// -----------------------------------------------------------------------------
// Presence heartbeat (backend-owned, one task per session)
// -----------------------------------------------------------------------------

enum Beat {
    Continue,
    /// Session over: replaced, closed, unsupported or the connection is gone.
    Stop,
}

fn lock_slot(mgr: &ConnectionManager) -> std::sync::MutexGuard<'_, PresenceSlot> {
    mgr.presence().lock().unwrap_or_else(|e| e.into_inner())
}

/// One open/refresh of session (epoch, serial). The reply is applied only if that
/// session is still the current one.
async fn beat(mgr: &ConnectionManager, epoch: u64, serial: u64) -> Beat {
    let Some(id) = lock_slot(mgr).open_id(epoch, serial) else {
        return Beat::Stop;
    };
    if mgr.connection_epoch() != epoch || !mgr.is_connected() {
        return Beat::Stop;
    }
    match execute_for(mgr, epoch, &Op::Presence { id, present: true }).await {
        Ok(status) => {
            if lock_slot(mgr).record_status(epoch, serial, status) {
                Beat::Continue
            } else {
                Beat::Stop
            }
        }
        Err(CallError::Unsupported) => {
            lock_slot(mgr).mark_unsupported(epoch, serial);
            Beat::Stop
        }
        Err(CallError::Stale) => Beat::Stop,
        Err(CallError::Failed(m)) => {
            // BUSY/full/link error: keep the session and try again next period.
            let mut slot = lock_slot(mgr);
            let changed = slot.current().is_some_and(|s| s.last_error.as_deref() != Some(&m));
            if changed {
                log::warn!("standby presence refresh failed: {m}");
            }
            if slot.record_error(epoch, serial, m) {
                Beat::Continue
            } else {
                Beat::Stop
            }
        }
    }
}

async fn heartbeat(mgr: ConnectionManager, epoch: u64, serial: u64, period: Duration) {
    loop {
        if matches!(beat(&mgr, epoch, serial).await, Beat::Stop) {
            break;
        }
        tokio::time::sleep(period).await;
    }
}

/// Begin the presence session of a freshly established connection (`epoch` is the
/// connection epoch captured right after the disconnect that preceded the connect).
pub fn start_presence(mgr: ConnectionManager, epoch: u64) {
    start_presence_every(mgr, epoch, Duration::from_secs(PRESENCE_REFRESH_SECS));
}

fn start_presence_every(mgr: ConnectionManager, epoch: u64, period: Duration) {
    let serial = lock_slot(&mgr).begin(epoch, new_client_id()).serial;
    tokio::spawn(heartbeat(mgr, epoch, serial, period));
}

/// Release the current session on the wire (best effort, bounded) and close it. The
/// reply is deliberately ignored: nothing a release answers may reach any session.
pub async fn close_presence(mgr: &ConnectionManager) {
    let release = lock_slot(mgr).close_current();
    if let Some((epoch, id)) = release {
        let op = Op::Presence { id, present: false };
        let _ = tokio::time::timeout(CLOSE_TIMEOUT, execute_for(mgr, epoch, &op)).await;
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

/// Register (`present = true`) or release (`false`) this app as a control client of
/// the CURRENT connection. The backend already runs this per connect (heartbeat
/// every `PRESENCE_REFRESH_SECS`, release on disconnect); the command is the explicit
/// form: `true` refreshes now (after a release it begins a new session with a new
/// id), `false` releases best-effort and ends the session.
///
/// Returns `Ok(Some(status))` when registered, `Ok(None)` when the firmware has no
/// standby and for every release. A release never errors: the TTL covers a lost link.
#[tauri::command]
pub async fn standby_presence(
    present: bool,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<Option<StandbyStatus>> {
    if !mgr.is_connected() {
        return if present {
            Err("Not connected".to_string())
        } else {
            Ok(None)
        };
    }
    if !present {
        close_presence(&mgr).await;
        return Ok(None);
    }
    let epoch = mgr.connection_epoch();
    let open = lock_slot(&mgr)
        .current()
        .filter(|s| s.epoch == epoch)
        .map(|s| (s.serial, s.phase));
    let serial = match open {
        Some((_, PresencePhase::Unsupported)) => return Ok(None),
        Some((serial, PresencePhase::Open)) => serial,
        // None, Closed, or a session of another epoch: a new session with a new id.
        _ => {
            let serial = lock_slot(&mgr).begin(epoch, new_client_id()).serial;
            tokio::spawn(heartbeat(
                (*mgr).clone(),
                epoch,
                serial,
                Duration::from_secs(PRESENCE_REFRESH_SECS),
            ));
            serial
        }
    };
    let Some(id) = lock_slot(&mgr).open_id(epoch, serial) else {
        return Ok(None);
    };
    match execute_for(&mgr, epoch, &Op::Presence { id, present: true }).await {
        Ok(status) => {
            lock_slot(&mgr).record_status(epoch, serial, status.clone());
            Ok(Some(status))
        }
        Err(CallError::Unsupported) => {
            lock_slot(&mgr).mark_unsupported(epoch, serial);
            Ok(None)
        }
        Err(e) => Err(into_cmd_error(e)),
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
    fn begin_numbers_sessions_and_replaces_the_previous_one() {
        let mut slot = PresenceSlot::default();
        let a = slot.begin(4, 111);
        let b = slot.begin(5, 222);
        assert_eq!((a.serial, b.serial), (1, 2));
        assert_eq!(slot.current().map(|s| (s.epoch, s.client_id)), Some((5, 222)));
        assert_eq!(slot.open_id(5, 2), Some(222));
        assert_eq!(slot.open_id(4, 1), None, "the replaced session is no longer live");
    }

    #[test]
    fn a_late_reply_of_a_replaced_session_cannot_update_the_new_one() {
        let mut slot = PresenceSlot::default();
        let old = slot.begin(4, 111);
        let new = slot.begin(5, 222);
        let st = parse_status(&record(0, 1, 0, 1, 300, 1)).unwrap();
        assert!(!slot.record_status(old.epoch, old.serial, st.clone()));
        assert!(!slot.record_error(old.epoch, old.serial, "late".into()));
        assert!(!slot.mark_unsupported(old.epoch, old.serial));
        assert!(slot.current().unwrap().status.is_none());
        assert!(slot.record_status(new.epoch, new.serial, st));
        assert!(slot.current().unwrap().status.is_some());
        // Same epoch, different serial (explicit re-registration) is also a different session.
        assert!(!slot.record_error(new.epoch, new.serial + 1, "x".into()));
    }

    #[test]
    fn close_releases_an_open_session_exactly_once() {
        let mut slot = PresenceSlot::default();
        slot.begin(7, 99);
        assert_eq!(slot.close_current(), Some((7, 99)));
        assert_eq!(slot.close_current(), None);
        assert_eq!(slot.open_id(7, 1), None);
        assert!(!slot.record_status(7, 1, parse_status(&record(0, 1, 0, 1, 300, 0)).unwrap()));
    }

    #[test]
    fn an_unsupported_session_has_nothing_to_release() {
        let mut slot = PresenceSlot::default();
        let s = slot.begin(7, 99);
        assert!(slot.mark_unsupported(s.epoch, s.serial));
        assert_eq!(slot.open_id(s.epoch, s.serial), None);
        assert_eq!(slot.close_current(), None);
    }

    #[test]
    fn busy_refusal_is_error_0x06_only() {
        assert!(is_busy_refusal("Device is busy processing another operation (error 0x06, cmd 0x01)"));
        assert!(!is_busy_refusal("The command is not recognized by the device (error 0x01, cmd 0x78)"));
        assert!(!is_busy_refusal("Command timeout (cmd=0x01)"));
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
    fn only_a_plain_404_or_405_means_old_firmware() {
        let good = serde_json::to_vec(&json!({
            "schema": 1, "state": "active", "ready": true, "timeoutSeconds": 300
        }))
        .unwrap();
        assert_eq!(parse_http_reply(200, &good).unwrap().state, "active");
        // Old firmware: esp-idf's plain-text 404 / 405.
        assert_eq!(parse_http_reply(404, b"Not found").unwrap_err(), CallError::Unsupported);
        assert_eq!(parse_http_reply(405, b"").unwrap_err(), CallError::Unsupported);
        // New firmware always answers a standby route with a JSON object.
        let refused = br#"{"ok":false,"error":"unknown standby route"}"#;
        assert!(matches!(parse_http_reply(404, refused), Err(CallError::Failed(_))));
    }

    #[test]
    fn malformed_full_and_busy_replies_are_failures_not_unsupported() {
        let bad = parse_http_reply(400, br#"{"ok":false,"error":"invalid clientId"}"#).unwrap_err();
        assert_eq!(bad, CallError::Failed("invalid clientId".into()));
        let full =
            parse_http_reply(409, br#"{"ok":false,"error":"busy","reason":"client table full"}"#)
                .unwrap_err();
        assert_eq!(full, CallError::Failed("busy: client table full".into()));
        let waking = parse_http_reply(
            503,
            br#"{"ok":false,"error":"standby","state":"waking","retryAfterMs":1500}"#,
        )
        .unwrap_err();
        assert_eq!(waking, CallError::Failed("standby".into()));
        assert_eq!(parse_http_reply(500, b"oops").unwrap_err(), CallError::Failed("HTTP 500".into()));
        assert!(matches!(parse_http_reply(200, b"<html>"), Err(CallError::Failed(_))));
    }

    #[test]
    fn http_routes_and_bodies() {
        let (get, path, _) = Op::Status.http();
        assert!(get && path == "/api/standby/status");
        let (get, path, body) = Op::Presence { id: 9, present: true }.http();
        assert!(!get && path == "/api/standby/presence");
        assert_eq!(body, json!({ "clientId": 9, "present": true }));
        let (_, path, body) = Op::Policy(60).http();
        assert_eq!(path, "/api/standby/policy");
        assert_eq!(body, json!({ "timeoutSeconds": 60 }));
        assert_eq!(Op::Wake.http().1, "/api/standby/wake");
    }

    #[test]
    fn exchange_is_a_get_without_body_or_a_json_post() {
        let get = Op::Status.exchange();
        assert_eq!((get.method, get.path.as_str()), ("GET", "/api/standby/status"));
        assert!(get.body.is_none());
        let post = Op::Policy(900).exchange();
        assert_eq!(post.method, "POST");
        let (bytes, ctype) = post.body.expect("policy has a body");
        assert_eq!(ctype, "application/json");
        assert_eq!(
            serde_json::from_slice::<Value>(&bytes).unwrap(),
            json!({ "timeoutSeconds": 900 })
        );
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

    // ---- ConnectionManager integration over a fake transport ----

    use crate::state::DeviceState;
    use crate::transport::Transport;
    use async_trait::async_trait;
    use std::sync::{Arc, Mutex as StdMutex};

    type Frames = Arc<StdMutex<Vec<(u8, Vec<u8>)>>>;
    type Reply = Arc<StdMutex<Result<Vec<u8>, String>>>;
    const TICK: Duration = Duration::from_millis(20);

    struct Fake {
        frames: Frames,
        reply: Reply,
        delay: Duration,
    }

    #[async_trait]
    impl Transport for Fake {
        async fn send_command(&self, cmd_id: u8, payload: &[u8]) -> anyhow::Result<Vec<u8>> {
            self.frames.lock().unwrap().push((cmd_id, payload.to_vec()));
            if !self.delay.is_zero() {
                tokio::time::sleep(self.delay).await;
            }
            self.reply.lock().unwrap().clone().map_err(|m| anyhow::anyhow!(m))
        }
        async fn get_status(&self) -> anyhow::Result<DeviceState> {
            Ok(DeviceState::default())
        }
        fn is_connected(&self) -> bool {
            true
        }
        async fn disconnect(&self) -> anyhow::Result<()> {
            Ok(())
        }
        fn transport_name(&self) -> &str {
            "Fake"
        }
    }

    fn fake(reply: Result<Vec<u8>, String>, delay_ms: u64) -> (Box<dyn Transport>, Frames, Reply) {
        let frames: Frames = Arc::default();
        let reply: Reply = Arc::new(StdMutex::new(reply));
        let t = Fake {
            frames: frames.clone(),
            reply: reply.clone(),
            delay: Duration::from_millis(delay_ms),
        };
        (Box::new(t), frames, reply)
    }

    fn active_reply() -> Result<Vec<u8>, String> {
        Ok(record(0, 1, 0, 1, 300, 1))
    }

    fn unsupported_reply() -> String {
        "The command is not recognized by the device (error 0x01, cmd 0x78)".to_string()
    }

    /// (client id, present) of every presence frame sent, in order.
    fn presence_frames(f: &Frames) -> Vec<(u32, bool)> {
        f.lock()
            .unwrap()
            .iter()
            .filter(|(cmd, p)| *cmd == CMD_STANDBY && p.first() == Some(&SUBOP_PRESENCE))
            .map(|(_, p)| (u32::from_le_bytes([p[1], p[2], p[3], p[4]]), p[5] == 1))
            .collect()
    }

    async fn settle(ticks: u32) {
        tokio::time::sleep(TICK * ticks).await;
    }

    #[tokio::test]
    async fn heartbeat_registers_one_random_id_and_refreshes_with_it() {
        let mgr = ConnectionManager::new();
        let (t, frames, _) = fake(active_reply(), 0);
        let epoch = mgr.attach_for_test(t, ConnectionMode::Usb, "COM6").await;
        start_presence_every(mgr.clone(), epoch, TICK);
        settle(6).await;
        let seen = presence_frames(&frames);
        assert!(seen.len() >= 3, "{seen:?}");
        let id = seen[0].0;
        assert_ne!(id, 0);
        assert!(seen.iter().all(|(i, present)| *i == id && *present), "{seen:?}");
        let slot = mgr.presence().lock().unwrap();
        let s = slot.current().unwrap();
        assert_eq!((s.epoch, s.client_id, s.phase), (epoch, id, PresencePhase::Open));
        assert_eq!(s.status.as_ref().map(|s| s.state.as_str()), Some("active"));
    }

    #[tokio::test]
    async fn disconnect_releases_best_effort_and_stops_the_heartbeat() {
        let mgr = ConnectionManager::new();
        let (t, frames, _) = fake(active_reply(), 0);
        let epoch = mgr.attach_for_test(t, ConnectionMode::Usb, "COM6").await;
        start_presence_every(mgr.clone(), epoch, TICK);
        settle(4).await;
        close_presence(&mgr).await;
        mgr.detach_for_test().await;
        let after_close = presence_frames(&frames);
        let (id, present) = *after_close.last().unwrap();
        assert!(!present, "the last frame is the release");
        assert_ne!(id, 0);
        settle(5).await;
        assert_eq!(presence_frames(&frames).len(), after_close.len(), "nothing after the release");
        // A second close has nothing left to release.
        close_presence(&mgr).await;
        assert_eq!(presence_frames(&frames).len(), after_close.len());
    }

    #[tokio::test]
    async fn reconnecting_to_the_same_address_is_a_new_epoch_with_a_new_session() {
        let mgr = ConnectionManager::new();
        let (t1, f1, _) = fake(active_reply(), 0);
        let e1 = mgr.attach_for_test(t1, ConnectionMode::Usb, "COM6").await;
        start_presence_every(mgr.clone(), e1, TICK);
        settle(3).await;
        let first = mgr.presence().lock().unwrap().current().unwrap().clone();
        close_presence(&mgr).await;
        mgr.detach_for_test().await;

        let (t2, f2, _) = fake(active_reply(), 0);
        let e2 = mgr.attach_for_test(t2, ConnectionMode::Usb, "COM6").await;
        start_presence_every(mgr.clone(), e2, TICK);
        settle(3).await;
        let second = mgr.presence().lock().unwrap().current().unwrap().clone();

        assert!(e2 > e1);
        assert_eq!((first.serial, second.serial), (1, 2));
        assert_eq!(second.epoch, e2);
        assert_ne!(first.client_id, second.client_id);
        let old = first.client_id;
        assert!(
            presence_frames(&f2).iter().all(|(i, _)| *i == second.client_id && *i != old),
            "the new connection never speaks with the old id"
        );
        assert!(presence_frames(&f1).iter().any(|(i, p)| *i == old && !*p), "old id was released");
    }

    #[tokio::test]
    async fn a_reply_that_outlives_its_session_cannot_update_the_replacement() {
        let mgr = ConnectionManager::new();
        let (t, frames, _) = fake(active_reply(), 120);
        let epoch = mgr.attach_for_test(t, ConnectionMode::Usb, "COM6").await;
        start_presence_every(mgr.clone(), epoch, TICK);
        settle(2).await; // first (slow) refresh is now in flight
        let old_id = presence_frames(&frames)[0].0;
        let replacement = mgr.presence().lock().unwrap().begin(epoch, 0xABCD_0001);
        settle(12).await;
        let slot = mgr.presence().lock().unwrap();
        let cur = slot.current().unwrap();
        assert_eq!((cur.serial, cur.client_id), (replacement.serial, 0xABCD_0001));
        assert!(cur.status.is_none(), "the old reply did not land on the new session");
        drop(slot);
        let old_refreshes = presence_frames(&frames).iter().filter(|(i, _)| *i == old_id).count();
        assert_eq!(old_refreshes, 1, "the replaced session stopped after its in-flight call");
    }

    #[tokio::test]
    async fn old_firmware_ends_the_session_and_sends_nothing_more() {
        let mgr = ConnectionManager::new();
        let (t, frames, _) = fake(Err(unsupported_reply()), 0);
        let epoch = mgr.attach_for_test(t, ConnectionMode::Usb, "COM6").await;
        start_presence_every(mgr.clone(), epoch, TICK);
        settle(6).await;
        assert_eq!(presence_frames(&frames).len(), 1);
        assert_eq!(
            mgr.presence().lock().unwrap().current().unwrap().phase,
            PresencePhase::Unsupported
        );
        close_presence(&mgr).await;
        assert_eq!(presence_frames(&frames).len(), 1, "no release for a session that never existed");
    }

    #[tokio::test]
    async fn malformed_busy_and_full_replies_are_not_old_firmware() {
        for code in ["0x03", "0x06", "0x07"] {
            let mgr = ConnectionManager::new();
            let msg = format!("device refused (error {code}, cmd 0x78)");
            let (t, frames, _) = fake(Err(msg.clone()), 0);
            let epoch = mgr.attach_for_test(t, ConnectionMode::Usb, "COM6").await;
            start_presence_every(mgr.clone(), epoch, TICK);
            settle(6).await;
            assert!(presence_frames(&frames).len() >= 3, "{code}: keeps retrying");
            let slot = mgr.presence().lock().unwrap();
            let s = slot.current().unwrap();
            assert_eq!(s.phase, PresencePhase::Open, "{code}");
            assert_eq!(s.last_error.as_deref(), Some(msg.as_str()));
        }
    }

    #[tokio::test]
    async fn a_call_for_a_retired_connection_is_refused_without_wire_traffic() {
        let mgr = ConnectionManager::new();
        let (t1, f1, _) = fake(active_reply(), 0);
        let e1 = mgr.attach_for_test(t1, ConnectionMode::Usb, "COM6").await;
        mgr.detach_for_test().await;
        let (t2, f2, _) = fake(active_reply(), 0);
        mgr.attach_for_test(t2, ConnectionMode::Usb, "COM6").await;
        let r = execute_for(&mgr, e1, &Op::Presence { id: 5, present: false }).await;
        assert_eq!(r.unwrap_err(), CallError::Stale);
        assert!(f1.lock().unwrap().is_empty() && f2.lock().unwrap().is_empty());
    }

    #[tokio::test]
    async fn the_heartbeat_ends_when_the_link_is_lost() {
        let mgr = ConnectionManager::new();
        let (t, frames, _) = fake(active_reply(), 0);
        let epoch = mgr.attach_for_test(t, ConnectionMode::Usb, "COM6").await;
        start_presence_every(mgr.clone(), epoch, TICK);
        settle(3).await;
        mgr.detach_for_test().await; // no release possible: the TTL covers it
        let n = presence_frames(&frames).len();
        settle(5).await;
        assert_eq!(presence_frames(&frames).len(), n);
    }
}
