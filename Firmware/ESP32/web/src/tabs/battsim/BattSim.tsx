// =============================================================================
// Battery Sim tab - live run status/controls, run browser, zoomable V/I/P/SOC
// history (IndexedDB-mirrored) and window statistics.
// =============================================================================

import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { PairingRequiredError } from "../../api/client";
import { deviceMac, pollIntervalFor } from "../../state/signals";
import * as dev from "./device";
import { ACT, actionLabel, isLongAction, isRefusal, prepareForNewRun, refusalText, runLongAction } from "./settle";
import {
  buildHistory, CHEM_NAMES, ERROR_TEXT, EVENT_NAMES, fmtDuration, fmtElapsedTick, fmtSi,
  STATE_NAMES, stats as winStats, stepSegments, timeStep, view as mkView, type BsStatus, type History, type View,
} from "./history";

const LANES = [
  { key: "v", label: "Voltage", unit: "V", color: "var(--green)" },
  { key: "i", label: "Current", unit: "A", color: "var(--amber)" },
  { key: "p", label: "Power", unit: "W", color: "var(--purple)" },
  { key: "s", label: "SOC", unit: "%", color: "var(--blue)" },
] as const;
const LEFT = 62, AXIS = 20, GAP = 8;

const errMsg = (e: unknown) => (e instanceof Error ? e.message : String(e));
const fmtDate = (s: number) => (s ? new Date(s * 1000).toLocaleString() : "date unknown");
const fmtBytes = (b: number) => (b >= 1 << 20 ? `${(b / 1048576).toFixed(2)} MiB` : `${(b / 1024).toFixed(0)} KiB`);

function cssVar(el: Element, v: string): string {
  const m = /^var\((--[\w-]+)\)$/.exec(v);
  return m ? getComputedStyle(el).getPropertyValue(m[1]!).trim() || "#888" : v;
}

function niceStep(span: number, target: number): number {
  const raw = Math.max(span / target, 1e-15);
  const mag = 10 ** Math.floor(Math.log10(raw));
  const n = raw / mag;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * mag;
}

function draw(c: HTMLCanvasElement, v: View | null, h: History | null, t0: number, t1: number,
              logI: boolean, hoverX: number | null, box: [number, number] | null) {
  const dpr = window.devicePixelRatio || 1;
  const W = c.clientWidth, H = c.clientHeight;
  if (W < 10 || H < 10) return;
  if (c.width !== Math.round(W * dpr)) c.width = Math.round(W * dpr);
  if (c.height !== Math.round(H * dpr)) c.height = Math.round(H * dpr);
  const g = c.getContext("2d")!;
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  g.font = "11px 'JetBrains Mono Variable', monospace";
  const grid = "rgba(148,163,184,0.14)", lab = "rgba(209,213,219,0.8)";
  const pw = W - LEFT - 10, lh = (H - AXIS - GAP * 3) / 4;
  const span = Math.max(t1 - t0, 1e-9);
  const xof = (t: number) => LEFT + ((t - t0) / span) * pw;

  const step = timeStep(span, Math.max(2, pw / 110));
  g.textAlign = "center";
  for (let t = Math.ceil(t0 / step) * step; t <= t1; t += step) {
    const x = Math.round(xof(t)) + 0.5;
    g.strokeStyle = grid; g.beginPath(); g.moveTo(x, 0); g.lineTo(x, H - AXIS); g.stroke();
    g.fillStyle = lab; g.fillText(fmtElapsedTick(t, step), x, H - 6);
  }
  LANES.forEach((ln, li) => {
    const top = li * (lh + GAP);
    g.strokeStyle = grid; g.strokeRect(LEFT + 0.5, top + 0.5, pw - 1, lh - 1);
    g.textAlign = "left"; g.fillStyle = lab; g.fillText(ln.label, LEFT + 6, top + 13);
    if (!v || !v.t.length) return;
    const [lo, av, hi] = ln.key === "v" ? [v.vMin, v.vAvg, v.vMax] : ln.key === "i" ? [v.iMin, v.iAvg, v.iMax]
      : ln.key === "p" ? [v.pAvg, v.pAvg, v.pAvg] : [v.soc, v.soc, v.soc];
    const log = ln.key === "i" && logI;
    const f = (x: number) => (log ? Math.log10(Math.max(x, 1e-10)) : x);
    let y0 = Infinity, y1 = -Infinity;
    // Log axis: zero / negative readings (output off, offset) would stretch it to 1e-10.
    lo.forEach((x) => { if (Number.isFinite(x) && (!log || x > 0)) y0 = Math.min(y0, f(x)); });
    hi.forEach((x) => { if (Number.isFinite(x) && (!log || x > 0)) y1 = Math.max(y1, f(x)); });
    if (log && !Number.isFinite(y0)) av.forEach((x) => { if (x > 0) y0 = Math.min(y0, f(x)); });
    if (!Number.isFinite(y0) || !Number.isFinite(y1)) { y0 = log ? -9 : 0; y1 = log ? 0 : 1; }
    const pad = y1 - y0 < 1e-12 ? (log ? 0.5 : Math.max(Math.abs(y1), 1e-9) * 0.05) : (y1 - y0) * 0.06;
    y0 -= pad; y1 += pad;
    const fy = (x: number) => top + lh - (((log ? Math.log10(Math.max(x, 10 ** y0)) : x) - y0) / (y1 - y0)) * lh;
    g.textAlign = "right";
    if (log) {
      const every = Math.max(1, Math.ceil((y1 - y0) / Math.max(1, lh / 24)));
      for (let d = Math.ceil(y0); d <= y1; d++) {
        if (d % every !== 0) continue;
        const y = Math.round(fy(10 ** d)) + 0.5;
        g.strokeStyle = grid; g.beginPath(); g.moveTo(LEFT, y); g.lineTo(LEFT + pw, y); g.stroke();
        g.fillStyle = lab; g.fillText(fmtSi(10 ** d, "A"), LEFT - 5, y + 4);
      }
    } else {
      const st = niceStep(y1 - y0, Math.max(2, Math.min(6, lh / 28)));
      for (let y = Math.ceil(y0 / st) * st; y <= y1; y += st) {
        const py = Math.round(fy(y)) + 0.5;
        g.strokeStyle = grid; g.beginPath(); g.moveTo(LEFT, py); g.lineTo(LEFT + pw, py); g.stroke();
        g.fillStyle = lab; g.fillText(ln.key === "s" ? `${y.toFixed(0)} %` : fmtSi(y, ln.unit), LEFT - 5, py + 4);
      }
    }
    const col = cssVar(c, ln.color);
    g.save(); g.beginPath(); g.rect(LEFT, top, pw, lh); g.clip();
    const segs = stepSegments(v.t, v.dt);
    if (lo !== av) {
      g.globalAlpha = 0.22; g.fillStyle = col;
      segs.forEach(([a, b], i) => {
        const ya = fy(hi[i]!), yb = fy(lo[i]!);
        g.fillRect(xof(a), Math.min(ya, yb), Math.max(1, xof(b) - xof(a)), Math.max(1, Math.abs(yb - ya)));
      });
      g.globalAlpha = 1;
    }
    g.strokeStyle = col; g.lineWidth = 1.5; g.beginPath();
    segs.forEach(([a, b, joined], i) => {
      const y = fy(av[i]!);
      if (joined) g.lineTo(xof(a), y); else g.moveTo(xof(a), y);
      g.lineTo(xof(b), y);
    });
    g.stroke(); g.lineWidth = 1; g.restore();
    g.fillStyle = cssVar(c, "var(--bg0)");
    g.fillRect(LEFT + 1, top + 1, g.measureText(ln.label).width + 12, 17);
    g.textAlign = "left"; g.fillStyle = lab; g.fillText(ln.label, LEFT + 6, top + 13);
  });
  if (h) {
    g.strokeStyle = g.fillStyle = cssVar(c, "var(--rose)"); g.textAlign = "left"; g.setLineDash([3, 3]);
    let lastLabel = -Infinity;
    for (const e of h.events) {
      if (e.t < t0 || e.t > t1) continue;
      const x = Math.round(xof(e.t)) + 0.5;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, H - AXIS); g.stroke();
      if (x - lastLabel < 48) continue;
      const label = EVENT_NAMES[e.code] ?? "event";
      const wl = g.measureText(label).width;
      g.fillText(label, x + 3 + wl > LEFT + pw ? x - 3 - wl : x + 3, H - AXIS - 4);
      lastLabel = x;
    }
    g.setLineDash([]);
  }
  if (box) { g.fillStyle = "rgba(96,165,250,0.18)"; g.fillRect(Math.min(...box), 0, Math.abs(box[1] - box[0]), H - AXIS); }
  if (hoverX !== null && hoverX >= LEFT && hoverX <= LEFT + pw) {
    g.strokeStyle = lab; g.beginPath(); g.moveTo(Math.round(hoverX) + 0.5, 0); g.lineTo(Math.round(hoverX) + 0.5, H - AXIS); g.stroke();
  }
}

export function BattSim() {
  const mac = deviceMac.value;
  const [st, setSt] = useState<BsStatus | null>(null);
  const [runs, setRuns] = useState<dev.RunSummary[]>([]);
  const [hist, setHist] = useState<History | null>(null);
  const [win, setWin] = useState<[number, number]>([0, 1]);
  const [msg, setMsg] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [logI, setLogI] = useState(true);
  const [hoverX, setHoverX] = useState<number | null>(null);
  const [box, setBox] = useState<[number, number] | null>(null);
  const [showNew, setShowNew] = useState(false);
  const cv = useRef<HTMLCanvasElement>(null);
  const drag = useRef<{ x0: number; w: [number, number]; box: boolean } | null>(null);

  const refreshRuns = async () => {
    try { setRuns(await dev.listRuns()); } catch (e) { setMsg(errMsg(e)); }
  };

  useEffect(() => {
    let alive = true;
    dev.setEpoch().catch(() => {});
    refreshRuns();
    const tick = async () => {
      if (!alive) return;
      try { setSt(await dev.status()); } catch { /* no DAQ HAT or old firmware */ }
      if (alive) setTimeout(tick, pollIntervalFor(2000));
    };
    tick();
    return () => { alive = false; };
  }, []);

  const bounds = (): [number, number] => {
    if (!hist || !hist.recs.length) return [0, 1];
    const a = hist.recs[0]!.t - hist.recs[0]!.dt;
    return [a, Math.max(hist.recs[hist.recs.length - 1]!.t, a + 60)];
  };
  const setWindow = (a: number, b: number) => {
    const [lo, hi] = bounds();
    const w = Math.min(Math.max(b - a, 30), Math.max(hi - lo, 30));
    const s = Math.min(Math.max(a, lo), Math.max(hi - w, lo));
    setWin([s, s + w]);
  };

  const openRun = async (id: number) => {
    setBusy("Opening run...");
    setMsg(null);
    try {
      const files = await dev.syncRun(id, (f, d, t) => setBusy(`Downloading ${f} ${Math.round((d / Math.max(t, 1)) * 100)} %`));
      const h = buildHistory(files);
      setHist(h);
      const a = h.recs.length ? h.recs[0]!.t - h.recs[0]!.dt : 0;
      setWin([a, Math.max(h.recs.length ? h.recs[h.recs.length - 1]!.t : 1, a + 60)]);
    } catch (e) { setMsg(errMsg(e)); }
    setBusy(null);
  };

  const v = useMemo(() => (hist ? mkView(hist, win[0], win[1], cv.current?.clientWidth ?? 1000) : null), [hist, win]);
  const s = useMemo(() => (hist ? winStats(hist, win[0], win[1]) : null), [hist, win]);

  useEffect(() => {
    const c = cv.current;
    if (!c) return;
    const redraw = () => draw(c, v, hist, win[0], win[1], logI, hoverX, box);
    redraw();
    window.addEventListener("resize", redraw);
    return () => window.removeEventListener("resize", redraw);
  }, [v, hist, win, logI, hoverX, box]);

  const xToT = (x: number) => {
    const c = cv.current!;
    return win[0] + ((x - LEFT) / (c.clientWidth - LEFT - 10)) * (win[1] - win[0]);
  };

  const act = async (id: number, run?: number) => {
    if (!mac) return;
    setMsg(null);
    try {
      if (isLongAction(id)) {
        // The P4 finishes these before it replies; a lost reply is not a failure, so poll until it shows.
        setMsg(actionLabel(id, run));
        const deps = { status: dev.status, runIds: dev.runIds, sleep: (ms: number) => new Promise<void>((r) => setTimeout(r, ms)), now: Date.now };
        await runLongAction(deps, () => dev.action(mac, id, run !== undefined ? { run } : {}), id, run);
        setMsg(null);
        if (id === ACT.del && hist?.meta.runId === run) setHist(null);
      } else {
        await dev.action(mac, id, run !== undefined ? { run } : {});
        await new Promise((r) => setTimeout(r, 400));
      }
      const now = await dev.status();
      setSt(now);
      if (now.lastError && !isLongAction(id)) setMsg(ERROR_TEXT[now.lastError] ?? `error ${now.lastError}`);
      if ([ACT.newRun, ACT.del, ACT.stop, ACT.unload, ACT.load, ACT.reopen].includes(id)) refreshRuns();
    } catch (e) {
      if (e instanceof PairingRequiredError) return;
      if (isRefusal(e)) {
        try {
          const now = await dev.status();
          setSt(now);
          if (now.lastError) { setMsg(refusalText(id, now.lastError, ERROR_TEXT)); return; }
        } catch { /* fall through to the raw message */ }
      }
      setMsg(errMsg(e));
    }
  };

  const hover = useMemo(() => {
    if (hoverX === null || !v || !v.t.length) return null;
    const t = xToT(hoverX);
    let i = v.t.findIndex((x) => x >= t);
    if (i < 0) i = v.t.length - 1;
    else if (i > 0 && t - v.t[i - 1]! < v.t[i]! - t) i -= 1;
    return { i, t: v.t[i]! };
  }, [hoverX, v]);

  const presets: [string, number][] = [["1h", 3600], ["6h", 21600], ["1d", 86400], ["7d", 604800], ["30d", 2592000], ["All", 0]];
  const span = win[1] - win[0];
  const [lo, hiB] = bounds();
  const activePreset = !hist ? "" : Math.abs(win[0] - lo) < 1 && Math.abs(win[1] - hiB) < 1 ? "All"
    : Math.abs(win[1] - hiB) < 1 ? presets.find(([, sec]) => sec && Math.abs(sec - span) < 1)?.[0] ?? "" : "";
  const tile = (title: string, rows: [string, string][], cls = "") => (
    <div class={"bsw-tile " + cls}>
      <div class="bsw-tile-t">{title}</div>
      {rows.map(([k, val]) => <div class="bsw-tile-r"><span>{k}</span><b class="mono">{val}</b></div>)}
    </div>
  );

  const state = st?.state ?? 0;
  return (
    <div class="bsw">
      {/* ---- Live status + run controls ---- */}
      <section class="glass-card bsw-bar">
        <span class={"bsw-st s" + state}>{STATE_NAMES[state] ?? "?"}</span>
        {!st ? <span class="text-dim">Battery simulator not reachable (DAQ HAT with battery-sim firmware required).</span> : <>
          {state !== 0 && <>
            <span class="bsw-kv"><i>Run</i><b class="mono">#{st.runId}</b></span>
            <span class="bsw-kv"><i>Battery</i><b class="mono">{CHEM_NAMES[st.chem] ?? "?"} {st.cells}S {st.capacityMah} mAh</b></span>
            <span class="bsw-soc" title="State of charge"><span style={`width:${Math.min(100, Math.max(0, st.socPct))}%`} /><b class="mono">{st.socPct.toFixed(1)} %</b></span>
            <span class="bsw-kv"><i>Voltage</i><b class="mono">{fmtSi(st.vMeas, "V")}</b></span>
            <span class="bsw-kv"><i>Current</i><b class="mono">{fmtSi(st.iMeas, "A")}</b></span>
            <span class="bsw-kv"><i>Elapsed</i><b class="mono">{fmtDuration(st.elapsedS)}</b></span>
            <span class="bsw-kv" title="From the last 30 min of total drain"><i>Remaining</i><b class="mono">{st.remainingS === null ? "-" : (st.provisional ? "~" : "") + fmtDuration(st.remainingS)}</b></span>
          </>}
          <span class="bsw-kv"><i>Storage</i><b class="mono">{fmtBytes(st.fsUsed)} / {fmtBytes(st.fsTotal)}</b></span>
        </>}
        <span class="bsw-spacer" />
        <div class="bsw-actions">
          <button class="btn primary" disabled={state !== 1} onClick={() => act(ACT.start)}>Start</button>
          <button class="btn" disabled={state !== 2} onClick={() => act(ACT.pause)}>Pause</button>
          <button class="btn" disabled={state !== 1 && state !== 2} onClick={() => act(ACT.stop)}>Stop</button>
          <button class="btn" disabled={state === 0} onClick={() => act(ACT.unload)}>Unload</button>
          <button class="btn" disabled={state === 2} onClick={() => setShowNew(true)}>New run</button>
        </div>
      </section>
      {msg && <div class="bsw-msg text-warn">{msg} <button class="btn" onClick={() => setMsg(null)}>Dismiss</button></div>}

      {/* ---- History chart (full width) ---- */}
      <section class="glass-card bsw-chart">
        <div class="bsw-chart-head">
          <select class="input bsw-pick" value={hist ? String(hist.meta.runId) : ""}
            onChange={(e) => { const id = Number((e.currentTarget as HTMLSelectElement).value); if (id) openRun(id); }}>
            <option value="">{runs.length ? "Open a run..." : "No runs stored"}</option>
            {[...runs].reverse().map((r) => (
              <option value={String(r.runId)}>#{r.runId} {r.meta?.name || ""}{r.active ? " (loaded)" : ""}</option>))}
          </select>
          {hist && <span class="text-muted bsw-chart-sub">
            {hist.recs.length} points{hist.meta.version < 2 ? " - energy estimated (format 1)" : ""}
          </span>}
          <span class="bsw-spacer" />
          <div class="segmented">
            {presets.map(([l, sec]) => (
              <button class={activePreset === l ? "active" : ""} disabled={!hist} onClick={() => {
                if (sec) setWindow(hiB - sec, hiB); else setWindow(lo, hiB);
              }}>{l}</button>))}
          </div>
          <div class="segmented">
            <button class={logI ? "active" : ""} onClick={() => setLogI(!logI)} title="Logarithmic current axis">log I</button>
          </div>
        </div>
        <div class="bsw-plot">
          <canvas ref={cv} class="bsw-canvas"
            onWheel={(e) => {
              if (!hist) return;
              e.preventDefault();
              const tc = xToT(e.offsetX), k = e.deltaY > 0 ? 1.25 : 0.8;
              setWindow(tc - (tc - win[0]) * k, tc + (win[1] - tc) * k);
            }}
            onMouseDown={(e) => { drag.current = { x0: e.offsetX, w: win, box: e.shiftKey }; }}
            onMouseMove={(e) => {
              setHoverX(e.offsetX);
              const d = drag.current;
              if (!d || !cv.current) return;
              if (d.box) { setBox([d.x0, e.offsetX]); return; }
              const dt = ((e.offsetX - d.x0) / (cv.current.clientWidth - LEFT - 10)) * (d.w[1] - d.w[0]);
              setWindow(d.w[0] - dt, d.w[1] - dt);
            }}
            onMouseUp={() => {
              if (box && Math.abs(box[1] - box[0]) > 4) setWindow(xToT(Math.min(...box)), xToT(Math.max(...box)));
              drag.current = null; setBox(null);
            }}
            onMouseLeave={() => { setHoverX(null); drag.current = null; setBox(null); }}
            onDblClick={() => setWindow(lo, hiB)} />
          {!hist && <div class="bsw-empty text-dim">{busy ?? "Pick a run above. Wheel = zoom, drag = pan, Shift+drag = zoom to a range, double-click = whole run."}</div>}
          {hist && busy && <div class="bsw-busy">{busy}</div>}
          {hover && v && (
            <div class="bsw-tip mono" style={(hoverX ?? 0) > (cv.current?.clientWidth ?? 0) - 260
              ? `right:${(cv.current?.clientWidth ?? 0) - (hoverX ?? 0) + 14}px` : `left:${(hoverX ?? 0) + 14}px`}>
              <b>+{fmtDuration(hover.t)}</b> <span class="text-muted">({["15 min", "1 min", "1 s"][v.tier[hover.i]! % 3]})</span>
              <div style="color:var(--green)">{fmtSi(v.vAvg[hover.i]!, "V")} [{fmtSi(v.vMin[hover.i]!, "V")} .. {fmtSi(v.vMax[hover.i]!, "V")}]</div>
              <div style="color:var(--amber)">{fmtSi(v.iAvg[hover.i]!, "A")} [{fmtSi(v.iMin[hover.i]!, "A")} .. {fmtSi(v.iMax[hover.i]!, "A")}]</div>
              <div style="color:var(--purple)">{fmtSi(v.pAvg[hover.i]!, "W")}</div>
              <div style="color:var(--blue)">SOC {v.soc[hover.i]!.toFixed(2)} %</div>
            </div>)}
        </div>
      </section>

      {/* ---- Window statistics + run management ---- */}
      <div class="bsw-lower">
        <section class="glass-card">
          <div class="card-header"><div class="card-title">Window statistics</div>
            {s && <div class="text-muted bsw-chart-sub">{fmtDuration(s.duration)} from +{fmtDuration(s.tStart)}</div>}</div>
          {!s ? <div class="text-dim">Open a run to see statistics for the visible window.</div> : (
            <div class="bsw-tiles">
              {tile("Voltage", [["min", fmtSi(s.vMin, "V")], ["avg", fmtSi(s.vAvg, "V")], ["max", fmtSi(s.vMax, "V")]], "c-v")}
              {tile("Current", [["min", fmtSi(s.iMin, "A")], ["avg", fmtSi(s.iAvg, "A")], ["max", fmtSi(s.iMax, "A")]], "c-i")}
              {tile("Current profile", [["baseline P10", fmtSi(s.iP10, "A")], ["median", fmtSi(s.iMedian, "A")], ["busy P99", fmtSi(s.iP99, "A")]], "c-i")}
              {tile("Power", [["avg", fmtSi(s.pAvg, "W")], ["peak", fmtSi(s.pMax, "W")], ["duty avg/peak", s.iMax > 0 ? `${((s.iAvg / s.iMax) * 100).toFixed(2)} %` : "-"]], "c-p")}
              {tile("Consumed", [["charge", fmtSi(s.chargeC / 3600, "Ah")], [s.energyEstimated ? "energy (est.)" : "energy", fmtSi(s.energyJ / 3600, "Wh")]], "c-p")}
              {tile("State of charge", [["start", `${s.socStart.toFixed(2)} %`], ["end", `${s.socEnd.toFixed(2)} %`], ["rate", `${s.socRatePerDay.toFixed(3)} %/day`]], "c-s")}
              {tile("Projection at window avg", [["full battery", fmtDuration(s.projectedLifeS)], ["remaining", fmtDuration(s.remainingAtAvgS)]], "c-s")}
              {(s.gaps > 0 || s.clamped > 0) && tile("Data quality", [["gaps", String(s.gaps)], ["clamped", String(s.clamped)]])}
            </div>)}
        </section>

        <section class="glass-card">
          <div class="card-header"><div class="card-title">Runs on device</div>
            <div class="card-actions"><button class="btn" onClick={refreshRuns}>Refresh</button></div></div>
          {!runs.length && <div class="text-dim">No runs stored.</div>}
          <div class="bsw-runs">
            {[...runs].reverse().map((r) => (
              <div key={r.runId} class={"bsw-run" + (hist?.meta.runId === r.runId ? " open" : "")}>
                <div class="bsw-run-main" onClick={() => openRun(r.runId)} title="Open history">
                  <div><b>#{r.runId}</b> {r.meta?.name || ""} {r.active && <span class="bsw-badge">loaded</span>}</div>
                  <div class="text-muted bsw-sub">
                    {r.meta ? `${CHEM_NAMES[r.meta.params.chem] ?? "?"} ${r.meta.params.cells}S ${r.meta.params.capacityMah} mAh - ${fmtDate(r.meta.createdEpoch)}` : "meta unreadable"} - {fmtBytes(r.bytes)}
                  </div>
                </div>
                <button class="btn" disabled={r.active} title="Load on device (paused)" onClick={() => act(ACT.load, r.runId)}>Load</button>
                <button class="btn danger" disabled={r.active} onClick={() => { if (confirm(`Delete run #${r.runId} from the device?`)) act(ACT.del, r.runId); }}>Delete</button>
              </div>))}
          </div>
        </section>
      </div>
      {showNew && mac && <NewRun mac={mac} onClose={() => setShowNew(false)} onDone={() => { setShowNew(false); refreshRuns(); }} onError={setMsg} />}
    </div>
  );
}

function NewRun({ mac, onClose, onDone, onError }: { mac: string; onClose: () => void; onDone: () => void; onError: (m: string) => void }) {
  const [f, setF] = useState({ name: "", chem: 0, cells: 1, cap: 2000, soc: 100, sd: false, sdPct: 3, ext: false, extUa: 0, dither: false });
  const [busy, setBusy] = useState(false);
  const set = (k: keyof typeof f) => (e: Event) => {
    const t = e.currentTarget as HTMLInputElement;
    setF({ ...f, [k]: t.type === "checkbox" ? t.checked : t.type === "number" || t.tagName === "SELECT" ? Number(t.value) : t.value });
  };
  const create = async () => {
    setBusy(true);
    try {
      const deps = { status: dev.status, runIds: dev.runIds, sleep: (ms: number) => new Promise<void>((r) => setTimeout(r, ms)), now: Date.now };
      await prepareForNewRun(deps, () => dev.action(mac, ACT.unload));
      await dev.cfgSet(mac, 0x0801, "enum", f.chem);
      await dev.cfgSet(mac, 0x0802, "u8", f.cells);
      await dev.cfgSet(mac, 0x0803, "u32", f.cap);
      await dev.action(mac, ACT.defaults);
      await dev.cfgSet(mac, 0x0804, "u16", Math.round(f.soc * 10));
      await dev.cfgSet(mac, 0x0808, "bool", f.sd);
      await dev.cfgSet(mac, 0x0809, "u16", Math.round(f.sdPct * 100));
      await dev.cfgSet(mac, 0x080a, "bool", f.ext);
      await dev.cfgSet(mac, 0x080b, "u32", f.extUa);
      await dev.cfgSet(mac, 0x080c, "bool", f.dither);
      await dev.cfgSet(mac, 0x080f, "str", f.name);
      await runLongAction(deps, () => dev.action(mac, ACT.newRun), ACT.newRun);
      onDone();
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        let why = errMsg(e);
        if (isRefusal(e)) {
          try {
            const st = await dev.status();
            if (st.lastError) why = refusalText(ACT.newRun, st.lastError, ERROR_TEXT);
          } catch { /* keep the raw message */ }
        }
        onError(`New run: ${why}`);
      }
    }
    setBusy(false);
  };
  return (
    <div class="bsw-modal" onClick={(e) => { if (e.target === e.currentTarget) onClose(); }}>
      <div class="glass-card bsw-dialog">
        <div class="card-title">New battery-sim run</div>
        <p class="text-dim">Created paused with the output off. Start needs a 9 V / 3 A USB-PD contract.</p>
        <div class="bsw-form">
          <label>Name<input class="input" maxLength={23} value={f.name} onInput={set("name")} /></label>
          <label>Chemistry<select class="input" value={f.chem} onChange={set("chem")}>{CHEM_NAMES.map((n, i) => <option value={i}>{n}</option>)}</select></label>
          <label>Series cells<input class="input" type="number" min={1} max={14} value={f.cells} onInput={set("cells")} /></label>
          <label>Capacity (mAh)<input class="input" type="number" min={1} value={f.cap} onInput={set("cap")} /></label>
          <label>Start SOC (%)<input class="input" type="number" min={0} max={100} step={0.1} value={f.soc} onInput={set("soc")} /></label>
          <label><input type="checkbox" checked={f.sd} onChange={set("sd")} /> Self-discharge</label>
          <label>Self-discharge (%/month)<input class="input" type="number" step={0.1} disabled={!f.sd} value={f.sdPct} onInput={set("sdPct")} /></label>
          <label><input type="checkbox" checked={f.ext} onChange={set("ext")} /> Virtual external load</label>
          <label>External load (uA)<input class="input" type="number" disabled={!f.ext} value={f.extUa} onInput={set("extUa")} /></label>
          <label><input type="checkbox" checked={f.dither} onChange={set("dither")} /> Sub-step dithering</label>
        </div>
        <div class="bsw-actions"><button class="btn" onClick={onClose}>Cancel</button>
          <button class="btn primary" disabled={busy} onClick={create}>{busy ? "Creating..." : "Create run"}</button></div>
      </div>
    </div>
  );
}
