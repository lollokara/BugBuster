// =============================================================================
// Battery Sim tab - live run status/controls, run browser, zoomable V/I/P/SOC
// history (IndexedDB-mirrored) and window statistics.
// =============================================================================

import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { PairingRequiredError } from "../../api/client";
import { deviceMac, pollIntervalFor } from "../../state/signals";
import * as dev from "./device";
import {
  buildHistory, CHEM_NAMES, ERROR_TEXT, EVENT_NAMES, fmtDuration, fmtElapsedTick, fmtSi,
  STATE_NAMES, stats as winStats, timeStep, view as mkView, type BsStatus, type History, type View,
} from "./history";

const ACT = { defaults: 14, newRun: 7, start: 8, pause: 9, stop: 10, unload: 11, load: 12, del: 13 };
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
    lo.forEach((x) => { if (Number.isFinite(x)) y0 = Math.min(y0, f(x)); });
    hi.forEach((x) => { if (Number.isFinite(x)) y1 = Math.max(y1, f(x)); });
    if (!Number.isFinite(y0)) { y0 = 0; y1 = 1; }
    const pad = y1 - y0 < 1e-12 ? (log ? 0.5 : Math.max(Math.abs(y1), 1e-9) * 0.05) : (y1 - y0) * 0.06;
    y0 -= pad; y1 += pad;
    const fy = (x: number) => top + lh - ((f(x) - y0) / (y1 - y0)) * lh;
    g.textAlign = "right";
    if (log) {
      for (let d = Math.ceil(y0); d <= y1; d++) {
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
    if (lo !== av) {
      g.globalAlpha = 0.18; g.fillStyle = col; g.beginPath();
      v.t.forEach((t, i) => (i ? g.lineTo(xof(t), fy(hi[i]!)) : g.moveTo(xof(t), fy(hi[i]!))));
      for (let i = v.t.length - 1; i >= 0; i--) g.lineTo(xof(v.t[i]!), fy(lo[i]!));
      g.closePath(); g.fill(); g.globalAlpha = 1;
    }
    g.strokeStyle = col; g.lineWidth = 1.5; g.beginPath();
    v.t.forEach((t, i) => (i ? g.lineTo(xof(t), fy(av[i]!)) : g.moveTo(xof(t), fy(av[i]!))));
    g.stroke(); g.lineWidth = 1; g.restore();
  });
  if (h) {
    g.strokeStyle = g.fillStyle = cssVar(c, "var(--rose)"); g.textAlign = "left"; g.setLineDash([3, 3]);
    for (const e of h.events) {
      if (e.t < t0 || e.t > t1) continue;
      const x = Math.round(xof(e.t)) + 0.5;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, H - AXIS); g.stroke();
      g.fillText(EVENT_NAMES[e.code] ?? "event", x + 3, H - AXIS - 4);
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
      await dev.action(mac, id, run !== undefined ? { run } : {});
      await new Promise((r) => setTimeout(r, 400));
      const now = await dev.status();
      setSt(now);
      if (now.lastError) setMsg(ERROR_TEXT[now.lastError] ?? `error ${now.lastError}`);
      if ([ACT.newRun, ACT.del, ACT.stop, ACT.unload, ACT.load].includes(id)) refreshRuns();
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) setMsg(errMsg(e));
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

  const state = st?.state ?? 0;
  return (
    <div class="tab-container bsw">
      <h2>Battery Simulator</h2>
      <GlassCard title="Live" actions={
        <div class="bsw-actions">
          <button class="btn primary" disabled={state !== 1} onClick={() => act(ACT.start)}>Start</button>
          <button class="btn" disabled={state !== 2} onClick={() => act(ACT.pause)}>Pause</button>
          <button class="btn" disabled={state !== 1 && state !== 2} onClick={() => act(ACT.stop)}>Stop</button>
          <button class="btn" disabled={state === 0} onClick={() => act(ACT.unload)}>Unload</button>
          <button class="btn" disabled={state === 2} onClick={() => setShowNew(true)}>New run</button>
        </div>}>
        {!st ? <div class="text-dim">Battery simulator not reachable (DAQ HAT and firmware with battery-sim support required).</div> : (
          <div class="bsw-live">
            <span class={"pill bsw-st s" + state}>{STATE_NAMES[state] ?? "?"}</span>
            {state !== 0 && <>
              <span class="bsw-kv"><i>Run</i><b class="mono">#{st.runId}</b></span>
              <span class="bsw-kv"><i>{CHEM_NAMES[st.chem] ?? "?"}</i><b class="mono">{st.cells}S {st.capacityMah} mAh</b></span>
              <span class="bsw-soc"><span style={`width:${Math.min(100, Math.max(0, st.socPct))}%`} /><b class="mono">{st.socPct.toFixed(1)} %</b></span>
              <span class="bsw-kv"><i>V</i><b class="mono">{fmtSi(st.vMeas, "V")}</b></span>
              <span class="bsw-kv"><i>I</i><b class="mono">{fmtSi(st.iMeas, "A")}</b></span>
              <span class="bsw-kv"><i>Elapsed</i><b class="mono">{fmtDuration(st.elapsedS)}</b></span>
              <span class="bsw-kv"><i>Remaining</i><b class="mono">{st.remainingS === null ? "-" : (st.provisional ? "~" : "") + fmtDuration(st.remainingS)}</b></span>
            </>}
            <span class="bsw-kv"><i>Storage</i><b class="mono">{fmtBytes(st.fsUsed)} / {fmtBytes(st.fsTotal)}</b></span>
          </div>)}
        {msg && <div class="text-warn bsw-msg">{msg}</div>}
      </GlassCard>

      <div class="bsw-grid">
        <GlassCard title="Runs" actions={<button class="btn" onClick={refreshRuns}>Refresh</button>}>
          {!runs.length && <div class="text-dim">No runs stored.</div>}
          {[...runs].reverse().map((r) => (
            <div key={r.runId} class={"bsw-run" + (hist?.meta.runId === r.runId ? " open" : "")}>
              <div class="bsw-run-main" onClick={() => openRun(r.runId)}>
                <b>#{r.runId}</b> {r.meta?.name || ""} {r.active && <span class="pill">loaded</span>}
                <div class="text-muted bsw-sub">
                  {r.meta ? `${CHEM_NAMES[r.meta.params.chem] ?? "?"} ${r.meta.params.cells}S ${r.meta.params.capacityMah} mAh - ${fmtDate(r.meta.createdEpoch)}` : "meta unreadable"} - {fmtBytes(r.bytes)}
                </div>
              </div>
              <button class="btn" disabled={r.active} title="Load on device (paused)" onClick={() => act(ACT.load, r.runId)}>Load</button>
              <button class="btn danger" disabled={r.active} onClick={() => { if (confirm(`Delete run #${r.runId} from the device?`)) act(ACT.del, r.runId); }}>Delete</button>
            </div>))}
        </GlassCard>

        <GlassCard title={hist ? `Run #${hist.meta.runId} ${hist.meta.name}` : "History"} actions={
          <div class="bsw-actions">
            {[["1h", 3600], ["6h", 21600], ["1d", 86400], ["7d", 604800], ["30d", 2592000], ["All", 0]].map(([l, sec]) => (
              <button key={l as string} class="btn" disabled={!hist} onClick={() => {
                const [lo, hi] = bounds();
                if (sec) setWindow(hi - (sec as number), hi); else setWindow(lo, hi);
              }}>{l}</button>))}
            <button class={"btn" + (logI ? " primary" : "")} onClick={() => setLogI(!logI)}>log I</button>
          </div>}>
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
              onDblClick={() => { const [lo, hi] = bounds(); setWindow(lo, hi); }} />
            {!hist && <div class="bsw-empty text-dim">{busy ?? "Open a run. Wheel = zoom, drag = pan, Shift+drag = zoom to range, double-click = whole run."}</div>}
            {hist && busy && <div class="bsw-busy">{busy}</div>}
            {hover && v && (
              <div class="bsw-tip mono" style={`left:${(hoverX ?? 0) + 14}px`}>
                <b>+{fmtDuration(hover.t)}</b> ({["15 min", "1 min", "1 s"][v.tier[hover.i]! % 3]})
                <div style="color:var(--green)">{fmtSi(v.vAvg[hover.i]!, "V")} [{fmtSi(v.vMin[hover.i]!, "V")} .. {fmtSi(v.vMax[hover.i]!, "V")}]</div>
                <div style="color:var(--amber)">{fmtSi(v.iAvg[hover.i]!, "A")} [{fmtSi(v.iMin[hover.i]!, "A")} .. {fmtSi(v.iMax[hover.i]!, "A")}]</div>
                <div style="color:var(--purple)">{fmtSi(v.pAvg[hover.i]!, "W")}</div>
                <div style="color:var(--blue)">SOC {v.soc[hover.i]!.toFixed(2)} %</div>
              </div>)}
          </div>
        </GlassCard>

        <GlassCard title="Window statistics">
          {!s ? <div class="text-dim">Open a run.</div> : (
            <table class="kv-table bsw-stats"><tbody>
              <tr><th>Span</th><td>{fmtDuration(s.duration)} from +{fmtDuration(s.tStart)}</td></tr>
              <tr><th>Voltage min / avg / max</th><td>{fmtSi(s.vMin, "V")} / {fmtSi(s.vAvg, "V")} / {fmtSi(s.vMax, "V")}</td></tr>
              <tr><th>Current min / avg / max</th><td>{fmtSi(s.iMin, "A")} / {fmtSi(s.iAvg, "A")} / {fmtSi(s.iMax, "A")}</td></tr>
              <tr><th>Baseline (P10) / median / P99</th><td>{fmtSi(s.iP10, "A")} / {fmtSi(s.iMedian, "A")} / {fmtSi(s.iP99, "A")}</td></tr>
              <tr><th>Power avg / peak</th><td>{fmtSi(s.pAvg, "W")} / {fmtSi(s.pMax, "W")}</td></tr>
              <tr><th>Charge</th><td>{fmtSi(s.chargeC / 3600, "Ah")}</td></tr>
              <tr><th>Energy{s.energyEstimated ? " (est.)" : ""}</th><td>{fmtSi(s.energyJ / 3600, "Wh")}</td></tr>
              <tr><th>SOC</th><td>{s.socStart.toFixed(2)} {"->"} {s.socEnd.toFixed(2)} % ({s.socRatePerDay.toFixed(3)} %/day)</td></tr>
              <tr><th>Full-battery life at avg</th><td>{fmtDuration(s.projectedLifeS)}</td></tr>
              <tr><th>Remaining at avg</th><td>{fmtDuration(s.remainingAtAvgS)}</td></tr>
              {(s.gaps > 0 || s.clamped > 0) && <tr><th>Data quality</th><td>{s.gaps} gap(s), {s.clamped} clamped</td></tr>}
            </tbody></table>)}
        </GlassCard>
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
      await dev.action(mac, ACT.newRun);
      onDone();
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) onError(`New run: ${errMsg(e)}`);
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
