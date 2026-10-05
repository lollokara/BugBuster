//! App-wide scripting runtime model and store.
//!
//! The first half is a pure, natively testable model (names, run status, log ring,
//! documents, REPL helpers). The second half is the reactive `ScriptStore`: one
//! instance lives in `App`, so run status, logs, polling and per-device unsaved
//! buffers survive navigation, disconnects and device swaps. All device traffic goes
//! through the single `script_request {operation, args}` Tauri command.

#![allow(dead_code)]

use std::collections::BTreeMap;

use leptos::prelude::*;
use leptos::task::spawn_local;
use serde_json::{json, Value};
use wasm_bindgen::prelude::*;

/// Standard-alphabet base64 (padding optional); None on any invalid character.
pub fn b64_decode(s: &str) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(s.len() / 4 * 3);
    let (mut acc, mut bits) = (0u32, 0u32);
    for c in s.bytes() {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            b'=' => break,
            b'\r' | b'\n' => continue,
            _ => return None,
        };
        acc = (acc << 6) | u32::from(v);
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    Some(out)
}

// -----------------------------------------------------------------------------
// Names and limits (mirror script_storage_validate_name and the backend checks)
// -----------------------------------------------------------------------------

pub const NAME_MAX: usize = 32;
pub const BODY_MAX: usize = 32 * 1024;
pub const LOG_PAGE_BYTES: usize = 4096;
pub const LOG_CAPACITY: usize = 5000;
pub const MAX_PAGES_PER_DRAIN: usize = 8;
pub const REPL_HISTORY_MAX: usize = 100;
pub const REPL_ENTRIES_MAX: usize = 200;
pub const REPL_OUTPUT_LINES_MAX: usize = 500;
/// Key of the untitled buffer inside a workspace (never a valid file name).
pub const SCRATCH: &str = "";

pub fn is_valid_name(name: &str) -> bool {
    let n = name.chars().count();
    (1..=NAME_MAX).contains(&n)
        && !name.starts_with('.')
        && name.ends_with(".py")
        && name
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '-'))
}

/// Trim, append ".py" when missing, and validate.
pub fn normalize_name(raw: &str) -> Option<String> {
    let t = raw.trim();
    if t.is_empty() {
        return None;
    }
    let name = if t.ends_with(".py") {
        t.to_string()
    } else {
        format!("{t}.py")
    };
    is_valid_name(&name).then_some(name)
}

pub fn name_rules() -> &'static str {
    "1-32 characters: letters, digits, _ . - (\".py\" is added)"
}

/// Validation for text headed to the device (UTF-8 bytes, not characters).
pub fn check_source(source: &str) -> Result<(), String> {
    if source.is_empty() {
        return Err("A script can't be empty".into());
    }
    if source.len() > BODY_MAX {
        return Err(format!(
            "Script is {} bytes; the device keeps at most {BODY_MAX}",
            source.len()
        ));
    }
    Ok(())
}

// -----------------------------------------------------------------------------
// Errors and replies
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ScriptError {
    pub kind: String,
    pub message: String,
    pub running: Option<String>,
    pub id: Option<u64>,
    /// The request may have reached the device (run, eval, stop, delete, autorun_*).
    pub outcome_unknown: bool,
}

impl ScriptError {
    pub fn new(kind: &str, message: impl Into<String>) -> Self {
        Self {
            kind: kind.into(),
            message: message.into(),
            ..Default::default()
        }
    }

    pub fn is_busy(&self) -> bool {
        self.kind == "busy"
    }

    /// Link-level failures where nothing is known about the device.
    pub fn is_link_error(&self) -> bool {
        matches!(
            self.kind.as_str(),
            "transport"
                | "not_connected"
                | "device_changed"
                | "protocol"
                | "malformed"
                | "in_flight"
        )
    }

    pub fn display(&self) -> String {
        let base = match self.kind.as_str() {
            "unsupported" => format!("Firmware update needed: {}", self.message),
            "unauthorized" => format!("Not authorised: {}", self.message),
            "not_connected" => "No device connected".to_string(),
            "device_changed" => "The connection changed while the request was running".to_string(),
            _ => self.message.clone(),
        };
        if self.outcome_unknown {
            format!(
                "{base} The device may have processed it; check the run status before retrying."
            )
        } else {
            base
        }
    }
}

/// `ok:true` objects pass through; anything else becomes a typed error.
pub fn decode_reply(v: Value) -> Result<Value, ScriptError> {
    if v.get("ok").and_then(Value::as_bool) == Some(true) {
        return Ok(v);
    }
    let s = |k: &str| v.get(k).and_then(Value::as_str).map(str::to_string);
    Err(ScriptError {
        kind: s("kind").unwrap_or_else(|| "malformed".into()),
        message: s("error").unwrap_or_else(|| "Unexpected reply from the device".into()),
        running: s("running"),
        id: v.get("id").and_then(Value::as_u64),
        outcome_unknown: v
            .get("outcomeUnknown")
            .and_then(Value::as_bool)
            .unwrap_or(false),
    })
}

#[derive(Debug, Clone, PartialEq)]
pub struct DeviceIdent {
    pub transport: String,
    pub address: String,
    pub mac: Option<String>,
}

impl DeviceIdent {
    pub fn from_reply(v: &Value) -> Option<Self> {
        let d = v.get("device")?;
        Some(Self {
            transport: d.get("transport")?.as_str()?.to_string(),
            address: d
                .get("address")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string(),
            mac: d
                .get("mac")
                .and_then(Value::as_str)
                .filter(|m| !m.is_empty())
                .map(str::to_string),
        })
    }

    /// Workspace key: the MAC when the firmware reports one, else the transport path.
    pub fn key(&self) -> String {
        match &self.mac {
            Some(m) => format!("mac:{}", m.to_lowercase()),
            None => format!("{}:{}", self.transport, self.address),
        }
    }
}

// -----------------------------------------------------------------------------
// Run status (iOS ScriptRunStatus)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SourceKind {
    #[default]
    Manual,
    Autorun,
    Repl,
    Unknown,
}

impl SourceKind {
    fn parse(s: Option<&str>) -> Self {
        match s {
            None => Self::Manual,
            Some("manual") => Self::Manual,
            Some("autorun") => Self::Autorun,
            Some("repl") => Self::Repl,
            Some(_) => Self::Unknown,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::Manual => "Manual",
            Self::Autorun => "Autorun",
            Self::Repl => "REPL",
            Self::Unknown => "Other",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum RunState {
    #[default]
    Idle,
    Running,
    Stopping,
    Error,
    Done,
    Unknown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ExitKind {
    #[default]
    None,
    Ok,
    Error,
    Stopped,
    Unknown,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct RunStatus {
    pub running: bool,
    pub current_script_id: u64,
    pub total_runs: u64,
    pub total_errors: u64,
    pub last_error: String,
    pub name: String,
    pub source: SourceKind,
    pub state: RunState,
    pub last_exit: ExitKind,
    pub started_at: f64,
    pub started_at_epoch: bool,
    pub file_slot_id: u64,
    pub file_slot_name: String,
    pub last_script_id: u64,
}

impl RunStatus {
    pub fn from_json(v: &Value) -> Self {
        let s = |k: &str| v.get(k).and_then(Value::as_str).map(str::to_string);
        let n = |k: &str| v.get(k).and_then(Value::as_u64).unwrap_or(0);
        let running = v.get("running").and_then(Value::as_bool).unwrap_or(false);
        let state = match s("state").as_deref() {
            None => {
                if running {
                    RunState::Running
                } else {
                    RunState::Idle
                }
            }
            Some("idle") => RunState::Idle,
            Some("running") => RunState::Running,
            Some("stopping") => RunState::Stopping,
            Some("error") => RunState::Error,
            Some("done") => RunState::Done,
            Some(_) => RunState::Unknown,
        };
        let last_exit = match s("lastExit").as_deref() {
            None | Some("none") => ExitKind::None,
            Some("ok") => ExitKind::Ok,
            Some("error") => ExitKind::Error,
            Some("stopped") => ExitKind::Stopped,
            Some(_) => ExitKind::Unknown,
        };
        Self {
            running,
            current_script_id: n("currentScriptId"),
            total_runs: n("totalRuns"),
            total_errors: n("totalErrors"),
            last_error: s("lastError").unwrap_or_default(),
            name: s("name").unwrap_or_default(),
            source: SourceKind::parse(s("source").as_deref()),
            state,
            last_exit,
            started_at: v.get("startedAt").and_then(Value::as_f64).unwrap_or(0.0),
            started_at_epoch: v
                .get("startedAtEpoch")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            file_slot_id: n("fileSlotId"),
            file_slot_name: s("fileSlotName").unwrap_or_default(),
            last_script_id: n("lastScriptId"),
        }
    }

    /// A file script holds the single slot: run-file answers 409 and REPL input is refused.
    pub fn holds_file_slot(&self) -> bool {
        !self.file_slot_name.is_empty()
    }

    pub fn is_active(&self) -> bool {
        matches!(self.state, RunState::Running | RunState::Stopping) || self.holds_file_slot()
    }

    pub fn display_name(&self) -> String {
        if self.holds_file_slot() {
            self.file_slot_name.clone()
        } else if !self.name.is_empty() {
            self.name.clone()
        } else if self.source == SourceKind::Repl {
            "REPL".into()
        } else {
            "script".into()
        }
    }

    pub fn state_label(&self) -> &'static str {
        match self.state {
            RunState::Running => "Running",
            RunState::Stopping => "Stopping...",
            RunState::Error => "Error",
            RunState::Done => match self.last_exit {
                ExitKind::Stopped => "Stopped",
                ExitKind::Error => "Error",
                _ => "Finished",
            },
            RunState::Idle | RunState::Unknown => "Idle",
        }
    }

    /// Tone for the dot: green running, orange stopping, red error, gray otherwise.
    pub fn tone(&self) -> &'static str {
        match self.state {
            RunState::Running => "green",
            RunState::Stopping => "orange",
            RunState::Error => "red",
            RunState::Done if self.last_exit == ExitKind::Error => "red",
            _ => "gray",
        }
    }

    /// Identity of the current run for elapsed anchoring.
    pub fn run_key(&self) -> (String, u64, u64) {
        let id = if self.file_slot_id != 0 {
            self.file_slot_id
        } else {
            self.current_script_id
        };
        (self.display_name(), id, self.started_at as u64)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct AutorunStatus {
    pub enabled: bool,
    pub has_script: bool,
    pub io12_high: bool,
    pub last_run_ok: bool,
    pub last_run_id: u64,
    pub script_name: String,
    pub ran_this_boot: bool,
    pub running: bool,
}

impl AutorunStatus {
    pub fn from_json(v: &Value) -> Self {
        let b = |k: &str, d: bool| v.get(k).and_then(Value::as_bool).unwrap_or(d);
        Self {
            enabled: b("enabled", false),
            has_script: b("has_script", false),
            io12_high: b("io12_high", true),
            last_run_ok: b("last_run_ok", false),
            last_run_id: v.get("last_run_id").and_then(Value::as_u64).unwrap_or(0),
            script_name: v
                .get("scriptName")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string(),
            ran_this_boot: b("ranThisBoot", false),
            running: b("running", false),
        }
    }

    pub fn state_text(&self) -> &'static str {
        if self.running {
            "Running"
        } else if self.ran_this_boot {
            if self.last_run_ok {
                "Finished OK"
            } else {
                "Failed"
            }
        } else if !self.enabled {
            "Off"
        } else if self.io12_high {
            "Runs at next boot"
        } else {
            "Suppressed: IO12 held low"
        }
    }

    pub fn rows(&self) -> Vec<(&'static str, String)> {
        let script = if !self.script_name.is_empty() {
            self.script_name.clone()
        } else if self.has_script {
            "autorun.py".into()
        } else {
            "None".into()
        };
        vec![
            ("Script", script),
            (
                "Ran this boot",
                if self.ran_this_boot { "Yes" } else { "No" }.into(),
            ),
            ("State", self.state_text().into()),
            (
                "IO12 gate",
                if self.io12_high {
                    "High: autorun allowed"
                } else {
                    "Low: autorun suppressed at boot"
                }
                .into(),
            ),
        ]
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct StorageInfo {
    pub total: f64,
    pub used: f64,
    pub free: f64,
    pub count: u64,
    pub max_scripts: u64,
}

impl StorageInfo {
    pub fn from_json(v: &Value) -> Self {
        let f = |k: &str| v.get(k).and_then(Value::as_f64).unwrap_or(0.0);
        Self {
            total: f("totalBytes"),
            used: f("usedBytes"),
            free: f("freeBytes"),
            count: v.get("scriptCount").and_then(Value::as_u64).unwrap_or(0),
            max_scripts: v.get("maxScripts").and_then(Value::as_u64).unwrap_or(0),
        }
    }

    pub fn text(&self) -> String {
        format!(
            "{:.1} of {:.1} KB used",
            self.used / 1024.0,
            self.total / 1024.0
        )
    }
}

// -----------------------------------------------------------------------------
// Elapsed time and polling cadence
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Elapsed {
    pub secs: f64,
    /// False when the start was only observed (the device reported uptime, no wall clock).
    pub exact: bool,
}

pub fn elapsed(
    status: &RunStatus,
    now_ms: f64,
    launched_ms: Option<f64>,
    first_seen_ms: Option<f64>,
) -> Option<Elapsed> {
    if !status.is_active() {
        return None;
    }
    if status.started_at_epoch && status.started_at > 0.0 {
        return Some(Elapsed {
            secs: (now_ms / 1000.0 - status.started_at).max(0.0),
            exact: true,
        });
    }
    if let Some(l) = launched_ms {
        return Some(Elapsed {
            secs: ((now_ms - l) / 1000.0).max(0.0),
            exact: true,
        });
    }
    first_seen_ms.map(|s| Elapsed {
        secs: ((now_ms - s) / 1000.0).max(0.0),
        exact: false,
    })
}

pub fn format_elapsed(secs: f64) -> String {
    let t = secs.floor().max(0.0) as u64;
    if t < 60 {
        format!("{t}s")
    } else if t < 3600 {
        format!("{}m {:02}s", t / 60, t % 60)
    } else {
        format!("{}h {:02}m", t / 3600, (t % 3600) / 60)
    }
}

impl Elapsed {
    pub fn text(&self) -> String {
        if self.exact {
            format_elapsed(self.secs)
        } else {
            format!(">= {}", format_elapsed(self.secs))
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PollPolicy {
    pub status_s: f64,
    pub logs_s: Option<f64>,
}

impl PollPolicy {
    pub fn make(console_visible: bool, scripts_visible: bool, active: bool) -> Self {
        if console_visible {
            Self {
                status_s: 1.0,
                logs_s: Some(1.0),
            }
        } else if scripts_visible {
            Self {
                status_s: 1.0,
                logs_s: active.then_some(5.0),
            }
        } else if active {
            Self {
                status_s: 5.0,
                logs_s: Some(5.0),
            }
        } else {
            Self {
                status_s: 5.0,
                logs_s: None,
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Log ring copy (cursor pages, byte-level line assembly)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum LogLevel {
    Error,
    Warning,
    Info,
    Debug,
}

impl LogLevel {
    pub const ALL: [LogLevel; 4] = [Self::Error, Self::Warning, Self::Info, Self::Debug];

    pub fn letter(self) -> &'static str {
        match self {
            Self::Error => "E",
            Self::Warning => "W",
            Self::Info => "I",
            Self::Debug => "D",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::Error => "Error",
            Self::Warning => "Warning",
            Self::Info => "Info",
            Self::Debug => "Debug",
        }
    }

    fn parse(s: &str) -> Option<Self> {
        match s {
            "E" => Some(Self::Error),
            "W" => Some(Self::Warning),
            "I" => Some(Self::Info),
            "D" => Some(Self::Debug),
            _ => None,
        }
    }

    pub fn index(self) -> usize {
        match self {
            Self::Error => 0,
            Self::Warning => 1,
            Self::Info => 2,
            Self::Debug => 3,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct LogLine {
    pub id: u64,
    pub ts_ms: Option<u32>,
    pub level: LogLevel,
    pub source: String,
    pub text: String,
    pub is_marker: bool,
}

impl LogLine {
    pub fn plain(&self) -> String {
        if self.is_marker {
            return format!("--- {} ---", self.text);
        }
        match self.ts_ms {
            None => self.text.clone(),
            Some(ts) => {
                let mut s = format!("{ts} {} {}", self.level.letter(), self.source);
                if !self.text.is_empty() {
                    s.push(' ');
                    s.push_str(&self.text);
                }
                s
            }
        }
    }

    /// `<ts_ms> <L> <src> <text>`; anything else is kept verbatim as an info line.
    pub fn parse(raw: &str, id: u64) -> Self {
        let mut it = raw.splitn(4, ' ');
        let (a, b, c, d) = (it.next(), it.next(), it.next(), it.next());
        if let (Some(a), Some(b), Some(c)) = (a, b, c) {
            if let (Ok(ts), Some(level)) = (a.parse::<u32>(), LogLevel::parse(b)) {
                if !c.is_empty() && c.chars().all(|ch| ch.is_ascii_alphabetic()) {
                    return Self {
                        id,
                        ts_ms: Some(ts),
                        level,
                        source: c.to_string(),
                        text: d.unwrap_or("").to_string(),
                        is_marker: false,
                    };
                }
            }
        }
        Self {
            id,
            ts_ms: None,
            level: LogLevel::Info,
            source: String::new(),
            text: raw.to_string(),
            is_marker: false,
        }
    }

    pub fn marker(text: &str, id: u64) -> Self {
        Self {
            id,
            ts_ms: None,
            level: LogLevel::Info,
            source: String::new(),
            text: text.to_string(),
            is_marker: true,
        }
    }

    pub fn timestamp(&self) -> String {
        self.ts_ms
            .map(|ms| format!("{}.{:03}", ms / 1000, ms % 1000))
            .unwrap_or_default()
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct LogPage {
    pub data: Vec<u8>,
    pub since: u64,
    pub next: u64,
    pub dropped: u64,
    pub restarted: bool,
}

impl LogPage {
    pub fn from_json(v: &Value) -> Option<Self> {
        let data = b64_decode(v.get("data")?.as_str()?)?;
        let since = v.get("since")?.as_u64()?;
        let next = v.get("next")?.as_u64()?;
        Some(Self {
            data,
            since,
            next,
            dropped: v.get("dropped").and_then(Value::as_u64).unwrap_or(0),
            restarted: v
                .get("restarted")
                .and_then(Value::as_bool)
                .unwrap_or(next < since),
        })
    }
}

/// Splits pages into lines at byte level: a page can end mid-line or mid UTF-8 character.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct LogAssembler {
    pending: Vec<u8>,
}

impl LogAssembler {
    pub const MAX_PARTIAL: usize = 4096;

    pub fn feed(&mut self, data: &[u8]) -> Vec<String> {
        self.pending.extend_from_slice(data);
        let mut lines = Vec::new();
        let mut start = 0usize;
        while let Some(rel) = self.pending[start..].iter().position(|b| *b == b'\n') {
            let end = start + rel;
            let mut line = &self.pending[start..end];
            if line.last() == Some(&b'\r') {
                line = &line[..line.len() - 1];
            }
            lines.push(String::from_utf8_lossy(line).into_owned());
            start = end + 1;
        }
        self.pending.drain(..start);
        if self.pending.len() > Self::MAX_PARTIAL {
            lines.push(String::from_utf8_lossy(&self.pending).into_owned());
            self.pending.clear();
        }
        lines
    }

    pub fn reset(&mut self) {
        self.pending.clear();
    }

    pub fn has_partial(&self) -> bool {
        !self.pending.is_empty()
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct LogFilter {
    pub levels: [bool; 4],
    pub query: String,
}

impl Default for LogFilter {
    fn default() -> Self {
        Self {
            levels: [true; 4],
            query: String::new(),
        }
    }
}

impl LogFilter {
    pub fn all_levels(&self) -> bool {
        self.levels.iter().all(|b| *b)
    }

    pub fn matches(&self, line: &LogLine) -> bool {
        let q = self.query.trim().to_lowercase();
        if line.is_marker {
            return q.is_empty() || line.text.to_lowercase().contains(&q);
        }
        if !self.levels[line.level.index()] {
            return false;
        }
        q.is_empty()
            || line.text.to_lowercase().contains(&q)
            || line.source.to_lowercase().contains(&q)
    }
}

/// The app's copy of the device log ring: the resumable cursor plus parsed lines.
#[derive(Debug, Clone, PartialEq)]
pub struct LogStore {
    pub lines: Vec<LogLine>,
    /// Byte offset to pass as `since` on the next poll. Clearing the view never moves it.
    pub cursor: u64,
    assembler: LogAssembler,
    next_id: u64,
    capacity: usize,
}

impl Default for LogStore {
    fn default() -> Self {
        Self::with_capacity(LOG_CAPACITY)
    }
}

impl LogStore {
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            lines: Vec::new(),
            cursor: 0,
            assembler: LogAssembler::default(),
            next_id: 0,
            capacity,
        }
    }

    fn take_id(&mut self) -> u64 {
        self.next_id += 1;
        self.next_id
    }

    /// Ingest one page; returns the lines it added (markers included) for the REPL router.
    pub fn ingest(&mut self, page: &LogPage) -> Vec<LogLine> {
        if page.restarted {
            self.assembler.reset();
            self.cursor = 0;
            let id = self.take_id();
            let m = LogLine::marker("Device log restarted", id);
            self.append(vec![m.clone()]);
            return vec![m];
        }
        let mut batch = Vec::new();
        // since == 0 is the first attach: older bytes were never ours to lose.
        if page.dropped > 0 && page.since > 0 {
            self.assembler.reset();
            let id = self.take_id();
            batch.push(LogLine::marker(
                &format!("{} bytes of log dropped (ring overflow)", page.dropped),
                id,
            ));
        }
        for raw in self.assembler.feed(&page.data) {
            let id = self.take_id();
            batch.push(LogLine::parse(&raw, id));
        }
        self.cursor = page.next;
        self.append(batch.clone());
        batch
    }

    pub fn append_marker(&mut self, text: &str) {
        let id = self.take_id();
        self.append(vec![LogLine::marker(text, id)]);
    }

    /// Hide what is on screen; the cursor stays, so cleared lines never come back.
    pub fn clear(&mut self) {
        self.lines.clear();
    }

    /// New device: forget everything and read its ring from the start.
    pub fn reset(&mut self) {
        self.lines.clear();
        self.cursor = 0;
        self.assembler.reset();
    }

    fn append(&mut self, batch: Vec<LogLine>) {
        if batch.is_empty() {
            return;
        }
        self.lines.extend(batch);
        if self.lines.len() > self.capacity {
            let extra = self.lines.len() - self.capacity;
            self.lines.drain(..extra);
        }
    }

    pub fn plain_text(&self) -> String {
        self.lines
            .iter()
            .map(LogLine::plain)
            .collect::<Vec<_>>()
            .join("\n")
    }

    pub fn filtered_text(&self, filter: &LogFilter) -> String {
        self.lines
            .iter()
            .filter(|l| filter.matches(l))
            .map(LogLine::plain)
            .collect::<Vec<_>>()
            .join("\n")
    }
}

// -----------------------------------------------------------------------------
// Lint presentation
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LintCause {
    Busy,
    Unsupported,
    Network,
    Unavailable,
    Other,
}

#[derive(Debug, Clone, PartialEq)]
pub enum LintBanner {
    Ok,
    /// A genuine MicroPython compile error (device syntax check, advisory to the editor).
    Syntax {
        message: String,
        line: Option<u32>,
    },
    /// The check did not run: not a syntax verdict.
    Skipped {
        cause: LintCause,
        text: String,
    },
}

#[derive(Debug, Clone, PartialEq)]
pub struct LintRecord {
    /// Buffer revision whose text was checked; a newer revision makes this stale.
    pub rev: u64,
    pub banner: LintBanner,
    /// The check followed a successful save of that revision.
    pub saved: bool,
}

/// The line number MicroPython puts in `File "...", line N`.
pub fn parse_error_line(message: &str) -> Option<u32> {
    let idx = message.find("line ")?;
    let digits: String = message[idx + 5..]
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect();
    digits.parse().ok().filter(|n| *n > 0)
}

pub fn lint_banner(r: &Result<Value, ScriptError>) -> LintBanner {
    match r {
        Ok(v) => {
            if v.get("valid").and_then(Value::as_bool) == Some(true) {
                LintBanner::Ok
            } else {
                let message = v
                    .get("message")
                    .and_then(Value::as_str)
                    .filter(|m| !m.is_empty())
                    .unwrap_or("Syntax error")
                    .to_string();
                let line = parse_error_line(&message);
                LintBanner::Syntax { message, line }
            }
        }
        Err(e) => match e.kind.as_str() {
            "busy" => LintBanner::Skipped {
                cause: LintCause::Busy,
                text: "Syntax check skipped: the interpreter is busy running a script.".into(),
            },
            "unsupported" => LintBanner::Skipped {
                cause: LintCause::Unsupported,
                text: format!("Syntax check needs newer firmware. {}", e.message),
            },
            "unavailable" => LintBanner::Skipped {
                cause: LintCause::Unavailable,
                text: format!("Syntax check unavailable: {}", e.message),
            },
            "transport" | "protocol" | "malformed" | "device_changed" | "not_connected"
            | "in_flight" => LintBanner::Skipped {
                cause: LintCause::Network,
                text: format!("Syntax check failed: {}", e.display()),
            },
            _ => LintBanner::Skipped {
                cause: LintCause::Other,
                text: format!("Syntax check failed: {}", e.display()),
            },
        },
    }
}

// -----------------------------------------------------------------------------
// Documents: per-device workspaces with dirty buffers
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Buffer {
    pub text: String,
    /// The text last known to be on the device (empty for an untitled buffer).
    pub base: String,
    /// Bumped by every edit; saves and checks are tagged with it.
    pub rev: u64,
    pub sel_anchor: u32,
    pub sel_head: u32,
    pub lint: Option<LintRecord>,
    /// The stored file was not valid UTF-8; saving would rewrite those bytes.
    pub lossy: bool,
}

impl Buffer {
    pub fn loaded(text: String, lossy: bool) -> Self {
        Self {
            base: text.clone(),
            text,
            lossy,
            ..Default::default()
        }
    }

    pub fn dirty(&self) -> bool {
        self.text != self.base
    }
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Workspace {
    pub files: Vec<String>,
    pub storage: Option<StorageInfo>,
    pub files_loaded: bool,
    /// File name, `Some("")` for the untitled buffer.
    pub open: Option<String>,
    pub buffers: BTreeMap<String, Buffer>,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Scripts {
    pub devices: BTreeMap<String, Workspace>,
    /// Workspace key of the last identified device; "" before any device was seen.
    pub active: String,
}

impl Scripts {
    pub fn ws(&self) -> Option<&Workspace> {
        self.devices.get(&self.active)
    }

    pub fn ws_mut(&mut self) -> &mut Workspace {
        self.devices.entry(self.active.clone()).or_default()
    }

    pub fn open_name(&self) -> Option<String> {
        self.ws().and_then(|w| w.open.clone())
    }

    pub fn buffer(&self, name: &str) -> Option<&Buffer> {
        self.ws().and_then(|w| w.buffers.get(name))
    }

    pub fn active_buffer(&self) -> Option<(String, &Buffer)> {
        let w = self.ws()?;
        let name = w.open.clone()?;
        w.buffers.get(&name).map(|b| (name, b))
    }

    pub fn is_dirty(&self, name: &str) -> bool {
        self.buffer(name).is_some_and(Buffer::dirty)
    }

    /// Insert a freshly read file unless the user already has a buffer for it.
    pub fn load_buffer(&mut self, dev: &str, name: &str, text: String, lossy: bool) {
        let ws = self.devices.entry(dev.to_string()).or_default();
        ws.buffers
            .entry(name.to_string())
            .or_insert_with(|| Buffer::loaded(text, lossy));
    }

    /// Discard local edits and take the device copy (revert).
    pub fn replace_buffer(&mut self, dev: &str, name: &str, text: String, lossy: bool) {
        let ws = self.devices.entry(dev.to_string()).or_default();
        let rev = ws.buffers.get(name).map_or(0, |b| b.rev + 1);
        let mut b = Buffer::loaded(text, lossy);
        b.rev = rev;
        ws.buffers.insert(name.to_string(), b);
    }

    pub fn edit(&mut self, name: &str, text: String) {
        let ws = self.ws_mut();
        let b = ws.buffers.entry(name.to_string()).or_default();
        if b.text != text {
            b.text = text;
            b.rev += 1;
        }
    }

    pub fn set_selection(&mut self, name: &str, anchor: u32, head: u32) {
        if let Some(b) = self.ws_mut().buffers.get_mut(name) {
            b.sel_anchor = anchor;
            b.sel_head = head;
        }
    }

    pub fn snapshot(&self, dev: &str, name: &str) -> Option<(u64, String)> {
        self.devices
            .get(dev)?
            .buffers
            .get(name)
            .map(|b| (b.rev, b.text.clone()))
    }

    /// The upload of `rev`'s text finished: it becomes the device copy. Newer edits stay dirty.
    pub fn save_finished(&mut self, dev: &str, name: &str, text: &str) {
        let ws = self.devices.entry(dev.to_string()).or_default();
        if let Some(b) = ws.buffers.get_mut(name) {
            b.base = text.to_string();
            b.lossy = false;
        }
        if !ws.files.iter().any(|f| f == name) && !name.is_empty() {
            ws.files.push(name.to_string());
            ws.files.sort_by_key(|f| f.to_lowercase());
        }
    }

    pub fn set_lint(&mut self, dev: &str, name: &str, record: LintRecord) {
        if let Some(b) = self
            .devices
            .get_mut(dev)
            .and_then(|w| w.buffers.get_mut(name))
        {
            b.lint = Some(record);
        }
    }

    /// The untitled buffer was saved under `name`. Text typed while it uploaded stays dirty.
    pub fn promote_scratch(&mut self, dev: &str, name: &str, saved: String) {
        let ws = self.devices.entry(dev.to_string()).or_default();
        let mut b = Buffer::loaded(saved, false);
        if let Some(old) = ws.buffers.remove(SCRATCH) {
            b.rev = old.rev;
            b.text = old.text;
            b.sel_anchor = old.sel_anchor;
            b.sel_head = old.sel_head;
        }
        ws.buffers.insert(name.to_string(), b);
        ws.open = Some(name.to_string());
        if !ws.files.iter().any(|f| f == name) {
            ws.files.push(name.to_string());
            ws.files.sort_by_key(|f| f.to_lowercase());
        }
    }

    pub fn remove_buffer(&mut self, dev: &str, name: &str) {
        if let Some(ws) = self.devices.get_mut(dev) {
            ws.buffers.remove(name);
            ws.files.retain(|f| f != name);
            if ws.open.as_deref() == Some(name) {
                ws.open = None;
            }
        }
    }

    /// Switch to another device's workspace. The offline untitled buffer follows the user
    /// to the first device that has no untitled text of its own.
    pub fn switch_device(&mut self, key: &str) {
        if self.active == key {
            return;
        }
        let was_offline = self.active.is_empty();
        self.active = key.to_string();
        if !was_offline || key.is_empty() {
            self.devices.entry(self.active.clone()).or_default();
            return;
        }
        let (scratch, open_scratch) = match self.devices.get_mut("") {
            Some(w) => (
                w.buffers
                    .get(SCRATCH)
                    .filter(|b| !b.text.is_empty())
                    .cloned(),
                w.open.as_deref() == Some(SCRATCH),
            ),
            None => (None, false),
        };
        let ws = self.devices.entry(key.to_string()).or_default();
        if let Some(sc) = scratch {
            let free = ws
                .buffers
                .get(SCRATCH)
                .map(|b| b.text.is_empty())
                .unwrap_or(true);
            if free {
                ws.buffers.insert(SCRATCH.to_string(), sc);
                if open_scratch && ws.open.is_none() {
                    ws.open = Some(SCRATCH.to_string());
                }
                if let Some(old) = self.devices.get_mut("") {
                    old.buffers.remove(SCRATCH);
                    if old.open.as_deref() == Some(SCRATCH) {
                        old.open = None;
                    }
                }
            }
        }
    }

    /// Dirty buffers in workspaces other than the active one: (device key, file).
    pub fn dirty_elsewhere(&self) -> Vec<(String, String)> {
        self.devices
            .iter()
            .filter(|(k, _)| **k != self.active)
            .flat_map(|(k, w)| {
                w.buffers
                    .iter()
                    .filter(|(_, b)| b.dirty())
                    .map(move |(n, _)| {
                        (
                            k.clone(),
                            if n.is_empty() {
                                "untitled".to_string()
                            } else {
                                n.clone()
                            },
                        )
                    })
            })
            .collect()
    }
}

/// Outcome of the save that precedes a run: it may only proceed if the saved snapshot is
/// still the current text.
pub fn run_after_save_allowed(saved_rev: u64, current_rev: u64) -> bool {
    saved_rev == current_rev
}

// -----------------------------------------------------------------------------
// REPL helpers
// -----------------------------------------------------------------------------

/// Typographic punctuation pasted into the REPL breaks MicroPython.
pub fn sanitize_repl(text: &str) -> String {
    text.replace("\r\n", "\n")
        .replace('\r', "\n")
        .replace(['\u{201C}', '\u{201D}'], "\"")
        .replace(['\u{2018}', '\u{2019}'], "'")
        .replace('\u{2014}', "--")
        .replace('\u{2013}', "-")
}

/// True when Enter should add a continuation line instead of evaluating.
pub fn repl_needs_more(text: &str) -> bool {
    if text.trim().is_empty() {
        return false;
    }
    let mut depth: i32 = 0;
    let mut quote: Option<char> = None;
    let mut triple = false;
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if let Some(q) = quote {
            if c == '\\' {
                i += 2;
                continue;
            }
            if triple {
                if c == q && chars.get(i + 1) == Some(&q) && chars.get(i + 2) == Some(&q) {
                    quote = None;
                    triple = false;
                    i += 3;
                    continue;
                }
            } else if c == q || c == '\n' {
                quote = None;
            }
        } else {
            match c {
                '#' => {
                    while i < chars.len() && chars[i] != '\n' {
                        i += 1;
                    }
                    continue;
                }
                '"' | '\'' => {
                    quote = Some(c);
                    triple = chars.get(i + 1) == Some(&c) && chars.get(i + 2) == Some(&c);
                    if triple {
                        i += 2;
                    }
                }
                '(' | '[' | '{' => depth += 1,
                ')' | ']' | '}' => depth -= 1,
                _ => {}
            }
        }
        i += 1;
    }
    if quote.is_some() && triple || depth > 0 {
        return true;
    }
    let lines: Vec<&str> = text.split('\n').collect();
    let last = lines.last().copied().unwrap_or("");
    if last.trim().is_empty() {
        return false;
    }
    let code_end = last.split('#').next().unwrap_or("").trim_end();
    if code_end.ends_with(':') || code_end.ends_with('\\') {
        return true;
    }
    let opens_block = lines
        .iter()
        .any(|l| l.split('#').next().unwrap_or("").trim_end().ends_with(':'));
    opens_block && last.starts_with([' ', '\t'])
}

/// Indentation to carry onto the next REPL line.
pub fn repl_next_indent(text: &str) -> String {
    let last = text.split('\n').next_back().unwrap_or("");
    let lead: String = last
        .chars()
        .take_while(|c| *c == ' ' || *c == '\t')
        .collect();
    let code = last.split('#').next().unwrap_or("").trim_end();
    if code.ends_with(':') {
        format!("{lead}    ")
    } else {
        lead
    }
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ReplEntry {
    pub echo: String,
    pub output: Vec<String>,
    pub error: Option<String>,
    pub pending: bool,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ReplState {
    pub entries: Vec<ReplEntry>,
    pub history: Vec<String>,
    /// Position while walking history with the arrow keys; None when editing a fresh line.
    pub cursor: Option<usize>,
    pub draft: String,
}

impl ReplState {
    pub fn push_history(&mut self, src: &str) {
        if self.history.last().map(String::as_str) != Some(src) {
            self.history.push(src.to_string());
            if self.history.len() > REPL_HISTORY_MAX {
                self.history.remove(0);
            }
        }
        self.cursor = None;
    }

    /// Older entry (Up). `current` is saved as the draft when leaving the fresh line.
    pub fn history_prev(&mut self, current: &str) -> Option<String> {
        if self.history.is_empty() {
            return None;
        }
        let idx = match self.cursor {
            None => {
                self.draft = current.to_string();
                self.history.len() - 1
            }
            Some(0) => 0,
            Some(i) => i - 1,
        };
        self.cursor = Some(idx);
        self.history.get(idx).cloned()
    }

    pub fn history_next(&mut self) -> Option<String> {
        match self.cursor {
            None => None,
            Some(i) if i + 1 < self.history.len() => {
                self.cursor = Some(i + 1);
                self.history.get(i + 1).cloned()
            }
            Some(_) => {
                self.cursor = None;
                Some(std::mem::take(&mut self.draft))
            }
        }
    }

    pub fn start_entry(&mut self, src: &str) {
        self.entries.push(ReplEntry {
            echo: src.to_string(),
            pending: true,
            ..Default::default()
        });
        if self.entries.len() > REPL_ENTRIES_MAX {
            self.entries.remove(0);
        }
    }

    /// Route VM output (`mpy` log lines) to the newest entry.
    pub fn route_output(&mut self, lines: &[LogLine]) {
        let Some(last) = self.entries.last_mut() else {
            return;
        };
        for l in lines.iter().filter(|l| !l.is_marker && l.source == "mpy") {
            if last.output.len() < REPL_OUTPUT_LINES_MAX {
                last.output.push(l.text.clone());
            }
        }
    }

    pub fn transcript(&self) -> String {
        let mut out = Vec::new();
        for e in &self.entries {
            for (i, part) in e.echo.split('\n').enumerate() {
                out.push(format!("{}{part}", if i == 0 { ">>> " } else { "... " }));
            }
            out.extend(e.output.iter().cloned());
            if let Some(err) = &e.error {
                out.push(format!("! {err}"));
            }
        }
        out.join("\n")
    }
}

// -----------------------------------------------------------------------------
// Wi-Fi WebSocket REPL (device terminal; backend: scripts_repl.rs)
// -----------------------------------------------------------------------------

pub const TERM_MAX: usize = 60_000;
/// Firmware `REPL_LINE_BUF_SIZE` (512) minus the terminator.
pub const WS_LINE_MAX: usize = 511;
const PROMPT: &str = ">>> ";

/// Why `src` cannot use the line-buffered device socket; `None` means it can. The firmware submits
/// every CR as its own line, so anything else needs an explicit eval.
pub fn ws_line_blocker(src: &str) -> Option<&'static str> {
    if src.contains('\n') {
        Some("multi-line input")
    } else if src.len() > WS_LINE_MAX {
        Some("line longer than 511 bytes")
    } else if !src.bytes().all(|b| b == b'\t' || (0x20..0x7f).contains(&b)) {
        Some("non-ASCII or control characters")
    } else {
        None
    }
}

/// Device terminal text: exactly what the firmware streams (echo, output, prompts), bounded.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Term {
    pub text: String,
    pub truncated: bool,
}

impl Term {
    pub fn feed(&mut self, data: &str) {
        for c in data.chars() {
            match c {
                '\r' => {}
                '\u{8}' | '\u{7f}' => {
                    if !self.text.ends_with('\n') {
                        self.text.pop();
                    }
                }
                '\n' | '\t' => self.text.push(c),
                c if c.is_control() => {}
                c => self.text.push(c),
            }
        }
        self.trim();
    }

    fn fresh_line(&mut self) {
        if !self.text.is_empty() && !self.text.ends_with('\n') {
            self.text.push('\n');
        }
    }

    pub fn marker(&mut self, text: &str) {
        self.fresh_line();
        self.text.push_str(&format!("[{text}]\n"));
        self.trim();
    }

    /// Local echo for input that did not travel over the socket (explicit eval).
    pub fn echo_local(&mut self, src: &str, note: &str) {
        if !self.text.ends_with(PROMPT) {
            self.fresh_line();
            self.text.push_str(PROMPT);
        }
        for (i, l) in src.split('\n').enumerate() {
            if i > 0 {
                self.text.push_str("\n... ");
            }
            self.text.push_str(l);
        }
        self.text.push('\n');
        self.marker(note);
    }

    pub fn end_with_prompt(&mut self) {
        if !self.text.ends_with(PROMPT) {
            self.fresh_line();
            self.text.push_str(PROMPT);
        }
    }

    pub fn clear(&mut self) {
        self.text.clear();
        self.truncated = false;
    }

    fn trim(&mut self) {
        if self.text.len() <= TERM_MAX {
            return;
        }
        let mut cut = self.text.len() - TERM_MAX * 3 / 4;
        while !self.text.is_char_boundary(cut) {
            cut += 1;
        }
        if let Some(nl) = self.text[cut..].find('\n') {
            cut += nl + 1;
        }
        self.text.drain(..cut);
        self.truncated = true;
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default)]
pub enum WsPhase {
    #[default]
    Idle,
    Connecting,
    Authenticating,
    Connected,
    Closed,
}

impl WsPhase {
    pub fn active(self) -> bool {
        matches!(self, Self::Connecting | Self::Authenticating | Self::Connected)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct WsFail {
    pub reason: String,
    pub message: String,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct WsRepl {
    pub phase: WsPhase,
    /// Id of the session the UI is attached to; events of any other id are ignored.
    pub session: u64,
    /// Highest id ever issued (the backend accepts only growing ids).
    pub last_session: u64,
    /// Backend connection epoch, learned from the first event of the session.
    pub generation: Option<u64>,
    seq: u64,
    pub term: Term,
    pub fail: Option<WsFail>,
}

impl WsRepl {
    /// Start a session: ids grow monotonically, also across page reloads.
    pub fn begin(&mut self, now_ms: f64) -> u64 {
        let id = (self.last_session + 1).max(now_ms as u64);
        self.last_session = id;
        self.session = id;
        self.generation = None;
        self.seq = 0;
        self.phase = WsPhase::Connecting;
        self.fail = None;
        id
    }

    /// The REPL view went away: detach without recording a failure.
    pub fn detach(&mut self) {
        self.phase = WsPhase::Idle;
        self.session = 0;
    }

    /// The selected connection changed: nothing of the old session or terminal survives.
    pub fn reset_link(&mut self) {
        *self = Self {
            last_session: self.last_session,
            ..Default::default()
        };
    }

    /// Record a failure that is not a `closed` event (open refused, input rejected).
    pub fn fail(&mut self, session: u64, reason: &str, message: &str) -> bool {
        if session != self.session || !self.phase.active() {
            return false;
        }
        self.phase = WsPhase::Closed;
        self.fail = Some(WsFail {
            reason: reason.into(),
            message: message.into(),
        });
        self.term.marker(&format!("REPL closed: {message}"));
        true
    }

    /// Apply one `script-repl` event; false when it belongs to another session or generation,
    /// repeats an earlier one, or arrives after the session ended.
    pub fn apply(&mut self, ev: &Value) -> bool {
        let Some(session) = ev.get("session").and_then(Value::as_u64) else {
            return false;
        };
        if session == 0 || session != self.session || !self.phase.active() {
            return false;
        }
        let generation = ev.get("generation").and_then(Value::as_u64);
        match (self.generation, generation) {
            (Some(g), Some(e)) if g != e => return false,
            (None, Some(e)) => self.generation = Some(e),
            _ => {}
        }
        let seq = ev.get("seq").and_then(Value::as_u64).unwrap_or(0);
        if seq <= self.seq {
            return false;
        }
        self.seq = seq;
        let text = |k: &str| ev.get(k).and_then(Value::as_str).unwrap_or("");
        match text("kind") {
            "state" => {
                let next = match text("state") {
                    "connecting" => WsPhase::Connecting,
                    "authenticating" => WsPhase::Authenticating,
                    "connected" => WsPhase::Connected,
                    _ => return false,
                };
                if next > self.phase {
                    self.phase = next;
                }
                true
            }
            "output" => {
                if ev.get("dropped").and_then(Value::as_u64).unwrap_or(0) > 0 {
                    self.term.marker("output was too fast; some text was dropped");
                }
                self.term.feed(text("data"));
                true
            }
            "closed" => {
                self.phase = WsPhase::Closed;
                let reason = text("reason");
                if reason != "local" {
                    self.fail = Some(WsFail {
                        reason: reason.into(),
                        message: text("message").into(),
                    });
                }
                self.term.marker(&format!("REPL closed: {}", text("message")));
                true
            }
            _ => false,
        }
    }

    pub fn status_text(&self) -> &'static str {
        match self.phase {
            WsPhase::Idle => "Not connected",
            WsPhase::Connecting => "Connecting...",
            WsPhase::Authenticating => "Authenticating...",
            WsPhase::Connected => "Connected",
            WsPhase::Closed => match self.fail.as_ref().map(|f| f.reason.as_str()) {
                Some("unauthorized") => "Unauthorized",
                Some("busy") => "REPL busy",
                Some("timeout") => "Timed out",
                Some("connect_failed") => "Connection failed",
                Some(_) => "Disconnected",
                None => "Closed",
            },
        }
    }

    pub fn tone(&self) -> &'static str {
        match (self.phase, self.fail.as_ref().map(|f| f.reason.as_str())) {
            (WsPhase::Connected, _) => "green",
            (WsPhase::Closed, Some("busy")) => "orange",
            (WsPhase::Closed, Some(_)) => "red",
            _ => "blue",
        }
    }
}

// -----------------------------------------------------------------------------
// Small shared bits
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub struct ReplacePrompt {
    pub running: String,
    pub requested: String,
    pub background: bool,
}

impl ReplacePrompt {
    pub fn message(&self) -> String {
        format!("Stop '{}' and run '{}'?", self.running, self.requested)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct PendingRun {
    pub id: u64,
    pub name: String,
    pub background: bool,
    pub started_ms: f64,
    pub settle_polls: u32,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Notice {
    pub text: String,
    /// "red", "orange" or "blue".
    pub tone: &'static str,
    pub seq: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct UploadProgress {
    pub name: String,
    pub sent: u64,
    pub total: u64,
}

pub fn finish_marker(name: &str, status: &RunStatus) -> String {
    match (status.state, status.last_exit) {
        (_, ExitKind::Stopped) => format!("Stopped '{name}'"),
        (RunState::Error, _) | (_, ExitKind::Error) => {
            let first = status.last_error.lines().next().unwrap_or("").trim();
            if first.is_empty() {
                format!("'{name}' failed")
            } else {
                format!("'{name}' failed: {first}")
            }
        }
        _ => format!("Finished '{name}'"),
    }
}

/// Group helper kept pure for tests: which buffer names are dirty in the active workspace.
pub fn dirty_names(s: &Scripts) -> Vec<String> {
    s.ws()
        .map(|w| {
            w.buffers
                .iter()
                .filter(|(_, b)| b.dirty())
                .map(|(n, _)| n.clone())
                .collect()
        })
        .unwrap_or_default()
}

// =============================================================================
// Reactive store
// =============================================================================

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Conn {
    /// "Disconnected", "Usb", "Http" or "Mock" (the app's own labels).
    pub mode: String,
    pub addr: String,
    /// Bumped on every link change; async replies from an older epoch are dropped.
    pub epoch: u64,
    /// A reply of this epoch identified the device, so device actions are safe.
    pub verified: bool,
}

impl Conn {
    pub fn connected(&self) -> bool {
        matches!(self.mode.as_str(), "Usb" | "Http")
    }

    pub fn transport_label(&self) -> &'static str {
        match self.mode.as_str() {
            "Usb" => "USB",
            "Http" => "Wi-Fi",
            _ => "",
        }
    }

    /// What the REPL really uses: the authenticated device WebSocket over Wi-Fi, persistent
    /// `eval` through the USB tunnel.
    pub fn repl_label(&self) -> &'static str {
        match self.mode.as_str() {
            "Usb" => "USB tunnel eval",
            "Http" => "Wi-Fi WebSocket REPL",
            _ => "offline",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Dialog {
    New { save_as: bool },
    Delete(String),
    Revert(String),
    AutorunEnable(String),
    AutorunDisable,
    ResetVm,
    SaveLossy(String),
}

#[derive(Debug, Default)]
struct Internal {
    action_epoch: u64,
    open_seq: u64,
    notice_seq: u64,
    status_due_ms: f64,
    logs_due_ms: f64,
    log_device: String,
    seen_key: Option<(String, u64, u64)>,
    finished: Vec<(String, u64)>,
    announced: Option<(String, u64, u64)>,
    draining: bool,
    drain_again: bool,
}

#[derive(Clone, Copy)]
pub struct ScriptStore {
    pub conn: RwSignal<Conn>,
    pub model: RwSignal<Scripts>,
    pub status: RwSignal<Option<RunStatus>>,
    /// The last status predates a disconnect or a failed poll.
    pub stale: RwSignal<bool>,
    pub status_error: RwSignal<Option<String>>,
    pub autorun: RwSignal<Option<AutorunStatus>>,
    pub log: RwSignal<LogStore>,
    pub replace: RwSignal<Option<ReplacePrompt>>,
    pub console_open: RwSignal<bool>,
    pub console_height: RwSignal<f64>,
    pub scripts_visible: RwSignal<bool>,
    pub repl_open: RwSignal<bool>,
    pub pending: RwSignal<Option<PendingRun>>,
    pub upload: RwSignal<Option<UploadProgress>>,
    pub notice: RwSignal<Option<Notice>>,
    pub repl: RwSignal<ReplState>,
    /// Wi-Fi only: the device terminal socket (USB keeps `repl`'s eval entries).
    pub repl_ws: RwSignal<WsRepl>,
    pub dialog: RwSignal<Option<Dialog>>,
    pub launching: RwSignal<bool>,
    pub saving: RwSignal<bool>,
    pub linting: RwSignal<bool>,
    pub files_loading: RwSignal<bool>,
    pub repl_sending: RwSignal<bool>,
    pub clock: RwSignal<f64>,
    /// Bumped when the editor must reload the active buffer (open, switch, revert).
    pub doc_load: RwSignal<u64>,
    pub list_open: RwSignal<bool>,
    pub docs_open: RwSignal<bool>,
    /// CodeMirror handle and state live here (not in the view) so view teardown never touches
    /// values owned by an already-disposed parent.
    pub editor_handle: StoredValue<Option<JsValue>, LocalStorage>,
    pub editor_ready: RwSignal<bool>,
    pub editor_failed: RwSignal<Option<String>>,
    seen_start: RwSignal<Option<f64>>,
    int: StoredValue<Internal>,
}

pub async fn sleep_ms(ms: i32) {
    let p = js_sys::Promise::new(&mut |r, _| {
        if let Some(w) = web_sys::window() {
            let _ = w.set_timeout_with_callback_and_timeout_and_arguments_0(&r, ms);
        }
    });
    let _ = wasm_bindgen_futures::JsFuture::from(p).await;
}

pub fn now_ms() -> f64 {
    js_sys::Date::now()
}

fn page_hidden() -> bool {
    web_sys::window()
        .and_then(|w| w.document())
        .map(|d| d.hidden())
        .unwrap_or(false)
}

/// Create the store, wire its effects and start the app-lifetime poll loop. Call once from `App`.
pub fn install(mode: ReadSignal<String>, addr: ReadSignal<String>) -> ScriptStore {
    let s = ScriptStore {
        conn: RwSignal::new(Conn {
            mode: "Disconnected".into(),
            ..Default::default()
        }),
        model: RwSignal::new(Scripts::default()),
        status: RwSignal::new(None),
        stale: RwSignal::new(false),
        status_error: RwSignal::new(None),
        autorun: RwSignal::new(None),
        log: RwSignal::new(LogStore::default()),
        replace: RwSignal::new(None),
        console_open: RwSignal::new(false),
        console_height: RwSignal::new(220.0),
        scripts_visible: RwSignal::new(false),
        repl_open: RwSignal::new(false),
        pending: RwSignal::new(None),
        upload: RwSignal::new(None),
        notice: RwSignal::new(None),
        repl: RwSignal::new(ReplState::default()),
        repl_ws: RwSignal::new(WsRepl::default()),
        dialog: RwSignal::new(None),
        launching: RwSignal::new(false),
        saving: RwSignal::new(false),
        linting: RwSignal::new(false),
        files_loading: RwSignal::new(false),
        repl_sending: RwSignal::new(false),
        clock: RwSignal::new(now_ms()),
        doc_load: RwSignal::new(0),
        list_open: RwSignal::new(true),
        docs_open: RwSignal::new(false),
        editor_handle: StoredValue::new_local(None),
        editor_ready: RwSignal::new(false),
        editor_failed: RwSignal::new(None),
        seen_start: RwSignal::new(None),
        int: StoredValue::new(Internal::default()),
    };

    // Follow the app's connection signals; a change is a new link epoch.
    Effect::new(move |prev: Option<(String, String)>| {
        let cur = (mode.get(), addr.get());
        if prev.as_ref() != Some(&cur) {
            s.on_link_change(cur.0.clone(), cur.1.clone());
        }
        cur
    });

    // Visibility changes poll immediately.
    Effect::new(move |_| {
        let _ = (
            s.scripts_visible.get(),
            s.console_open.get(),
            s.repl_open.get(),
        );
        s.int.update_value(|i| {
            i.status_due_ms = 0.0;
            i.logs_due_ms = 0.0;
        });
    });

    // Load the file list and autorun state once a device is identified and Scripts is on screen.
    Effect::new(move |_| {
        let c = s.conn.get();
        let visible = s.scripts_visible.get();
        if c.connected() && c.verified && visible {
            spawn_local(async move {
                s.refresh_files().await;
                s.refresh_autorun().await;
            });
        }
    });

    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<
                crate::tauri_bridge::TauriEvent<crate::tauri_bridge::ScriptUploadProgress>,
            >(event)
            {
                let p = evt.payload;
                s.upload.update(|u| {
                    if u.as_ref().is_some_and(|cur| cur.name == p.name) {
                        *u = Some(UploadProgress {
                            name: p.name,
                            sent: p.sent,
                            total: p.total,
                        });
                    }
                });
            }
        });
        crate::tauri_bridge::listen("script-upload-progress", &closure).await;
        // INTENTIONAL: app-lifetime listener
        closure.forget();
    });

    // Device terminal events (Wi-Fi). Stale sessions and generations are filtered by `WsRepl::apply`.
    spawn_local(async move {
        let closure = Closure::new(move |event: JsValue| {
            if let Ok(evt) = serde_wasm_bindgen::from_value::<
                crate::tauri_bridge::TauriEvent<Value>,
            >(event)
            {
                s.repl_ws.update(|w| {
                    w.apply(&evt.payload);
                });
            }
        });
        crate::tauri_bridge::listen("script-repl", &closure).await;
        // INTENTIONAL: app-lifetime listener
        closure.forget();
    });

    // The socket exists only while the REPL is on screen: the device allows one session, so it
    // must not be held in the background. Errors are never retried here; Reconnect is explicit.
    Effect::new(move |_| {
        let want = s.conn.with(|c| c.mode == "Http" && c.verified)
            && s.scripts_visible.get()
            && s.repl_open.get();
        let phase = s.repl_ws.with(|w| w.phase);
        if want && phase == WsPhase::Idle {
            s.ws_open();
        } else if !want && phase.active() {
            s.ws_close();
        }
    });

    spawn_local(async move {
        loop {
            let wait = s.poll_once().await;
            sleep_ms(wait).await;
        }
    });
    s
}

impl ScriptStore {
    // ---- reactive helpers ----

    /// A device is connected and has identified itself in this link epoch.
    pub fn ready(&self) -> bool {
        self.conn.with(|c| c.connected() && c.verified)
    }

    pub fn connected(&self) -> bool {
        self.conn.with(Conn::connected)
    }

    pub fn is_active(&self) -> bool {
        self.status
            .with(|s| s.as_ref().is_some_and(RunStatus::is_active))
    }

    pub fn holds_file_slot(&self) -> bool {
        self.status
            .with(|s| s.as_ref().is_some_and(RunStatus::holds_file_slot))
    }

    /// The script that occupies the slot, if it is `name`.
    pub fn is_running_file(&self, name: &str) -> bool {
        self.status.with(|s| {
            s.as_ref()
                .is_some_and(|s| s.holds_file_slot() && s.file_slot_name == name)
        })
    }

    pub fn elapsed(&self) -> Option<Elapsed> {
        if self.stale.get() {
            return None;
        }
        let now = self.clock.get();
        let seen = self.seen_start.get();
        let status = self.status.get()?;
        let launched = self.pending.with(|p| {
            p.as_ref()
                .filter(|p| {
                    p.id != 0 && (p.id == status.file_slot_id || p.id == status.current_script_id)
                        || p.name == status.display_name()
                })
                .map(|p| p.started_ms)
        });
        elapsed(&status, now, launched, seen)
    }

    pub fn notify(&self, text: impl Into<String>, tone: &'static str) {
        let text = text.into();
        let seq = {
            let mut seq = 0;
            self.int.update_value(|i| {
                i.notice_seq += 1;
                seq = i.notice_seq;
            });
            seq
        };
        if !self.scripts_visible.get_untracked() {
            crate::tauri_bridge::show_toast(&text, if tone == "red" { "err" } else { "info" });
        }
        self.notice.set(Some(Notice { text, tone, seq }));
    }

    fn epoch(&self) -> u64 {
        self.conn.with_untracked(|c| c.epoch)
    }

    fn bump_action(&self) -> u64 {
        let mut e = 0;
        self.int.update_value(|i| {
            i.action_epoch += 1;
            e = i.action_epoch;
        });
        e
    }

    fn action_epoch(&self) -> u64 {
        self.int.with_value(|i| i.action_epoch)
    }

    fn active_device(&self) -> String {
        self.model.with_untracked(|m| m.active.clone())
    }

    // ---- connection ----

    fn on_link_change(&self, mode: String, addr: String) {
        self.conn.update(|c| {
            c.mode = mode;
            c.addr = addr;
            c.epoch += 1;
            c.verified = false;
        });
        self.int.update_value(|i| {
            i.status_due_ms = 0.0;
            i.logs_due_ms = 0.0;
            i.action_epoch += 1;
        });
        // What was observed before the link changed is stale, not "stopped".
        self.stale.set(true);
        self.status_error.set(None);
        self.replace.set(None);
        self.pending.set(None);
        self.upload.set(None);
        self.launching.set(false);
        self.saving.set(false);
        self.linting.set(false);
        self.repl_sending.set(false);
        self.repl
            .update(|r| r.entries.iter_mut().for_each(|e| e.pending = false));
        // The backend closes the old socket on its own; nothing from it may reach the new link.
        self.repl_ws.update(WsRepl::reset_link);
    }

    /// Accept a reply only if it belongs to the current link and the identified device.
    fn learn(&self, epoch: u64, ident: &DeviceIdent) -> bool {
        let c = self.conn.get_untracked();
        if c.epoch != epoch || c.mode.to_lowercase() != ident.transport {
            return false;
        }
        let key = ident.key();
        if c.verified {
            return self.model.with_untracked(|m| m.active == key);
        }
        let changed = self.model.with_untracked(|m| m.active != key);
        if changed {
            self.model.update(|m| m.switch_device(&key));
        }
        let new_log_device = self.int.with_value(|i| i.log_device != key);
        if new_log_device {
            self.log.update(LogStore::reset);
            self.status.set(None);
            self.autorun.set(None);
            self.pending.set(None);
            self.replace.set(None);
            self.repl.update(|r| r.entries.clear());
            self.int.update_value(|i| {
                i.log_device = key;
                i.finished.clear();
                i.announced = None;
                i.seen_key = None;
            });
        }
        self.conn.update(|c| {
            if c.epoch == epoch {
                c.verified = true;
            }
        });
        true
    }

    /// One scripting request. `None` means the link changed (or the reply belonged to another
    /// device) and the result must be discarded without touching any state.
    pub async fn request(&self, op: &str, args: Value) -> Option<Result<Value, ScriptError>> {
        let epoch = self.epoch();
        let raw = match crate::tauri_bridge::script_request(op, args).await {
            Ok(v) => v,
            Err(e) => {
                if self.epoch() != epoch {
                    return None;
                }
                return Some(Err(ScriptError::new("protocol", e)));
            }
        };
        if self.epoch() != epoch {
            return None;
        }
        let ident = DeviceIdent::from_reply(&raw);
        if let Some(id) = &ident {
            if !self.learn(epoch, id) {
                return None;
            }
        }
        Some(decode_reply(raw))
    }

    // ---- polling ----

    fn policy(&self) -> PollPolicy {
        let visible = self.scripts_visible.get_untracked();
        let console =
            visible && (self.console_open.get_untracked() || self.repl_open.get_untracked());
        let active = self
            .status
            .with_untracked(|s| s.as_ref().is_some_and(RunStatus::is_active))
            || self.pending.with_untracked(Option::is_some);
        PollPolicy::make(console, visible, active)
    }

    async fn poll_once(&self) -> i32 {
        if !self.conn.with_untracked(Conn::connected) {
            return 500;
        }
        if page_hidden() {
            return 1000;
        }
        let now = now_ms();
        self.clock.set(now);
        let policy = self.policy();
        let (status_due, logs_due) = self.int.with_value(|i| (i.status_due_ms, i.logs_due_ms));
        if now >= status_due {
            self.refresh_status().await;
            self.int
                .update_value(|i| i.status_due_ms = now + policy.status_s * 1000.0);
        }
        if let Some(every) = policy.logs_s {
            if now >= logs_due && self.conn.with_untracked(|c| c.verified) {
                self.drain_logs().await;
                self.int
                    .update_value(|i| i.logs_due_ms = now + every * 1000.0);
            }
        }
        let (s, l) = self.int.with_value(|i| (i.status_due_ms, i.logs_due_ms));
        let next = if policy.logs_s.is_some() { s.min(l) } else { s };
        (next - now_ms()).clamp(200.0, 1000.0) as i32
    }

    pub async fn refresh_status(&self) {
        let Some(r) = self.request("status", json!({})).await else {
            return;
        };
        match r {
            Ok(v) => {
                let fresh = RunStatus::from_json(&v);
                let prev = self.status.get_untracked();
                let was_active = prev.as_ref().is_some_and(RunStatus::is_active);
                let key = fresh.run_key();
                let mut transition = was_active && !fresh.is_active();
                if fresh.is_active() {
                    let changed = self.int.with_value(|i| i.seen_key.as_ref() != Some(&key));
                    if changed {
                        transition = true;
                        self.int.update_value(|i| i.seen_key = Some(key.clone()));
                        self.seen_start.set(Some(now_ms()));
                    }
                } else {
                    self.int.update_value(|i| i.seen_key = None);
                    self.seen_start.set(None);
                }
                self.status.set(Some(fresh.clone()));
                self.stale.set(false);
                self.status_error.set(None);

                let pending = self.pending.get_untracked();
                if fresh.is_active() && !was_active && pending.is_none() {
                    // Started elsewhere (another client, boot autorun).
                    let new = self.int.with_value(|i| i.announced.as_ref() != Some(&key));
                    if new {
                        self.int.update_value(|i| i.announced = Some(key.clone()));
                        let text = format!(
                            "'{}' running ({})",
                            fresh.display_name(),
                            fresh.source.label()
                        );
                        self.log.update(|l| l.append_marker(&text));
                    }
                }
                if was_active && !fresh.is_active() {
                    let p = prev.expect("was_active implies prev");
                    let name = p.display_name();
                    let id = p.run_key().1;
                    let launched_id = pending.as_ref().filter(|q| q.name == name).map(|q| q.id);
                    let first = launched_id
                        .map(|qid| self.mark_finished(&name, qid))
                        .unwrap_or(false);
                    if self.mark_finished(&name, id) || first {
                        let text = finish_marker(&name, &fresh);
                        self.log.update(|l| l.append_marker(&text));
                    }
                    // The run just ended: fetch its last lines (a traceback) even if nothing polls logs.
                    self.drain_logs().await;
                }
                self.settle_pending(&fresh).await;
                // A boot autorun or another client changes what the autorun panel should say.
                if transition {
                    self.refresh_autorun().await;
                }
            }
            Err(e) => {
                self.stale.set(true);
                self.status_error.set(Some(e.display()));
            }
        }
    }

    fn mark_finished(&self, name: &str, id: u64) -> bool {
        let mut fresh = false;
        self.int.update_value(|i| {
            let k = (name.to_string(), id);
            if !i.finished.contains(&k) {
                i.finished.push(k);
                if i.finished.len() > 16 {
                    i.finished.remove(0);
                }
                fresh = true;
            }
        });
        fresh
    }

    /// A run we launched is complete once the slot is free and its id is the last finished one,
    /// even if polling never observed it running (rapid scripts).
    async fn settle_pending(&self, fresh: &RunStatus) {
        let Some(p) = self.pending.get_untracked() else {
            return;
        };
        if fresh.is_active() {
            self.pending.update(|p| {
                if let Some(p) = p {
                    p.settle_polls = 0;
                }
            });
            return;
        }
        let settle = p.settle_polls + 1;
        self.drain_logs().await;
        let by_id = fresh.last_script_id == p.id;
        let timed_out = settle >= 3 || now_ms() - p.started_ms > 600_000.0;
        if by_id && self.mark_finished(&p.name, p.id) {
            let text = finish_marker(&p.name, fresh);
            self.log.update(|l| l.append_marker(&text));
        }
        self.pending.update(|slot| {
            if by_id || timed_out {
                *slot = None;
            } else if let Some(p) = slot {
                p.settle_polls = settle;
            }
        });
    }

    /// Read pages until one comes back short (caught up), at most `MAX_PAGES_PER_DRAIN` per pass.
    pub async fn drain_logs(&self) {
        let already = {
            let mut busy = false;
            self.int.update_value(|i| {
                if i.draining {
                    i.drain_again = true;
                    busy = true;
                } else {
                    i.draining = true;
                }
            });
            busy
        };
        if already {
            return;
        }
        loop {
            self.drain_pass().await;
            let again = {
                let mut again = false;
                self.int.update_value(|i| {
                    again = std::mem::take(&mut i.drain_again);
                    if !again {
                        i.draining = false;
                    }
                });
                again
            };
            if !again {
                break;
            }
        }
    }

    async fn drain_pass(&self) {
        for _ in 0..MAX_PAGES_PER_DRAIN {
            let since = self.log.with_untracked(|l| l.cursor);
            let Some(r) = self.request("logs", json!({ "since": since })).await else {
                return;
            };
            match r {
                Ok(v) => {
                    let Some(page) = LogPage::from_json(&v) else {
                        self.status_error
                            .set(Some("Unexpected log page from the device".into()));
                        return;
                    };
                    if self.log.with_untracked(|l| l.cursor) != since {
                        return;
                    }
                    let added = self.log.try_update(|l| l.ingest(&page)).unwrap_or_default();
                    if !added.is_empty() && self.repl.with_untracked(|r| !r.entries.is_empty()) {
                        self.repl.update(|r| r.route_output(&added));
                    }
                    if page.restarted {
                        continue;
                    }
                    if page.data.len() < LOG_PAGE_BYTES {
                        return;
                    }
                }
                Err(e) => {
                    self.status_error.set(Some(e.display()));
                    return;
                }
            }
        }
    }

    // ---- files ----

    pub async fn refresh_files(&self) {
        if !self.ready() {
            return;
        }
        self.files_loading.set(true);
        let r = self.request("files", json!({})).await;
        match r {
            Some(Ok(v)) => {
                let mut files: Vec<String> = v
                    .get("files")
                    .and_then(Value::as_array)
                    .map(|a| {
                        a.iter()
                            .filter_map(|f| f.as_str().map(str::to_string))
                            .collect()
                    })
                    .unwrap_or_default();
                files.sort_by_key(|f| f.to_lowercase());
                self.model.update(|m| {
                    let ws = m.ws_mut();
                    ws.files = files;
                    ws.files_loaded = true;
                });
                if let Some(Ok(sv)) = self.request("storage", json!({})).await {
                    let info = StorageInfo::from_json(&sv);
                    self.model.update(|m| m.ws_mut().storage = Some(info));
                }
            }
            Some(Err(e)) => self.notify(format!("Failed loading scripts: {}", e.display()), "red"),
            None => {}
        }
        self.files_loading.set(false);
    }

    pub fn refresh(&self) {
        let s = *self;
        spawn_local(async move {
            s.refresh_files().await;
            s.refresh_autorun().await;
        });
    }

    fn bump_doc(&self) {
        self.doc_load.update(|n| *n += 1);
    }

    /// Open a stored script; a local buffer (possibly dirty) wins over the device copy.
    pub fn open_file(&self, name: String) {
        let s = *self;
        let seq = {
            let mut seq = 0;
            self.int.update_value(|i| {
                i.open_seq += 1;
                seq = i.open_seq;
            });
            seq
        };
        spawn_local(async move {
            // A fresh link is verified by its first reply; do that before choosing a buffer.
            if s.connected() && !s.ready() {
                s.refresh_status().await;
                if s.int.with_value(|i| i.open_seq) != seq {
                    return;
                }
            }
            let have = s.model.with_untracked(|m| m.buffer(&name).is_some());
            if !have {
                if !s.ready() {
                    let msg = if s.connected() {
                        "The device has not identified itself yet. Try again in a moment."
                    } else {
                        "Connect to a device to open stored scripts."
                    };
                    s.notify(msg, "orange");
                    return;
                }
                let dev = s.active_device();
                let Some(r) = s.request("get", json!({ "name": name })).await else {
                    return;
                };
                match r {
                    Ok(v) => {
                        let text = v
                            .get("source")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_string();
                        let lossy = v.get("lossy").and_then(Value::as_bool).unwrap_or(false);
                        if lossy {
                            s.notify(
                                format!("'{name}' contains bytes that are not valid UTF-8; saving will replace them."),
                                "orange",
                            );
                        }
                        s.model.update(|m| m.load_buffer(&dev, &name, text, lossy));
                    }
                    Err(e) => {
                        s.notify(format!("Load failed: {}", e.display()), "red");
                        return;
                    }
                }
            }
            if s.int.with_value(|i| i.open_seq) != seq {
                return;
            }
            s.model.update(|m| m.ws_mut().open = Some(name));
            s.repl_open.set(false);
            s.bump_doc();
        });
    }

    pub fn open_scratch(&self) {
        self.model.update(|m| {
            let ws = m.ws_mut();
            ws.buffers.entry(SCRATCH.to_string()).or_default();
            ws.open = Some(SCRATCH.to_string());
        });
        self.repl_open.set(false);
        self.bump_doc();
    }

    /// Close the editor view; the buffer (and any unsaved edits) is kept.
    pub fn close_open(&self) {
        self.model.update(|m| m.ws_mut().open = None);
        self.bump_doc();
    }

    pub fn edit(&self, name: &str, text: String) {
        self.model.update(|m| m.edit(name, text));
    }

    pub fn store_selection(&self, name: &str, anchor: u32, head: u32) {
        self.model
            .update_untracked(|m| m.set_selection(name, anchor, head));
    }

    pub fn create_file(&self, raw: String, content: Option<String>) {
        let s = *self;
        spawn_local(async move {
            let Some(name) = normalize_name(&raw) else {
                s.notify(format!("Invalid name. {}", name_rules()), "red");
                return;
            };
            if s.model
                .with_untracked(|m| m.ws().is_some_and(|w| w.files.contains(&name)))
            {
                s.notify(format!("'{name}' already exists"), "red");
                return;
            }
            if !s.ready() {
                s.notify("Connect to a device to create a script.", "orange");
                return;
            }
            let promote = content.is_some();
            let text = content
                .unwrap_or_else(|| format!("# {name}\n# Write your MicroPython code here\n"));
            if let Err(e) = check_source(&text) {
                s.notify(e, "red");
                return;
            }
            let dev = s.active_device();
            let Some(r) = s
                .request("save", json!({ "name": name, "source": text }))
                .await
            else {
                return;
            };
            match r {
                Ok(_) => {
                    s.model.update(|m| {
                        if promote {
                            m.promote_scratch(&dev, &name, text.clone());
                        } else {
                            m.load_buffer(&dev, &name, text.clone(), false);
                            m.save_finished(&dev, &name, &text);
                            m.ws_mut().open = Some(name.clone());
                        }
                    });
                    s.repl_open.set(false);
                    s.bump_doc();
                    s.refresh_files().await;
                }
                Err(e) => s.notify(format!("Create failed: {}", e.display()), "red"),
            }
        });
    }

    pub fn delete_file(&self, name: String) {
        let s = *self;
        spawn_local(async move {
            if !s.ready() {
                return;
            }
            let dev = s.active_device();
            let Some(r) = s.request("delete", json!({ "name": name })).await else {
                return;
            };
            match r {
                Ok(_) => {
                    let was_open = s
                        .model
                        .with_untracked(|m| m.open_name().as_deref() == Some(name.as_str()));
                    s.model.update(|m| m.remove_buffer(&dev, &name));
                    if was_open {
                        s.bump_doc();
                    }
                    s.refresh_files().await;
                }
                Err(e) => {
                    s.notify(format!("Delete failed: {}", e.display()), "red");
                    if e.outcome_unknown {
                        s.refresh_files().await;
                    }
                }
            }
        });
    }

    /// Drop local edits and take the device copy.
    pub fn revert_file(&self, name: String) {
        let s = *self;
        spawn_local(async move {
            if !s.ready() {
                s.notify("Connect to a device to reload from it.", "orange");
                return;
            }
            let dev = s.active_device();
            let Some(r) = s.request("get", json!({ "name": name })).await else {
                return;
            };
            match r {
                Ok(v) => {
                    let text = v
                        .get("source")
                        .and_then(Value::as_str)
                        .unwrap_or("")
                        .to_string();
                    let lossy = v.get("lossy").and_then(Value::as_bool).unwrap_or(false);
                    s.model
                        .update(|m| m.replace_buffer(&dev, &name, text, lossy));
                    s.bump_doc();
                }
                Err(e) => s.notify(format!("Reload failed: {}", e.display()), "red"),
            }
        });
    }

    // ---- save and syntax check ----

    /// Ctrl/Cmd+S. An untitled buffer asks for a name first.
    pub fn save_active(&self) {
        let Some(name) = self.model.with_untracked(Scripts::open_name) else {
            return;
        };
        if name.is_empty() {
            self.dialog.set(Some(Dialog::New { save_as: true }));
            return;
        }
        if self
            .model
            .with_untracked(|m| m.buffer(&name).is_some_and(|b| b.lossy))
        {
            self.dialog.set(Some(Dialog::SaveLossy(name)));
            return;
        }
        let s = *self;
        spawn_local(async move {
            s.save_named(&name, true).await;
        });
    }

    pub fn save_confirmed(&self, name: String) {
        let s = *self;
        spawn_local(async move {
            s.save_named(&name, true).await;
        });
    }

    /// Upload a snapshot of `name`; edits made while it uploads stay dirty. Returns the saved
    /// revision. With `lint` the stored file is syntax-checked afterwards; a lint failure never
    /// undoes the save.
    async fn save_named(&self, name: &str, lint: bool) -> Option<u64> {
        if !self.ready() {
            self.notify("Connect to a device to save.", "orange");
            return None;
        }
        if self.saving.get_untracked() {
            self.notify("A save is already in progress.", "orange");
            return None;
        }
        let dev = self.active_device();
        let (rev, text) = self.model.with_untracked(|m| m.snapshot(&dev, name))?;
        if let Err(e) = check_source(&text) {
            self.notify(format!("Save failed: {e}"), "red");
            return None;
        }
        self.saving.set(true);
        self.upload.set(Some(UploadProgress {
            name: name.to_string(),
            sent: 0,
            total: text.len() as u64,
        }));
        let r = self
            .request("save", json!({ "name": name, "source": text }))
            .await;
        self.saving.set(false);
        self.upload.set(None);
        match r {
            None => {
                self.notify(
                    "Save interrupted: the connection changed. Your edits are kept.",
                    "orange",
                );
                None
            }
            Some(Err(e)) => {
                self.notify(format!("Save failed: {}", e.display()), "red");
                None
            }
            Some(Ok(_)) => {
                self.model.update(|m| m.save_finished(&dev, name, &text));
                if lint {
                    self.lint_stored(&dev, name, rev).await;
                }
                if let Some(Ok(sv)) = self.request("storage", json!({})).await {
                    let info = StorageInfo::from_json(&sv);
                    self.model.update(|m| m.ws_mut().storage = Some(info));
                }
                Some(rev)
            }
        }
    }

    async fn lint_stored(&self, dev: &str, name: &str, rev: u64) {
        self.linting.set(true);
        let r = self.request("lint", json!({ "name": name })).await;
        self.linting.set(false);
        if let Some(r) = r {
            let banner = lint_banner(&r);
            self.model.update(|m| {
                m.set_lint(
                    dev,
                    name,
                    LintRecord {
                        rev,
                        banner,
                        saved: true,
                    },
                )
            });
        }
    }

    /// Explicit compile-only check of the current text (no save, nothing is executed).
    pub fn check_syntax(&self) {
        let s = *self;
        spawn_local(async move {
            let Some(name) = s.model.with_untracked(Scripts::open_name) else {
                return;
            };
            if !s.ready() {
                s.notify("Connect to a device to check syntax.", "orange");
                return;
            }
            let dev = s.active_device();
            let Some((rev, text)) = s.model.with_untracked(|m| m.snapshot(&dev, &name)) else {
                return;
            };
            if text.trim().is_empty() {
                s.notify("Nothing to check: the script is empty.", "orange");
                return;
            }
            if let Err(e) = check_source(&text) {
                s.notify(e, "red");
                return;
            }
            s.linting.set(true);
            let r = s.request("lint", json!({ "source": text })).await;
            s.linting.set(false);
            if let Some(r) = r {
                let banner = lint_banner(&r);
                s.model.update(|m| {
                    m.set_lint(
                        &dev,
                        &name,
                        LintRecord {
                            rev,
                            banner,
                            saved: false,
                        },
                    )
                });
            }
        });
    }

    // ---- run control ----

    pub fn run(&self, name: String, background: bool) {
        let s = *self;
        spawn_local(async move { s.run_flow(name, background, false).await });
    }

    pub fn confirm_replace(&self) {
        let Some(p) = self.replace.get_untracked() else {
            return;
        };
        self.replace.set(None);
        let s = *self;
        spawn_local(async move { s.run_flow(p.requested, p.background, true).await });
    }

    pub fn cancel_replace(&self) {
        self.bump_action();
        self.replace.set(None);
    }

    async fn run_flow(&self, name: String, background: bool, replace: bool) {
        if !self.ready() {
            self.notify("Connect to a device to run scripts.", "orange");
            return;
        }
        if self.launching.get_untracked() {
            return;
        }
        self.launching.set(true);
        let epoch = self.bump_action();
        self.run_inner(&name, background, replace, epoch).await;
        self.launching.set(false);
    }

    async fn run_inner(&self, name: &str, background: bool, replace: bool, epoch: u64) {
        if !replace && self.model.with_untracked(|m| m.is_dirty(name)) {
            // The device runs the stored file: save the snapshot first.
            let Some(saved_rev) = self.save_named(name, false).await else {
                return;
            };
            let current = self
                .model
                .with_untracked(|m| m.buffer(name).map_or(saved_rev, |b| b.rev));
            if !run_after_save_allowed(saved_rev, current) {
                self.notify(
                    "Not run: the script changed while it was saving. Save again, then run.",
                    "orange",
                );
                return;
            }
        }
        if self.action_epoch() != epoch {
            return;
        }
        let r = self
            .request(
                "run",
                json!({ "name": name, "background": background, "replace": replace }),
            )
            .await;
        if self.action_epoch() != epoch {
            return;
        }
        match r {
            None => {}
            Some(Ok(v)) => {
                let id = v.get("id").and_then(Value::as_u64).unwrap_or(0);
                let started = v
                    .get("name")
                    .and_then(Value::as_str)
                    .unwrap_or(name)
                    .to_string();
                let bg = v
                    .get("background")
                    .and_then(Value::as_bool)
                    .unwrap_or(background);
                self.pending.set(Some(PendingRun {
                    id,
                    name: started.clone(),
                    background: bg,
                    started_ms: now_ms(),
                    settle_polls: 0,
                }));
                let text = if bg {
                    format!("Started '{started}' in background")
                } else {
                    format!("Started '{started}'")
                };
                self.log.update(|l| l.append_marker(&text));
                if !bg {
                    self.console_open.set(true);
                }
                self.refresh_status().await;
                self.drain_logs().await;
            }
            Some(Err(e)) if e.is_busy() => {
                let holder = e
                    .running
                    .clone()
                    .filter(|r| !r.is_empty())
                    .or_else(|| self.status.get_untracked().map(|s| s.display_name()))
                    .unwrap_or_else(|| "a script".into());
                self.replace.set(Some(ReplacePrompt {
                    running: holder,
                    requested: name.to_string(),
                    background,
                }));
            }
            Some(Err(e)) => {
                self.notify(format!("Run failed: {}", e.display()), "red");
                if e.outcome_unknown {
                    self.refresh_status().await;
                }
            }
        }
    }

    pub fn stop(&self) {
        let s = *self;
        spawn_local(async move {
            if !s.ready() {
                return;
            }
            let epoch = s.bump_action();
            let r = s.request("stop", json!({})).await;
            if s.action_epoch() != epoch {
                return;
            }
            match r {
                Some(Ok(_)) => s.refresh_status().await,
                Some(Err(e)) => {
                    s.notify(format!("Stop failed: {}", e.display()), "red");
                    if e.outcome_unknown {
                        s.refresh_status().await;
                    }
                }
                None => {}
            }
        });
    }

    // ---- autorun ----

    pub async fn refresh_autorun(&self) {
        if !self.ready() {
            return;
        }
        match self.request("autorun_status", json!({})).await {
            Some(Ok(v)) => self.autorun.set(Some(AutorunStatus::from_json(&v))),
            Some(Err(e)) => self.notify(format!("Autorun status: {}", e.display()), "red"),
            None => {}
        }
    }

    /// Only ever called from a confirmation dialog: autorun is never enabled implicitly.
    pub fn set_autorun(&self, name: Option<String>) {
        let s = *self;
        spawn_local(async move {
            if !s.ready() {
                return;
            }
            let r = match &name {
                Some(n) => s.request("autorun_enable", json!({ "name": n })).await,
                None => s.request("autorun_disable", json!({})).await,
            };
            match r {
                Some(Ok(_)) => s.refresh_autorun().await,
                Some(Err(e)) => {
                    s.notify(format!("Autorun change failed: {}", e.display()), "red");
                    if e.outcome_unknown {
                        s.refresh_autorun().await;
                    }
                }
                None => {}
            }
        });
    }

    // ---- Wi-Fi WebSocket REPL ----

    /// One backend REPL call. `None`: the link changed meanwhile and the result is discarded.
    async fn repl_call(&self, op: &str, args: Value) -> Option<Result<Value, (String, String)>> {
        let epoch = self.epoch();
        let r = crate::tauri_bridge::script_repl_request(op, args).await;
        if self.epoch() != epoch {
            return None;
        }
        let text = |v: &Value, k: &str, d: &str| {
            v.get(k).and_then(Value::as_str).unwrap_or(d).to_string()
        };
        Some(match r {
            Ok(v) if v.get("ok").and_then(Value::as_bool) == Some(true) => Ok(v),
            Ok(v) => Err((text(&v, "kind", "malformed"), text(&v, "error", "Unexpected reply"))),
            Err(e) => Err(("ipc".into(), e)),
        })
    }

    /// Open a fresh device session. Progress and the banner arrive as `script-repl` events;
    /// the state becomes Connected only once the banner has been received.
    pub fn ws_open(&self) {
        let s = *self;
        let mut session = 0;
        s.repl_ws.update(|w| session = w.begin(now_ms()));
        spawn_local(async move {
            if let Some(Err((kind, msg))) = s.repl_call("open", json!({ "session": session })).await {
                s.repl_ws.update(|w| {
                    w.fail(session, &kind, &msg);
                });
            }
        });
    }

    /// Detach from the socket and release the device's single REPL slot.
    pub fn ws_close(&self) {
        let s = *self;
        let session = s.repl_ws.with_untracked(|w| w.session);
        s.repl_ws.update(WsRepl::detach);
        if session != 0 {
            spawn_local(async move {
                let _ = s.repl_call("close", json!({ "session": session })).await;
            });
        }
    }

    /// The only way back from an error: a new session. Nothing typed earlier is resent.
    pub fn ws_reconnect(&self) {
        if !self.ready() || self.conn.with_untracked(|c| c.mode != "Http") {
            return;
        }
        if self.repl_ws.with_untracked(|w| w.phase.active()) {
            return;
        }
        self.ws_open();
    }

    /// Ctrl-C: sent only on the user's click. The firmware raises KeyboardInterrupt in the REPL
    /// line it is running; a running file script is stopped from its own controls.
    pub fn ws_interrupt(&self) {
        let s = *self;
        spawn_local(async move {
            let (phase, session) = s.repl_ws.with_untracked(|w| (w.phase, w.session));
            if phase != WsPhase::Connected {
                return;
            }
            if s.holds_file_slot() {
                s.notify("A script is running; stop it from the Scripts controls.", "orange");
                return;
            }
            if let Some(Err((kind, msg))) =
                s.repl_call("interrupt", json!({ "session": session })).await
            {
                s.ws_send_failed(session, &kind, &msg);
            }
        });
    }

    fn ws_send_failed(&self, session: u64, kind: &str, msg: &str) {
        match kind {
            "closed" | "stale" | "device_changed" | "not_connected" | "ipc" => {
                self.repl_ws.update(|w| {
                    w.fail(session, kind, msg);
                });
            }
            _ => self.notify(format!("REPL: {msg}"), "red"),
        }
    }

    async fn repl_submit_ws(&self, src: String) {
        let (phase, session) = self.repl_ws.with_untracked(|w| (w.phase, w.session));
        if phase != WsPhase::Connected {
            self.notify("The device REPL is not connected. Use Reconnect; nothing was sent.", "orange");
            return;
        }
        let blocker = ws_line_blocker(&src);
        self.repl.update(|r| r.push_history(&src));
        self.repl_sending.set(true);
        let Some(why) = blocker else {
            let r = self
                .repl_call("send_line", json!({ "session": session, "line": src }))
                .await;
            self.repl_sending.set(false);
            if let Some(Err((kind, msg))) = r {
                self.ws_send_failed(session, &kind, &msg);
            }
            return;
        };
        // The socket is line-buffered; this input runs as one persistent eval, and says so.
        self.repl_ws
            .update(|w| w.term.echo_local(&src, &format!("Wi-Fi REST eval: {why}")));
        let r = self
            .request("eval", json!({ "source": src, "persist": true }))
            .await;
        self.repl_sending.set(false);
        match r {
            None => {}
            Some(Ok(_)) => {
                sleep_ms(400).await;
                self.repl_ws.update(|w| {
                    if w.session == session {
                        w.term.end_with_prompt();
                    }
                });
            }
            Some(Err(e)) => self.repl_ws.update(|w| {
                if w.session == session {
                    w.term.marker(&format!("eval failed: {}", e.display()));
                    w.term.end_with_prompt();
                }
            }),
        }
    }

    // ---- REPL (USB: persistent eval; Wi-Fi: device WebSocket, see above) ----

    pub fn repl_submit(&self, raw: String) {
        let s = *self;
        spawn_local(async move {
            let src = sanitize_repl(&raw).trim_end().to_string();
            if src.trim().is_empty() {
                return;
            }
            if let Err(e) = check_source(&src) {
                s.notify(e, "red");
                return;
            }
            if !s.ready() {
                s.notify("Connect to a device to use the REPL.", "orange");
                return;
            }
            if s.holds_file_slot() {
                s.notify("The REPL is read-only while a script runs.", "orange");
                return;
            }
            if s.repl_sending.get_untracked() {
                return;
            }
            if s.conn.with_untracked(|c| c.mode == "Http") {
                s.repl_submit_ws(src).await;
                return;
            }
            s.repl.update(|r| {
                r.push_history(&src);
                r.start_entry(&src);
            });
            s.repl_sending.set(true);
            let r = s
                .request("eval", json!({ "source": src, "persist": true }))
                .await;
            s.repl_sending.set(false);
            match r {
                None => s.repl.update(|r| {
                    if let Some(e) = r.entries.last_mut() {
                        e.pending = false;
                        e.error = Some("Connection changed before the result was known.".into());
                    }
                }),
                Some(Ok(_)) => {
                    s.repl.update(|r| {
                        if let Some(e) = r.entries.last_mut() {
                            e.pending = false;
                        }
                    });
                    s.drain_logs().await;
                    sleep_ms(500).await;
                    s.drain_logs().await;
                }
                Some(Err(e)) => s.repl.update(|r| {
                    if let Some(last) = r.entries.last_mut() {
                        last.pending = false;
                        last.error = Some(e.display());
                    }
                }),
            }
        });
    }

    /// Reset the MicroPython VM (persistent globals). The device itself is never reset.
    pub fn reset_vm(&self) {
        let s = *self;
        spawn_local(async move {
            if !s.ready() {
                return;
            }
            match s.request("reset", json!({})).await {
                Some(Ok(_)) => {
                    s.log.update(|l| l.append_marker("Interpreter reset"));
                    s.refresh_status().await;
                }
                Some(Err(e)) => s.notify(format!("Reset failed: {}", e.display()), "red"),
                None => {}
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn names_follow_storage_rules() {
        assert!(is_valid_name("a.py"));
        assert!(is_valid_name(&format!("{}.py", "x".repeat(29))));
        assert!(!is_valid_name(&format!("{}.py", "x".repeat(30))));
        for bad in [
            ".hidden.py",
            "a.txt",
            "a b.py",
            "a/b.py",
            "",
            "py",
            "\u{e9}.py",
        ] {
            assert!(!is_valid_name(bad), "{bad}");
        }
        assert_eq!(normalize_name("  blink  ").as_deref(), Some("blink.py"));
        assert_eq!(normalize_name("blink.py").as_deref(), Some("blink.py"));
        assert_eq!(normalize_name("   "), None);
        assert_eq!(normalize_name("bad name"), None);
    }

    #[test]
    fn source_limits_count_bytes() {
        assert!(check_source("").is_err());
        assert!(check_source(&"a".repeat(BODY_MAX)).is_ok());
        assert!(check_source(&"a".repeat(BODY_MAX + 1)).is_err());
        // 2-byte characters: 16384 chars = 32768 bytes is the limit.
        assert!(check_source(&"\u{e9}".repeat(BODY_MAX / 2)).is_ok());
        assert!(check_source(&"\u{e9}".repeat(BODY_MAX / 2 + 1)).is_err());
    }

    #[test]
    fn replies_decode_to_typed_errors() {
        assert!(decode_reply(json!({"ok": true, "files": []})).is_ok());
        let busy = decode_reply(json!({"ok": false, "kind": "busy", "error": "'a.py' is running", "running": "a.py", "id": 7}))
            .unwrap_err();
        assert!(busy.is_busy());
        assert_eq!((busy.running.as_deref(), busy.id), (Some("a.py"), Some(7)));
        let lost = decode_reply(
            json!({"ok": false, "kind": "transport", "error": "timeout", "outcomeUnknown": true}),
        )
        .unwrap_err();
        assert!(lost.outcome_unknown);
        assert!(lost.display().contains("may have processed"));
        let old = decode_reply(
            json!({"ok": false, "kind": "unsupported", "error": "update the firmware"}),
        )
        .unwrap_err();
        assert!(old.display().starts_with("Firmware update needed"));
        assert_eq!(
            decode_reply(json!({"foo": 1})).unwrap_err().kind,
            "malformed"
        );
    }

    #[test]
    fn device_key_prefers_mac() {
        let v = json!({"device": {"transport": "usb", "address": "COM6", "mac": "AA:BB"}});
        assert_eq!(DeviceIdent::from_reply(&v).unwrap().key(), "mac:aa:bb");
        let v = json!({"device": {"transport": "http", "address": "http://1.2.3.4", "mac": null}});
        assert_eq!(
            DeviceIdent::from_reply(&v).unwrap().key(),
            "http:http://1.2.3.4"
        );
        assert!(DeviceIdent::from_reply(&json!({"ok": true})).is_none());
    }

    #[test]
    fn status_decodes_wire_keys_and_legacy() {
        let s = RunStatus::from_json(&json!({
            "running": true, "state": "running", "source": "autorun", "name": "boot.py",
            "startedAt": 12.5, "startedAtEpoch": false, "fileSlotId": 4, "fileSlotName": "boot.py",
            "lastScriptId": 3, "lastExit": "ok"
        }));
        assert!(s.is_active() && s.holds_file_slot());
        assert_eq!(s.source, SourceKind::Autorun);
        assert_eq!(s.display_name(), "boot.py");
        let legacy = RunStatus::from_json(&json!({"running": true}));
        assert_eq!(legacy.state, RunState::Running);
        assert_eq!(RunStatus::from_json(&json!({})).state, RunState::Idle);
        let done = RunStatus::from_json(&json!({"state": "done", "lastExit": "stopped"}));
        assert!(!done.is_active());
        assert_eq!(done.state_label(), "Stopped");
        let unknown = RunStatus::from_json(&json!({"state": "weird", "source": "x"}));
        assert_eq!(unknown.state, RunState::Unknown);
        assert_eq!(unknown.source, SourceKind::Unknown);
        let repl = RunStatus::from_json(&json!({"state": "running", "source": "repl"}));
        assert_eq!(repl.display_name(), "REPL");
        assert!(!repl.holds_file_slot());
    }

    #[test]
    fn background_file_slot_stays_active_after_state_done() {
        let s = RunStatus::from_json(&json!({"state": "idle", "fileSlotName": "bg.py"}));
        assert!(s.is_active());
    }

    #[test]
    fn autorun_texts() {
        let a = AutorunStatus::from_json(
            &json!({"enabled": true, "io12_high": false, "scriptName": "x.py"}),
        );
        assert_eq!(a.state_text(), "Suppressed: IO12 held low");
        let a = AutorunStatus::from_json(&json!({"enabled": true, "scriptName": "x.py"}));
        assert_eq!(a.state_text(), "Runs at next boot");
        let a = AutorunStatus::from_json(&json!({"ranThisBoot": true, "last_run_ok": true}));
        assert_eq!(a.state_text(), "Finished OK");
        assert_eq!(AutorunStatus::from_json(&json!({})).state_text(), "Off");
        assert_eq!(
            AutorunStatus::from_json(&json!({"has_script": true})).rows()[0].1,
            "autorun.py"
        );
    }

    #[test]
    fn elapsed_prefers_exact_sources() {
        let mut s = RunStatus::from_json(
            &json!({"state": "running", "startedAt": 100.0, "startedAtEpoch": true}),
        );
        assert_eq!(elapsed(&s, 130_000.0, None, None).unwrap().secs, 30.0);
        s.started_at_epoch = false;
        let launched = elapsed(&s, 5_000.0, Some(2_000.0), Some(4_000.0)).unwrap();
        assert!(launched.exact && launched.secs == 3.0);
        let seen = elapsed(&s, 5_000.0, None, Some(4_000.0)).unwrap();
        assert!(!seen.exact);
        assert_eq!(seen.text(), ">= 1s");
        assert!(elapsed(&s, 1.0, None, None).is_none());
        s.state = RunState::Done;
        assert!(elapsed(&s, 1.0, Some(0.0), None).is_none());
        assert_eq!(format_elapsed(59.9), "59s");
        assert_eq!(format_elapsed(61.0), "1m 01s");
        assert_eq!(format_elapsed(3720.0), "1h 02m");
    }

    #[test]
    fn poll_policy_matches_ios() {
        assert_eq!(
            PollPolicy::make(true, false, false),
            PollPolicy {
                status_s: 1.0,
                logs_s: Some(1.0)
            }
        );
        assert_eq!(PollPolicy::make(false, true, true).logs_s, Some(5.0));
        assert_eq!(PollPolicy::make(false, true, false).logs_s, None);
        assert_eq!(
            PollPolicy::make(false, false, true),
            PollPolicy {
                status_s: 5.0,
                logs_s: Some(5.0)
            }
        );
        assert_eq!(PollPolicy::make(false, false, false).logs_s, None);
    }

    fn page(data: &[u8], since: u64, next: u64, dropped: u64) -> LogPage {
        LogPage {
            data: data.to_vec(),
            since,
            next,
            dropped,
            restarted: next < since,
        }
    }

    #[test]
    fn log_lines_parse_structured_and_plain() {
        let l = LogLine::parse("1234 E mpy boom: bad", 1);
        assert_eq!(
            (l.ts_ms, l.level, l.source.as_str(), l.text.as_str()),
            (Some(1234), LogLevel::Error, "mpy", "boom: bad")
        );
        assert_eq!(l.timestamp(), "1.234");
        let plain = LogLine::parse("hello world", 2);
        assert!(plain.ts_ms.is_none() && plain.text == "hello world");
        assert_eq!(LogLine::parse("12 X mpy t", 3).text, "12 X mpy t");
        assert_eq!(LogLine::parse("12 I", 4).text, "12 I");
        assert_eq!(LogLine::parse("5 I sys", 5).text, "");
    }

    #[test]
    fn assembler_waits_for_newline_and_split_utf8() {
        let mut a = LogAssembler::default();
        let euro = "\u{20ac}".as_bytes(); // 3 bytes
        let mut first = b"1 I mpy ".to_vec();
        first.extend_from_slice(&euro[..2]);
        assert!(a.feed(&first).is_empty());
        let mut rest = euro[2..].to_vec();
        rest.extend_from_slice(b" ok\r\n2 I mpy two\n");
        let lines = a.feed(&rest);
        assert_eq!(
            lines,
            vec!["1 I mpy \u{20ac} ok".to_string(), "2 I mpy two".to_string()]
        );
        assert!(!a.has_partial());
        let runaway = vec![b'a'; LogAssembler::MAX_PARTIAL + 1];
        assert_eq!(a.feed(&runaway).len(), 1);
    }

    #[test]
    fn log_store_cursor_restart_and_gap() {
        let mut s = LogStore::default();
        let added = s.ingest(&page(b"1 I mpy a\n2 I mpy b\n", 0, 20, 0));
        assert_eq!(added.len(), 2);
        assert_eq!(s.cursor, 20);
        // Overflow after the first attach produces a marker, not at since == 0.
        s.ingest(&page(b"3 I mpy c\n", 20, 99, 40));
        assert!(s
            .lines
            .iter()
            .any(|l| l.is_marker && l.text.contains("40 bytes")));
        let mut fresh = LogStore::default();
        fresh.ingest(&page(b"1 I mpy a\n", 0, 10, 500));
        assert!(!fresh.lines.iter().any(|l| l.is_marker));
        // Reboot: cursor goes back to 0 and the view says so.
        s.ingest(&page(b"", 99, 5, 0));
        assert_eq!(s.cursor, 0);
        assert_eq!(s.lines.last().unwrap().text, "Device log restarted");
        // Local clear keeps the cursor.
        s.ingest(&page(b"9 I mpy z\n", 0, 11, 0));
        let cursor = s.cursor;
        s.clear();
        assert!(s.lines.is_empty());
        assert_eq!(s.cursor, cursor);
        s.reset();
        assert_eq!(s.cursor, 0);
    }

    #[test]
    fn log_store_is_bounded() {
        let mut s = LogStore::with_capacity(10);
        let mut data = String::new();
        for i in 0..25 {
            data.push_str(&format!("{i} I mpy line{i}\n"));
        }
        s.ingest(&page(data.as_bytes(), 0, data.len() as u64, 0));
        assert_eq!(s.lines.len(), 10);
        assert_eq!(s.lines[0].text, "line15");
    }

    #[test]
    fn log_filter_levels_and_text() {
        let err = LogLine::parse("1 E mpy Boom", 1);
        let dbg = LogLine::parse("2 D sys tick", 2);
        let mark = LogLine::marker("Started 'a.py'", 3);
        let mut f = LogFilter::default();
        assert!(f.matches(&err) && f.matches(&dbg));
        f.levels[LogLevel::Debug.index()] = false;
        assert!(f.matches(&err) && !f.matches(&dbg) && f.matches(&mark));
        f.query = "boom".into();
        assert!(f.matches(&err) && !f.matches(&mark));
        f.query = "SYS".into();
        f.levels = [true; 4];
        assert!(f.matches(&dbg));
    }

    #[test]
    fn page_json_decodes_base64() {
        // "1 I mpy x\n"
        let p = LogPage::from_json(&json!({"data": "MSBJIG1weSB4Cg==", "since": 0, "next": 10, "dropped": 0, "restarted": false}))
            .unwrap();
        assert_eq!(p.data, b"1 I mpy x\n");
        assert!(LogPage::from_json(&json!({"since": 0, "next": 1})).is_none());
        assert_eq!(b64_decode("aGk"), Some(b"hi".to_vec()));
        assert_eq!(b64_decode(""), Some(vec![]));
        assert!(b64_decode("a$b").is_none());
    }

    #[test]
    fn lint_results_are_classified() {
        let ok = Ok(json!({"ok": true, "valid": true}));
        assert_eq!(lint_banner(&ok), LintBanner::Ok);
        let bad = Ok(
            json!({"ok": true, "valid": false, "message": "File \"<stdin>\", line 3\nSyntaxError: invalid syntax"}),
        );
        assert!(matches!(
            lint_banner(&bad),
            LintBanner::Syntax { line: Some(3), .. }
        ));
        let busy = Err(ScriptError::new("busy", "Interpreter is busy"));
        assert!(matches!(
            lint_banner(&busy),
            LintBanner::Skipped {
                cause: LintCause::Busy,
                ..
            }
        ));
        let old = Err(ScriptError::new("unsupported", "update"));
        assert!(matches!(
            lint_banner(&old),
            LintBanner::Skipped {
                cause: LintCause::Unsupported,
                ..
            }
        ));
        let net = Err(ScriptError::new("transport", "timeout"));
        assert!(matches!(
            lint_banner(&net),
            LintBanner::Skipped {
                cause: LintCause::Network,
                ..
            }
        ));
        let unavailable = Err(ScriptError::new("unavailable", "queue full"));
        assert!(matches!(
            lint_banner(&unavailable),
            LintBanner::Skipped {
                cause: LintCause::Unavailable,
                ..
            }
        ));
        assert_eq!(parse_error_line("no location"), None);
    }

    #[test]
    fn buffers_stay_dirty_when_edited_during_save() {
        let mut s = Scripts::default();
        s.switch_device("dev");
        s.load_buffer("dev", "a.py", "v1".into(), false);
        s.ws_mut().open = Some("a.py".into());
        s.edit("a.py", "v2".into());
        let (rev, snap) = s.snapshot("dev", "a.py").unwrap();
        s.edit("a.py", "v3".into());
        s.save_finished("dev", "a.py", &snap);
        let b = s.buffer("a.py").unwrap();
        assert!(b.dirty(), "typing during upload stays dirty");
        assert!(!run_after_save_allowed(rev, b.rev));
        s.save_finished("dev", "a.py", "v3");
        assert!(!s.buffer("a.py").unwrap().dirty());
    }

    #[test]
    fn lint_record_is_tied_to_revision() {
        let mut s = Scripts::default();
        s.load_buffer("", "a.py", "x".into(), false);
        s.edit("a.py", "y".into());
        let rev = s.buffer("a.py").unwrap().rev;
        s.set_lint(
            "",
            "a.py",
            LintRecord {
                rev,
                banner: LintBanner::Ok,
                saved: true,
            },
        );
        s.edit("a.py", "z".into());
        let b = s.buffer("a.py").unwrap();
        assert_ne!(b.lint.as_ref().unwrap().rev, b.rev);
    }

    #[test]
    fn devices_are_isolated_and_survive_swaps() {
        let mut s = Scripts::default();
        s.switch_device("A");
        s.load_buffer("A", "x.py", "a".into(), false);
        s.ws_mut().open = Some("x.py".into());
        s.edit("x.py", "a-dirty".into());
        s.switch_device("B");
        assert!(s.buffer("x.py").is_none());
        s.load_buffer("B", "x.py", "b".into(), false);
        assert_eq!(s.buffer("x.py").unwrap().text, "b");
        assert_eq!(
            s.dirty_elsewhere(),
            vec![("A".to_string(), "x.py".to_string())]
        );
        s.switch_device("A");
        assert_eq!(s.buffer("x.py").unwrap().text, "a-dirty");
        assert_eq!(s.open_name().as_deref(), Some("x.py"));
    }

    #[test]
    fn offline_scratch_follows_to_first_device() {
        let mut s = Scripts::default();
        s.ws_mut().open = Some(SCRATCH.into());
        s.edit(SCRATCH, "print(1)".into());
        s.switch_device("dev1");
        assert_eq!(s.buffer(SCRATCH).unwrap().text, "print(1)");
        assert_eq!(s.open_name().as_deref(), Some(SCRATCH));
        assert!(s.devices.get("").unwrap().buffers.get(SCRATCH).is_none());
        // A second device does not steal it.
        s.switch_device("dev2");
        assert!(s.buffer(SCRATCH).is_none());
        s.promote_scratch("dev1", "n.py", "print(1)".into());
        assert!(s
            .devices
            .get("dev1")
            .unwrap()
            .files
            .contains(&"n.py".to_string()));
    }

    #[test]
    fn promoted_scratch_keeps_text_typed_during_upload() {
        let mut s = Scripts::default();
        s.switch_device("d");
        s.ws_mut().open = Some(SCRATCH.into());
        s.edit(SCRATCH, "a".into());
        s.edit(SCRATCH, "ab".into());
        s.promote_scratch("d", "n.py", "a".into());
        let b = s.buffer("n.py").unwrap();
        assert_eq!((b.text.as_str(), b.base.as_str()), ("ab", "a"));
        assert!(b.dirty());
        assert!(s.buffer(SCRATCH).is_none());
        assert_eq!(s.open_name().as_deref(), Some("n.py"));
    }

    #[test]
    fn revert_and_remove() {
        let mut s = Scripts::default();
        s.switch_device("d");
        s.load_buffer("d", "a.py", "one".into(), false);
        s.edit("a.py", "two".into());
        s.replace_buffer("d", "a.py", "one".into(), false);
        assert!(!s.is_dirty("a.py"));
        s.ws_mut().open = Some("a.py".into());
        s.remove_buffer("d", "a.py");
        assert!(s.open_name().is_none() && s.buffer("a.py").is_none());
    }

    #[test]
    fn repl_continuation_rules() {
        assert!(!repl_needs_more("1 + 1"));
        assert!(repl_needs_more("for i in range(3):"));
        assert!(repl_needs_more("for i in range(3):\n    print(i)"));
        assert!(!repl_needs_more("for i in range(3):\n    print(i)\n"));
        assert!(repl_needs_more("x = [1,\n 2"));
        assert!(!repl_needs_more("x = [1, 2]"));
        assert!(repl_needs_more("s = \"\"\"abc"));
        assert!(!repl_needs_more("s = '(' # ("));
        assert!(!repl_needs_more(""));
        assert_eq!(repl_next_indent("if x:"), "    ");
        assert_eq!(repl_next_indent("    y = 1"), "    ");
    }

    #[test]
    fn repl_history_walk_and_bounds() {
        let mut r = ReplState::default();
        for i in 0..(REPL_HISTORY_MAX + 5) {
            r.push_history(&format!("cmd{i}"));
        }
        assert_eq!(r.history.len(), REPL_HISTORY_MAX);
        r.push_history("cmd104");
        assert_eq!(r.history.len(), REPL_HISTORY_MAX);
        assert_eq!(r.history_prev("typing").as_deref(), Some("cmd104"));
        assert_eq!(r.history_prev("typing").as_deref(), Some("cmd103"));
        assert_eq!(r.history_next().as_deref(), Some("cmd104"));
        assert_eq!(r.history_next().as_deref(), Some("typing"));
        assert!(r.history_next().is_none());
        assert_eq!(sanitize_repl("\u{201C}x\u{201D} \u{2014}"), "\"x\" --");
    }

    #[test]
    fn repl_routes_only_vm_output() {
        let mut r = ReplState::default();
        r.route_output(&[LogLine::parse("1 I mpy lost", 1)]);
        r.start_entry("print(1)");
        r.route_output(&[
            LogLine::parse("2 I mpy 1", 2),
            LogLine::parse("3 I sys other", 3),
            LogLine::marker("m", 4),
        ]);
        assert_eq!(r.entries[0].output, vec!["1".to_string()]);
        assert_eq!(r.transcript(), ">>> print(1)\n1");
    }

    #[test]
    fn finish_markers() {
        let s = RunStatus::from_json(
            &json!({"state": "done", "lastExit": "error", "name": "a.py", "lastError": "Traceback\nboom"}),
        );
        assert_eq!(finish_marker("a.py", &s), "'a.py' failed: Traceback");
        let s = RunStatus::from_json(&json!({"state": "done", "lastExit": "ok", "name": "a.py"}));
        assert_eq!(finish_marker("a.py", &s), "Finished 'a.py'");
        let s =
            RunStatus::from_json(&json!({"state": "done", "lastExit": "stopped", "name": "a.py"}));
        assert_eq!(finish_marker("a.py", &s), "Stopped 'a.py'");
    }
}

#[cfg(test)]
mod ws_repl_tests {
    use super::*;

    fn ev(session: u64, seq: u64, kind: &str, extra: Value) -> Value {
        let mut v = json!({"session": session, "generation": 3, "seq": seq, "kind": kind});
        for (k, x) in extra.as_object().unwrap() {
            v[k] = x.clone();
        }
        v
    }

    fn started() -> (WsRepl, u64) {
        let mut w = WsRepl::default();
        let id = w.begin(1000.0);
        (w, id)
    }

    #[test]
    fn terminal_renders_device_echo_backspace_and_prompts() {
        let mut t = Term::default();
        t.feed("MicroPython REPL ready. Type and press Enter.\r\n>>> ");
        t.feed("prinx\u{8} \u{8}t(1)\r\n");
        t.feed("\r\n1\r\n>>> ");
        assert_eq!(
            t.text,
            "MicroPython REPL ready. Type and press Enter.\n>>> print(1)\n\n1\n>>> "
        );
        t.feed("\u{8} \u{8}");
        assert!(t.text.ends_with(">>>"), "backspace never eats a previous line");
        let mut t = Term::default();
        t.feed("a\n\u{8} \u{8}\u{1b}[0m\u{3}b");
        assert_eq!(t.text, "a\n[0mb");
    }

    #[test]
    fn terminal_is_bounded_on_a_line_boundary() {
        let mut t = Term::default();
        for i in 0..4000 {
            t.feed(&format!("line number {i}\n"));
        }
        assert!(t.text.len() <= TERM_MAX && t.truncated);
        assert!(t.text.starts_with("line number "));
        assert!(t.text.ends_with("line number 3999\n"));
    }

    #[test]
    fn local_echo_continues_the_device_prompt_and_marks_the_path() {
        let mut t = Term::default();
        t.feed("hi\r\n>>> ");
        t.echo_local("for i in range(2):\n    print(i)", "Wi-Fi REST eval: multi-line input");
        assert_eq!(
            t.text,
            "hi\n>>> for i in range(2):\n...     print(i)\n[Wi-Fi REST eval: multi-line input]\n"
        );
        t.end_with_prompt();
        t.end_with_prompt();
        assert!(t.text.ends_with("]\n>>> ") && !t.text.ends_with(">>> >>> "));
    }

    #[test]
    fn single_lines_only_use_the_socket() {
        assert_eq!(ws_line_blocker("print(1)"), None);
        assert_eq!(ws_line_blocker("a\nb"), Some("multi-line input"));
        assert_eq!(ws_line_blocker(&"a".repeat(512)), Some("line longer than 511 bytes"));
        assert_eq!(ws_line_blocker(&"a".repeat(511)), None);
        assert!(ws_line_blocker("caf\u{e9}").is_some());
    }

    #[test]
    fn only_the_current_session_and_generation_are_applied() {
        let (mut w, id) = started();
        assert_eq!(w.phase, WsPhase::Connecting);
        assert!(w.apply(&ev(id, 1, "state", json!({"state": "authenticating"}))));
        assert!(!w.apply(&ev(id + 1, 2, "state", json!({"state": "connected"}))), "other session");
        assert!(!w.apply(&ev(id, 2, "output", json!({"data": "x", "generation": 4}))), "other generation");
        assert_eq!(w.phase, WsPhase::Authenticating);
        assert!(w.apply(&ev(id, 2, "state", json!({"state": "connected"}))));
        assert!(!w.apply(&ev(id, 2, "output", json!({"data": "dup"}))), "repeated seq");
        assert!(w.apply(&ev(id, 3, "output", json!({"data": "ok\r\n"}))));
        assert_eq!(w.term.text, "ok\n");
        assert_eq!(w.phase, WsPhase::Connected);
    }

    #[test]
    fn connected_needs_the_banner_event_not_a_send() {
        let (mut w, id) = started();
        assert!(w.apply(&ev(id, 1, "state", json!({"state": "authenticating"}))));
        assert_ne!(w.phase, WsPhase::Connected);
        assert!(!w.apply(&ev(id, 2, "bogus", json!({}))));
        assert!(!w.apply(&ev(id, 3, "state", json!({"state": "mystery"}))));
        assert_eq!(w.phase, WsPhase::Authenticating);
    }

    #[test]
    fn close_events_record_the_failure_and_end_the_session() {
        let (mut w, id) = started();
        assert!(w.apply(&ev(id, 1, "closed", json!({"reason": "unauthorized", "code": 4001, "message": "rejected"}))));
        assert_eq!((w.phase, w.status_text(), w.tone()), (WsPhase::Closed, "Unauthorized", "red"));
        assert!(!w.apply(&ev(id, 2, "output", json!({"data": "late"}))), "nothing after close");
        assert!(w.term.text.contains("[REPL closed: rejected]"));

        let (mut w, id) = started();
        w.apply(&ev(id, 1, "closed", json!({"reason": "busy", "message": "held"})));
        assert_eq!((w.status_text(), w.tone()), ("REPL busy", "orange"));

        let (mut w, id) = started();
        w.apply(&ev(id, 1, "closed", json!({"reason": "local", "message": "REPL closed"})));
        assert!(w.fail.is_none());
    }

    #[test]
    fn reconnect_uses_a_newer_session_and_stale_events_are_dropped() {
        let (mut w, old) = started();
        assert!(w.apply(&ev(old, 1, "closed", json!({"reason": "remote", "message": "gone"}))));
        let new = w.begin(500.0);
        assert!(new > old, "ids grow even when the clock is behind");
        assert!(w.fail.is_none() && w.phase == WsPhase::Connecting);
        assert!(!w.apply(&ev(old, 5, "output", json!({"data": "from the old socket"}))));
        assert!(w.apply(&ev(new, 1, "state", json!({"state": "connected"}))));
        assert!(!w.term.text.contains("old socket"));
    }

    #[test]
    fn link_change_and_detach_drop_state_but_keep_the_id_floor() {
        let (mut w, id) = started();
        w.apply(&ev(id, 1, "output", json!({"data": "text"})));
        w.detach();
        assert!(!w.apply(&ev(id, 2, "output", json!({"data": "more"}))));
        let id2 = w.begin(0.0);
        w.reset_link();
        assert!(w.term.text.is_empty() && w.session == 0 && w.phase == WsPhase::Idle);
        assert!(w.begin(0.0) > id2);
    }

    #[test]
    fn failures_only_apply_to_the_live_session() {
        let (mut w, id) = started();
        assert!(!w.fail(id + 1, "stale", "x"));
        assert!(w.fail(id, "unauthorized", "no token"));
        assert!(!w.fail(id, "again", "y"), "already closed");
        assert_eq!(w.fail.as_ref().unwrap().reason, "unauthorized");
    }
}