import { useCallback, useEffect, useRef, useState } from "preact/hooks";
import type { ComponentChildren } from "preact";
import { EditorView, keymap, lineNumbers } from "@codemirror/view";
import { EditorState, Prec } from "@codemirror/state";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { closeBrackets, autocompletion } from "@codemirror/autocomplete";
import { python } from "@codemirror/lang-python";
import { oneDark } from "@codemirror/theme-one-dark";
import { api, PairingRequiredError, type AutorunStatus, type ScriptStatus, type ScriptStorageStatus } from "../../api/client";
import { deviceMac } from "../../state/signals";
import { bugbusterCompletions } from "./completions";
import { apiDocs, functionConstants, type ApiDocEntry } from "./apiDocs";
import { Repl } from "./Repl";

const SCRIPT_UI_BUILD = "scripts-fix-20260427-1150";

type PanelId = "files" | "device" | "docs";
type BottomPanel = "logs" | "repl";

interface EditorProps {
  initialDoc: string;
  onDocChange: (doc: string) => void;
  editorViewRef: { current: EditorView | null };
}

interface DevicePanelState {
  storage: ScriptStorageStatus | null;
  usbpd: any | null;
  diagnostics: any | null;
  selftest: any | null;
  error: string | null;
}

const EMPTY_DEVICE_PANEL: DevicePanelState = {
  storage: null,
  usbpd: null,
  diagnostics: null,
  selftest: null,
  error: null,
};

function ensureScriptName(raw: string): string {
  const trimmed = raw.trim();
  if (!trimmed) return "";
  return trimmed.endsWith(".py") ? trimmed : `${trimmed}.py`;
}

function formatBytes(value: number | undefined): string {
  if (value === undefined || Number.isNaN(value)) return "--";
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`;
  return `${(value / (1024 * 1024)).toFixed(2)} MB`;
}

function normalizeMode(mode: ScriptStatus["mode"]): string {
  if (mode === "PERSISTENT" || mode === "persistent") return "Persistent";
  if (mode === "EPHEMERAL" || mode === "ephemeral") return "Ephemeral";
  if (mode === undefined || mode === null) return "--";
  return String(mode);
}

function statusText(status: ScriptStatus | null): string {
  if (!status) return "Unknown";
  return status.running ? "Running" : "Idle";
}

function getMacOrPair(): string {
  const mac = deviceMac.value;
  if (!mac) {
    window.dispatchEvent(new CustomEvent("bb:pairing-required"));
    throw new PairingRequiredError();
  }
  return mac;
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
          closeBrackets(),
          autocompletion(),
          Prec.highest(EditorState.languageData.of(() => [{ autocomplete: bugbusterCompletions }])),
          EditorView.updateListener.of((update) => {
            if (update.docChanged) onDocChange(update.state.doc.toString());
          }),
          EditorView.theme({
            "&": {
              height: "100%",
              fontSize: "13px",
              backgroundColor: "#0b1020",
            },
            ".cm-scroller": {
              overflow: "auto",
              fontFamily: "\"JetBrains Mono Variable\", \"JetBrains Mono\", ui-monospace, monospace",
            },
            ".cm-gutters": {
              backgroundColor: "#070b16",
              color: "#53607a",
              borderRight: "1px solid rgba(56, 189, 248, 0.16)",
            },
            ".cm-activeLine": { backgroundColor: "rgba(34, 211, 238, 0.07)" },
            ".cm-activeLineGutter": { backgroundColor: "rgba(34, 211, 238, 0.10)" },
            ".cm-cursor": { borderLeftColor: "#22d3ee" },
            ".cm-selectionBackground": { background: "rgba(34, 211, 238, 0.22) !important" },
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
  }, []);

  return <div class="script-editor-surface" ref={containerRef} />;
}

function MiniMetric({ label, value, tone = "cyan" }: { label: string; value: string; tone?: "cyan" | "green" | "amber" | "rose" }) {
  return (
    <div class={`script-mini-metric ${tone}`}>
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function ScriptStatusBar({ status, selectedFile, isDirty }: { status: ScriptStatus | null; selectedFile: string | null; isDirty: boolean }) {
  return (
    <footer class="script-status-bar">
      <span class={`script-run-dot ${status?.running ? "running" : ""}`} />
      <span>{statusText(status)}</span>
      <span>VM {normalizeMode(status?.mode)}</span>
      <span>Runs {status?.totalRuns ?? 0}</span>
      <span>Errors {status?.totalErrors ?? 0}</span>
      <span class="script-status-file">{selectedFile ? `${selectedFile}${isDirty ? " *" : ""}` : "No file selected"}</span>
    </footer>
  );
}

function ActivityButton({ id, activePanel, icon, label, onClick }: {
  id: PanelId;
  activePanel: PanelId | null;
  icon: string;
  label: string;
  onClick: (id: PanelId) => void;
}) {
  const active = activePanel === id;
  return (
    <button class={`script-activity-btn ${active ? "active" : ""}`} onClick={() => onClick(id)} title={label}>
      <span>{icon}</span>
      <small>{label}</small>
    </button>
  );
}

function FilesPanel(props: {
  files: string[];
  selectedFile: string | null;
  isUnsaved: boolean;
  isDirty: boolean;
  newFileName: string;
  busy: string | null;
  setNewFileName: (value: string) => void;
  onNew: () => void;
  onRefresh: () => void;
  onSelect: (name: string) => void;
  onUpload: (file: File) => void;
  onDownload: () => void;
  onDelete: () => void;
}) {
  const uploadRef = useRef<HTMLInputElement>(null);
  return (
    <div class="script-side-content">
      <div class="script-panel-head">
        <div>
          <span class="uppercase-tag">Explorer</span>
          <h3>Scripts</h3>
        </div>
        <button class="script-icon-btn" onClick={props.onRefresh} title="Refresh files">refresh</button>
      </div>

      <div class="script-new-row">
        <input
          class="input"
          placeholder="new-script.py"
          value={props.newFileName}
          onInput={(e) => props.setNewFileName((e.currentTarget as HTMLInputElement).value)}
          onKeyDown={(e) => { if (e.key === "Enter") props.onNew(); }}
        />
        <button class="btn primary" onClick={props.onNew}>New</button>
      </div>

      <div class="script-file-actions">
        <input
          ref={uploadRef}
          type="file"
          accept=".py,text/x-python,text/plain"
          style={{ display: "none" }}
          onChange={(e) => {
            const file = (e.currentTarget as HTMLInputElement).files?.[0];
            if (file) props.onUpload(file);
            (e.currentTarget as HTMLInputElement).value = "";
          }}
        />
        <button class="btn" onClick={() => uploadRef.current?.click()} disabled={props.busy !== null}>Upload</button>
        <button class="btn" onClick={props.onDownload} disabled={!props.selectedFile || props.isUnsaved || props.busy !== null}>Download</button>
        <button class="btn danger" onClick={props.onDelete} disabled={!props.selectedFile || props.isUnsaved || props.busy !== null}>Delete</button>
      </div>

      <div class="script-file-list">
        {props.files.length === 0 && <div class="script-empty">No scripts on device</div>}
        {props.files.map((file) => {
          const active = props.selectedFile === file && !props.isUnsaved;
          return (
            <button key={file} class={`script-file-row ${active ? "active" : ""}`} onClick={() => props.onSelect(file)}>
              <span class="script-file-icon">PY</span>
              <span title={file}>{file}</span>
              {active && props.isDirty && <b>*</b>}
            </button>
          );
        })}
      </div>
    </div>
  );
}

function DevicePanel({ vmStatus, device }: { vmStatus: ScriptStatus | null; device: DevicePanelState }) {
  const storage = device.storage;
  const usedPct = storage && storage.totalBytes > 0
    ? Math.min(100, Math.round((storage.usedBytes / storage.totalBytes) * 100))
    : 0;
  const pd = device.usbpd ?? {};
  const diagnostics = device.diagnostics ?? {};
  const selftest = device.selftest ?? {};
  const dieTemp = diagnostics.dieTemp ?? diagnostics.die_temp;

  return (
    <div class="script-side-content">
      <div class="script-panel-head">
        <div>
          <span class="uppercase-tag">Device</span>
          <h3>Status</h3>
        </div>
      </div>

      {device.error && <div class="script-alert">{device.error}</div>}

      <div class="script-metric-grid">
        <MiniMetric label="VM" value={statusText(vmStatus)} tone={vmStatus?.running ? "green" : "cyan"} />
        <MiniMetric label="Mode" value={normalizeMode(vmStatus?.mode)} />
        <MiniMetric label="Runs" value={String(vmStatus?.totalRuns ?? 0)} />
        <MiniMetric label="Errors" value={String(vmStatus?.totalErrors ?? 0)} tone={(vmStatus?.totalErrors ?? 0) > 0 ? "rose" : "green"} />
      </div>

      <section class="script-device-section">
        <h4>Storage</h4>
        <div class="script-storage-bar"><span style={{ width: `${usedPct}%` }} /></div>
        <div class="kv-row"><span class="text-dim">Used</span><span class="mono">{formatBytes(storage?.usedBytes)} / {formatBytes(storage?.totalBytes)}</span></div>
        <div class="kv-row"><span class="text-dim">Free</span><span class="mono">{formatBytes(storage?.freeBytes)}</span></div>
        <div class="kv-row"><span class="text-dim">Scripts</span><span class="mono">{storage?.scriptCount ?? "--"} / {storage?.maxScripts ?? "--"}</span></div>
      </section>

      <section class="script-device-section">
        <h4>Power</h4>
        <div class="kv-row"><span class="text-dim">USB-PD</span><span class="mono">{pd.voltageV ?? pd.voltage ?? "--"} V</span></div>
        <div class="kv-row"><span class="text-dim">Current</span><span class="mono">{pd.currentA ?? pd.current ?? "--"} A</span></div>
        <div class="kv-row"><span class="text-dim">Power</span><span class="mono">{pd.powerW ?? "--"} W</span></div>
      </section>

      <section class="script-device-section">
        <h4>Diagnostics</h4>
        <div class="kv-row"><span class="text-dim">Die temp</span><span class="mono">{dieTemp === undefined ? "--" : `${Number(dieTemp).toFixed(1)} C`}</span></div>
        <div class="kv-row"><span class="text-dim">Selftest worker</span><span class="mono">{selftest.workerEnabled ? "ON" : "OFF"}</span></div>
        <div class="kv-row"><span class="text-dim">Supply monitor</span><span class="mono">{selftest.supplyMonitorActive ? "ACTIVE" : "IDLE"}</span></div>
      </section>

      {vmStatus?.lastError && (
        <section class="script-device-section">
          <h4>Last Script Error</h4>
          <pre class="script-side-pre">{vmStatus.lastError}</pre>
        </section>
      )}
    </div>
  );
}

function DocEntry({ entry }: { entry: ApiDocEntry }) {
  const [open, setOpen] = useState(false);
  return (
    <article class={`script-doc-entry ${open ? "open" : ""}`}>
      <button onClick={() => setOpen(!open)}>
        <span>{open ? "v" : ">"}</span>
        <strong>{entry.title}</strong>
      </button>
      {open && (
        <div class="script-doc-body">
          <code>{entry.signature}</code>
          <p>{entry.summary}</p>
          {entry.args && (
            <dl>
              {entry.args.map((arg) => (
                <div key={arg.name}>
                  <dt>{arg.name}</dt>
                  <dd>{arg.detail}</dd>
                </div>
              ))}
            </dl>
          )}
          {entry.returns && <p><b>Returns:</b> {entry.returns}</p>}
          {entry.raises && <p><b>Raises:</b> {entry.raises.join(" ")}</p>}
          <pre>{entry.example}</pre>
        </div>
      )}
    </article>
  );
}

function DocsPanel() {
  const groups = Array.from(new Set(apiDocs.map((entry) => entry.group)));
  return (
    <div class="script-side-content">
      <div class="script-panel-head">
        <div>
          <span class="uppercase-tag">Reference</span>
          <h3>bugbuster API</h3>
        </div>
      </div>

      {groups.map((group) => (
        <section class="script-doc-group" key={group}>
          <h4>{group}</h4>
          {apiDocs.filter((entry) => entry.group === group).map((entry) => <DocEntry key={entry.id} entry={entry} />)}
        </section>
      ))}

      <section class="script-doc-group">
        <h4>Channel Constants</h4>
        <div class="script-constant-list">
          {functionConstants.map(([name, detail]) => (
            <div key={name}>
              <code>bugbuster.{name}</code>
              <span>{detail}</span>
            </div>
          ))}
        </div>
      </section>
    </div>
  );
}

function Sidebar(props: {
  activePanel: PanelId | null;
  filesPanel: ComponentChildren;
  devicePanel: ComponentChildren;
  docsPanel: ComponentChildren;
  onCollapse: () => void;
}) {
  if (!props.activePanel) return null;
  return (
    <aside class="script-sidebar">
      <button class="script-collapse-btn" onClick={props.onCollapse} title="Collapse sidebar">x</button>
      {props.activePanel === "files" && props.filesPanel}
      {props.activePanel === "device" && props.devicePanel}
      {props.activePanel === "docs" && props.docsPanel}
    </aside>
  );
}

export function Scripts() {
  const [files, setFiles] = useState<string[]>([]);
  const [selectedFile, setSelectedFile] = useState<string | null>(null);
  const [isUnsaved, setIsUnsaved] = useState(false);
  const [isDirty, setIsDirty] = useState(false);
  const [editorDoc, setEditorDoc] = useState("");
  const [editorKey, setEditorKey] = useState(0);
  const [newFileName, setNewFileName] = useState("");
  const [vmStatus, setVmStatus] = useState<ScriptStatus | null>(null);
  const [autorunStatus, setAutorunStatus] = useState<AutorunStatus | null>(null);
  const [logs, setLogs] = useState("");
  const [logError, setLogError] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [persistEval, setPersistEval] = useState(false);
  const [activePanel, setActivePanel] = useState<PanelId | null>("files");
  const [bottomPanel, setBottomPanel] = useState<BottomPanel>("logs");
  const [devicePanel, setDevicePanel] = useState<DevicePanelState>(EMPTY_DEVICE_PANEL);
  const editorViewRef = useRef<EditorView | null>(null);
  const logsRef = useRef<HTMLPreElement>(null);
  const logCursorRef = useRef(0);

  const currentSource = useCallback(() => editorViewRef.current?.state.doc.toString() ?? editorDoc, [editorDoc]);

  const drainLogs = useCallback(async () => {
    const { text, next } = await api.scripts.logs(getMacOrPair(), logCursorRef.current);
    logCursorRef.current = next;
    setLogError(null);
    if (text) setLogs((prev) => prev + text);
    return text;
  }, []);

  const runScriptAction = useCallback(async <T,>(name: string, action: (mac: string) => Promise<T>) => {
    setBusy(name);
    setError(null);
    try {
      return await action(getMacOrPair());
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) setError(e instanceof Error ? e.message : String(e));
      return null;
    } finally {
      setBusy(null);
    }
  }, []);

  const fetchFiles = useCallback(async () => {
    try {
      const data = await api.scripts.files(getMacOrPair());
      setFiles(Array.isArray(data.files) ? data.files.sort() : []);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("scripts: list failed", e);
    }
  }, []);

  const fetchAutorun = useCallback(async () => {
    try {
      setAutorunStatus(await api.scripts.autorun(getMacOrPair()));
    } catch {
      /* non-fatal */
    }
  }, []);

  const refreshDevicePanel = useCallback(async () => {
    try {
      const mac = getMacOrPair();
      const [storage, usbpd, diagnostics, selftest] = await Promise.allSettled([
        api.scripts.storage(mac),
        api.usbpd(),
        api.diagnostics(),
        api.selftest(),
      ]);
      setDevicePanel({
        storage: storage.status === "fulfilled" ? storage.value : null,
        usbpd: usbpd.status === "fulfilled" ? usbpd.value : null,
        diagnostics: diagnostics.status === "fulfilled" ? diagnostics.value : null,
        selftest: selftest.status === "fulfilled" ? selftest.value : null,
        error: null,
      });
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setDevicePanel((prev) => ({ ...prev, error: e instanceof Error ? e.message : String(e) }));
      }
    }
  }, []);

  useEffect(() => {
    fetchFiles();
    fetchAutorun();
    refreshDevicePanel();
  }, [fetchFiles, fetchAutorun, refreshDevicePanel]);

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const mac = getMacOrPair();
        const status = await api.scripts.status(mac);
        if (alive) setVmStatus(status);
      } catch {
        /* pairing modal handles auth failures */
      }
      if (alive) window.setTimeout(tick, 1000);
    };
    tick();
    return () => { alive = false; };
  }, []);

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const { text, next } = await api.scripts.logs(getMacOrPair(), logCursorRef.current);
        if (alive) {
          logCursorRef.current = next;
          setLogError(null);
          if (text) setLogs((prev) => prev + text);
        }
      } catch (e) {
        if (alive && !(e instanceof PairingRequiredError)) {
          setLogError(e instanceof Error ? e.message : String(e));
        }
      }
      if (alive) window.setTimeout(tick, 500);
    };
    tick();
    return () => { alive = false; };
  }, []);

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (activePanel === "device") await refreshDevicePanel();
      if (alive) window.setTimeout(tick, 2500);
    };
    tick();
    return () => { alive = false; };
  }, [activePanel, refreshDevicePanel]);

  useEffect(() => {
    if (bottomPanel !== "logs") return;
    const node = logsRef.current;
    if (node) node.scrollTop = node.scrollHeight;
  }, [logs, bottomPanel]);

  const loadFile = useCallback(async (name: string) => {
    await runScriptAction("load", async (mac) => {
      const text = await api.scripts.download(mac, name);
      setSelectedFile(name);
      setEditorDoc(text);
      setEditorKey((key) => key + 1);
      setIsUnsaved(false);
      setIsDirty(false);
    });
  }, [runScriptAction]);

  const selectFile = (name: string) => {
    if (isDirty && !window.confirm("Discard unsaved editor changes?")) return;
    loadFile(name);
  };

  const newFile = () => {
    const name = ensureScriptName(newFileName);
    if (!name) return;
    if (files.includes(name)) {
      setError("File already exists");
      return;
    }
    if (isDirty && !window.confirm("Discard unsaved editor changes?")) return;
    setSelectedFile(name);
    setIsUnsaved(true);
    setIsDirty(false);
    setEditorDoc("");
    setEditorKey((key) => key + 1);
    setNewFileName("");
    setError(null);
  };

  const saveFile = async () => {
    if (!selectedFile) return;
    const source = currentSource();
    const result = await runScriptAction("save", (mac) => api.scripts.upload(mac, selectedFile, source));
    if (result?.ok === false) setError(result.err ?? "Save failed");
    if (result?.ok !== false) {
      setIsUnsaved(false);
      setIsDirty(false);
      await fetchFiles();
      await refreshDevicePanel();
    }
  };

  const uploadFile = async (file: File) => {
    const name = ensureScriptName(file.name);
    if (!name) return;
    const text = await file.text();
    const result = await runScriptAction("upload", (mac) => api.scripts.upload(mac, name, text));
    if (result?.ok === false) {
      setError(result.err ?? "Upload failed");
      return;
    }
    setSelectedFile(name);
    setEditorDoc(text);
    setEditorKey((key) => key + 1);
    setIsUnsaved(false);
    setIsDirty(false);
    await fetchFiles();
    await refreshDevicePanel();
  };

  const downloadFile = async () => {
    if (!selectedFile || isUnsaved) return;
    await runScriptAction("download", async (mac) => {
      const text = await api.scripts.download(mac, selectedFile);
      const blob = new Blob([text], { type: "text/x-python;charset=utf-8" });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement("a");
      anchor.href = url;
      anchor.download = selectedFile;
      anchor.click();
      URL.revokeObjectURL(url);
    });
  };

  const deleteFile = async () => {
    if (!selectedFile || isUnsaved) return;
    if (!window.confirm(`Delete ${selectedFile}?`)) return;
    const name = selectedFile;
    const result = await runScriptAction("delete", (mac) => api.scripts.delete(mac, name));
    if (result?.ok === false) {
      setError(result.err ?? "Delete failed");
      return;
    }
    setSelectedFile(null);
    setEditorDoc("");
    setEditorKey((key) => key + 1);
    setIsDirty(false);
    setIsUnsaved(false);
    await fetchFiles();
    await refreshDevicePanel();
  };

  const runFile = async () => {
    if (!selectedFile || isUnsaved || isDirty) return;
    const result = await runScriptAction("run", (mac) => api.scripts.runFile(mac, selectedFile));
    if (result?.ok === false) setError(result.err ?? "Run failed");
    window.setTimeout(() => { drainLogs().catch(() => undefined); }, 150);
    window.setTimeout(() => { drainLogs().catch(() => undefined); }, 600);
  };

  const evalEditor = async () => {
    const result = await runScriptAction("eval", (mac) => api.scripts.eval(mac, currentSource(), persistEval));
    if (result?.ok === false) setError(result.err ?? "Eval failed");
    window.setTimeout(() => { drainLogs().catch(() => undefined); }, 150);
    window.setTimeout(() => { drainLogs().catch(() => undefined); }, 600);
  };

  const stopScript = () => runScriptAction("stop", (mac) => api.scripts.stop(mac));

  const resetVm = async () => {
    if (!window.confirm("Reset the MicroPython VM? This clears persistent globals.")) return;
    await runScriptAction("reset", (mac) => api.scripts.reset(mac));
  };

  const enableAutorun = async () => {
    if (!selectedFile || isUnsaved) return;
    const result = await runScriptAction("autorun", (mac) => api.scripts.enableAutorun(mac, selectedFile));
    if (result?.ok === false) setError(result.err ?? "Autorun setup failed");
    await fetchAutorun();
  };

  const disableAutorun = async () => {
    const result = await runScriptAction("autorun-off", (mac) => api.scripts.disableAutorun(mac));
    if (result?.ok === false) setError(result.err ?? "Autorun disable failed");
    await fetchAutorun();
  };

  const canSave = !!selectedFile && busy === null;
  const canRunFile = !!selectedFile && !isUnsaved && !isDirty && busy === null;
  const autorunEnabled = !!autorunStatus?.enabled;

  return (
    <div class="script-workbench">
      <div class="script-titlebar">
        <div>
          <span class="uppercase-tag">On-Device Python</span>
          <h2>MicroPython Workbench</h2>
        </div>
        <div class="script-title-actions">
          <MiniMetric label="State" value={statusText(vmStatus)} tone={vmStatus?.running ? "green" : "cyan"} />
          <MiniMetric label="Storage" value={formatBytes(devicePanel.storage?.freeBytes)} />
          <span class="script-build-pill">UI {SCRIPT_UI_BUILD}</span>
          {autorunEnabled && <span class="script-autorun-pill">Autorun enabled</span>}
        </div>
      </div>

      <div class="script-ide">
        <nav class="script-activity">
          <ActivityButton id="files" activePanel={activePanel} icon="EX" label="Files" onClick={(id) => setActivePanel(activePanel === id ? null : id)} />
          <ActivityButton id="device" activePanel={activePanel} icon="DV" label="Device" onClick={(id) => setActivePanel(activePanel === id ? null : id)} />
          <ActivityButton id="docs" activePanel={activePanel} icon="API" label="Docs" onClick={(id) => setActivePanel(activePanel === id ? null : id)} />
        </nav>

        <Sidebar
          activePanel={activePanel}
          onCollapse={() => setActivePanel(null)}
          filesPanel={
            <FilesPanel
              files={files}
              selectedFile={selectedFile}
              isUnsaved={isUnsaved}
              isDirty={isDirty}
              newFileName={newFileName}
              busy={busy}
              setNewFileName={setNewFileName}
              onNew={newFile}
              onRefresh={fetchFiles}
              onSelect={selectFile}
              onUpload={uploadFile}
              onDownload={downloadFile}
              onDelete={deleteFile}
            />
          }
          devicePanel={<DevicePanel vmStatus={vmStatus} device={devicePanel} />}
          docsPanel={<DocsPanel />}
        />

        <main class="script-main">
          <div class="script-editor-header">
            <div class="script-tab-strip">
              <span class={`script-file-tab ${selectedFile ? "active" : ""}`}>
                {selectedFile ? `${selectedFile}${isDirty ? " *" : ""}${isUnsaved ? " (new)" : ""}` : "Welcome"}
              </span>
            </div>
            <div class="script-toolbar">
              <button class="btn primary" disabled={!canSave} onClick={saveFile}>{busy === "save" ? "Saving..." : "Save"}</button>
              <button class="btn" disabled={!canRunFile} onClick={runFile}>{busy === "run" ? "Running..." : "Run File"}</button>
              <button class="btn" disabled={busy !== null} onClick={evalEditor}>{busy === "eval" ? "Evaluating..." : "Eval"}</button>
              <label class="script-check">
                <input type="checkbox" checked={persistEval} onChange={(e) => setPersistEval((e.currentTarget as HTMLInputElement).checked)} />
                persist
              </label>
              <button class="btn" disabled={busy !== null} onClick={stopScript}>Stop</button>
              <button class="btn" disabled={busy !== null} onClick={resetVm}>Reset VM</button>
              <button class={`btn ${autorunEnabled ? "primary" : ""}`} disabled={!selectedFile || isUnsaved || busy !== null} onClick={enableAutorun}>Set Autorun</button>
              <button class="btn" disabled={!autorunEnabled || busy !== null} onClick={disableAutorun}>Disable Autorun</button>
            </div>
          </div>

          {error && <div class="script-error-strip">{error}</div>}

          <section class="script-editor-pane">
            {selectedFile ? (
              <CodeEditor
                key={editorKey}
                initialDoc={editorDoc}
                onDocChange={(doc) => {
                  setEditorDoc(doc);
                  setIsDirty(true);
                }}
                editorViewRef={editorViewRef}
              />
            ) : (
              <div class="script-welcome">
                <div>
                  <span class="uppercase-tag">Ready</span>
                  <h3>Select or create a Python script</h3>
                  <p>Use the Files panel to edit scripts stored on the ESP32 SPIFFS volume, or upload a local .py file.</p>
                </div>
              </div>
            )}
          </section>

          <section class="script-bottom">
            <div class="script-bottom-tabs">
              <button class={bottomPanel === "logs" ? "active" : ""} onClick={() => setBottomPanel("logs")}>Logs</button>
              <button class={bottomPanel === "repl" ? "active" : ""} onClick={() => setBottomPanel("repl")}>REPL</button>
              {logError && <span class="script-log-error">Log poll failed: {logError}</span>}
              <button class="script-clear-log" onClick={() => setLogs("")}>Clear logs</button>
            </div>
            <div class="script-bottom-body">
              {bottomPanel === "logs" ? (
                <pre ref={logsRef} class="script-log-output">{logs || "(no output yet)"}</pre>
              ) : (
                <Repl />
              )}
            </div>
          </section>
        </main>
      </div>

      <ScriptStatusBar status={vmStatus} selectedFile={selectedFile} isDirty={isDirty} />
    </div>
  );
}
