// =============================================================================
// ScopeCanvas — hand-rolled oscilloscope canvas.
// Ported (simplified) from DesktopApp/BugBuster/src/tabs/scope.rs.
//
// Responsibilities:
//  - DPR-aware canvas that resizes with ResizeObserver.
//  - SSE consumer of /api/scope/stream (with fallback poll on /api/scope).
//  - RAF draw loop with grid, per-channel polylines, trigger, cursors.
//  - Mouse pan on X axis, wheel zoom on scopeTimeBase.
// =============================================================================

import { useEffect, useRef, useState } from "preact/hooks";
import { api } from "../api/client";
import {
  scopeBuffer,
  scopeChannelEnabled,
  scopeChannelInvert,
  scopeChannelOffset,
  scopePlotMode,
  scopeRunning,
  scopeSeq,
  scopeTimeBase,
  scopeTriggerLevel,
  pushScopeSamples,
} from "../state/signals";

export const CH_COLORS = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"] as const;
const GRID_COLOR = "rgba(100,140,200,0.08)";
const ZERO_LINE = "rgba(148,163,184,0.15)";
const BG_COLOR = "#0a0f1e";
const CURSOR_A = "#06b6d4";
const CURSOR_B = "#ec4899";

interface Cursor {
  x: number | null; // fraction 0..1 within plot area
}

export function ScopeCanvas() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const wrapRef = useRef<HTMLDivElement | null>(null);
  const sizeRef = useRef({ w: 800, h: 400, dpr: 1 });
  const panRef = useRef({ panT: 0, dragging: false, startX: 0, startPanT: 0 });

  const [cursorA, setCursorA] = useState<Cursor>({ x: null });
  const [cursorB, setCursorB] = useState<Cursor>({ x: null });

  // ---- Resize observer -----------------------------------------------------
  useEffect(() => {
    const canvas = canvasRef.current;
    const wrap = wrapRef.current;
    if (!canvas || !wrap) return;
    const applySize = () => {
      const rect = wrap.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      const w = Math.max(200, rect.width);
      const h = Math.max(200, rect.height);
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
      canvas.style.width = w + "px";
      canvas.style.height = h + "px";
      sizeRef.current = { w, h, dpr };
    };
    applySize();
    const ro = new ResizeObserver(applySize);
    ro.observe(wrap);
    return () => ro.disconnect();
  }, []);

  // ---- Scope stream via SSE -----------------------------------------------
  // Replaces the previous 33 ms (~30 req/s) polling loop. We open a single
  // EventSource against /api/scope/stream and let the firmware push new
  // sample buckets as they're produced. Net effect: per-tab polling drops
  // from ~30 req/s to ~0 req/s while idle, and the device sends data only
  // when it actually has new samples. EventSource handles auto-reconnect
  // for transient disconnects on its own. We fall back to one-shot polling
  // via /api/scope on browsers without EventSource, and gracefully degrade
  // if the device firmware predates this endpoint (we'll see an immediate
  // error event and silently stop trying).
  useEffect(() => {
    let alive = true;
    let es: EventSource | null = null;
    let pollFallbackTimer: number | null = null;
    let usingFallback = false;
    let consecutiveErrors = 0;

    const handleSamples = (seq: number, samples: number[][]) => {
      if (!Array.isArray(samples) || samples.length === 0) return;
      pushScopeSamples(seq, samples);
    };

    const startFallbackPolling = () => {
      if (usingFallback) return;
      usingFallback = true;
      const tick = async () => {
        if (!alive) return;
        if (
          scopeRunning.value &&
          typeof document !== "undefined" &&
          document.visibilityState !== "hidden"
        ) {
          try {
            const resp = await api.scope(scopeSeq.value);
            if (resp && Array.isArray(resp.samples) && resp.samples.length > 0) {
              handleSamples(
                typeof resp.seq === "number" ? resp.seq : scopeSeq.value,
                resp.samples,
              );
            }
          } catch {
            /* swallow */
          }
        }
        if (alive) pollFallbackTimer = window.setTimeout(tick, 250);
      };
      void tick();
    };

    if (typeof EventSource === "undefined") {
      startFallbackPolling();
    } else {
      try {
        es = new EventSource("/api/scope/stream");
        es.onmessage = (ev) => {
          if (!alive || !scopeRunning.value) return;
          try {
            const data = JSON.parse(ev.data);
            const seq =
              typeof data.seq === "number" ? data.seq : scopeSeq.value;
            handleSamples(seq, data.samples ?? []);
          } catch {
            /* malformed event — drop */
          }
        };
        es.onerror = () => {
          // EventSource will auto-retry on its own, but if the endpoint
          // does not exist (older firmware), error fires immediately and
          // repeatedly. After three quick errors, give up and poll.
          consecutiveErrors += 1;
          if (consecutiveErrors >= 3 && !usingFallback) {
            es?.close();
            es = null;
            startFallbackPolling();
          }
        };
      } catch {
        startFallbackPolling();
      }
    }

    return () => {
      alive = false;
      if (es) {
        es.close();
        es = null;
      }
      if (pollFallbackTimer !== null) {
        window.clearTimeout(pollFallbackTimer);
        pollFallbackTimer = null;
      }
    };
  }, []);

  // ---- RAF draw loop -------------------------------------------------------
  useEffect(() => {
    let raf = 0;
    const draw = () => {
      const canvas = canvasRef.current;
      if (!canvas) {
        raf = requestAnimationFrame(draw);
        return;
      }
      const ctx = canvas.getContext("2d");
      if (!ctx) {
        raf = requestAnimationFrame(draw);
        return;
      }
      const { w, h, dpr } = sizeRef.current;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.fillStyle = BG_COLOR;
      ctx.fillRect(0, 0, w, h);

      // Grid
      ctx.strokeStyle = GRID_COLOR;
      ctx.lineWidth = 1;
      const gridCols = 10;
      const gridRows = 8;
      ctx.beginPath();
      for (let i = 1; i < gridCols; i++) {
        const x = (i / gridCols) * w;
        ctx.moveTo(x, 0);
        ctx.lineTo(x, h);
      }
      for (let i = 1; i < gridRows; i++) {
        const y = (i / gridRows) * h;
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
      }
      ctx.stroke();

      const buf = scopeBuffer.value;
      if (buf.length < 2) {
        raf = requestAnimationFrame(draw);
        return;
      }

      const mode = scopePlotMode.value;
      const enabled = scopeChannelEnabled.value;
      const offsets = scopeChannelOffset.value;
      const inverts = scopeChannelInvert.value;
      const timeBase = scopeTimeBase.value;
      const panT = panRef.current.panT;

      // Time window: [tEnd - timeBase - panT, tEnd - panT]
      const tEnd = buf[buf.length - 1]!.t - panT;
      const tStart = tEnd - timeBase;

      // Gather per-channel min/max for overlay mode
      let gMin = Infinity;
      let gMax = -Infinity;
      const effectiveVals: number[][] = [[], [], [], []];
      for (const p of buf) {
        if (p.t < tStart || p.t > tEnd) continue;
        for (let c = 0; c < 4; c++) {
          if (!enabled[c]) continue;
          let v = p.v[c] ?? 0;
          if (inverts[c]) v = -v;
          v += offsets[c] ?? 0;
          effectiveVals[c]!.push(v);
          if (v < gMin) gMin = v;
          if (v > gMax) gMax = v;
        }
      }
      if (!Number.isFinite(gMin) || !Number.isFinite(gMax)) {
        raf = requestAnimationFrame(draw);
        return;
      }
      if (gMax - gMin < 1e-6) {
        gMin -= 0.5;
        gMax += 0.5;
      }
      const pad = (gMax - gMin) * 0.08;
      gMin -= pad;
      gMax += pad;

      // Zero baseline overlay mode
      if (mode === "overlay" && gMin < 0 && gMax > 0) {
        const y0 = h - ((-gMin) / (gMax - gMin)) * h;
        ctx.strokeStyle = ZERO_LINE;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(0, y0);
        ctx.lineTo(w, y0);
        ctx.stroke();
      }

      // Plot polylines
      const tSpan = tEnd - tStart || 1;
      for (let c = 0; c < 4; c++) {
        if (!enabled[c]) continue;
        ctx.strokeStyle = CH_COLORS[c]!;
        ctx.lineWidth = 1.2;
        ctx.beginPath();

        let laneTop = 0;
        let laneH = h;
        let yMin = gMin;
        let yMax = gMax;
        if (mode === "stacked") {
          laneH = h / 4;
          laneTop = c * laneH;
          // Per-lane range from this channel's samples
          yMin = Infinity;
          yMax = -Infinity;
          for (const v of effectiveVals[c]!) {
            if (v < yMin) yMin = v;
            if (v > yMax) yMax = v;
          }
          if (!Number.isFinite(yMin) || !Number.isFinite(yMax)) continue;
          if (yMax - yMin < 1e-6) {
            yMin -= 0.5;
            yMax += 0.5;
          }
          const lp = (yMax - yMin) * 0.1;
          yMin -= lp;
          yMax += lp;
        }
        const ySpan = yMax - yMin || 1;

        let started = false;
        for (const p of buf) {
          if (p.t < tStart || p.t > tEnd) continue;
          let v = p.v[c] ?? 0;
          if (inverts[c]) v = -v;
          v += offsets[c] ?? 0;
          const x = ((p.t - tStart) / tSpan) * w;
          const norm = (v - yMin) / ySpan;
          const y = laneTop + laneH - norm * laneH;
          if (!started) {
            ctx.moveTo(x, y);
            started = true;
          } else {
            ctx.lineTo(x, y);
          }
        }
        ctx.stroke();
      }

      // Trigger level — dashed, in channel-0 color, only overlay mode
      if (mode === "overlay") {
        const trig = scopeTriggerLevel.value;
        if (trig >= gMin && trig <= gMax) {
          const y = h - ((trig - gMin) / (gMax - gMin)) * h;
          ctx.strokeStyle = CH_COLORS[0]!;
          ctx.lineWidth = 1;
          ctx.setLineDash([6, 4]);
          ctx.beginPath();
          ctx.moveTo(0, y);
          ctx.lineTo(w, y);
          ctx.stroke();
          ctx.setLineDash([]);
        }
      }

      // Cursors
      const drawCursor = (xf: number | null, color: string, label: string) => {
        if (xf == null) return;
        const x = xf * w;
        ctx.strokeStyle = color;
        ctx.lineWidth = 1;
        ctx.setLineDash([3, 3]);
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, h);
        ctx.stroke();
        ctx.setLineDash([]);
        // pill at top
        ctx.fillStyle = "rgba(6,10,20,0.85)";
        const pillW = 52;
        const pillH = 18;
        ctx.fillRect(x - pillW / 2, 4, pillW, pillH);
        ctx.strokeStyle = color;
        ctx.strokeRect(x - pillW / 2, 4, pillW, pillH);
        ctx.fillStyle = color;
        ctx.font = "10px 'JetBrains Mono', monospace";
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        const t = (tStart + xf * tSpan).toFixed(3);
        ctx.fillText(`${label} ${t}s`, x, 4 + pillH / 2);
      };
      drawCursor(cursorA.x, CURSOR_A, "A");
      drawCursor(cursorB.x, CURSOR_B, "B");

      // Delta pill if both cursors present
      if (cursorA.x != null && cursorB.x != null) {
        const dt = (cursorB.x - cursorA.x) * tSpan;
        ctx.fillStyle = "rgba(6,10,20,0.85)";
        const pillW = 140;
        const pillH = 18;
        const px = w - pillW - 8;
        const py = 4;
        ctx.fillRect(px, py, pillW, pillH);
        ctx.strokeStyle = "rgba(100,140,200,0.3)";
        ctx.strokeRect(px, py, pillW, pillH);
        ctx.fillStyle = "#e2e8f0";
        ctx.font = "10px 'JetBrains Mono', monospace";
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(`Δt ${dt.toFixed(4)}s`, px + pillW / 2, py + pillH / 2);
      }

      raf = requestAnimationFrame(draw);
    };
    raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);
  }, [cursorA, cursorB]);

  // ---- Mouse handlers ------------------------------------------------------
  const onMouseDown = (e: MouseEvent) => {
    panRef.current.dragging = true;
    panRef.current.startX = e.clientX;
    panRef.current.startPanT = panRef.current.panT;
  };
  const onMouseMove = (e: MouseEvent) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    if (panRef.current.dragging) {
      const { w } = sizeRef.current;
      const dx = e.clientX - panRef.current.startX;
      const frac = dx / w;
      panRef.current.panT = panRef.current.startPanT + frac * scopeTimeBase.value;
      if (panRef.current.panT < 0) panRef.current.panT = 0;
    }
  };
  const onMouseUp = () => {
    panRef.current.dragging = false;
  };
  const onDblClick = (e: MouseEvent) => {
    // Set cursor A then B on alternating double-clicks
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const xf = (e.clientX - rect.left) / rect.width;
    if (cursorA.x == null) {
      setCursorA({ x: xf });
    } else if (cursorB.x == null) {
      setCursorB({ x: xf });
    } else {
      setCursorA({ x: xf });
      setCursorB({ x: null });
    }
  };
  const onWheel = (e: WheelEvent) => {
    e.preventDefault();
    const factor = e.deltaY > 0 ? 1.2 : 1 / 1.2;
    let next = scopeTimeBase.value * factor;
    if (next < 0.01) next = 0.01;
    if (next > 60) next = 60;
    scopeTimeBase.value = next;
  };

  return (
    <div
      ref={wrapRef}
      class="scope-canvas-wrap"
      onMouseDown={onMouseDown}
      onMouseMove={onMouseMove}
      onMouseUp={onMouseUp}
      onMouseLeave={onMouseUp}
      onDblClick={onDblClick}
      onWheel={onWheel}
    >
      <canvas ref={canvasRef} class="scope-canvas" />
    </div>
  );
}
