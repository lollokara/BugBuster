import type { ComponentChildren, JSX } from "preact";

export interface ChDOverlayProps {
  active: boolean;
  children: ComponentChildren;
  label?: string;
  overlayStyle?: JSX.CSSProperties;
}

export function ChDOverlay({
  active,
  children,
  label = "Used for internal diagnostic",
  overlayStyle,
}: ChDOverlayProps) {
  return (
    <div style={{ position: "relative", height: "100%", minHeight: 0 }}>
      {children}
      {active && (
        <div
          style={{
            position: "absolute",
            inset: 0,
            zIndex: 30,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            padding: "12px",
            background: "rgba(15, 23, 42, 0.72)",
            border: "1px solid rgba(148, 163, 184, 0.24)",
            borderRadius: "var(--radius-sm)",
            color: "var(--text)",
            textAlign: "center",
            fontSize: "0.76rem",
            fontWeight: 700,
            letterSpacing: "1.6px",
            textTransform: "uppercase",
            backdropFilter: "grayscale(1) blur(1px)",
            WebkitBackdropFilter: "grayscale(1) blur(1px)",
            pointerEvents: "auto",
            ...overlayStyle,
          }}
        >
          {label}
        </div>
      )}
    </div>
  );
}
