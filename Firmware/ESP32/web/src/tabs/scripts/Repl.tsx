// =============================================================================
// Repl.tsx — xterm.js WebSocket REPL for BugBuster on-device MicroPython.
//
// Opens a WebSocket to /api/scripts/repl/ws, sends the admin bearer token as
// the first text frame (auth), then pipes:
//   xterm input  → ws.send
//   ws.onmessage → terminal.write
//
// The component is lazy-loaded from Scripts.tsx so xterm.js (~140 KB gz) only
// ships when the REPL panel is first opened.
// =============================================================================

import { useEffect, useRef, useState } from "preact/hooks";
import type { IDisposable } from "@xterm/xterm";
import { Terminal } from "@xterm/xterm";
import "@xterm/xterm/css/xterm.css";
import { getCachedToken, PairingRequiredError } from "../../api/client";
import { deviceMac } from "../../state/signals";

// ---------------------------------------------------------------------------
// Connection status type
// ---------------------------------------------------------------------------

type ConnStatus = "disconnected" | "connecting" | "auth" | "connected" | "disconnecting" | "error";

// ---------------------------------------------------------------------------
// Build the WebSocket URL relative to the current origin.
// Works for both http:// (→ ws://) and https:// (→ wss://).
// ---------------------------------------------------------------------------

function wsUrl(path: string): string {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${location.host}${path}`;
}

// ---------------------------------------------------------------------------
// Repl component
// ---------------------------------------------------------------------------

export function Repl() {
  const termRef = useRef<HTMLDivElement>(null);
  const termInstance = useRef<Terminal | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const inputDisposable = useRef<IDisposable | null>(null);
  const [status, setStatus] = useState<ConnStatus>("disconnected");
  const [errorMsg, setErrorMsg] = useState<string>("");

  // -------------------------------------------------------------------------
  // Mount terminal (once)
  // -------------------------------------------------------------------------

  useEffect(() => {
    if (!termRef.current) return;

    const term = new Terminal({
      theme: {
        background: "#0d1117",
        foreground: "#e6edf3",
        cursor:     "#58a6ff",
        black:      "#0d1117",
        red:        "#ff7b72",
        green:      "#3fb950",
        yellow:     "#d29922",
        blue:       "#58a6ff",
        magenta:    "#bc8cff",
        cyan:       "#39c5cf",
        white:      "#e6edf3",
      },
      fontFamily: '"JetBrains Mono Variable", "JetBrains Mono", "Fira Code", monospace',
      fontSize: 13,
      lineHeight: 1.35,
      cursorBlink: true,
      scrollback: 2000,
      convertEol: true,
      cols: 96,
      rows: 8,
    });

    term.open(termRef.current);

    termInstance.current = term;

    const resizeTerminal = () => {
      const el = termRef.current;
      if (!el) return;
      const rect = el.getBoundingClientRect();
      const cols = Math.max(24, Math.floor(rect.width / 8));
      const rows = Math.max(4, Math.floor(rect.height / 18));
      try {
        term.resize(cols, rows);
        term.scrollToBottom();
      } catch {
        /* ignore during first layout */
      }
    };

    requestAnimationFrame(resizeTerminal);

    // Resize observer: re-fit when the container dimensions change.
    const ro = new ResizeObserver(resizeTerminal);
    ro.observe(termRef.current);

    return () => {
      ro.disconnect();
      term.dispose();
      termInstance.current = null;
    };
  }, []);

  // -------------------------------------------------------------------------
  // Connect / disconnect
  // -------------------------------------------------------------------------

  const connect = () => {
    if (wsRef.current) return; // already open

    const mac = deviceMac.value;
    const token = mac ? getCachedToken(mac) : null;
    if (!token) {
      window.dispatchEvent(new CustomEvent("bb:pairing-required"));
      throw new PairingRequiredError();
    }

    setStatus("connecting");
    setErrorMsg("");
    termInstance.current?.writeln("\r\n\x1b[90mConnecting…\x1b[0m");

    const ws = new WebSocket(wsUrl("/api/scripts/repl/ws"));
    wsRef.current = ws;

    ws.onopen = () => {
      // First frame = auth token.
      ws.send(token);
      setStatus("auth");
    };

    ws.onmessage = (ev) => {
      // Once the server echoes the welcome banner the session is live.
      if (status !== "connected") setStatus("connected");
      if (termInstance.current) {
        termInstance.current.write(
          typeof ev.data === "string" ? ev.data : new Uint8Array(ev.data),
          () => {
            termInstance.current?.scrollToBottom();
          },
        );
      }
    };

    ws.onclose = (ev) => {
      inputDisposable.current?.dispose();
      inputDisposable.current = null;
      wsRef.current = null;
      const code = ev.code;
      if (code === 4001) {
        setStatus("error");
        setErrorMsg("Auth failed (4001) — check pairing token.");
        termInstance.current?.writeln("\r\n\x1b[31mAuth rejected (4001). Re-pair and retry.\x1b[0m");
      } else if (code === 4002) {
        setStatus("error");
        setErrorMsg("Session in use (4002) — another client is connected.");
        termInstance.current?.writeln("\r\n\x1b[33mSession in use (4002). Disconnect the other client first.\x1b[0m");
      } else {
        setStatus("disconnected");
        termInstance.current?.writeln("\r\n\x1b[90mDisconnected.\x1b[0m");
      }
    };

    ws.onerror = () => {
      setStatus("error");
      setErrorMsg("WebSocket error — check device connectivity.");
      termInstance.current?.writeln("\r\n\x1b[31mConnection error.\x1b[0m");
    };

    // Pipe xterm input to WebSocket.
    inputDisposable.current?.dispose();
    inputDisposable.current = termInstance.current?.onData((data) => {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(data);
      }
    }) ?? null;
  };

  const disconnect = () => {
    const ws = wsRef.current;
    if (!ws) {
      setStatus("disconnected");
      return;
    }
    setStatus("disconnecting");
    if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
      ws.close(1000, "client disconnect");
    }
    requestAnimationFrame(() => termInstance.current?.scrollToBottom());
  };

  // Disconnect on unmount.
  useEffect(() => () => {
    inputDisposable.current?.dispose();
    wsRef.current?.close(1000, "component unmount");
  }, []);

  // -------------------------------------------------------------------------
  // Status indicator helpers
  // -------------------------------------------------------------------------

  const statusColor: Record<ConnStatus, string> = {
    disconnected: "var(--text-muted)",
    connecting:   "var(--amber)",
    auth:         "var(--amber)",
    connected:    "var(--green)",
    disconnecting: "var(--amber)",
    error:        "var(--rose)",
  };

  const statusLabel: Record<ConnStatus, string> = {
    disconnected: "Disconnected",
    connecting:   "Connecting…",
    auth:         "Authenticating…",
    connected:    "Connected",
    disconnecting: "Disconnecting...",
    error:        "Error",
  };

  // -------------------------------------------------------------------------
  // Render
  // -------------------------------------------------------------------------

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%", gap: "8px" }}>

      {/* Header bar */}
      <div
        class="glass-card"
        style={{ padding: "8px 16px", display: "flex", alignItems: "center", gap: "12px", flexShrink: 0 }}
      >
        <span class="card-title" style={{ marginBottom: 0 }}>MicroPython REPL</span>

        {/* Status dot + label */}
        <span style={{ display: "flex", alignItems: "center", gap: "6px" }}>
          <span
            style={{
              width: "8px",
              height: "8px",
              borderRadius: "50%",
              background: statusColor[status],
              display: "inline-block",
              flexShrink: 0,
            }}
          />
          <span style={{ fontSize: "0.78rem", color: statusColor[status] }}>
            {statusLabel[status]}
          </span>
        </span>

        {errorMsg && (
          <span style={{ fontSize: "0.78rem", color: "var(--rose)", marginLeft: "4px" }}>
            {errorMsg}
          </span>
        )}

        <div style={{ marginLeft: "auto", display: "flex", gap: "8px" }}>
          <button
            class="btn primary"
            disabled={status === "connecting" || status === "auth" || status === "connected" || status === "disconnecting"}
            onClick={connect}
          >
            Connect
          </button>
          <button
            class="btn"
            disabled={status === "disconnected" || status === "disconnecting" || status === "error"}
            onClick={disconnect}
          >
            Disconnect
          </button>
        </div>
      </div>

      {/* xterm.js terminal */}
      <div
        class="script-repl-terminal"
        ref={termRef}
      />
    </div>
  );
}
