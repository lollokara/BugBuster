// =============================================================================
// scripts.rs - on-device MicroPython scripting backend.
//
// One typed Tauri command, `script_request(operation, args)`, runs over the
// selected transport only: HTTP `/api/scripts/*` on Wi-Fi, the SCRIPT_AUTORUN
// JSON tunnel (sub 6/7) on USB. Replies follow the iOS `ScriptsClient`
// contracts; `ok:false` replies carry a `kind` the UI can switch on.
// =============================================================================

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use async_trait::async_trait;
use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use serde_json::{json, Map, Value};
use tauri::{AppHandle, Emitter, State};

use crate::bbp;
use crate::connection_manager::ConnectionManager;
use crate::state::ConnectionMode;
use crate::transport::{HttpExchange, HttpReply};

pub const NAME_MAX: usize = 32;
pub const BODY_MAX: usize = 32 * 1024;

/// Mirrors `script_storage_validate_name()` (1-32 of [A-Za-z0-9_.-], ends in
/// ".py", no leading '.').
pub fn valid_name(name: &str) -> bool {
    let n = name.len();
    (1..=NAME_MAX).contains(&n)
        && !name.starts_with('.')
        && name.ends_with(".py")
        && name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'.' || b == b'-')
}

// -----------------------------------------------------------------------------
// USB JSON tunnel codec (SCRIPT_AUTORUN 0xFD, sub 6 CALL / sub 7 FETCH)
// -----------------------------------------------------------------------------
pub mod tunnel {
    use crate::bbp::{PayloadReader, PayloadWriter};

    pub const SUB_CALL: u8 = 6;
    pub const SUB_FETCH: u8 = 7;
    /// Request bytes per CALL frame (BBP payload cap 1018 minus the 6-byte head).
    pub const REQ_CHUNK: usize = 1000;
    /// Largest staged request / response (u16 totals on the wire).
    pub const MAX_TOTAL: usize = 65535;

    pub const ST_MORE: u8 = 0;
    pub const ST_DONE: u8 = 1;
    pub const ST_REJECT: u8 = 2;

    pub const REJ_UNKNOWN_OP: u16 = 1;
    pub const REJ_OFFSET: u16 = 2;
    pub const REJ_TOO_LARGE: u16 = 3;
    pub const REJ_NO_MEMORY: u16 = 4;
    pub const REJ_NO_REQUEST: u16 = 5;
    pub const REJ_STALE: u16 = 6;

    /// Operation ids; the order is the firmware dispatch table.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    #[repr(u8)]
    pub enum Op {
        /// Capability probe; the app detects old firmware from the first CALL's BBP error instead.
        #[allow(dead_code)]
        Caps = 0,
        Status = 1,
        Logs = 2,
        Stop = 3,
        Files = 4,
        Storage = 5,
        Get = 6,
        Delete = 7,
        Chunk = 8,
        Run = 9,
        Eval = 10,
        Lint = 11,
        AutorunStatus = 12,
        AutorunEnable = 13,
        AutorunDisable = 14,
        AutorunRun = 15,
        Reset = 16,
    }

    pub fn encode_call(op: Op, total: usize, off: usize, chunk: &[u8]) -> Vec<u8> {
        let mut w = PayloadWriter::new();
        w.put_u8(SUB_CALL);
        w.put_u8(op as u8);
        w.put_u16(total as u16);
        w.put_u16(off as u16);
        w.buf.extend_from_slice(chunk);
        w.buf
    }

    pub fn encode_fetch(token: u8, off: usize) -> Vec<u8> {
        let mut w = PayloadWriter::new();
        w.put_u8(SUB_FETCH);
        w.put_u8(token);
        w.put_u16(off as u16);
        w.buf
    }

    #[derive(Debug, PartialEq, Eq)]
    pub struct Reply {
        pub state: u8,
        pub token: u8,
        /// MORE: bytes received; DONE: total response length; REJECT: reason.
        pub a: u16,
        /// DONE: length of `data`.
        pub b: u16,
        pub data: Vec<u8>,
    }

    pub fn decode_reply(p: &[u8]) -> Option<Reply> {
        let mut r = PayloadReader::new(p);
        let state = r.get_u8()?;
        let token = r.get_u8()?;
        let a = r.get_u16()?;
        let b = r.get_u16()?;
        let data = p[r.pos()..].to_vec();
        if state == ST_DONE && data.len() != b as usize {
            return None;
        }
        Some(Reply { state, token, a, b, data })
    }
}

// -----------------------------------------------------------------------------
// Results
// -----------------------------------------------------------------------------

/// Failure of one operation. Serialised as `{ok:false, kind, error, ...extra}`.
#[derive(Debug, Clone, PartialEq)]
pub struct OpErr {
    pub kind: &'static str,
    pub error: String,
    pub extra: Map<String, Value>,
}

impl OpErr {
    pub fn new(kind: &'static str, error: impl Into<String>) -> Self {
        Self { kind, error: error.into(), extra: Map::new() }
    }
    pub fn with(mut self, key: &str, v: Value) -> Self {
        self.extra.insert(key.to_string(), v);
        self
    }
    pub fn into_value(self, operation: &str) -> Value {
        let mut m = self.extra;
        m.insert("ok".into(), json!(false));
        m.insert("kind".into(), json!(self.kind));
        m.insert("error".into(), json!(self.error));
        m.insert("operation".into(), json!(operation));
        Value::Object(m)
    }
}

/// Firmware text for a refused lint that is NOT a syntax error
/// (`scripting_lint_string`): the check never ran.
pub fn lint_unavailable_kind(msg: &str) -> Option<&'static str> {
    const BUSY: [&str; 2] = ["Interpreter is busy", "Scripting queue is full"];
    const UNAVAILABLE: [&str; 5] = [
        "Interpreter timed out",
        "Scripting engine not enabled",
        "Out of memory",
        "Failed to create semaphore",
        "scripting disabled",
    ];
    if BUSY.iter().any(|p| msg.starts_with(p)) {
        Some("busy")
    } else if UNAVAILABLE.iter().any(|p| msg.starts_with(p)) {
        Some("unavailable")
    } else {
        None
    }
}

/// HTTP status the webserver would send for a tunnelled `api_core` reply
/// (`send_scripts_result`): a string `running` means the file slot is taken.
pub fn tunnel_status(body: &[u8]) -> u16 {
    match serde_json::from_slice::<Value>(body) {
        Ok(Value::Object(o)) => {
            if matches!(o.get("running"), Some(Value::String(_))) {
                409
            } else if o.contains_key("error") {
                400
            } else {
                200
            }
        }
        _ => 200,
    }
}

/// BBP error code embedded in `UsbTransport::device_error_message` text.
pub fn bbp_error_code(msg: &str) -> Option<u8> {
    let i = msg.find("(error 0x")? + "(error 0x".len();
    u8::from_str_radix(msg.get(i..i + 2)?, 16).ok()
}

/// Old firmware rejects SCRIPT_AUTORUN sub 6/7 as ERR_INVALID_PARAM (0x03).
pub fn is_unsupported_sub(msg: &str) -> bool {
    matches!(bbp_error_code(msg), Some(0x01) | Some(0x03))
}

pub const WIRE_TIMEOUT: Duration = Duration::from_secs(10);
const LINT_TIMEOUT: Duration = Duration::from_secs(12);
/// Raw bytes per USB `files/chunk` request (base64 stays under the 4096-char firmware cap).
const USB_SAVE_CHUNK: usize = 1536;
/// Matches `SCRIPTS_FILE_PAGE` / `MP_LOG_RESP_MAX` in the firmware.
const FILE_PAGE: usize = 3072;
const LOG_PAGE: usize = 4096;

/// BBP SCRIPT_AUTORUN timeout: CALL frames may block in the interpreter (lint 5 s, replace
/// 3 s) and autorun run-now waits up to AUTORUN_MAX_WALL_MS (30 s).
pub fn autorun_timeout_ms(payload: &[u8]) -> u64 {
    match payload {
        [tunnel::SUB_CALL, op, ..] if *op == tunnel::Op::AutorunRun as u8 => 40_000,
        [tunnel::SUB_CALL, ..] => 15_000,
        _ => 2_000,
    }
}

// -----------------------------------------------------------------------------
// Wire seam
// -----------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Usb,
    Http,
}

#[derive(Debug, Clone, PartialEq)]
pub enum WireError {
    /// Firmware lacks the feature; the UI shows update-needed (no cross-transport fallback).
    Unsupported(String),
    Transport(String),
    TooLarge(String),
    Protocol(String),
}

impl From<WireError> for OpErr {
    fn from(e: WireError) -> Self {
        match e {
            WireError::Unsupported(m) => OpErr::new("unsupported", m),
            WireError::Transport(m) => OpErr::new("transport", m),
            WireError::TooLarge(m) => OpErr::new("too_large", m),
            WireError::Protocol(m) => OpErr::new("protocol", m),
        }
    }
}

#[async_trait]
pub trait ScriptWire: Send + Sync {
    fn kind(&self) -> Kind;
    /// REST exchange; only called when `kind() == Http`.
    async fn http(&self, req: HttpExchange) -> Result<HttpReply, WireError>;
    /// One JSON request/response through the SCRIPT_AUTORUN tunnel; only called when `kind() == Usb`.
    async fn tunnel(&self, op: tunnel::Op, request: &[u8]) -> Result<Vec<u8>, WireError>;
}

/// BBP command sender (the selected USB transport).
#[async_trait]
pub trait BbpSend: Send + Sync {
    async fn bbp(&self, cmd: u8, payload: &[u8]) -> anyhow::Result<Vec<u8>>;
}

#[async_trait]
impl BbpSend for ConnectionManager {
    async fn bbp(&self, cmd: u8, payload: &[u8]) -> anyhow::Result<Vec<u8>> {
        self.send_command(cmd, payload).await
    }
}

fn reject_error(r: &tunnel::Reply, op: tunnel::Op) -> WireError {
    match r.a {
        tunnel::REJ_UNKNOWN_OP => WireError::Unsupported(format!(
            "firmware has no scripting operation {op:?} (id {}): update the firmware",
            op as u8
        )),
        tunnel::REJ_TOO_LARGE => WireError::TooLarge("request or reply exceeds the USB tunnel limit".into()),
        tunnel::REJ_NO_MEMORY => WireError::Transport("device is out of memory".into()),
        tunnel::REJ_OFFSET => WireError::Protocol(format!("tunnel offset mismatch (device expects {})", r.b)),
        tunnel::REJ_NO_REQUEST => WireError::Protocol("tunnel has no request in progress".into()),
        tunnel::REJ_STALE => WireError::Protocol("tunnel reply was replaced by a newer request".into()),
        other => WireError::Protocol(format!("tunnel rejected the frame (reason {other})")),
    }
}

/// Send `request` (JSON) through the tunnel and collect the full reply.
pub async fn tunnel_exchange(
    s: &dyn BbpSend,
    op: tunnel::Op,
    request: &[u8],
) -> Result<Vec<u8>, WireError> {
    if request.is_empty() || request.len() > tunnel::MAX_TOTAL {
        return Err(WireError::TooLarge(format!(
            "request is {} bytes; the USB tunnel carries 1..={}",
            request.len(),
            tunnel::MAX_TOTAL
        )));
    }
    let total = request.len();
    let mut off = 0usize;
    let first = loop {
        let end = (off + tunnel::REQ_CHUNK).min(total);
        let payload = tunnel::encode_call(op, total, off, &request[off..end]);
        let rsp = s
            .bbp(bbp::CMD_SCRIPT_AUTORUN, &payload)
            .await
            .map_err(|e| {
                let m = e.to_string();
                if off == 0 && is_unsupported_sub(&m) {
                    WireError::Unsupported(
                        "firmware has no USB scripting tunnel (SCRIPT_AUTORUN sub 6): update the firmware".into(),
                    )
                } else {
                    WireError::Transport(m)
                }
            })?;
        let r = tunnel::decode_reply(&rsp)
            .ok_or_else(|| WireError::Protocol("malformed tunnel reply".into()))?;
        match r.state {
            tunnel::ST_MORE if end < total && r.a as usize == end => off = end,
            tunnel::ST_DONE if end == total => break r,
            tunnel::ST_REJECT => return Err(reject_error(&r, op)),
            _ => return Err(WireError::Protocol("tunnel state out of sequence".into())),
        }
    };
    let want = first.a as usize;
    let token = first.token;
    let mut out = first.data;
    if out.len() > want {
        return Err(WireError::Protocol("tunnel page longer than the reply".into()));
    }
    while out.len() < want {
        let rsp = s
            .bbp(bbp::CMD_SCRIPT_AUTORUN, &tunnel::encode_fetch(token, out.len()))
            .await
            .map_err(|e| WireError::Transport(e.to_string()))?;
        let r = tunnel::decode_reply(&rsp)
            .ok_or_else(|| WireError::Protocol("malformed tunnel page".into()))?;
        if r.state != tunnel::ST_DONE || r.token != token || r.a as usize != want || r.data.is_empty() {
            return Err(WireError::Protocol(format!("tunnel page refused (state {}, reason {})", r.state, r.a)));
        }
        out.extend_from_slice(&r.data);
    }
    Ok(out)
}

/// The selected connection as a `ScriptWire`.
pub struct LiveWire<'a> {
    pub mgr: &'a ConnectionManager,
    pub epoch: u64,
    pub transport_kind: Kind,
}

#[async_trait]
impl BbpSend for LiveWire<'_> {
    async fn bbp(&self, cmd: u8, payload: &[u8]) -> anyhow::Result<Vec<u8>> {
        self.mgr.script_command(self.epoch, cmd, payload).await
    }
}

#[async_trait]
impl ScriptWire for LiveWire<'_> {
    fn kind(&self) -> Kind {
        self.transport_kind
    }

    async fn http(&self, req: HttpExchange) -> Result<HttpReply, WireError> {
        self.mgr
            .script_http_exchange(self.epoch, req)
            .await
            .map_err(|e| WireError::Transport(e.to_string()))
    }

    async fn tunnel(&self, op: tunnel::Op, request: &[u8]) -> Result<Vec<u8>, WireError> {
        tunnel_exchange(self, op, request).await
    }
}

// -----------------------------------------------------------------------------
// Request plumbing (the iOS ScriptsClient `call` equivalent)
// -----------------------------------------------------------------------------

type R<T> = Result<T, OpErr>;

pub type Progress<'a> = &'a (dyn Fn(&str, usize, usize) + Send + Sync);

struct Route {
    op: tunnel::Op,
    method: &'static str,
    path: &'static str,
    query: Vec<(String, String)>,
    http_body: Option<(Vec<u8>, &'static str)>,
    /// JSON args for the USB tunnel (typed: numbers and bools, not query strings).
    usb: Value,
    timeout: Duration,
}

impl Route {
    fn get(op: tunnel::Op, path: &'static str) -> Self {
        Self::new(op, "GET", path)
    }
    fn post(op: tunnel::Op, path: &'static str) -> Self {
        Self::new(op, "POST", path)
    }
    fn new(op: tunnel::Op, method: &'static str, path: &'static str) -> Self {
        Self { op, method, path, query: vec![], http_body: None, usb: json!({}), timeout: WIRE_TIMEOUT }
    }
    fn query(mut self, k: &str, v: impl ToString) -> Self {
        self.query.push((k.to_string(), v.to_string()));
        self
    }
    fn usb(mut self, v: Value) -> Self {
        self.usb = v;
        self
    }
    fn text(mut self, s: &str) -> Self {
        self.http_body = Some((s.as_bytes().to_vec(), "text/plain"));
        self
    }
    fn timeout(mut self, t: Duration) -> Self {
        self.timeout = t;
        self
    }
}

struct Raw {
    status: u16,
    headers: HashMap<String, String>,
    body: Vec<u8>,
}

async fn rpc(w: &dyn ScriptWire, r: Route) -> R<Raw> {
    match w.kind() {
        Kind::Http => {
            let reply = w
                .http(HttpExchange {
                    method: r.method,
                    path: r.path.to_string(),
                    query: r.query,
                    body: r.http_body,
                    timeout: r.timeout,
                })
                .await?;
            Ok(Raw { status: reply.status, headers: reply.headers, body: reply.body })
        }
        Kind::Usb => {
            let req = serde_json::to_vec(&r.usb)
                .map_err(|e| OpErr::new("invalid", format!("request is not JSON: {e}")))?;
            let body = w.tunnel(r.op, &req).await?;
            Ok(Raw { status: tunnel_status(&body), headers: HashMap::new(), body })
        }
    }
}

fn object(raw: &Raw) -> Option<Map<String, Value>> {
    match serde_json::from_slice::<Value>(&raw.body) {
        Ok(Value::Object(o)) => Some(o),
        _ => None,
    }
}

fn error_text(o: &Map<String, Value>) -> Option<String> {
    ["error", "err"]
        .iter()
        .find_map(|k| o.get(*k).and_then(Value::as_str))
        .filter(|s| !s.is_empty())
        .map(str::to_string)
}

/// Map a refused request to an error; None when the reply is a success.
fn failure(raw: &Raw, obj: &Option<Map<String, Value>>, allow_not_ok: bool) -> Option<OpErr> {
    if raw.status == 401 {
        return Some(OpErr::new("unauthorized", "The device rejected the admin token"));
    }
    if let Some(o) = obj {
        if let Some(running) = o.get("running").and_then(Value::as_str) {
            let id = o.get("id").cloned().unwrap_or(json!(0));
            return Some(
                OpErr::new("busy", format!("'{running}' is running"))
                    .with("running", json!(running))
                    .with("id", id),
            );
        }
    }
    let ok_false = obj.as_ref().is_some_and(|o| o.get("ok") == Some(&json!(false)));
    let success = (200..300).contains(&raw.status);
    if success && (!ok_false || allow_not_ok) {
        return None;
    }
    let text = obj.as_ref().and_then(error_text);
    let message = text.clone().unwrap_or_else(|| {
        if success { "request failed".into() } else { format!("Device returned HTTP {}", raw.status) }
    });
    let kind = if text.as_deref() == Some("script not found") || raw.status == 404 {
        "not_found"
    } else {
        "firmware"
    };
    Some(OpErr::new(kind, message))
}

/// Run a route and return its JSON object, or the mapped failure.
async fn call(w: &dyn ScriptWire, r: Route, allow_not_ok: bool) -> R<Map<String, Value>> {
    let raw = rpc(w, r).await?;
    let obj = object(&raw);
    if let Some(e) = failure(&raw, &obj, allow_not_ok) {
        return Err(e);
    }
    obj.ok_or_else(|| OpErr::new("malformed", "Unexpected reply from the device (not a JSON object)"))
}

// -----------------------------------------------------------------------------
// Arguments
// -----------------------------------------------------------------------------

fn arg_str<'a>(args: &'a Value, key: &str) -> Option<&'a str> {
    args.get(key).and_then(Value::as_str)
}

fn arg_bool(args: &Value, key: &str, default: bool) -> bool {
    args.get(key).and_then(Value::as_bool).unwrap_or(default)
}

fn need_name(args: &Value) -> R<String> {
    let name = arg_str(args, "name").unwrap_or("");
    if valid_name(name) {
        Ok(name.to_string())
    } else {
        Err(OpErr::new(
            "invalid",
            format!("'{name}' is not a valid script name (1-32 of A-Z a-z 0-9 _ . -, ending in .py)"),
        ))
    }
}

fn check_source(source: &str, allow_empty: bool) -> R<()> {
    if source.is_empty() && !allow_empty {
        return Err(OpErr::new("invalid", "A script can't be empty"));
    }
    if source.len() > BODY_MAX {
        return Err(OpErr::new(
            "too_large",
            format!("Script is {} bytes; the device keeps at most {BODY_MAX}", source.len()),
        )
        .with("bytes", json!(source.len())));
    }
    Ok(())
}

fn with_ok(mut o: Map<String, Value>) -> Value {
    o.insert("ok".into(), json!(true));
    Value::Object(o)
}

// -----------------------------------------------------------------------------
// Operations
// -----------------------------------------------------------------------------

async fn passthrough(w: &dyn ScriptWire, op: tunnel::Op, path: &'static str) -> R<Value> {
    Ok(with_ok(call(w, Route::get(op, path), false).await?))
}

async fn op_files(w: &dyn ScriptWire) -> R<Value> {
    let o = call(w, Route::get(tunnel::Op::Files, "/api/scripts/files"), false).await?;
    match o.get("files") {
        Some(Value::Array(_)) => Ok(with_ok(o)),
        _ => Err(OpErr::new("malformed", "Unexpected reply from the device (files)")),
    }
}

/// Whole stored file as text. HTTP returns it raw; USB pages base64 `files/get`.
async fn read_file(w: &dyn ScriptWire, name: &str) -> R<Vec<u8>> {
    match w.kind() {
        Kind::Http => {
            let raw = rpc(
                w,
                Route::get(tunnel::Op::Get, "/api/scripts/files/get").query("name", name),
            )
            .await?;
            if (200..300).contains(&raw.status) {
                return Ok(raw.body);
            }
            let obj = object(&raw);
            Err(failure(&raw, &obj, false).unwrap_or_else(|| OpErr::new("firmware", "request failed")))
        }
        Kind::Usb => {
            let mut out: Vec<u8> = Vec::new();
            let mut size = usize::MAX;
            for _ in 0..(BODY_MAX / 256 + 2) {
                if out.len() >= size {
                    break;
                }
                let o = call(
                    w,
                    Route::get(tunnel::Op::Get, "/api/scripts/files/get")
                        .usb(json!({"name": name, "off": out.len(), "len": FILE_PAGE})),
                    false,
                )
                .await?;
                let total = o.get("size").and_then(Value::as_u64);
                let chunk = o.get("data").and_then(Value::as_str).and_then(|b| B64.decode(b).ok());
                let (Some(total), Some(chunk)) = (total, chunk) else {
                    return Err(OpErr::new("malformed", "Unexpected reply from the device (files/get page)"));
                };
                size = total as usize;
                if chunk.is_empty() {
                    break;
                }
                out.extend_from_slice(&chunk);
            }
            // A truncated file in the editor would be saved back truncated.
            if out.len() != size {
                return Err(OpErr::new(
                    "malformed",
                    format!("files/get stopped at {} of {} bytes", out.len(), size),
                ));
            }
            Ok(out)
        }
    }
}

async fn op_get(w: &dyn ScriptWire, args: &Value) -> R<Value> {
    let name = need_name(args)?;
    let bytes = read_file(w, &name).await?;
    let lossy = std::str::from_utf8(&bytes).is_err();
    Ok(json!({
        "ok": true, "name": name, "size": bytes.len(),
        "source": String::from_utf8_lossy(&bytes), "lossy": lossy,
    }))
}

async fn save_usb(w: &dyn ScriptWire, name: &str, data: &[u8], progress: Progress<'_>) -> R<()> {
    let count = data.chunks(USB_SAVE_CHUNK).count();
    let mut sent = 0usize;
    for (i, chunk) in data.chunks(USB_SAVE_CHUNK).enumerate() {
        let body = json!({"name": name, "off": sent, "b64": B64.encode(chunk), "final": i + 1 == count});
        let o = call(w, Route::post(tunnel::Op::Chunk, "/api/scripts/files/chunk").usb(body), false).await?;
        let want = (sent + chunk.len()) as u64;
        if o.get("received").and_then(Value::as_u64) != Some(want) {
            return Err(OpErr::new("malformed", format!("chunk ack missing or not {want}")));
        }
        sent += chunk.len();
        progress(name, sent, data.len());
    }
    Ok(())
}

async fn op_save(w: &dyn ScriptWire, args: &Value, progress: Progress<'_>) -> R<Value> {
    let name = need_name(args)?;
    let source = arg_str(args, "source").unwrap_or("");
    check_source(source, false)?;
    match w.kind() {
        Kind::Http => {
            let r = Route::post(tunnel::Op::Chunk, "/api/scripts/files").query("name", &name).text(source);
            call(w, r, false).await?;
            progress(&name, source.len(), source.len());
        }
        Kind::Usb => {
            // A lost reply leaves the device at an unknown offset; off=0 truncates the
            // temp file, so one restart is always safe.
            match save_usb(w, &name, source.as_bytes(), progress).await {
                Err(e) if e.kind == "transport" => save_usb(w, &name, source.as_bytes(), progress).await?,
                other => other?,
            }
        }
    }
    Ok(json!({"ok": true, "name": name, "bytes": source.len()}))
}

async fn op_delete(w: &dyn ScriptWire, args: &Value) -> R<Value> {
    let name = need_name(args)?;
    let r = Route::post(tunnel::Op::Delete, "/api/scripts/files/delete")
        .query("name", &name)
        .usb(json!({"name": name}));
    call(w, r, false).await?;
    Ok(json!({"ok": true, "name": name}))
}

async fn op_lint(w: &dyn ScriptWire, args: &Value) -> R<Value> {
    let source = arg_str(args, "source");
    let name = match (source, arg_str(args, "name")) {
        (None, Some(_)) => Some(need_name(args)?),
        (None, None) => return Err(OpErr::new("invalid", "source or name required")),
        _ => None,
    };
    if let Some(s) = source {
        check_source(s, true)?;
    }
    let route = match (w.kind(), source, &name) {
        (Kind::Usb, Some(s), _) => Route::post(tunnel::Op::Lint, "/api/scripts/lint").usb(json!({"src": s})),
        (Kind::Usb, None, Some(n)) => Route::post(tunnel::Op::Lint, "/api/scripts/lint").usb(json!({"name": n})),
        (Kind::Http, Some(s), _) => Route::post(tunnel::Op::Lint, "/api/scripts/lint").text(s),
        (Kind::Http, None, Some(n)) => {
            // The REST route lints a request body only: compile the stored text, not a copy.
            let bytes = read_file(w, n).await?;
            let text = String::from_utf8_lossy(&bytes).into_owned();
            Route::post(tunnel::Op::Lint, "/api/scripts/lint").text(&text)
        }
        _ => return Err(OpErr::new("invalid", "source or name required")),
    }
    .timeout(LINT_TIMEOUT);
    let o = call(w, route, true).await?;
    match o.get("ok").and_then(Value::as_bool) {
        Some(true) => Ok(json!({"ok": true, "valid": true})),
        Some(false) => {
            let msg = error_text(&o).unwrap_or_default();
            match lint_unavailable_kind(&msg) {
                Some(kind) => Err(OpErr::new(kind, msg)),
                None => Ok(json!({"ok": true, "valid": false, "message": msg})),
            }
        }
        None => Err(OpErr::new("malformed", "Unexpected reply from the device (lint)")),
    }
}

async fn op_run(w: &dyn ScriptWire, args: &Value) -> R<Value> {
    let name = need_name(args)?;
    let background = arg_bool(args, "background", false);
    let replace = arg_bool(args, "replace", false);
    let mut r = Route::post(tunnel::Op::Run, "/api/scripts/run-file")
        .query("name", &name)
        .usb(json!({"name": name, "background": background, "replace": replace}))
        .timeout(LINT_TIMEOUT);
    if background {
        r = r.query("background", 1);
    }
    if replace {
        r = r.query("replace", 1);
    }
    let o = call(w, r, false).await?;
    let id = o.get("id").and_then(Value::as_u64).ok_or_else(|| OpErr::new("malformed", "run-file reply has no id"))?;
    Ok(json!({
        "ok": true, "id": id,
        "name": o.get("name").and_then(Value::as_str).unwrap_or(&name),
        "background": o.get("background").and_then(Value::as_bool).unwrap_or(background),
    }))
}

async fn op_eval(w: &dyn ScriptWire, args: &Value) -> R<Value> {
    let source = arg_str(args, "source").unwrap_or("");
    check_source(source, false)?;
    let persist = arg_bool(args, "persist", true);
    let mut r = Route::post(tunnel::Op::Eval, "/api/scripts/eval")
        .usb(json!({"src": source, "persist": persist}))
        .text(source);
    if persist {
        r = r.query("persist", 1);
    }
    let o = call(w, r, false).await?;
    let id = o.get("id").and_then(Value::as_u64).ok_or_else(|| OpErr::new("malformed", "eval reply has no id"))?;
    Ok(json!({"ok": true, "id": id}))
}

async fn op_logs(w: &dyn ScriptWire, args: &Value) -> R<Value> {
    let since = args.get("since").and_then(Value::as_u64).unwrap_or(0);
    let r = Route::get(tunnel::Op::Logs, "/api/scripts/logs")
        .query("since", since)
        .usb(json!({"since": since.to_string()}));
    match w.kind() {
        Kind::Http => {
            let raw = rpc(w, r).await?;
            if !(200..300).contains(&raw.status) {
                let obj = object(&raw);
                return Err(failure(&raw, &obj, false).unwrap_or_else(|| OpErr::new("firmware", "request failed")));
            }
            let next = raw.headers.get("x-bugbuster-log-next").and_then(|v| v.parse::<u64>().ok()).ok_or_else(|| {
                OpErr::new("unsupported", "logs: no X-BugBuster-Log-Next header (firmware predates log cursors): update the firmware")
            })?;
            let n = raw.body.len() as u64;
            let start = next.saturating_sub(n);
            Ok(log_page(&raw.body, since, next, start.saturating_sub(since)))
        }
        Kind::Usb => {
            let o = call(w, r, false).await?;
            let next = o.get("next").and_then(Value::as_u64);
            let data = o.get("data").and_then(Value::as_str).and_then(|b| B64.decode(b).ok());
            let (Some(next), Some(data)) = (next, data) else {
                return Err(OpErr::new("malformed", "Unexpected reply from the device (logs page)"));
            };
            let dropped = o.get("dropped").and_then(Value::as_u64).unwrap_or(0);
            Ok(log_page(&data, since, next, dropped))
        }
    }
}

fn log_page(data: &[u8], since: u64, next: u64, dropped: u64) -> Value {
    json!({
        "ok": true, "data": B64.encode(data), "n": data.len(), "since": since,
        "next": next, "dropped": dropped, "more": data.len() >= LOG_PAGE,
        // The firmware clamps a cursor beyond its counter: the device rebooted or the ring reset.
        "restarted": next < since,
    })
}

async fn simple_post(w: &dyn ScriptWire, op: tunnel::Op, path: &'static str, usb: Value, q: Option<(&str, &str)>) -> R<Value> {
    let mut r = Route::post(op, path).usb(usb);
    if let Some((k, v)) = q {
        r = r.query(k, v);
    }
    call(w, r, false).await?;
    Ok(json!({"ok": true}))
}

async fn op_autorun_run(w: &dyn ScriptWire) -> R<Value> {
    if w.kind() == Kind::Http {
        return Err(OpErr::new(
            "unsupported",
            "Run now needs a USB connection: the device has no Wi-Fi route for it",
        ));
    }
    let o = call(w, Route::post(tunnel::Op::AutorunRun, "/api/scripts/autorun/run").timeout(Duration::from_secs(40)), false).await?;
    Ok(with_ok(o))
}

/// Operations whose outcome is unknown after a lost reply and must never be retried.
fn outcome_unknown_on_transport_error(operation: &str) -> bool {
    matches!(
        operation,
        "run" | "eval" | "reset" | "stop" | "delete" | "autorun_run" | "autorun_enable" | "autorun_disable"
    )
}

pub const OPERATIONS: [&str; 16] = [
    "files", "get", "save", "delete", "lint", "run", "stop", "status", "logs", "eval", "reset",
    "autorun_status", "autorun_enable", "autorun_disable", "autorun_run", "storage",
];

/// Run one scripting operation. Unknown operations are the caller's bug (`Err`); every device
/// or validation failure is an `ok:false` object with a `kind`.
pub async fn execute(w: &dyn ScriptWire, operation: &str, args: &Value, progress: Progress<'_>) -> Result<Value, String> {
    let result = match operation {
        "files" => op_files(w).await,
        "storage" => passthrough(w, tunnel::Op::Storage, "/api/scripts/storage").await,
        "status" => passthrough(w, tunnel::Op::Status, "/api/scripts/status").await,
        "autorun_status" => passthrough(w, tunnel::Op::AutorunStatus, "/api/scripts/autorun/status").await,
        "get" => op_get(w, args).await,
        "save" => op_save(w, args, progress).await,
        "delete" => op_delete(w, args).await,
        "lint" => op_lint(w, args).await,
        "run" => op_run(w, args).await,
        "eval" => op_eval(w, args).await,
        "logs" => op_logs(w, args).await,
        "stop" => simple_post(w, tunnel::Op::Stop, "/api/scripts/stop", json!({}), None).await,
        "reset" => simple_post(w, tunnel::Op::Reset, "/api/scripts/reset", json!({}), None).await,
        "autorun_disable" => {
            simple_post(w, tunnel::Op::AutorunDisable, "/api/scripts/autorun/disable", json!({}), None).await
        }
        "autorun_enable" => match need_name(args) {
            Ok(n) => {
                simple_post(w, tunnel::Op::AutorunEnable, "/api/scripts/autorun/enable", json!({"name": n}), Some(("name", n.as_str()))).await
            }
            Err(e) => Err(e),
        },
        "autorun_run" => op_autorun_run(w).await,
        other => return Err(format!("unknown script operation '{other}'")),
    };
    Ok(match result {
        Ok(v) => v,
        Err(mut e) => {
            if e.kind == "transport" && outcome_unknown_on_transport_error(operation) {
                e = e.with("outcomeUnknown", json!(true));
            }
            e.into_value(operation)
        }
    })
}

// -----------------------------------------------------------------------------
// Tauri command
// -----------------------------------------------------------------------------

/// Only one launch-type request (run, eval, reset, run-now) may be in flight, so a double
/// click or a UI retry can never start a script twice.
static LAUNCH_IN_FLIGHT: AtomicBool = AtomicBool::new(false);
static SCRIPT_REQUEST_LOCK: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());

struct LaunchGuard;

impl LaunchGuard {
    fn acquire() -> Option<Self> {
        LAUNCH_IN_FLIGHT
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .ok()
            .map(|_| LaunchGuard)
    }
}

impl Drop for LaunchGuard {
    fn drop(&mut self) {
        LAUNCH_IN_FLIGHT.store(false, Ordering::Release);
    }
}

#[cfg(test)]
pub(crate) fn launch_guard_for_test() -> Option<impl Drop> {
    LaunchGuard::acquire()
}

fn is_launch(operation: &str) -> bool {
    matches!(operation, "run" | "eval" | "reset" | "autorun_run")
}

fn identity(mgr: &ConnectionManager) -> Value {
    let s = mgr.get_connection_status();
    let transport = match s.mode {
        ConnectionMode::Usb => "usb",
        ConnectionMode::Http => "http",
        ConnectionMode::Disconnected => "none",
    };
    json!({
        "transport": transport,
        "address": s.port_or_url,
        "mac": s.device_info.and_then(|d| d.mac_address),
    })
}

/// `script_request(operation, args)`: every scripting action over the selected transport.
#[tauri::command]
pub async fn script_request(
    app: AppHandle,
    mgr: State<'_, ConnectionManager>,
    operation: String,
    args: Option<Value>,
) -> Result<Value, String> {
    let args = args.unwrap_or(Value::Null);
    if !OPERATIONS.contains(&operation.as_str()) {
        return Err(format!("unknown script operation '{operation}'"));
    }
    let epoch = mgr.connection_epoch();
    let before = identity(&mgr);
    if before["transport"] == "none" {
        return Ok(OpErr::new("not_connected", "No device connected").into_value(&operation));
    }
    let _guard = if is_launch(&operation) {
        match LaunchGuard::acquire() {
            Some(g) => Some(g),
            None => {
                return Ok(OpErr::new("in_flight", "Another run or evaluation request is still in progress")
                    .into_value(&operation));
            }
        }
    } else {
        None
    };
    let _request = match tokio::time::timeout(Duration::from_secs(15), SCRIPT_REQUEST_LOCK.lock()).await {
        Ok(guard) => guard,
        Err(_) => return Ok(OpErr::new("in_flight", "The scripting operation queue is busy")
            .into_value(&operation)),
    };
    if mgr.connection_epoch() != epoch {
        return Ok(OpErr::new("device_changed", "The connection changed before the request started")
            .into_value(&operation));
    }
    let progress = |name: &str, sent: usize, total: usize| {
        let _ = app.emit("script-upload-progress", json!({"name": name, "sent": sent, "total": total}));
    };
    let wire = LiveWire {
        mgr: &mgr,
        epoch,
        transport_kind: if before["transport"] == "http" { Kind::Http } else { Kind::Usb },
    };
    let mut out = execute(&wire, &operation, &args, &progress).await?;
    // A reply that raced a device switch must not be attributed to the new device.
    let after = identity(&mgr);
    if after != before || mgr.connection_epoch() != epoch {
        return Ok(OpErr::new("device_changed", "The connection changed while the request was running")
            .into_value(&operation));
    }
    if let Value::Object(o) = &mut out {
        o.insert("operation".into(), json!(operation));
        o.insert("device".into(), before);
    }
    Ok(out)
}

#[cfg(test)]
mod model_tests {
    use super::*;

    #[tokio::test]
    async fn staged_requests_have_one_exclusive_owner() {
        let guard = SCRIPT_REQUEST_LOCK.lock().await;
        assert!(SCRIPT_REQUEST_LOCK.try_lock().is_err());
        drop(guard);
        assert!(SCRIPT_REQUEST_LOCK.try_lock().is_ok());
    }

    #[tokio::test]
    async fn stale_frames_are_rejected_before_touching_the_selected_link() {
        let mgr = ConnectionManager::new();
        let stale_epoch = mgr.connection_epoch().wrapping_add(1);
        let result = mgr.script_command(stale_epoch, bbp::CMD_SCRIPT_AUTORUN, &[6]).await;
        assert_eq!(result.unwrap_err().to_string(), "Scripting connection changed");
        let result = mgr.script_http_exchange(stale_epoch, HttpExchange {
            method: "GET".into(),
            path: "/api/scripts/status".into(),
            query: vec![],
            body: None,
            timeout: Duration::from_secs(1),
        }).await;
        assert_eq!(result.err().unwrap().to_string(), "Scripting connection changed");
    }

    #[test]
    fn names_follow_script_storage_rules() {
        assert!(valid_name("a.py"));
        assert!(valid_name("My-script_1.v2.py"));
        assert!(valid_name(&format!("{}.py", "x".repeat(29))));
        assert!(!valid_name(&format!("{}.py", "x".repeat(30))));
        for bad in [".hidden.py", "a.txt", "a b.py", "a/b.py", "..py/", "", "py", "é.py"] {
            assert!(!valid_name(bad), "{bad}");
        }
    }

    #[test]
    fn call_frame_is_little_endian_and_golden() {
        let f = tunnel::encode_call(tunnel::Op::Eval, 0x0102, 0x0304, b"ab");
        assert_eq!(f, [6, 10, 0x02, 0x01, 0x04, 0x03, b'a', b'b']);
        assert_eq!(tunnel::encode_fetch(9, 0x0A0B), [7, 9, 0x0B, 0x0A]);
    }

    #[test]
    fn reply_decoding_checks_done_length() {
        let done = [1, 3, 5, 0, 2, 0, b'{', b'}'];
        let r = tunnel::decode_reply(&done).unwrap();
        assert_eq!((r.state, r.token, r.a, r.b), (1, 3, 5, 2));
        assert_eq!(r.data, b"{}");
        assert!(tunnel::decode_reply(&[1, 3, 5, 0, 3, 0, b'{', b'}']).is_none());
        assert!(tunnel::decode_reply(&[1, 3]).is_none());
        let more = tunnel::decode_reply(&[0, 0, 0x10, 0, 0, 0]).unwrap();
        assert_eq!((more.state, more.a), (tunnel::ST_MORE, 16));
    }

    #[test]
    fn tunnel_status_mirrors_webserver() {
        assert_eq!(tunnel_status(br#"{"ok":false,"error":"x","running":"a.py","id":3}"#), 409);
        assert_eq!(tunnel_status(br#"{"ok":false,"error":"x"}"#), 400);
        assert_eq!(tunnel_status(br#"{"ok":true}"#), 200);
        assert_eq!(tunnel_status(b"not json"), 200);
    }

    #[test]
    fn lint_refusals_are_not_syntax_errors() {
        assert_eq!(lint_unavailable_kind("Interpreter is busy running a script"), Some("busy"));
        assert_eq!(lint_unavailable_kind("Interpreter timed out (5s)"), Some("unavailable"));
        assert_eq!(lint_unavailable_kind("Traceback:\n  SyntaxError: invalid syntax"), None);
    }

    #[test]
    fn unsupported_sub_is_detected_from_device_error_text() {
        let m = "invalid parameter (error 0x03, cmd 0xFD)";
        assert_eq!(bbp_error_code(m), Some(3));
        assert!(is_unsupported_sub(m));
        assert!(!is_unsupported_sub("busy (error 0x06, cmd 0xFD)"));
        assert!(!is_unsupported_sub("Command timeout (cmd=0xFD)"));
    }

    #[test]
    fn op_error_serialises_kind_and_extras() {
        let v = OpErr::new("busy", "'a.py' is running")
            .with("running", json!("a.py"))
            .with("id", json!(4))
            .into_value("run");
        assert_eq!(v["ok"], false);
        assert_eq!(v["kind"], "busy");
        assert_eq!(v["running"], "a.py");
        assert_eq!(v["operation"], "run");
    }
}
