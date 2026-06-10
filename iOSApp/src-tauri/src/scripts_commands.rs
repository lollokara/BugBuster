use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, State};
use tokio::sync::{mpsc, Mutex as TokioMutex};
use tokio_tungstenite::{connect_async, tungstenite::protocol::Message};
use futures::{SinkExt, StreamExt};

use crate::connection_manager::ConnectionManager;

type CmdResult<T> = Result<T, String>;

fn map_err(e: impl std::fmt::Display) -> String {
    e.to_string()
}

// -----------------------------------------------------------------------------
// Shared types (mirrored in tauri_bridge.rs on the frontend)
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScriptFileInfo {
    pub name: String,
    pub size: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ScriptStorageInfo {
    pub total_bytes: u64,
    pub used_bytes: u64,
    pub free_bytes: u64,
    pub script_count: u32,
    pub max_script_bytes: u32,
    pub max_scripts: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ScriptStatus {
    pub running: bool,
    pub current_script_id: u32,
    pub total_runs: u32,
    pub total_errors: u32,
    pub last_error: Option<String>,
    pub mode: String,
    pub idle_for_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ScriptLogsResult {
    pub text: String,
    pub next_cursor: u64,
}

// -----------------------------------------------------------------------------
// REPL state — stores the sender half of the WS write channel
// -----------------------------------------------------------------------------

pub struct ReplState {
    tx: TokioMutex<Option<mpsc::UnboundedSender<String>>>,
}

impl ReplState {
    pub fn new() -> Self {
        Self {
            tx: TokioMutex::new(None),
        }
    }
}

// -----------------------------------------------------------------------------
// File manager commands
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn scripts_list_files(mgr: State<'_, ConnectionManager>) -> CmdResult<Vec<ScriptFileInfo>> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    let resp = client
        .get(format!("{}/api/scripts/files", base))
        .header("X-BugBuster-Admin-Token", &token)
        .send()
        .await
        .map_err(map_err)?;
    let json: serde_json::Value = resp.json().await.map_err(map_err)?;
    let files = json["files"]
        .as_array()
        .map(|arr| {
            arr.iter()
                .filter_map(|v| {
                    let name = v.as_str().map(|s| s.to_string()).or_else(|| {
                        v.get("name").and_then(|n| n.as_str()).map(|s| s.to_string())
                    })?;
                    let size = v
                        .get("size")
                        .and_then(|s| s.as_u64())
                        .unwrap_or(0) as u32;
                    Some(ScriptFileInfo { name, size })
                })
                .collect()
        })
        .unwrap_or_default();
    Ok(files)
}

#[tauri::command]
pub async fn scripts_get_file(
    name: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<String> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    let text = client
        .get(format!("{}/api/scripts/files/get", base))
        .query(&[("name", &name)])
        .header("X-BugBuster-Admin-Token", &token)
        .send()
        .await
        .map_err(map_err)?
        .text()
        .await
        .map_err(map_err)?;
    Ok(text)
}

#[tauri::command]
pub async fn scripts_save_file(
    name: String,
    source: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    let resp: serde_json::Value = client
        .post(format!("{}/api/scripts/files", base))
        .query(&[("name", &name)])
        .header("X-BugBuster-Admin-Token", &token)
        .header("Content-Type", "text/plain; charset=utf-8")
        .body(source)
        .send()
        .await
        .map_err(map_err)?
        .json()
        .await
        .map_err(map_err)?;
    if resp["ok"].as_bool() == Some(false) {
        let msg = resp["err"].as_str().unwrap_or("save failed").to_string();
        return Err(msg);
    }
    Ok(())
}

#[tauri::command]
pub async fn scripts_delete_file(
    name: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    let resp: serde_json::Value = client
        .delete(format!("{}/api/scripts/files", base))
        .query(&[("name", &name)])
        .header("X-BugBuster-Admin-Token", &token)
        .send()
        .await
        .map_err(map_err)?
        .json()
        .await
        .map_err(map_err)?;
    if resp["ok"].as_bool() == Some(false) {
        let msg = resp["err"].as_str().unwrap_or("delete failed").to_string();
        return Err(msg);
    }
    Ok(())
}

#[tauri::command]
pub async fn scripts_get_storage(mgr: State<'_, ConnectionManager>) -> CmdResult<ScriptStorageInfo> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    let info = client
        .get(format!("{}/api/scripts/storage", base))
        .header("X-BugBuster-Admin-Token", &token)
        .send()
        .await
        .map_err(map_err)?
        .json::<ScriptStorageInfo>()
        .await
        .map_err(map_err)?;
    Ok(info)
}

// -----------------------------------------------------------------------------
// Execution commands
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn scripts_run_file(
    name: String,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<()> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    let resp: serde_json::Value = client
        .post(format!("{}/api/scripts/run-file", base))
        .query(&[("name", &name)])
        .header("X-BugBuster-Admin-Token", &token)
        .send()
        .await
        .map_err(map_err)?
        .json()
        .await
        .map_err(map_err)?;
    if resp["ok"].as_bool() == Some(false) {
        let msg = resp["err"].as_str().unwrap_or("run failed").to_string();
        return Err(msg);
    }
    Ok(())
}

#[tauri::command]
pub async fn scripts_stop(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    client
        .post(format!("{}/api/scripts/stop", base))
        .header("X-BugBuster-Admin-Token", &token)
        .send()
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn scripts_reset_vm(mgr: State<'_, ConnectionManager>) -> CmdResult<()> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    client
        .post(format!("{}/api/scripts/reset", base))
        .header("X-BugBuster-Admin-Token", &token)
        .send()
        .await
        .map_err(map_err)?;
    Ok(())
}

#[tauri::command]
pub async fn scripts_get_status(mgr: State<'_, ConnectionManager>) -> CmdResult<ScriptStatus> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    let status = client
        .get(format!("{}/api/scripts/status", base))
        .header("X-BugBuster-Admin-Token", &token)
        .send()
        .await
        .map_err(map_err)?
        .json::<ScriptStatus>()
        .await
        .map_err(map_err)?;
    Ok(status)
}

#[tauri::command]
pub async fn scripts_get_logs(
    since: Option<u64>,
    mgr: State<'_, ConnectionManager>,
) -> CmdResult<ScriptLogsResult> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;
    let client = reqwest::Client::new();
    let mut req = client
        .get(format!("{}/api/scripts/logs", base))
        .header("X-BugBuster-Admin-Token", &token);
    if let Some(cursor) = since {
        req = req.query(&[("since", cursor.to_string())]);
    }
    let resp = req.send().await.map_err(map_err)?;
    let next_cursor = resp
        .headers()
        .get("X-BugBuster-Log-Next")
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.parse::<u64>().ok())
        .unwrap_or(0);
    let text = resp.text().await.map_err(map_err)?;
    Ok(ScriptLogsResult { text, next_cursor })
}

// -----------------------------------------------------------------------------
// REPL WebSocket commands
// -----------------------------------------------------------------------------

#[tauri::command]
pub async fn scripts_repl_connect(
    mgr: State<'_, ConnectionManager>,
    repl: State<'_, ReplState>,
    app: AppHandle,
) -> CmdResult<()> {
    let (base, token) = mgr.get_http_base_and_token().map_err(map_err)?;

    // Disconnect any existing session first.
    {
        let mut tx = repl.tx.lock().await;
        *tx = None;
    }

    let ws_url = base
        .replacen("https://", "wss://", 1)
        .replacen("http://", "ws://", 1);
    let ws_url = format!("{}/api/scripts/repl/ws", ws_url);

    let (ws_stream, _) = connect_async(&ws_url).await.map_err(map_err)?;
    let (mut write, mut read) = ws_stream.split();

    // Send auth token as first frame.
    write
        .send(Message::Text(token.clone().into()))
        .await
        .map_err(map_err)?;

    let (tx, mut rx) = mpsc::unbounded_channel::<String>();

    // Write task: forward messages from the channel to the WS.
    tokio::spawn(async move {
        while let Some(msg) = rx.recv().await {
            if write.send(Message::Text(msg.into())).await.is_err() {
                break;
            }
        }
        let _ = write.close().await;
    });

    // Read task: forward WS messages to frontend as Tauri events.
    let app_read = app.clone();
    tokio::spawn(async move {
        while let Some(msg) = read.next().await {
            match msg {
                Ok(Message::Text(text)) => {
                    let _ = app_read.emit("repl-output", text.as_str());
                }
                Ok(Message::Binary(bytes)) => {
                    if let Ok(s) = String::from_utf8(bytes) {
                        let _ = app_read.emit("repl-output", s);
                    }
                }
                Ok(Message::Close(_)) | Err(_) => break,
                _ => {}
            }
        }
        let _ = app_read.emit("repl-disconnected", ());
    });

    {
        let mut guard = repl.tx.lock().await;
        *guard = Some(tx);
    }

    Ok(())
}

#[tauri::command]
pub async fn scripts_repl_send(
    text: String,
    repl: State<'_, ReplState>,
) -> CmdResult<()> {
    let guard = repl.tx.lock().await;
    match &*guard {
        Some(tx) => tx.send(text).map_err(map_err),
        None => Err("REPL not connected".to_string()),
    }
}

#[tauri::command]
pub async fn scripts_repl_disconnect(repl: State<'_, ReplState>) -> CmdResult<()> {
    let mut guard = repl.tx.lock().await;
    *guard = None;
    Ok(())
}
