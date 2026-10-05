// =============================================================================
// scripts_repl.rs - authenticated WebSocket REPL over the selected Wi-Fi link.
//
// `script_repl_request(operation, args)`: open / send_line / interrupt / close.
// The URL and the admin token always come from the selected HTTP connection; the
// frontend never supplies (or receives) a host or token. Progress is reported by
// `script-repl` events tagged with the caller's session id and the connection
// epoch. USB keeps using persistent `eval` through `script_request`.
//
// Firmware contract (`mp/repl_ws.cpp`): token in the first text frame, banner =
// authenticated, close 4001 = auth failed, 4002 = another session holds the slot.
// Input is line-buffered (512 bytes) and every CR submits one line, so multi-line
// input cannot travel over this socket; the frontend runs it as an explicit eval.
// =============================================================================

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

use futures::{SinkExt, StreamExt};
use serde_json::{json, Map, Value};
use tauri::{AppHandle, Emitter, State};
use tokio::sync::{mpsc, Mutex};
use tokio::task::JoinHandle;
use tokio::time::{timeout, MissedTickBehavior};
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::protocol::frame::coding::CloseCode;
use tokio_tungstenite::tungstenite::protocol::{CloseFrame, WebSocketConfig};
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::connect_async_with_config;

use crate::connection_manager::ConnectionManager;
use crate::scripts::OpErr;
use crate::state::ConnectionMode;

pub const WS_PATH: &str = "/api/scripts/repl/ws";
pub const CLOSE_UNAUTHORIZED: u16 = 4001;
pub const CLOSE_BUSY: u16 = 4002;
/// Firmware `REPL_LINE_BUF_SIZE` (512) minus the terminator.
pub const LINE_MAX: usize = 511;
/// Bytes buffered between flushes; older output is dropped first.
pub const RING_MAX: usize = 64 * 1024;
/// Bytes per `output` event.
pub const OUTPUT_CHUNK: usize = 8 * 1024;
const MESSAGE_MAX: usize = 64 * 1024;
const FRAME_MAX: usize = 16 * 1024;
const CONTROL_QUEUE: usize = 16;
const CLOSE_GRACE: Duration = Duration::from_millis(1500);
const EVENT: &str = "script-repl";
const OPERATIONS: [&str; 4] = ["open", "send_line", "interrupt", "close"];

#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub connect: Duration,
    /// From the token to the banner (the firmware drops unauthenticated sockets after 10 s).
    pub banner: Duration,
    pub send: Duration,
    pub flush: Duration,
    /// The link guard runs every this many flush ticks.
    pub link_every: u32,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            connect: Duration::from_secs(8),
            banner: Duration::from_secs(12),
            send: Duration::from_secs(5),
            flush: Duration::from_millis(25),
            link_every: 10,
        }
    }
}

// -----------------------------------------------------------------------------
// Pure helpers
// -----------------------------------------------------------------------------

/// `ws://host[:port]/api/scripts/repl/ws` from the selected connection's base URL.
pub fn ws_url(base: &str) -> Result<String, String> {
    let b = base.trim();
    let lower = b.to_ascii_lowercase();
    let host = if lower.starts_with("http://") {
        b[7..].trim_end_matches('/')
    } else if lower.starts_with("https://") {
        return Err("The device WebSocket REPL is plain ws:// only; https is not supported".into());
    } else if b.contains("://") {
        return Err("The selected connection is not an HTTP address".into());
    } else {
        b.trim_end_matches('/')
    };
    let ok = !host.is_empty()
        && !host.ends_with(':')
        && host.len() <= 255
        && host
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'-' | b':' | b'[' | b']'));
    if !ok {
        return Err("The selected connection address is not a plain host[:port]".into());
    }
    Ok(format!("ws://{host}{WS_PATH}"))
}

/// One REPL line for the firmware's line buffer: printable ASCII and tabs only. Anything else
/// (multi-line text, non-ASCII, too long) must go through an explicit eval instead.
pub fn validate_line(line: &str) -> Result<(), String> {
    if line.is_empty() {
        return Err("Nothing to send".into());
    }
    if line.len() > LINE_MAX {
        return Err(format!("A REPL line is limited to {LINE_MAX} bytes by the firmware"));
    }
    if line.contains('\n') || line.contains('\r') {
        return Err("The WebSocket REPL takes one line at a time".into());
    }
    if !line.bytes().all(|b| b == b'\t' || (0x20..0x7F).contains(&b)) {
        return Err("The WebSocket REPL takes printable ASCII only".into());
    }
    Ok(())
}

/// Token-free, bounded text for events and replies.
pub fn scrub(msg: &str, token: &str) -> String {
    let mut out = if token.is_empty() { msg.to_string() } else { msg.replace(token, "***") };
    if out.len() > 240 {
        let mut cut = 240;
        while !out.is_char_boundary(cut) {
            cut -= 1;
        }
        out.truncate(cut);
    }
    out
}

/// Bounded text buffer between the socket and the event stream; the oldest bytes go first.
#[derive(Debug, Default)]
pub struct OutputRing {
    buf: String,
    cap: usize,
    dropped: u64,
}

impl OutputRing {
    pub fn with_capacity(cap: usize) -> Self {
        Self { buf: String::new(), cap, dropped: 0 }
    }

    pub fn push(&mut self, s: &str) {
        self.buf.push_str(s);
        if self.buf.len() > self.cap {
            let mut cut = self.buf.len() - self.cap;
            while !self.buf.is_char_boundary(cut) {
                cut += 1;
            }
            self.dropped += cut as u64;
            self.buf.drain(..cut);
        }
    }

    /// Up to `max` bytes (whole characters) plus the bytes dropped since the last take.
    pub fn take(&mut self, max: usize) -> Option<(String, u64)> {
        if self.buf.is_empty() {
            return None;
        }
        let mut n = self.buf.len().min(max);
        while !self.buf.is_char_boundary(n) {
            n -= 1;
        }
        if n == 0 {
            n = self.buf.chars().next().map(char::len_utf8).unwrap_or(0);
        }
        let chunk: String = self.buf.drain(..n).collect();
        Some((chunk, std::mem::take(&mut self.dropped)))
    }
}

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

pub trait EventSink: Send + Sync {
    fn emit(&self, event: Value);
}

struct AppSink(AppHandle);

impl EventSink for AppSink {
    fn emit(&self, event: Value) {
        let _ = self.0.emit(EVENT, event);
    }
}

/// Returns false when the selected connection is no longer the one the session opened on.
pub type LinkCheck = Arc<dyn Fn() -> bool + Send + Sync>;

struct SessionEvents {
    sink: Arc<dyn EventSink>,
    session: u64,
    generation: u64,
    device: Value,
    seq: u64,
}

impl SessionEvents {
    fn emit(&mut self, kind: &str, fields: Value) {
        self.seq += 1;
        let mut m = match fields {
            Value::Object(m) => m,
            _ => Map::new(),
        };
        m.insert("kind".into(), json!(kind));
        m.insert("session".into(), json!(self.session));
        m.insert("generation".into(), json!(self.generation));
        m.insert("device".into(), self.device.clone());
        m.insert("seq".into(), json!(self.seq));
        self.sink.emit(Value::Object(m));
    }

    fn state(&mut self, state: &str) {
        self.emit("state", json!({ "state": state }));
    }

    fn flush(&mut self, ring: &mut OutputRing) {
        while let Some((data, dropped)) = ring.take(OUTPUT_CHUNK) {
            self.emit("output", json!({ "data": data, "dropped": dropped }));
        }
    }

    fn closed(&mut self, c: &Closed) {
        self.emit(
            "closed",
            json!({ "reason": c.reason, "code": c.code, "message": c.message }),
        );
    }
}

#[derive(Debug, Clone, PartialEq)]
struct Closed {
    reason: &'static str,
    code: Option<u16>,
    message: String,
}

impl Closed {
    fn new(reason: &'static str, message: impl Into<String>) -> Self {
        Self { reason, code: None, message: message.into() }
    }

    fn from_close(code: Option<u16>) -> Self {
        match code {
            Some(CLOSE_UNAUTHORIZED) => Self {
                reason: "unauthorized",
                code,
                message: "The device rejected the saved admin token (close 4001)".into(),
            },
            Some(CLOSE_BUSY) => Self {
                reason: "busy",
                code,
                message: "Another client already holds the device REPL (close 4002)".into(),
            },
            _ => Self { reason: "remote", code, message: "The device closed the REPL".into() },
        }
    }
}

enum Control {
    Line(String),
    Interrupt,
    Close,
}

fn close_code(frame: &Option<CloseFrame>) -> Option<u16> {
    frame.as_ref().map(|f| u16::from(f.code))
}

// -----------------------------------------------------------------------------
// Session pump
// -----------------------------------------------------------------------------

async fn pump(
    url: &str,
    token: &str,
    lim: Limits,
    em: &mut SessionEvents,
    rx: &mut mpsc::Receiver<Control>,
    link_ok: &LinkCheck,
) -> Closed {
    em.state("connecting");
    let req = match url.into_client_request() {
        Ok(r) => r,
        Err(e) => return Closed::new("connect_failed", scrub(&e.to_string(), token)),
    };
    let cfg = WebSocketConfig::default()
        .max_message_size(Some(MESSAGE_MAX))
        .max_frame_size(Some(FRAME_MAX));
    let mut ws = match timeout(lim.connect, connect_async_with_config(req, Some(cfg), true)).await {
        Err(_) => return Closed::new("connect_failed", "Timed out connecting to the device"),
        Ok(Err(e)) => {
            return Closed::new(
                "connect_failed",
                format!(
                    "Could not open the REPL socket (the device may already have another REPL session): {}",
                    scrub(&e.to_string(), token)
                ),
            )
        }
        Ok(Ok((ws, _))) => ws,
    };

    // The token travels only as the first frame.
    match timeout(lim.send, ws.send(Message::text(token.to_string()))).await {
        Ok(Ok(())) => {}
        Ok(Err(e)) => return Closed::new("send_failed", scrub(&e.to_string(), token)),
        Err(_) => return Closed::new("send_failed", "Timed out sending the authentication frame"),
    }
    em.state("authenticating");

    let mut ring = OutputRing::with_capacity(RING_MAX);
    let mut connected = false;
    let banner_deadline = tokio::time::sleep(lim.banner);
    tokio::pin!(banner_deadline);
    let mut tick = tokio::time::interval(lim.flush);
    tick.set_missed_tick_behavior(MissedTickBehavior::Delay);
    let mut ticks: u32 = 0;

    loop {
        tokio::select! {
            ctl = rx.recv() => {
                let text = match ctl {
                    None | Some(Control::Close) => {
                        let _ = timeout(
                            lim.send,
                            ws.close(Some(CloseFrame { code: CloseCode::Normal, reason: "".into() })),
                        ).await;
                        em.flush(&mut ring);
                        return Closed::new("local", "REPL closed");
                    }
                    Some(Control::Line(l)) => format!("{l}\r"),
                    Some(Control::Interrupt) => "\u{3}".to_string(),
                };
                if !connected {
                    em.emit("rejected", json!({ "reason": "not_connected" }));
                    continue;
                }
                match timeout(lim.send, ws.send(Message::text(text))).await {
                    Ok(Ok(())) => {}
                    Ok(Err(e)) => return Closed::new("send_failed", scrub(&e.to_string(), token)),
                    Err(_) => return Closed::new("send_failed", "Timed out sending to the device"),
                }
            }
            msg = ws.next() => {
                match msg {
                    None => {
                        em.flush(&mut ring);
                        return Closed::new("remote", "The device ended the connection");
                    }
                    Some(Err(e)) => {
                        em.flush(&mut ring);
                        use tokio_tungstenite::tungstenite::Error as E;
                        return match e {
                            E::ConnectionClosed | E::AlreadyClosed => Closed::new("remote", "The device ended the connection"),
                            other => Closed::new("protocol", scrub(&other.to_string(), token)),
                        };
                    }
                    Some(Ok(Message::Text(t))) => {
                        ring.push(t.as_str());
                        if !connected {
                            connected = true;
                            em.state("connected");
                        }
                    }
                    Some(Ok(Message::Close(frame))) => {
                        em.flush(&mut ring);
                        return Closed::from_close(close_code(&frame));
                    }
                    Some(Ok(_)) => {}
                }
            }
            _ = tick.tick() => {
                em.flush(&mut ring);
                ticks = ticks.wrapping_add(1);
                if lim.link_every != 0 && ticks % lim.link_every == 0 && !link_ok() {
                    return Closed::new("link_changed", "The device connection changed");
                }
            }
            _ = &mut banner_deadline, if !connected => {
                return Closed::new("timeout", "The device did not answer the authentication frame");
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Session registry
// -----------------------------------------------------------------------------

pub struct OpenSpec {
    pub session: u64,
    pub generation: u64,
    pub device: Value,
    pub url: String,
    pub token: String,
}

impl std::fmt::Debug for OpenSpec {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("OpenSpec")
            .field("session", &self.session)
            .field("generation", &self.generation)
            .field("url", &self.url)
            .field("token", &"<redacted>")
            .finish()
    }
}

struct Active {
    session: u64,
    tx: mpsc::Sender<Control>,
    task: JoinHandle<()>,
}

#[derive(Default)]
struct HubInner {
    last: AtomicU64,
    active: Mutex<Option<Active>>,
}

/// At most one REPL socket (the firmware allows one session); session ids only grow.
#[derive(Clone, Default)]
pub struct ReplHub {
    inner: Arc<HubInner>,
}

impl ReplHub {
    pub async fn open(
        &self,
        spec: OpenSpec,
        sink: Arc<dyn EventSink>,
        link_ok: LinkCheck,
        lim: Limits,
    ) -> Result<(), OpErr> {
        if spec.session == 0 {
            return Err(OpErr::new("invalid", "session must be positive"));
        }
        let prev = self.inner.last.fetch_max(spec.session, Ordering::AcqRel);
        if spec.session <= prev {
            return Err(OpErr::new("stale", "A newer REPL session was already requested"));
        }
        let mut slot = self.inner.active.lock().await;
        if let Some(old) = slot.take() {
            shutdown(old).await;
        }
        let (tx, mut rx) = mpsc::channel(CONTROL_QUEUE);
        let mut em = SessionEvents {
            sink,
            session: spec.session,
            generation: spec.generation,
            device: spec.device,
            seq: 0,
        };
        let OpenSpec { url, token, session, .. } = spec;
        let task = tokio::spawn(async move {
            let closed = pump(&url, &token, lim, &mut em, &mut rx, &link_ok).await;
            em.closed(&closed);
        });
        *slot = Some(Active { session, tx, task });
        Ok(())
    }

    async fn send(&self, session: u64, ctl: Control) -> Result<(), OpErr> {
        let slot = self.inner.active.lock().await;
        match slot.as_ref() {
            Some(a) if a.session == session && !a.tx.is_closed() => a
                .tx
                .try_send(ctl)
                .map_err(|_| OpErr::new("queue_full", "The REPL is busy sending; try again")),
            Some(a) if a.session == session => {
                Err(OpErr::new("closed", "The REPL session is closed; reconnect"))
            }
            _ => Err(OpErr::new("stale", "That REPL session is no longer current")),
        }
    }

    pub async fn send_line(&self, session: u64, line: &str) -> Result<(), OpErr> {
        validate_line(line).map_err(|m| OpErr::new("invalid", m))?;
        self.send(session, Control::Line(line.to_string())).await
    }

    pub async fn interrupt(&self, session: u64) -> Result<(), OpErr> {
        self.send(session, Control::Interrupt).await
    }

    /// Idempotent: closing a session that is already gone (or superseded) succeeds.
    pub async fn close(&self, session: u64) {
        let mut slot = self.inner.active.lock().await;
        if slot.as_ref().is_some_and(|a| a.session == session) {
            if let Some(a) = slot.take() {
                shutdown(a).await;
            }
        }
    }
}

async fn shutdown(a: Active) {
    let _ = a.tx.try_send(Control::Close);
    let Active { mut task, .. } = a;
    if timeout(CLOSE_GRACE, &mut task).await.is_err() {
        task.abort();
    }
}

// -----------------------------------------------------------------------------
// Tauri command
// -----------------------------------------------------------------------------

fn args_object<'a>(args: &'a Value, allowed: &[&str]) -> Result<&'a Map<String, Value>, OpErr> {
    let m = args
        .as_object()
        .ok_or_else(|| OpErr::new("invalid", "args must be an object"))?;
    if let Some(k) = m.keys().find(|k| !allowed.contains(&k.as_str())) {
        return Err(OpErr::new("invalid", format!("unknown argument '{k}'")));
    }
    Ok(m)
}

fn arg_session(m: &Map<String, Value>) -> Result<u64, OpErr> {
    m.get("session")
        .and_then(Value::as_u64)
        .filter(|s| *s > 0)
        .ok_or_else(|| OpErr::new("invalid", "session must be a positive integer"))
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

async fn handle(
    app: &AppHandle,
    mgr: &ConnectionManager,
    hub: &ReplHub,
    operation: &str,
    args: &Value,
) -> Result<Value, OpErr> {
    match operation {
        "open" => {
            let session = arg_session(args_object(args, &["session"])?)?;
            let epoch = mgr.connection_epoch();
            let status = mgr.get_connection_status();
            let device = identity(mgr);
            match status.mode {
                ConnectionMode::Disconnected => {
                    return Err(OpErr::new("not_connected", "No device connected"))
                }
                ConnectionMode::Usb => {
                    return Err(OpErr::new(
                        "unsupported",
                        "The WebSocket REPL needs a Wi-Fi connection; USB uses persistent eval",
                    ))
                }
                ConnectionMode::Http => {}
            }
            let token = status
                .admin_token
                .clone()
                .filter(|t| !t.is_empty())
                .ok_or_else(|| OpErr::new("no_token", "No saved admin token for this device; pair it over USB"))?;
            let url = ws_url(&status.port_or_url).map_err(|m| OpErr::new("unsupported", m))?;
            let addr = status.port_or_url.clone();
            let m = mgr.clone();
            let link_ok: LinkCheck = Arc::new(move || {
                m.connection_epoch() == epoch && {
                    let s = m.get_connection_status();
                    matches!(s.mode, ConnectionMode::Http) && s.port_or_url == addr
                }
            });
            hub.open(
                OpenSpec { session, generation: epoch, device: device.clone(), url, token },
                Arc::new(AppSink(app.clone())),
                link_ok,
                Limits::default(),
            )
            .await?;
            Ok(json!({ "session": session, "generation": epoch, "device": device }))
        }
        "send_line" => {
            let m = args_object(args, &["session", "line"])?;
            let session = arg_session(m)?;
            let line = m
                .get("line")
                .and_then(Value::as_str)
                .ok_or_else(|| OpErr::new("invalid", "line must be a string"))?;
            guard_link(mgr, hub, session).await?;
            hub.send_line(session, line).await?;
            Ok(json!({ "session": session }))
        }
        "interrupt" => {
            let session = arg_session(args_object(args, &["session"])?)?;
            guard_link(mgr, hub, session).await?;
            hub.interrupt(session).await?;
            Ok(json!({ "session": session }))
        }
        "close" => {
            let session = arg_session(args_object(args, &["session"])?)?;
            hub.close(session).await;
            Ok(json!({ "session": session }))
        }
        _ => unreachable!("operation validated by the caller"),
    }
}

/// Input must never reach a socket that belongs to a previous connection.
async fn guard_link(mgr: &ConnectionManager, hub: &ReplHub, session: u64) -> Result<(), OpErr> {
    if matches!(mgr.get_connection_status().mode, ConnectionMode::Http) {
        return Ok(());
    }
    hub.close(session).await;
    Err(OpErr::new("device_changed", "The Wi-Fi connection is no longer selected"))
}

/// `script_repl_request(operation, args)`: `open{session}`, `send_line{session,line}`,
/// `interrupt{session}`, `close{session}`. Replies are `{ok, operation, ...}` objects.
#[tauri::command]
pub async fn script_repl_request(
    app: AppHandle,
    mgr: State<'_, ConnectionManager>,
    hub: State<'_, ReplHub>,
    operation: String,
    args: Option<Value>,
) -> Result<Value, String> {
    if !OPERATIONS.contains(&operation.as_str()) {
        return Err(format!("unknown script REPL operation '{operation}'"));
    }
    let args = args.unwrap_or_else(|| Value::Object(Map::new()));
    Ok(match handle(&app, &mgr, &hub, &operation, &args).await {
        Ok(Value::Object(mut o)) => {
            o.insert("ok".into(), json!(true));
            o.insert("operation".into(), json!(operation));
            Value::Object(o)
        }
        Ok(other) => other,
        Err(e) => e.into_value(&operation),
    })
}

#[cfg(test)]
pub(crate) type Seen = Arc<std::sync::Mutex<Vec<Value>>>;

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicBool;
    use tokio::net::TcpListener;
    use tokio_tungstenite::accept_async;

    const TOKEN: &str = "tok-0123456789abcdef";

    struct Collect(Seen);

    impl EventSink for Collect {
        fn emit(&self, event: Value) {
            self.0.lock().unwrap().push(event);
        }
    }

    fn fast() -> Limits {
        Limits {
            connect: Duration::from_millis(800),
            banner: Duration::from_millis(400),
            send: Duration::from_millis(500),
            flush: Duration::from_millis(5),
            link_every: 2,
        }
    }

    fn always() -> LinkCheck {
        Arc::new(|| true)
    }

    fn setup() -> (ReplHub, Seen, Arc<dyn EventSink>) {
        let seen: Seen = Arc::default();
        (ReplHub::default(), seen.clone(), Arc::new(Collect(seen)))
    }

    fn spec(session: u64, port: u16, token: &str) -> OpenSpec {
        OpenSpec {
            session,
            generation: 7,
            device: json!({"transport": "http", "address": "http://127.0.0.1", "mac": "aa:bb"}),
            url: format!("ws://127.0.0.1:{port}{WS_PATH}"),
            token: token.into(),
        }
    }

    async fn wait_for(seen: &Seen, what: &str, f: impl Fn(&Value) -> bool) -> Value {
        for _ in 0..400 {
            if let Some(v) = seen.lock().unwrap().iter().find(|v| f(v)) {
                return v.clone();
            }
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
        panic!("timed out waiting for {what}; saw {:#?}", seen.lock().unwrap());
    }

    fn kinds(seen: &Seen) -> Vec<String> {
        seen.lock()
            .unwrap()
            .iter()
            .map(|v| match v["kind"].as_str().unwrap() {
                "state" => v["state"].as_str().unwrap().to_string(),
                "closed" => format!("closed:{}", v["reason"].as_str().unwrap()),
                k => k.to_string(),
            })
            .collect()
    }

    fn output(seen: &Seen) -> String {
        seen.lock()
            .unwrap()
            .iter()
            .filter(|v| v["kind"] == "output")
            .map(|v| v["data"].as_str().unwrap().to_string())
            .collect()
    }

    /// Mini firmware: token frame, banner, echo + prompt per CR line, Ctrl-C note.
    async fn firmware(listener: TcpListener, token: &'static str, got: Arc<std::sync::Mutex<Vec<String>>>) {
        let (tcp, _) = listener.accept().await.unwrap();
        let mut ws = accept_async(tcp).await.unwrap();
        let first = match ws.next().await {
            Some(Ok(Message::Text(t))) => t.as_str().to_string(),
            _ => return,
        };
        if first != token {
            let _ = ws
                .close(Some(CloseFrame { code: CloseCode::from(CLOSE_UNAUTHORIZED), reason: "".into() }))
                .await;
            return;
        }
        ws.send(Message::text("MicroPython REPL ready.\r\n>>> ")).await.unwrap();
        while let Some(Ok(m)) = ws.next().await {
            match m {
                Message::Text(t) => {
                    got.lock().unwrap().push(t.as_str().to_string());
                    let line = t.as_str().trim_end_matches('\r').to_string();
                    let reply = if t.as_str() == "\u{3}" {
                        "\r\n>>> ".to_string()
                    } else {
                        format!("{line}\r\n{line}!\r\n>>> ")
                    };
                    ws.send(Message::text(reply)).await.unwrap();
                }
                Message::Close(_) => {
                    got.lock().unwrap().push("<close>".into());
                    return;
                }
                _ => {}
            }
        }
    }

    async fn listener() -> (TcpListener, u16) {
        let l = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let p = l.local_addr().unwrap().port();
        (l, p)
    }

    #[test]
    fn urls_come_only_from_a_plain_http_base() {
        assert_eq!(ws_url("http://192.168.3.51").unwrap(), "ws://192.168.3.51/api/scripts/repl/ws");
        assert_eq!(ws_url("http://bugbuster.local:8080/").unwrap(), "ws://bugbuster.local:8080/api/scripts/repl/ws");
        assert_eq!(ws_url("192.168.0.2").unwrap(), "ws://192.168.0.2/api/scripts/repl/ws");
        for bad in ["https://x", "ftp://x", "", "http://", "http://a@b", "http://a/b", "http://a b", "http://a?x=1", "http:"] {
            assert!(ws_url(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn lines_must_fit_the_firmware_line_buffer() {
        assert!(validate_line("print(1)").is_ok());
        assert!(validate_line("\tx = 1").is_ok());
        assert!(validate_line(&"a".repeat(LINE_MAX)).is_ok());
        for bad in ["", "a\nb", "a\rb", "\u{3}", "\u{4}", "caf\u{e9}", "\x7f"] {
            assert!(validate_line(bad).is_err(), "{bad:?}");
        }
        assert!(validate_line(&"a".repeat(LINE_MAX + 1)).is_err());
    }

    #[test]
    fn output_ring_drops_oldest_whole_characters() {
        let mut r = OutputRing::with_capacity(8);
        r.push("abcdef");
        r.push("\u{e9}\u{e9}\u{e9}");
        let (chunk, dropped) = r.take(100).unwrap();
        assert!(chunk.len() <= 8 && chunk.ends_with('\u{e9}'));
        assert_eq!(dropped as usize, 12 - chunk.len());
        assert!(r.take(100).is_none());
        let mut r = OutputRing::with_capacity(100);
        r.push("\u{e9}\u{e9}");
        assert_eq!(r.take(1).unwrap().0, "\u{e9}");
        assert_eq!(r.take(3).unwrap(), ("\u{e9}".to_string(), 0));
    }

    #[test]
    fn scrub_hides_tokens_and_bounds_length() {
        assert_eq!(scrub("bad tok-1 here", "tok-1"), "bad *** here");
        assert!(scrub(&"x".repeat(1000), "").len() <= 240);
    }

    #[test]
    fn open_spec_debug_never_prints_the_token() {
        let s = format!("{:?}", spec(1, 80, TOKEN));
        assert!(!s.contains(TOKEN) && s.contains("redacted"));
    }

    #[tokio::test]
    async fn authenticates_streams_output_and_closes() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        let got = Arc::new(std::sync::Mutex::new(Vec::new()));
        let srv = tokio::spawn(firmware(l, TOKEN, got.clone()));
        hub.open(spec(1, port, TOKEN), sink, always(), fast()).await.unwrap();

        wait_for(&seen, "connected", |v| v["state"] == "connected").await;
        wait_for(&seen, "banner", |v| v["kind"] == "output").await;
        assert_eq!(kinds(&seen)[..3], ["connecting", "authenticating", "connected"]);
        assert!(output(&seen).starts_with("MicroPython REPL ready.\r\n>>> "));

        hub.send_line(1, "print(1)").await.unwrap();
        wait_for(&seen, "echo", |v| v["data"].as_str().is_some_and(|d| d.contains("print(1)!"))).await;
        hub.interrupt(1).await.unwrap();
        wait_for(&seen, "ctrl-c", |v| v["data"].as_str().is_some_and(|d| d == "\r\n>>> ")).await;
        assert_eq!(*got.lock().unwrap(), ["print(1)\r", "\u{3}"]);

        hub.close(1).await;
        wait_for(&seen, "closed", |v| v["kind"] == "closed").await;
        srv.await.unwrap();
        assert_eq!(got.lock().unwrap().last().unwrap(), "<close>");
        let closed = seen.lock().unwrap().last().unwrap().clone();
        assert_eq!((closed["reason"].as_str(), closed["session"].as_u64(), closed["generation"].as_u64()), (Some("local"), Some(1), Some(7)));
        assert_eq!(closed["device"]["mac"], "aa:bb");
        let all = serde_json::to_string(&*seen.lock().unwrap()).unwrap();
        assert!(!all.contains(TOKEN), "token leaked into events");
    }

    #[tokio::test]
    async fn wrong_token_closes_4001_and_never_reports_connected() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        let got = Arc::default();
        tokio::spawn(firmware(l, TOKEN, got));
        hub.open(spec(1, port, "wrong-token"), sink, always(), fast()).await.unwrap();
        let c = wait_for(&seen, "closed", |v| v["kind"] == "closed").await;
        assert_eq!((c["reason"].as_str(), c["code"].as_u64()), (Some("unauthorized"), Some(4001)));
        assert!(!kinds(&seen).contains(&"connected".to_string()));
        assert!(!serde_json::to_string(&*seen.lock().unwrap()).unwrap().contains("wrong-token"));
        let err = hub.send_line(1, "x").await.unwrap_err();
        assert_eq!(err.kind, "closed");
    }

    #[tokio::test]
    async fn busy_close_4002_is_reported_as_busy() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        tokio::spawn(async move {
            let (tcp, _) = l.accept().await.unwrap();
            let mut ws = accept_async(tcp).await.unwrap();
            let _ = ws.next().await;
            let _ = ws
                .close(Some(CloseFrame { code: CloseCode::from(CLOSE_BUSY), reason: "".into() }))
                .await;
        });
        hub.open(spec(1, port, TOKEN), sink, always(), fast()).await.unwrap();
        let c = wait_for(&seen, "closed", |v| v["kind"] == "closed").await;
        assert_eq!((c["reason"].as_str(), c["code"].as_u64()), (Some("busy"), Some(4002)));
        assert!(!kinds(&seen).contains(&"connected".to_string()));
    }

    #[tokio::test]
    async fn sending_the_token_is_not_authentication() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        tokio::spawn(async move {
            let (tcp, _) = l.accept().await.unwrap();
            let mut ws = accept_async(tcp).await.unwrap();
            while ws.next().await.is_some() {}
        });
        hub.open(spec(1, port, TOKEN), sink, always(), fast()).await.unwrap();
        let c = wait_for(&seen, "closed", |v| v["kind"] == "closed").await;
        assert_eq!(c["reason"], "timeout");
        assert_eq!(kinds(&seen), ["connecting", "authenticating", "closed:timeout"]);
    }

    #[tokio::test]
    async fn refused_connections_report_without_the_token() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        drop(l);
        hub.open(spec(1, port, TOKEN), sink, always(), fast()).await.unwrap();
        let c = wait_for(&seen, "closed", |v| v["kind"] == "closed").await;
        assert_eq!(c["reason"], "connect_failed");
        assert!(!c["message"].as_str().unwrap().contains(TOKEN));
    }

    #[tokio::test]
    async fn remote_close_after_connect_is_reported_once() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        tokio::spawn(async move {
            let (tcp, _) = l.accept().await.unwrap();
            let mut ws = accept_async(tcp).await.unwrap();
            let _ = ws.next().await;
            ws.send(Message::text("hi\r\n>>> ")).await.unwrap();
            tokio::time::sleep(Duration::from_millis(30)).await;
            let _ = ws.close(None).await;
        });
        hub.open(spec(1, port, TOKEN), sink, always(), fast()).await.unwrap();
        wait_for(&seen, "closed", |v| v["kind"] == "closed").await;
        tokio::time::sleep(Duration::from_millis(50)).await;
        let k = kinds(&seen);
        assert_eq!(k.iter().filter(|k| k.starts_with("closed")).count(), 1);
        assert_eq!(k.last().unwrap(), "closed:remote");
        assert_eq!(output(&seen), "hi\r\n>>> ");
    }

    #[tokio::test]
    async fn losing_the_selected_link_closes_the_session() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        let got = Arc::default();
        tokio::spawn(firmware(l, TOKEN, got));
        let ok = Arc::new(AtomicBool::new(true));
        let flag = ok.clone();
        hub.open(spec(1, port, TOKEN), sink, Arc::new(move || flag.load(Ordering::Relaxed)), fast())
            .await
            .unwrap();
        wait_for(&seen, "connected", |v| v["state"] == "connected").await;
        ok.store(false, Ordering::Relaxed);
        let c = wait_for(&seen, "closed", |v| v["kind"] == "closed").await;
        assert_eq!(c["reason"], "link_changed");
    }

    #[tokio::test]
    async fn stale_sessions_cannot_open_send_or_close_the_new_one() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        let got = Arc::new(std::sync::Mutex::new(Vec::new()));
        tokio::spawn(firmware(l, TOKEN, got.clone()));
        hub.open(spec(5, port, TOKEN), sink.clone(), always(), fast()).await.unwrap();
        wait_for(&seen, "connected", |v| v["state"] == "connected").await;

        assert_eq!(hub.open(spec(5, port, TOKEN), sink.clone(), always(), fast()).await.unwrap_err().kind, "stale");
        assert_eq!(hub.open(spec(3, port, TOKEN), sink.clone(), always(), fast()).await.unwrap_err().kind, "stale");
        assert_eq!(hub.send_line(4, "x=1").await.unwrap_err().kind, "stale");
        assert_eq!(hub.interrupt(4).await.unwrap_err().kind, "stale");
        hub.close(4).await;
        assert_eq!(hub.send_line(5, "ok=1").await.map(|_| ()), Ok(()));
        wait_for(&seen, "echo", |v| v["data"].as_str().is_some_and(|d| d.contains("ok=1!"))).await;
        assert_eq!(*got.lock().unwrap(), ["ok=1\r"]);
    }

    #[tokio::test]
    async fn reconnect_is_explicit_and_never_replays_input() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        let first = Arc::new(std::sync::Mutex::new(Vec::new()));
        let f2 = first.clone();
        tokio::spawn(async move {
            let (tcp, _) = l.accept().await.unwrap();
            let mut ws = accept_async(tcp).await.unwrap();
            let _ = ws.next().await;
            ws.send(Message::text(">>> ")).await.unwrap();
            if let Some(Ok(Message::Text(t))) = ws.next().await {
                f2.lock().unwrap().push(t.as_str().to_string());
            }
            // Drop the socket without a close frame.
            drop(ws);
            // A second connection must see only the token.
            let (tcp, _) = l.accept().await.unwrap();
            let mut ws = accept_async(tcp).await.unwrap();
            let mut frames = Vec::new();
            ws.send(Message::text(">>> ")).await.ok();
            let _ = timeout(Duration::from_millis(300), async {
                while let Some(Ok(Message::Text(t))) = ws.next().await {
                    frames.push(t.as_str().to_string());
                }
            })
            .await;
            *f2.lock().unwrap() = frames;
        });
        hub.open(spec(1, port, TOKEN), sink.clone(), always(), fast()).await.unwrap();
        wait_for(&seen, "connected", |v| v["state"] == "connected").await;
        hub.send_line(1, "danger()").await.unwrap();
        wait_for(&seen, "closed", |v| v["kind"] == "closed" && v["session"] == 1).await;
        assert_eq!(hub.send_line(1, "danger()").await.unwrap_err().kind, "closed");

        hub.open(spec(2, port, TOKEN), sink, always(), fast()).await.unwrap();
        wait_for(&seen, "connected 2", |v| v["state"] == "connected" && v["session"] == 2).await;
        tokio::time::sleep(Duration::from_millis(350)).await;
        assert_eq!(*first.lock().unwrap(), [TOKEN.to_string()], "the second socket saw more than the token");
    }

    #[tokio::test]
    async fn a_new_open_supersedes_and_closes_the_old_socket() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        let got = Arc::new(std::sync::Mutex::new(Vec::new()));
        let g2 = got.clone();
        tokio::spawn(async move {
            for _ in 0..2 {
                let (tcp, _) = l.accept().await.unwrap();
                let g = g2.clone();
                tokio::spawn(firmware_stream(tcp, g));
            }
        });
        async fn firmware_stream(tcp: tokio::net::TcpStream, got: Arc<std::sync::Mutex<Vec<String>>>) {
            let mut ws = accept_async(tcp).await.unwrap();
            let _ = ws.next().await;
            ws.send(Message::text(">>> ")).await.unwrap();
            while let Some(Ok(m)) = ws.next().await {
                if matches!(m, Message::Close(_)) {
                    got.lock().unwrap().push("<close>".into());
                    return;
                }
            }
        }
        hub.open(spec(1, port, TOKEN), sink.clone(), always(), fast()).await.unwrap();
        wait_for(&seen, "connected 1", |v| v["state"] == "connected" && v["session"] == 1).await;
        hub.open(spec(2, port, TOKEN), sink, always(), fast()).await.unwrap();
        wait_for(&seen, "connected 2", |v| v["state"] == "connected" && v["session"] == 2).await;
        let c = wait_for(&seen, "old closed", |v| v["kind"] == "closed" && v["session"] == 1).await;
        assert_eq!(c["reason"], "local");
        assert_eq!(got.lock().unwrap().len(), 1);
    }

    #[tokio::test]
    async fn bulk_output_is_chunked_and_bounded() {
        let (hub, seen, sink) = setup();
        let (l, port) = listener().await;
        tokio::spawn(async move {
            let (tcp, _) = l.accept().await.unwrap();
            let mut ws = accept_async(tcp).await.unwrap();
            let _ = ws.next().await;
            ws.send(Message::text("ready")).await.unwrap();
            for _ in 0..8 {
                ws.send(Message::text("z".repeat(FRAME_MAX - 64))).await.unwrap();
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        });
        hub.open(spec(1, port, TOKEN), sink, always(), fast()).await.unwrap();
        wait_for(&seen, "closed", |v| v["kind"] == "closed").await;
        let events = seen.lock().unwrap().clone();
        let outs: Vec<&Value> = events.iter().filter(|v| v["kind"] == "output").collect();
        assert!(outs.iter().all(|v| v["data"].as_str().unwrap().len() <= OUTPUT_CHUNK));
        let kept: usize = outs.iter().map(|v| v["data"].as_str().unwrap().len()).sum();
        let dropped: u64 = outs.iter().map(|v| v["dropped"].as_u64().unwrap()).sum();
        assert_eq!(kept as u64 + dropped, 5 + 8 * (FRAME_MAX as u64 - 64));
        let mut seqs: Vec<u64> = events.iter().map(|v| v["seq"].as_u64().unwrap()).collect();
        let sorted = { let mut s = seqs.clone(); s.sort_unstable(); s };
        assert_eq!(seqs, sorted);
        seqs.dedup();
        assert_eq!(seqs.len(), events.len());
    }

    #[test]
    fn strict_argument_shapes() {
        let ok = json!({"session": 3});
        assert_eq!(arg_session(args_object(&ok, &["session"]).unwrap()).unwrap(), 3);
        for bad in [json!(null), json!([]), json!({"session": 0}), json!({"session": "3"}), json!({"session": -1}), json!({"session": 3, "host": "x"}), json!({"session": 3, "token": "x"})] {
            let r = args_object(&bad, &["session"]).and_then(arg_session);
            assert_eq!(r.unwrap_err().kind, "invalid", "{bad}");
        }
    }
}
