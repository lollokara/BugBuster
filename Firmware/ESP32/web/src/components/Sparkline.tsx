// =============================================================================
// Sparkline — DPR-aware canvas polyline of recent samples.
// Math ported from DesktopApp/BugBuster/src/components/channel_sparkline.rs.
// =============================================================================

import { useEffect, useRef } from "preact/hooks";

export interface SparklineProps {
  values: number[];
  color?: string;
  height?: number;
}

export function Sparkline({ values, color = "var(--blue)", height = 60 }: SparklineProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height || height;
    if (w <= 1 || h <= 1) return;
    const cw = Math.round(w * dpr);
    const ch = Math.round(h * dpr);
    if (canvas.width !== cw) canvas.width = cw;
    if (canvas.height !== ch) canvas.height = ch;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    if (values.length < 2) return;

    // Compute min/max from values
    let vmin = Infinity;
    let vmax = -Infinity;
    for (const v of values) {
      if (Number.isFinite(v)) {
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
      }
    }
    if (!Number.isFinite(vmin) || !Number.isFinite(vmax)) return;
    // Guard against flat lines
    if (vmax - vmin < 1e-6) {
      vmin -= 0.5;
      vmax += 0.5;
    }
    const span = vmax - vmin;

    // Subtle zero baseline if 0 is inside the range
    if (vmin < 0 && vmax > 0) {
      const y0 = h - ((-vmin) / span) * h;
      ctx.strokeStyle = "rgba(148,163,184,0.15)";
      ctx.lineWidth = 0.5;
      ctx.beginPath();
      ctx.moveTo(0, y0);
      ctx.lineTo(w, y0);
      ctx.stroke();
    }

    // Resolve color via computed style when it's a CSS var.
    let strokeColor = color;
    if (color.startsWith("var(")) {
      const name = color.slice(4, -1).trim();
      const resolved = getComputedStyle(canvas).getPropertyValue(name).trim();
      if (resolved) strokeColor = resolved;
    }

    ctx.strokeStyle = strokeColor;
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    const n = values.length;
    const step = w / Math.max(n - 1, 1);
    for (let i = 0; i < n; i++) {
      const v = values[i]!;
      const x = i * step;
      let norm = (v - vmin) / span;
      if (norm < 0) norm = 0;
      if (norm > 1) norm = 1;
      const y = h - norm * h;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
  }, [values, color, height]);

  return (
    <canvas
      ref={canvasRef}
      class="sparkline"
      style={{ height: `${height}px` }}
    />
  );
}
