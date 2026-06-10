// =============================================================================
// SystemSection — collapsible section header for the System tab
// =============================================================================
import { useState } from "preact/hooks";
export function SystemSection({ title, children, defaultOpen = true }) {
    const [open, setOpen] = useState(defaultOpen);
    return (<div style={{ marginBottom: "8px" }}>
      <button onClick={() => setOpen(o => !o)} style={{
            width: "100%",
            display: "flex",
            alignItems: "center",
            gap: "8px",
            background: "none",
            border: "none",
            borderBottom: "1px solid var(--border)",
            padding: "6px 2px",
            cursor: "pointer",
            color: "var(--text-dim)",
            fontSize: "10px",
            fontWeight: "700",
            letterSpacing: "0.08em",
            textTransform: "uppercase",
            marginBottom: open ? "12px" : "0",
        }}>
        <span style={{ color: open ? "var(--accent)" : "var(--text-muted)", fontSize: "12px", lineHeight: 1, userSelect: "none" }}>
          {open ? "▾" : "▸"}
        </span>
        {title}
      </button>
      {open && (<div style={{ display: "flex", flexDirection: "column", gap: "12px" }}>
          {children}
        </div>)}
    </div>);
}
