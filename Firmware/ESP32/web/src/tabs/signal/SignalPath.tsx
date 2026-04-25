// =============================================================================
// SignalPath tab — ADGS2414D mux visualisation.
//
// Byte-for-byte port of the desktop renderer at
// DesktopApp/BugBuster/src/tabs/signal_path.rs. Full Canvas 2D drawing of the
// 4-channel MUX topology: PSU bars, level shifters (U13/U15), GPIO pairs (direct
// green + 2kΩ yellow), ADC (blue), EXT (orange), e-fuses, and 4 output
// connectors (P1..P4). Click a switch bar inside any MUX to toggle it.
// =============================================================================

import { useEffect, useRef } from "preact/hooks";
import { useSignal } from "@preact/signals";
import { api, PairingRequiredError } from "../../api/client";
import {
  deviceStatus,
  deviceMac,
  supplyMonitorActive,
  startSelftestStatusPolling,
} from "../../state/signals";
import { ChDOverlay } from "../../components/ChDOverlay";

/* ---------- Constants (verbatim from signal_path.rs) ---------- */

const PRESETS: ReadonlyArray<readonly [string, readonly [number, number, number, number]]> = [
  ["All Open", [0x00, 0x00, 0x00, 0x00]],
  ["GPIO Direct", [0x51, 0x51, 0x51, 0x51]],
  ["ADC Read", [0x04, 0x04, 0x04, 0x04]],
  ["External", [0x08, 0x08, 0x08, 0x08]],
] as const;

const C_GPIO = "#22c55e";   // Green - direct GPIO
const C_GPIO_R = "#eab308"; // Yellow - GPIO via 2kΩ
const C_ADC = "#3b82f6";    // Blue - ADC
const C_EXT = "#f97316";    // Orange - external
const C_BG = "#070d1a";
const C_CHIP = "#0e1629";
const C_CHIP_BD = "#1e3050";

const ACCENTS = ["#3b82f6", "#10b981", "#f59e0b", "#a855f7"] as const;
const MUX_REF = ["U10", "U11", "U17", "U16"] as const;

const GPIO_PAIR_LABELS: ReadonlyArray<readonly [string, string, string]> = [
  ["IO3", "IO2", "IO1"],
  ["IO6", "IO5", "IO4"],
  ["IO9", "IO8", "IO7"],
  ["IO12", "IO11", "IO10"],
];

const ADC_LABELS = ["CH A", "CH B", "CH D", "CH C"] as const;
const EXT_LABELS = ["EXT 1", "EXT 2", "EXT 3", "EXT 4"] as const;

const EFUSE_CTRL_NAMES = ["efuse1", "efuse2", "efuse3", "efuse4"] as const;

/* ---------- Drawing helpers ---------- */

function rrect(c: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number): void {
  c.beginPath();
  c.moveTo(x + r, y);
  c.lineTo(x + w - r, y);
  c.arcTo(x + w, y, x + w, y + r, r);
  c.lineTo(x + w, y + h - r);
  c.arcTo(x + w, y + h, x + w - r, y + h, r);
  c.lineTo(x + r, y + h);
  c.arcTo(x, y + h, x, y + h - r, r);
  c.lineTo(x, y + r);
  c.arcTo(x, y, x + r, y, r);
  c.closePath();
}

function drawResistor(c: CanvasRenderingContext2D, x: number, y: number, w: number, color: string): void {
  c.strokeStyle = color;
  c.lineWidth = 1.0;
  c.beginPath();
  const steps = 4;
  const stepW = w / steps;
  const amp = 3.0;
  c.moveTo(x, y);
  for (let i = 0; i < steps; i++) {
    const sx = x + i * stepW;
    c.lineTo(sx + stepW * 0.25, y - amp);
    c.lineTo(sx + stepW * 0.75, y + amp);
    c.lineTo(sx + stepW, y);
  }
  c.stroke();
  c.fillStyle = color;
  c.font = "5px monospace";
  c.textAlign = "center";
  c.fillText("2k", x + w / 2, y - 5);
}

function psuBar(
  c: CanvasRenderingContext2D,
  x: number, y: number, w: number, h: number,
  name: string, feeds: string, on: boolean,
): void {
  rrect(c, x, y, w, h, 5);
  c.fillStyle = on ? "#180808" : "#0a1020";
  c.fill();
  c.strokeStyle = on ? "#5b1818" : "#182030";
  c.lineWidth = 1.0;
  c.stroke();
  c.fillStyle = on ? "#ef4444" : "#475569";
  c.font = "bold 11px Inter, sans-serif";
  c.textAlign = "left";
  c.fillText(name, x + 10, y + 16);
  c.fillStyle = "#3b4a60";
  c.font = "8px monospace";
  c.fillText(`LTM8063 · DS4424 · 3-15V  ${feeds}`, x + 10, y + 30);
  c.fillStyle = on ? "#10b981" : "#1e293b";
  c.beginPath();
  c.arc(x + w - 20, y + h / 2, 4, 0, Math.PI * 2);
  c.fill();
  c.fillStyle = on ? "#f59e0b" : "#1e293b";
  c.beginPath();
  c.arc(x + w - 6, y + h / 2, 4, 0, Math.PI * 2);
  c.fill();
}

/* ---------- Component ---------- */

export function SignalPath() {
  const mux = useSignal<[number, number, number, number]>([0, 0, 0, 0]);
  const psu = useSignal<[boolean, boolean]>([false, false]);
  const ef = useSignal<[boolean, boolean, boolean, boolean]>([false, false, false, false]);
  const oe = useSignal<boolean>(false);
  const tooSmall = useSignal<boolean>(false);
  const opStatus = useSignal<string | null>(null);

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const wrapRef = useRef<HTMLDivElement | null>(null);
  const rafRef = useRef<number | null>(null);
  const dimsRef = useRef<{ w: number; h: number }>({ w: 0, h: 0 });

  useEffect(() => startSelftestStatusPolling(), []);

  /* ---- Poll /api/mux state (fallback when status.muxStates missing) ---- */
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const st = deviceStatus.value;
        if (st?.muxStates && Array.isArray(st.muxStates) && st.muxStates.length >= 4) {
          mux.value = [
            Number(st.muxStates[0]) & 0xff,
            Number(st.muxStates[1]) & 0xff,
            Number(st.muxStates[2]) & 0xff,
            Number(st.muxStates[3]) & 0xff,
          ];
        } else {
          const r = await api.mux();
          if (alive && r?.states && Array.isArray(r.states) && r.states.length >= 4) {
            mux.value = [
              Number(r.states[0]) & 0xff,
              Number(r.states[1]) & 0xff,
              Number(r.states[2]) & 0xff,
              Number(r.states[3]) & 0xff,
            ];
          }
        }
      } catch {
        /* ignore transient errors */
      }
    };
    tick();
    const id = window.setInterval(tick, 500);
    return () => { alive = false; window.clearInterval(id); };
  }, []);

  /* ---- Poll /api/ioexp for PSU + efuse state ---- */
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const r = await api.ioexp();
        if (!alive || !r) return;
        const en = r.enables ?? r.enable ?? r.en ?? r;
        const v1 = !!(en?.vadj1 ?? r.vadj1_en ?? r.vadj1);
        const v2 = !!(en?.vadj2 ?? r.vadj2_en ?? r.vadj2);
        psu.value = [v1, v2];
        const efuses = Array.isArray(r.efuses) ? r.efuses : [];
        const arr: [boolean, boolean, boolean, boolean] = [false, false, false, false];
        for (let i = 0; i < 4; i++) {
          const e = efuses[i];
          if (e) arr[i] = !!(e.enabled ?? e.en);
        }
        ef.value = arr;
        if (typeof r.lshiftOe === "boolean") oe.value = r.lshiftOe;
        else if (typeof r.lshift_oe === "boolean") oe.value = r.lshift_oe;
      } catch {
        /* ignore */
      }
    };
    tick();
    const id = window.setInterval(tick, 500);
    return () => { alive = false; window.clearInterval(id); };
  }, []);

  /* ---- Also read lshift state from deviceStatus if present ---- */
  useEffect(() => {
    const st = deviceStatus.value;
    if (st && typeof st.lshiftEnabled === "boolean") oe.value = st.lshiftEnabled;
  });

  /* ---- Canvas size observer ---- */
  useEffect(() => {
    const wrap = wrapRef.current;
    const canvas = canvasRef.current;
    if (!wrap || !canvas) return;
    const measure = () => {
      const rect = wrap.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      dimsRef.current = { w: rect.width, h: rect.height };
      tooSmall.value = rect.width < 600 || rect.height < 400;
      canvas.width = Math.max(1, Math.floor(rect.width * dpr));
      canvas.height = Math.max(1, Math.floor(rect.height * dpr));
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(wrap);
    return () => ro.disconnect();
  }, []);

  /* ---- RAF draw loop ---- */
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    let disposed = false;

    const draw = () => {
      if (disposed) return;
      rafRef.current = window.requestAnimationFrame(draw);
      const { w, h } = dimsRef.current;
      if (w < 600 || h < 400) return;
      const ctx = canvas.getContext("2d");
      if (!ctx) return;
      const dpr = window.devicePixelRatio || 1;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      render(ctx, w, h, mux.value, psu.value, ef.value, oe.value);
    };
    rafRef.current = window.requestAnimationFrame(draw);
    return () => {
      disposed = true;
      if (rafRef.current !== null) window.cancelAnimationFrame(rafRef.current);
    };
  }, []);

  /* ---- Click-to-toggle a switch bar inside a MUX chip ---- */
  const onCanvasClick = (e: MouseEvent) => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    const w = rect.width;
    const h = rect.height;
    if (w < 600 || h < 400) return;

    const psuH = 42;
    const rt = psuH + 10;
    const ra = h - rt - 4;
    const rh = ra / 4;
    const muxL = w * 0.18;
    const muxR = w * 0.50;

    if (y < rt || x < muxL || x > muxR) return;
    const ch = Math.floor((y - rt) / rh);
    if (ch < 0 || ch >= 4) return;

    const ry = rt + ch * rh;
    const lh = (rh - 12) / 8.5;
    const g = lh * 0.4;
    const syArr: number[] = new Array(8);
    for (let s = 0; s < 8; s++) {
      const gap = s >= 6 ? g * 2 : s >= 4 ? g : 0;
      syArr[s] = ry + 6 + s * lh + gap;
    }
    for (let s = 0; s < 8; s++) {
      syArr[s] = 2 * ry + rh - syArr[s]!;
    }
    let best = -1;
    let bestD = Infinity;
    for (let s = 0; s < 8; s++) {
      const d = Math.abs(y - syArr[s]!);
      if (d < bestD) { bestD = d; best = s; }
    }
    if (best < 0 || bestD > lh * 0.8) return;
    toggleSwitch(ch, best);
  };

  const toggleSwitch = (d: number, s: number) => {
    opStatus.value = null;
    const cur = mux.value.slice() as [number, number, number, number];
    const prev = cur.slice() as [number, number, number, number];
    const on = ((cur[d]! >> s) & 1) !== 0;
    const mac = deviceMac.value;
    let newByte = cur[d]!;
    if (!on) {
      const groupMask = s < 4 ? 0x0f : s < 6 ? 0x30 : 0xc0;
      newByte &= ~groupMask & 0xff;
      newByte |= (1 << s) & 0xff;
    } else {
      newByte &= ~(1 << s) & 0xff;
    }
    cur[d] = newByte;
    mux.value = cur;
    if (mac) {
      api.mux.setSwitch(mac, d, s, !on).catch((e) => {
        mux.value = prev;
        if (!(e instanceof PairingRequiredError)) {
          opStatus.value = e instanceof Error ? e.message : "MUX switch update failed";
        }
      });
    }
  };

  const applyPreset = (states: readonly [number, number, number, number]) => {
    opStatus.value = null;
    const mac = deviceMac.value;
    const prev = mux.value.slice() as [number, number, number, number];
    mux.value = [states[0], states[1], states[2], states[3]];
    if (mac) {
      api.mux
        .setAll(mac, [states[0], states[1], states[2], states[3]])
        .catch((e) => {
          mux.value = prev;
          if (!(e instanceof PairingRequiredError)) {
            opStatus.value = e instanceof Error ? e.message : "MUX preset apply failed";
          }
        });
    }
  };

  const togglePsu = (i: 0 | 1) => {
    opStatus.value = null;
    const mac = deviceMac.value;
    const prev = psu.value.slice() as [boolean, boolean];
    const next = (psu.value.slice() as [boolean, boolean]);
    next[i] = !next[i];
    psu.value = next;
    if (mac) {
      api.ioexp.setControl(mac, i === 0 ? "vadj1" : "vadj2", next[i]).catch((e) => {
        psu.value = prev;
        if (!(e instanceof PairingRequiredError)) {
          opStatus.value = e instanceof Error ? e.message : "PSU toggle failed";
        }
      });
    }
  };

  const toggleEfuse = (i: 0 | 1 | 2 | 3) => {
    opStatus.value = null;
    const mac = deviceMac.value;
    const prev = ef.value.slice() as [boolean, boolean, boolean, boolean];
    const next = ef.value.slice() as [boolean, boolean, boolean, boolean];
    next[i] = !next[i];
    ef.value = next;
    if (mac) {
      api.ioexp.setControl(mac, EFUSE_CTRL_NAMES[i]!, next[i]).catch((e) => {
        ef.value = prev;
        if (!(e instanceof PairingRequiredError)) {
          opStatus.value = e instanceof Error ? e.message : "EFuse toggle failed";
        }
      });
    }
  };

  const toggleOe = () => {
    opStatus.value = null;
    const mac = deviceMac.value;
    const prev = oe.value;
    const next = !oe.value;
    oe.value = next;
    if (mac) {
      api.lshift.setOe(mac, next).catch((e) => {
        oe.value = prev;
        if (!(e instanceof PairingRequiredError)) {
          opStatus.value = e instanceof Error ? e.message : "LShift OE update failed";
        }
      });
    }
  };

  return (
    <div class="signal-layout">
      <div class="signal-toolbar">
        <span class="uppercase-tag">Signal Path</span>
        <div class="signal-preset-pills">
          {PRESETS.map(([name, states]) => (
            <button key={name} class="pill" onClick={() => applyPreset(states)}>{name}</button>
          ))}
        </div>
        <button class={"pill" + (oe.value ? " active" : "")} onClick={toggleOe}>LShift OE</button>
        <button class={"pill" + (psu.value[0] ? " active" : "")} onClick={() => togglePsu(0)}>V_ADJ1</button>
        <button class={"pill" + (psu.value[1] ? " active" : "")} onClick={() => togglePsu(1)}>V_ADJ2</button>
        {[0, 1, 2, 3].map((i) => (
          <button
            key={`ef${i}`}
            class={"pill" + (ef.value[i] ? " active" : "")}
            onClick={() => toggleEfuse(i as 0 | 1 | 2 | 3)}
          >EF{i + 1}</button>
        ))}
      </div>

      <div class="signal-legend">
        <span class="signal-leg-item" style={{ color: C_GPIO }}>● GPIO (direct)</span>
        <span class="signal-leg-item" style={{ color: C_GPIO_R }}>● GPIO (2kΩ)</span>
        <span class="signal-leg-item" style={{ color: C_ADC }}>● ADC Channel</span>
        <span class="signal-leg-item" style={{ color: C_EXT }}>● External</span>
        <span class="signal-leg-item" style={{ color: "#ef4444" }}>● Power</span>
      </div>

      {opStatus.value && (
        <div class="text-err" style={{ fontSize: "0.8rem", marginTop: "6px" }}>
          {opStatus.value}
        </div>
      )}

      <div ref={wrapRef} class="signal-canvas-wrap">
        <ChDOverlay
          active={supplyMonitorActive.value}
          overlayStyle={{
            top: "calc(42px + 10px + ((100% - 42px - 10px - 4px) / 4) * 2)",
            bottom: "auto",
            height: "calc((100% - 42px - 10px - 4px) / 4)",
            borderLeft: "none",
            borderRight: "none",
            borderRadius: 0,
          }}
        >
          <canvas ref={canvasRef} class="signal-canvas" onClick={onCanvasClick} />
          {tooSmall.value && (
            <div class="signal-too-small">Window too narrow — widen to at least 600×400 px.</div>
          )}
        </ChDOverlay>
      </div>
    </div>
  );
}

/* ---------- Full renderer (verbatim port of Leptos/Rust version) ---------- */

function render(
  c: CanvasRenderingContext2D,
  w: number, h: number,
  ms: readonly [number, number, number, number],
  ps: readonly [boolean, boolean],
  es: readonly [boolean, boolean, boolean, boolean],
  oeOn: boolean,
): void {
  c.fillStyle = C_BG;
  c.fillRect(0, 0, w, h);

  const psuH = 42;
  const rt = psuH + 10;
  const ra = h - rt - 4;
  const rh = ra / 4;

  const gpioX = w * 0.06;
  const lsL = w * 0.085;
  const lsR = w * 0.115;
  const adcX = w * 0.155;
  const muxL = w * 0.18;
  const muxR = w * 0.50;
  const outX = w * 0.52;
  const efX = w * 0.63;
  const cnL = w * 0.76;
  const cnR = w * 0.95;

  /* --- PSU bars --- */
  psuBar(c, 8, 4, w * 0.48 - 12, psuH, "V_ADJ1", "→ P1, P2", ps[0]);
  psuBar(c, w * 0.5 + 4, 4, w * 0.48 - 12, psuH, "V_ADJ2", "→ P3, P4", ps[1]);

  /* --- Level shifters U13 (rows 0+1), U15 (rows 2+3) --- */
  for (let pair = 0; pair < 2; pair++) {
    const y1 = rt + pair * 2 * rh;
    const y2 = y1 + 2 * rh;
    const lsName = pair === 0 ? "U13" : "U15";
    const lsPad = 4;

    rrect(c, lsL - 1, y1 + lsPad, lsR - lsL + 2, (y2 - y1) - lsPad * 2, 3);
    c.fillStyle = oeOn ? "#0c1a12" : "#0a0f1c";
    c.fill();
    c.strokeStyle = oeOn ? "#1a4030" : "#1a2a40";
    c.lineWidth = 1.0;
    c.stroke();

    c.fillStyle = oeOn ? "#22c55e" : "#2a3f5f";
    c.font = "bold 7px monospace";
    c.textAlign = "center";
    c.fillText(lsName, (lsL + lsR) / 2, y1 + lsPad - 2);

    const oeY = y2 - lsPad - 6;
    c.fillStyle = oeOn ? "#22c55e" : "#1e293b";
    c.beginPath();
    c.arc((lsL + lsR) / 2, oeY, 3, 0, Math.PI * 2);
    c.fill();
    if (oeOn) {
      c.fillStyle = "rgba(34,197,94,0.15)";
      c.beginPath();
      c.arc((lsL + lsR) / 2, oeY, 7, 0, Math.PI * 2);
      c.fill();
    }
    c.fillStyle = "#334155";
    c.font = "5px monospace";
    c.fillText("OE", (lsL + lsR) / 2, oeY + 10);
  }

  /* --- Channel rows --- */
  for (let ch = 0; ch < 4; ch++) {
    const ry = rt + ch * rh;
    const st = ms[ch]!;
    const ac = ACCENTS[ch]!;
    const pi = ch < 2 ? 0 : 1;
    const psuOn = ps[pi];
    const efOn = es[ch]!;

    if (ch > 0) {
      c.strokeStyle = "#111828";
      c.lineWidth = 0.5;
      c.beginPath();
      c.moveTo(0, ry);
      c.lineTo(w, ry);
      c.stroke();
    }

    /* --- Switch Y positions --- */
    const lh = (rh - 12) / 8.5;
    const g = lh * 0.4;
    const sy: number[] = new Array(8);
    for (let s = 0; s < 8; s++) {
      const gap = s >= 6 ? g * 2 : s >= 4 ? g : 0;
      sy[s] = ry + 6 + s * lh + gap;
    }
    for (let s = 0; s < 8; s++) {
      sy[s] = 2 * ry + rh - sy[s]!;
    }

    /* --- MUX chip --- */
    const mt = ry + 2;
    const mh = rh - 4;
    c.fillStyle = C_CHIP;
    c.fillRect(muxL, mt, muxR - muxL, mh);
    c.strokeStyle = C_CHIP_BD;
    c.lineWidth = 1.0;
    c.strokeRect(muxL, mt, muxR - muxL, mh);

    /* --- Group separators --- */
    c.strokeStyle = "#162540";
    c.lineWidth = 0.5;
    const sep1 = (sy[3]! + sy[4]!) / 2;
    const sep2 = (sy[5]! + sy[6]!) / 2;
    c.beginPath(); c.moveTo(muxL + 3, sep1); c.lineTo(muxR - 3, sep1); c.stroke();
    c.beginPath(); c.moveTo(muxL + 3, sep2); c.lineTo(muxR - 3, sep2); c.stroke();

    /* --- MUX label --- */
    c.fillStyle = ac;
    c.font = "bold 9px monospace";
    c.textAlign = "center";
    c.fillText(`MUX ${ch + 1} · ${MUX_REF[ch]}`, (muxL + muxR) / 2, mt + mh - 3);

    /* --- GPIO pairs: IO -> LS -> split --- */
    const gpioPairs: ReadonlyArray<readonly [number, number, number]> = [[0, 1, 0], [4, 5, 1], [6, 7, 2]];
    for (const [sd, sr, pi2] of gpioPairs) {
      const yd = sy[sd]!;
      const yr = sy[sr]!;
      const ym = (yd + yr) / 2;
      const lbl = GPIO_PAIR_LABELS[ch]![pi2]!;

      c.font = "bold 9px monospace";
      c.textAlign = "right";
      c.fillStyle = "#e2e8f0";
      c.fillText(lbl, gpioX, ym + 3);

      c.strokeStyle = "#94a3b8";
      c.lineWidth = 1.0;
      c.beginPath(); c.moveTo(gpioX + 3, ym); c.lineTo(lsL, ym); c.stroke();

      c.fillStyle = "#94a3b8";
      c.beginPath(); c.arc(lsL, ym, 1.5, 0, Math.PI * 2); c.fill();
      c.beginPath(); c.arc(lsR, ym, 1.5, 0, Math.PI * 2); c.fill();

      const sp = lsR + 4;
      c.strokeStyle = "#94a3b8";
      c.lineWidth = 1.0;
      c.beginPath(); c.moveTo(lsR, ym); c.lineTo(sp, ym); c.stroke();
      c.strokeStyle = "#475569";
      c.lineWidth = 0.5;
      c.beginPath(); c.moveTo(sp, yd); c.lineTo(sp, yr); c.stroke();

      c.strokeStyle = C_GPIO;
      c.lineWidth = 1.0;
      c.beginPath(); c.moveTo(sp, yd); c.lineTo(muxL, yd); c.stroke();

      c.strokeStyle = C_GPIO_R;
      c.lineWidth = 1.0;
      c.beginPath(); c.moveTo(sp, yr); c.lineTo(muxL, yr); c.stroke();
      const rz = (sp + muxL) / 2;
      drawResistor(c, rz - 8, yr, 16, C_GPIO_R);
    }

    /* --- ADC (S3) --- */
    c.font = "8px monospace";
    c.textAlign = "right";
    c.fillStyle = C_ADC;
    c.fillText(ADC_LABELS[ch]!, adcX, sy[2]! + 3);
    c.strokeStyle = C_ADC;
    c.lineWidth = 1.0;
    c.beginPath(); c.moveTo(adcX + 3, sy[2]!); c.lineTo(muxL, sy[2]!); c.stroke();

    /* --- EXT (S4) --- */
    c.fillStyle = C_EXT;
    c.fillText(EXT_LABELS[ch]!, adcX, sy[3]! + 3);
    c.strokeStyle = C_EXT;
    c.lineWidth = 1.0;
    c.beginPath(); c.moveTo(adcX + 3, sy[3]!); c.lineTo(muxL, sy[3]!); c.stroke();

    /* --- All 8 switch bars --- */
    for (let s = 0; s < 8; s++) {
      const y = sy[s]!;
      const on = ((st >> s) & 1) !== 0;
      const bl = muxL + 4;
      const br = muxR - 8;
      if (on) {
        c.fillStyle = ac;
        c.fillRect(bl, y - 2, br - bl, 4);
        c.fillStyle = `${ac}44`;
        c.beginPath(); c.arc(br, y, 6, 0, Math.PI * 2); c.fill();
        c.fillStyle = ac;
        c.beginPath(); c.arc(br, y, 3, 0, Math.PI * 2); c.fill();
      } else {
        c.fillStyle = "#0b1322";
        c.fillRect(bl, y - 1, br - bl, 2);
        c.fillStyle = "#152030";
        c.beginPath(); c.arc(br, y, 2, 0, Math.PI * 2); c.fill();
      }
      const sc =
        s === 0 || s === 4 || s === 6 ? C_GPIO
        : s === 1 || s === 5 || s === 7 ? C_GPIO_R
        : s === 2 ? C_ADC
        : s === 3 ? C_EXT
        : "#fff";
      c.fillStyle = on ? sc : "#1e2d40";
      c.font = "bold 7px monospace";
      c.textAlign = "left";
      c.fillText(`S${s + 1}`, bl + 2, y + 3);
    }

    /* --- Output traces, colored per active switch --- */
    const grps: ReadonlyArray<readonly [number, number, string]> = [[0, 4, "Main"], [4, 6, "Aux1"], [6, 8, "Aux2"]];
    for (const [s0, s1, lbl] of grps) {
      const cy = (sy[s0]! + sy[s1 - 1]!) / 2;
      let activeSw: number | null = null;
      for (let s = s0; s < s1; s++) {
        if (((st >> s) & 1) !== 0) { activeSw = s; break; }
      }
      const any = activeSw !== null;
      const tc = activeSw === null ? "#0c1525"
        : activeSw === 0 || activeSw === 4 || activeSw === 6 ? C_GPIO
        : activeSw === 1 || activeSw === 5 || activeSw === 7 ? C_GPIO_R
        : activeSw === 2 ? C_ADC
        : activeSw === 3 ? C_EXT
        : ac;
      c.strokeStyle = tc;
      c.lineWidth = any ? 1.5 : 0.3;
      c.beginPath(); c.moveTo(muxR, cy); c.lineTo(efX - 28, cy); c.stroke();
      if (any) {
        c.strokeStyle = `${tc}15`;
        c.lineWidth = 6.0;
        c.beginPath(); c.moveTo(muxR, cy); c.lineTo(efX - 28, cy); c.stroke();
        c.strokeStyle = tc;
        c.lineWidth = 1.0;
        c.beginPath(); c.moveTo(efX + 28, cy); c.lineTo(cnL, cy); c.stroke();
      }
      c.fillStyle = any ? "#e2e8f0" : "#253040";
      c.font = "8px monospace";
      c.textAlign = "left";
      c.fillText(lbl, outX, cy + 3);
    }

    /* --- E-Fuse --- */
    const efW = 25;
    const efTop = ry + rh * 0.1;
    const efH2 = rh * 0.75;
    let efFill: string;
    let efBd: string;
    let efTxt: string;
    if (!psuOn) {
      efFill = C_CHIP; efBd = C_CHIP_BD; efTxt = "#334155";
    } else if (!efOn) {
      efFill = "#1a1508"; efBd = "#8b6020"; efTxt = "#f59e0b";
    } else {
      efFill = "#081a10"; efBd = "#20603a"; efTxt = "#10b981";
    }
    rrect(c, efX - efW, efTop, efW * 2, efH2, 4);
    c.fillStyle = efFill; c.fill();
    c.strokeStyle = efBd; c.lineWidth = 1.0; c.stroke();
    c.fillStyle = efTxt; c.font = "bold 7px monospace"; c.textAlign = "center";
    c.fillText("E-FUSE", efX, efTop + 14);
    c.font = "6px monospace";
    c.fillText("TPS1641", efX, efTop + 24);
    c.fillStyle = efTxt;
    c.beginPath(); c.arc(efX, efTop + efH2 - 10, 4, 0, Math.PI * 2); c.fill();

    /* --- Connector P1..P4 --- */
    const ct = ry + 3;
    const connH = rh - 6;
    const connW = cnR - cnL;
    rrect(c, cnL, ct, connW, connH, 5);
    c.fillStyle = "#0a1222"; c.fill();
    c.strokeStyle = `${ac}55`;
    c.lineWidth = 1.5;
    c.stroke();

    c.fillStyle = ac;
    c.font = "bold 13px Inter, sans-serif";
    c.textAlign = "center";
    c.fillText(`P${ch + 1}`, cnL + connW / 2, ct + 16);

    const pw = psuOn && efOn;
    const psuLbl = pi === 0 ? "V_ADJ1" : "V_ADJ2";

    const mainCy = (sy[0]! + sy[3]!) / 2;
    const aux1Cy = (sy[4]! + sy[5]!) / 2;
    const aux2Cy = (sy[6]! + sy[7]!) / 2;

    const findActive = (a: number, b: number): number | null => {
      for (let s = a; s < b; s++) if (((st >> s) & 1) !== 0) return s;
      return null;
    };
    const swColor = (sw: number | null): string => {
      if (sw === null) return "#253040";
      if (sw === 0 || sw === 4 || sw === 6) return C_GPIO;
      if (sw === 1 || sw === 5 || sw === 7) return C_GPIO_R;
      if (sw === 2) return C_ADC;
      if (sw === 3) return C_EXT;
      return "#253040";
    };
    const mainSw = findActive(0, 4);
    const aux1Sw = findActive(4, 6);
    const aux2Sw = findActive(6, 8);
    const mainOn = mainSw !== null;
    const aux1On = aux1Sw !== null;
    const aux2On = aux2Sw !== null;
    const mainC = swColor(mainSw);
    const aux1C = swColor(aux1Sw);
    const aux2C = swColor(aux2Sw);

    const pinYs = [
      ct + 14,
      aux2Cy,
      aux1Cy,
      mainCy,
      ct + connH - 8,
    ];

    const pinX = cnL + 8;
    const numX = cnR - 14;

    const ioLabels = GPIO_PAIR_LABELS[ch]!;

    /* Pin 1: GND */
    c.font = "8px monospace"; c.textAlign = "left";
    c.fillStyle = "#1e2d40";
    c.fillText("GND", pinX, pinYs[0]! + 3);
    c.textAlign = "right"; c.fillStyle = "#253040"; c.font = "7px monospace";
    c.fillText("1", numX, pinYs[0]! + 3);

    /* Pin 2: IOx (Group C) */
    c.font = "bold 10px monospace"; c.textAlign = "left";
    c.fillStyle = aux2On ? aux2C : "#253040";
    c.fillText(ioLabels[2]!, pinX, pinYs[1]! + 4);
    c.textAlign = "right"; c.fillStyle = "#334155"; c.font = "7px monospace";
    c.fillText("2", numX, pinYs[1]! + 3);

    /* Pin 3: IOx+1 (Group B) */
    c.font = "bold 10px monospace"; c.textAlign = "left";
    c.fillStyle = aux1On ? aux1C : "#253040";
    c.fillText(ioLabels[1]!, pinX, pinYs[2]! + 4);
    c.textAlign = "right"; c.fillStyle = "#334155"; c.font = "7px monospace";
    c.fillText("3", numX, pinYs[2]! + 3);

    /* Pin 4: analog-capable IO (Group A) */
    c.font = "bold 10px monospace"; c.textAlign = "left";
    c.fillStyle = mainOn ? mainC : "#253040";
    c.fillText(ioLabels[0]!, pinX, pinYs[3]! + 4);
    c.textAlign = "right"; c.fillStyle = "#334155"; c.font = "7px monospace";
    c.fillText("4", numX, pinYs[3]! + 3);

    /* Pin 5: V_ADJ */
    c.font = "8px monospace"; c.textAlign = "left";
    c.fillStyle = pw ? "#ef444499" : "#1e2d40";
    c.fillText(psuLbl, pinX, pinYs[4]! + 3);
    c.textAlign = "right"; c.fillStyle = "#253040"; c.font = "7px monospace";
    c.fillText("5", numX, pinYs[4]! + 3);
    c.fillStyle = pw ? "#ef4444" : "#1e293b";
    c.beginPath(); c.arc(numX - 10, pinYs[4]!, 4, 0, Math.PI * 2); c.fill();
    if (pw) {
      c.fillStyle = "rgba(239,68,68,0.12)";
      c.beginPath(); c.arc(numX - 10, pinYs[4]!, 8, 0, Math.PI * 2); c.fill();
    }
  }
}
