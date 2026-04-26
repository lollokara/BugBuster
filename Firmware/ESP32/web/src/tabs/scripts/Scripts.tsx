// =============================================================================
// Scripts tab — MicroPython on-device scripting (V2-F).
//
// Endpoints consumed:
//   GET  /api/scripts/files              — list files
//   POST /api/scripts/files?name=X       — upload (raw body)
//   GET  /api/scripts/files/get?name=X   — download (raw text)
//   DELETE /api/scripts/files?name=X     — delete
//   POST /api/scripts/eval?persist=t|f   — eval (raw body)
//   POST /api/scripts/run-file?name=X    — run saved file
//   POST /api/scripts/stop               — stop running script
//   POST /api/scripts/reset              — reset MicroPython VM
//   GET  /api/scripts/status             — VM status (polls 1 s)
//   GET  /api/scripts/logs               — drain log ring (polls 500 ms)
//   GET  /api/scripts/autorun/status     — autorun info
//   POST /api/scripts/autorun/enable?name=X
//   POST /api/scripts/autorun/disable
//
// V2-B: <Repl /> here when V2-B lands
// =============================================================================

import { useEffect, useRef, useState, useCallback } from "preact/hooks";
import { EditorView, lineNumbers, keymap } from "@codemirror/view";
import { EditorState } from "@codemirror/state";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { python } from "@codemirror/lang-python";
import { oneDark } from "@codemirror/theme-one-dark";
import {
  ADMIN_TOKEN_HEADER,
  getCachedToken,
  PairingRequiredError,
} from "../../api/client";
import { deviceMac } from "../../state/signals";

// ---------------------------------------------------------------------------
// Auth helper — raw fetch with admin token attached.
// ---------------------------------------------------------------------------

function authHeaders(mac: string | null): Record<string, string> {
  const h: Record<string, string> = {};
  if (mac) {
    const tok = getCachedToken(mac);
    if (!tok) {
      window.dispatchEvent(new CustomEvent("bb:pairing-required"));
      throw new PairingRequiredError();
    }
    h[ADMIN_TOKEN_HEADER] = tok;
  }
  return h;
}

async function apiFetch(path: string, init: RequestInit = {}): Promise<Response> {
  const mac = deviceMac.value;
  const res = await fetch(path, {
    ...init,
    headers: {
      ...(init.headers as Record<string, string> | undefined),
      ...authHeaders(mac),
    },
  });
  if (res.status === 401) {
    if (mac) {
      const { clearCachedToken } = await import("../../api/client");
      clearCachedToken(mac);
    }
    window.dispatchEvent(new CustomEvent("bb:pairing-required"));
    throw new PairingRequiredError();
  }
  return res;
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

interface ScriptStatus {
  mode?: string;            // "ephemeral" | "persistent"
  globalsBytes?: number;
  watermarkSoftHit?: boolean;
  autoResetCount?: number;
  lastError?: string;
  running?: boolean;
  scriptId?: number;
}

interface AutorunStatus {
  enabled?: boolean;
  name?: string;
}

// ---------------------------------------------------------------------------
// CodeMirror editor sub-component
// ---------------------------------------------------------------------------

interface EditorProps {
  initialDoc: string;
  onDocChange: (doc: string) => void;
  editorViewRef: React.MutableRefObject<EditorView | null>;
}

function CodeEditor({ initialDoc, onDocChange, editorViewRef }: EditorProps) {
  const containerRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!containerRef.current) return;

    const view = new EditorView({
      state: EditorState.create({
        doc: initialDoc,
        extensions: [
          history(),
          keymap.of([...defaultKeymap, ...historyKeymap]),
          lineNumbers(),
          python(),
          oneDark,
          EditorView.updateListener.of((update) => {
            if (update.docChanged) {
              onDocChange(update.state.doc.toString());
            }
          }),
          EditorView.theme({
            "&": { height: "100%", fontSize: "13px" },
            ".cm-scroller": { overflow: "auto" },
          }),
        ],
      }),
      parent: containerRef.current,
    });

    editorViewRef.current = view;

    return () => {
      view.destroy();
      editorViewRef.current = null;
    };
    // initialDoc intentionally excluded — we update the view content externally
    // via dispatch when the file changes rather than recreating the editor.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <div
      ref={containerRef}
      style={{
        flex: 1,
        minHeight: 0,
        overflow: "hidden",
        border: "1px solid var(--border)",
        borderRadius: "var(--radius-sm)",
      }}
    />
  );
}

// ---------------------------------------------------------------------------
// Status badge
// ---------------------------------------------------------------------------

function StatusBadge({ status }: { status: ScriptStatus | null }) {
  if (!status) {
    return <span class="text-dim" style={{ fontSize: "0.78rem" }}>—</span>;
  }
  const kb = status.globalsBytes != null
    ? (status.globalsBytes / 1024).toFixed(1) + " KB"
    : "?";
  return (
    <span style={{ fontSize: "0.78rem", display: "flex", gap: "10px", alignItems: "center", flexWrap: "wrap" }}>
      <span class="uppercase-tag">{status.mode ?? "—"}</span>
      <span class="mono text-dim">heap: {kb}</span>
      {status.autoResetCount != null && (
        <span class="mono text-dim">resets: {status.autoResetCount}</span>
      )}
      {status.watermarkSoftHit && (
        <span style={{ color: "var(--rose)", fontWeight: 600 }}>MEM!</span>
      )}
      {status.running && (
        <span style={{ color: "var(--green)" }}>running</span>
      )}
      {status.lastError && (
        <span style={{ color: "var(--rose)", maxWidth: "300px", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }} title={status.lastError}>
          err: {status.lastError}
        </span>
      )}
    </span>
  );
}

// ---------------------------------------------------------------------------
// Main Scripts component
// ---------------------------------------------------------------------------

export function Scripts() {
  // File list
  const [files, setFiles] = useState<string[]>([]);
  const [selectedFile, setSelectedFile] = useState<string | null>(null);
  // "unsaved" means a new file not yet on the device
  const [isUnsaved, setIsUnsaved] = useState(false);
  const [isDirty, setIsDirty] = useState(false);

  // Editor state (tracked outside of CodeMirror for toolbar logic)
  const editorViewRef = useRef<EditorView | null>(null);
  const [editorDoc, setEditorDoc] = useState("");
  const [editorKey, setEditorKey] = useState(0); // bump to recreate CM instance

  // Status / logs / autorun
  const [vmStatus, setVmStatus] = useState<ScriptStatus | null>(null);
  const [autorunStatus, setAutorunStatus] = useState<AutorunStatus | null>(null);
  const [logs, setLogs] = useState<string>("");
  const logsEndRef = useRef<HTMLDivElement>(null);

  // UI feedback
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [persistEval, setPersistEval] = useState(false);

  // New-file name input
  const [newFileName, setNewFileName] = useState("");

  // -------------------------------------------------------------------------
  // Fetch file list
  // -------------------------------------------------------------------------

  const fetchFiles = useCallback(async () => {
    try {
      const res = await apiFetch("/api/scripts/files");
      const data = await res.json() as { files?: string[] };
      setFiles(Array.isArray(data.files) ? data.files : []);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        console.warn("scripts: list failed", e);
      }
    }
  }, []);

  useEffect(() => {
    fetchFiles();
  }, [fetchFiles]);

  // -------------------------------------------------------------------------
  // Load a file into the editor
  // -------------------------------------------------------------------------

  const loadFile = useCallback(async (name: string) => {
    setBusy("load");
    setError(null);
    try {
      const res = await apiFetch(`/api/scripts/files/get?name=${encodeURIComponent(name)}`);
      const text = await res.text();
      setEditorDoc(text);
      setEditorKey((k) => k + 1); // recreate CodeMirror with fresh doc
      setSelectedFile(name);
      setIsUnsaved(false);
      setIsDirty(false);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  }, []);

  const handleFileSelect = (name: string) => {
    if (isDirty && !window.confirm("You have unsaved changes. Discard them?")) return;
    loadFile(name);
  };

  // -------------------------------------------------------------------------
  // New file
  // -------------------------------------------------------------------------

  const handleNewFile = () => {
    if (isDirty && !window.confirm("You have unsaved changes. Discard them?")) return;
    const name = newFileName.trim() || "untitled.py";
    setSelectedFile(name);
    setIsUnsaved(true);
    setIsDirty(false);
    setEditorDoc("");
    setEditorKey((k) => k + 1);
    setNewFileName("");
    setError(null);
  };

  // -------------------------------------------------------------------------
  // Save
  // -------------------------------------------------------------------------

  const handleSave = async () => {
    if (!selectedFile) return;
    const code = editorViewRef.current?.state.doc.toString() ?? editorDoc;
    setBusy("save");
    setError(null);
    try {
      await apiFetch(`/api/scripts/files?name=${encodeURIComponent(selectedFile)}`, {
        method: "POST",
        headers: { "Content-Type": "text/plain" },
        body: code,
      });
      setIsUnsaved(false);
      setIsDirty(false);
      await fetchFiles();
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  // -------------------------------------------------------------------------
  // Run file
  // -------------------------------------------------------------------------

  const handleRunFile = async () => {
    if (!selectedFile || isDirty || isUnsaved) return;
    setBusy("run");
    setError(null);
    try {
      const res = await apiFetch(`/api/scripts/run-file?name=${encodeURIComponent(selectedFile)}`, {
        method: "POST",
      });
      const data = await res.json() as { ok?: boolean; id?: number };
      if (!data.ok) setError("Run failed");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  // -------------------------------------------------------------------------
  // Run (eval)
  // -------------------------------------------------------------------------

  const handleEval = async () => {
    const code = editorViewRef.current?.state.doc.toString() ?? editorDoc;
    setBusy("eval");
    setError(null);
    try {
      const res = await apiFetch(`/api/scripts/eval?persist=${persistEval ? "true" : "false"}`, {
        method: "POST",
        headers: { "Content-Type": "text/plain" },
        body: code,
      });
      const data = await res.json() as { ok?: boolean; id?: number };
      if (!data.ok) setError("Eval failed");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  // -------------------------------------------------------------------------
  // Stop
  // -------------------------------------------------------------------------

  const handleStop = async () => {
    setBusy("stop");
    setError(null);
    try {
      await apiFetch("/api/scripts/stop", { method: "POST" });
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  // -------------------------------------------------------------------------
  // Reset VM
  // -------------------------------------------------------------------------

  const handleReset = async () => {
    if (!window.confirm("Reset the MicroPython VM? All runtime state will be cleared.")) return;
    setBusy("reset");
    setError(null);
    try {
      await apiFetch("/api/scripts/reset", { method: "POST" });
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  // -------------------------------------------------------------------------
  // Autorun
  // -------------------------------------------------------------------------

  const handleSetAutorun = async () => {
    if (!selectedFile || isUnsaved) return;
    setBusy("autorun-enable");
    setError(null);
    try {
      await apiFetch(`/api/scripts/autorun/enable?name=${encodeURIComponent(selectedFile)}`, {
        method: "POST",
      });
      await fetchAutorunStatus();
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  const handleDisableAutorun = async () => {
    setBusy("autorun-disable");
    setError(null);
    try {
      await apiFetch("/api/scripts/autorun/disable", { method: "POST" });
      await fetchAutorunStatus();
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  const fetchAutorunStatus = async () => {
    try {
      const res = await apiFetch("/api/scripts/autorun/status");
      const data = await res.json() as AutorunStatus;
      setAutorunStatus(data);
    } catch {
      /* ignore */
    }
  };

  // -------------------------------------------------------------------------
  // Delete
  // -------------------------------------------------------------------------

  const handleDelete = async () => {
    if (!selectedFile || isUnsaved) return;
    if (!window.confirm(`Delete "${selectedFile}"?`)) return;
    setBusy("delete");
    setError(null);
    try {
      await apiFetch(`/api/scripts/files?name=${encodeURIComponent(selectedFile)}`, {
        method: "DELETE",
      });
      setSelectedFile(null);
      setIsUnsaved(false);
      setIsDirty(false);
      setEditorDoc("");
      setEditorKey((k) => k + 1);
      await fetchFiles();
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setError(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  // -------------------------------------------------------------------------
  // Status polling (1 s)
  // -------------------------------------------------------------------------

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const res = await apiFetch("/api/scripts/status");
        const data = await res.json() as ScriptStatus;
        if (alive) setVmStatus(data);
      } catch {
        /* ignore */
      }
      if (alive) setTimeout(tick, 1000);
    };
    tick();
    // Also fetch autorun status once on mount
    fetchAutorunStatus();
    return () => { alive = false; };
  }, []);

  // -------------------------------------------------------------------------
  // Log polling (500 ms) — drain semantics: accumulate locally
  // -------------------------------------------------------------------------

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const res = await apiFetch("/api/scripts/logs");
        const text = await res.text();
        if (alive && text.length > 0) {
          setLogs((prev) => prev + text);
        }
      } catch {
        /* ignore */
      }
      if (alive) setTimeout(tick, 500);
    };
    tick();
    return () => { alive = false; };
  }, []);

  // Auto-scroll logs
  useEffect(() => {
    if (logsEndRef.current) {
      logsEndRef.current.scrollIntoView({ behavior: "smooth" });
    }
  }, [logs]);

  // -------------------------------------------------------------------------
  // Track editor doc changes for dirty flag
  // -------------------------------------------------------------------------

  const handleDocChange = useCallback((doc: string) => {
    setEditorDoc(doc);
    setIsDirty(true);
  }, []);

  // -------------------------------------------------------------------------
  // Derived state
  // -------------------------------------------------------------------------

  const canRunFile = !!selectedFile && !isUnsaved && !isDirty && busy === null;
  const canSave = !!selectedFile && busy === null;
  const canDelete = !!selectedFile && !isUnsaved && busy === null;
  const canSetAutorun = !!selectedFile && !isUnsaved && busy === null;
  const autorunActive = !!autorunStatus?.enabled;
  const autorunName = autorunStatus?.name ?? null;

  // -------------------------------------------------------------------------
  // Render
  // -------------------------------------------------------------------------

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: "12px", height: "calc(100vh - 110px)", minHeight: "500px" }}>

      {/* Header row: title + VM status */}
      <div class="glass-card" style={{ padding: "10px 16px" }}>
        <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", flexWrap: "wrap", gap: "8px" }}>
          <div style={{ display: "flex", alignItems: "center", gap: "10px" }}>
            <span class="card-title" style={{ marginBottom: 0 }}>MicroPython Scripts</span>
            {autorunActive && (
              <span class="uppercase-tag" style={{ color: "var(--green)", fontSize: "0.7rem" }}>
                autorun: {autorunName ?? "?"}
              </span>
            )}
          </div>
          <StatusBadge status={vmStatus} />
        </div>
      </div>

      {/* Main editor area */}
      <div style={{ display: "flex", gap: "12px", flex: 1, minHeight: 0 }}>

        {/* Left pane: file tree */}
        <div class="glass-card" style={{ width: "180px", flexShrink: 0, display: "flex", flexDirection: "column", gap: "6px", overflow: "hidden" }}>
          <div class="card-title" style={{ marginBottom: "6px" }}>Files</div>

          {/* New file controls */}
          <div style={{ display: "flex", flexDirection: "column", gap: "4px" }}>
            <input
              class="input"
              placeholder="new-file.py"
              value={newFileName}
              style={{ fontSize: "0.78rem", padding: "4px 8px" }}
              onInput={(e) => setNewFileName((e.currentTarget as HTMLInputElement).value)}
              onKeyDown={(e) => { if (e.key === "Enter") handleNewFile(); }}
            />
            <button class="btn" style={{ fontSize: "0.78rem", padding: "4px 8px" }} onClick={handleNewFile}>
              + New
            </button>
          </div>

          <div style={{ width: "100%", height: "1px", background: "var(--border)", margin: "2px 0" }} />

          {/* File list */}
          <div style={{ overflowY: "auto", flex: 1 }}>
            {files.length === 0 && (
              <div class="text-dim" style={{ fontSize: "0.78rem", padding: "4px 0" }}>No files</div>
            )}
            {files.map((f) => (
              <div
                key={f}
                onClick={() => handleFileSelect(f)}
                style={{
                  padding: "5px 8px",
                  borderRadius: "6px",
                  cursor: "pointer",
                  fontSize: "0.82rem",
                  fontFamily: "var(--mono, monospace)",
                  background: selectedFile === f && !isUnsaved ? "rgba(59,130,246,0.12)" : "transparent",
                  color: selectedFile === f && !isUnsaved ? "var(--blue)" : "var(--text-dim)",
                  overflow: "hidden",
                  textOverflow: "ellipsis",
                  whiteSpace: "nowrap",
                  transition: "background 0.15s",
                }}
              >
                {f}
              </div>
            ))}
          </div>
        </div>

        {/* Right pane: toolbar + editor */}
        <div style={{ display: "flex", flexDirection: "column", flex: 1, minWidth: 0, gap: "8px" }}>

          {/* Toolbar */}
          <div class="glass-card" style={{ padding: "8px 12px" }}>
            <div style={{ display: "flex", flexWrap: "wrap", gap: "6px", alignItems: "center" }}>

              {/* File name display */}
              {selectedFile && (
                <span class="mono" style={{ fontSize: "0.82rem", color: isDirty ? "var(--amber)" : "var(--text-dim)", marginRight: "4px" }}>
                  {selectedFile}{isDirty ? " *" : ""}{isUnsaved ? " (new)" : ""}
                </span>
              )}

              <button
                class="btn primary"
                disabled={!canSave}
                onClick={handleSave}
                title="Save to device"
              >
                {busy === "save" ? "Saving…" : "Save"}
              </button>

              <button
                class="btn"
                disabled={!canRunFile}
                onClick={handleRunFile}
                title="Run saved file"
              >
                {busy === "run" ? "Running…" : "Run"}
              </button>

              <div style={{ display: "flex", alignItems: "center", gap: "4px" }}>
                <button
                  class="btn"
                  disabled={busy !== null}
                  onClick={handleEval}
                  title="Eval editor contents"
                >
                  {busy === "eval" ? "Eval…" : "Run (eval)"}
                </button>
                <label style={{ fontSize: "0.75rem", color: "var(--text-dim)", display: "flex", alignItems: "center", gap: "3px", cursor: "pointer" }}>
                  <input
                    type="checkbox"
                    checked={persistEval}
                    onChange={(e) => setPersistEval((e.currentTarget as HTMLInputElement).checked)}
                  />
                  persist
                </label>
              </div>

              <button
                class="btn"
                disabled={busy !== null}
                onClick={handleStop}
                title="Stop running script"
              >
                {busy === "stop" ? "Stopping…" : "Stop"}
              </button>

              <button
                class="btn"
                disabled={busy !== null}
                onClick={handleReset}
                title="Reset MicroPython VM"
              >
                {busy === "reset" ? "Resetting…" : "Reset VM"}
              </button>

              <div style={{ height: "20px", width: "1px", background: "var(--border)" }} />

              <button
                class={`btn${autorunActive && autorunName === selectedFile ? " primary" : ""}`}
                disabled={!canSetAutorun}
                onClick={handleSetAutorun}
                title="Set as autorun on boot"
              >
                {busy === "autorun-enable" ? "Setting…" : "Set autorun"}
              </button>

              <button
                class="btn"
                disabled={!autorunActive || busy !== null}
                onClick={handleDisableAutorun}
                title="Disable autorun"
              >
                {busy === "autorun-disable" ? "Disabling…" : "Disable autorun"}
              </button>

              <div style={{ height: "20px", width: "1px", background: "var(--border)" }} />

              <button
                class="btn"
                style={{ color: "var(--rose)", borderColor: "rgba(239,68,68,0.3)" }}
                disabled={!canDelete}
                onClick={handleDelete}
                title="Delete file"
              >
                {busy === "delete" ? "Deleting…" : "Delete"}
              </button>
            </div>

            {error && (
              <div style={{ marginTop: "6px", color: "var(--rose)", fontSize: "0.78rem" }}>
                {error}
              </div>
            )}
          </div>

          {/* CodeMirror editor */}
          <div style={{ flex: 1, minHeight: 0, display: "flex", flexDirection: "column" }}>
            {selectedFile ? (
              <CodeEditor
                key={editorKey}
                initialDoc={editorDoc}
                onDocChange={handleDocChange}
                editorViewRef={editorViewRef as any}
              />
            ) : (
              <div
                class="glass-card"
                style={{
                  flex: 1,
                  display: "flex",
                  alignItems: "center",
                  justifyContent: "center",
                  color: "var(--text-muted)",
                  fontSize: "0.85rem",
                }}
              >
                Select a file or create a new one
              </div>
            )}
          </div>
        </div>
      </div>

      {/* Bottom pane: logs */}
      <div class="glass-card" style={{ height: "180px", display: "flex", flexDirection: "column", gap: "0", padding: "10px 16px" }}>
        <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", marginBottom: "6px" }}>
          <span class="card-title">Script Logs</span>
          <button
            class="btn"
            style={{ fontSize: "0.72rem", padding: "2px 8px" }}
            onClick={() => setLogs("")}
          >
            Clear
          </button>
        </div>
        <div
          style={{
            flex: 1,
            overflowY: "auto",
            fontFamily: "\"JetBrains Mono Variable\", \"JetBrains Mono\", monospace",
            fontSize: "0.78rem",
            lineHeight: "1.5",
            color: "var(--text-dim)",
            whiteSpace: "pre-wrap",
            wordBreak: "break-all",
          }}
        >
          {logs || <span style={{ color: "var(--text-muted)" }}>(no output yet)</span>}
          <div ref={logsEndRef} />
        </div>
      </div>

    </div>
  );
}
