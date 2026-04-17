// =============================================================================
// Analog tab — ADC / VDAC / IDAC / IIN / Diagnostics.
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { BigValue } from "../../components/BigValue";
import { api, PairingRequiredError } from "../../api/client";
import { deviceStatus, deviceMac } from "../../state/signals";

const CH_NAMES = ["A", "B", "C", "D"] as const;

// TODO: verify shape from firmware — these are the scope ranges from tauri_bridge.rs.
const ADC_RANGES: { code: number; label: string }[] = [
  { code: 0, label: "±2.5V" },
  { code: 1, label: "±12V" },
  { code: 2, label: "0–12V" },
  { code: 3, label: "±25V" },
  { code: 4, label: "0–25V" },
  { code: 5, label: "0–104mV" },
  { code: 6, label: "±104mV" },
  { code: 7, label: "0–25mA" },
];

const ADC_RATES: { code: number; label: string }[] = [
  { code: 0, label: "10 SPS" },
  { code: 1, label: "20 SPS" },
  { code: 2, label: "200 SPS" },
  { code: 3, label: "1 kSPS" },
  { code: 4, label: "4.8 kSPS" },
];

interface ChannelState {
  adcValue?: number;
  function?: string;
  functionCode?: number;
  rangeCode?: number;
  rateCode?: number;
  muxCode?: number;
  dacCode?: number;
  vdacReadback?: number;
  iinReadback?: number;
}

function readChannel(status: any, i: number): ChannelState {
  const arr = status?.channels;
  if (!Array.isArray(arr)) return {};
  const c = arr[i] ?? {};
  return {
    adcValue: Number(c.adcValue ?? c.adc_value ?? NaN),
    function: c.function,
    functionCode: Number(c.functionCode ?? c.function_code ?? c.function ?? -1),
    rangeCode: Number(c.rangeCode ?? c.adc_range ?? c.range ?? 0),
    rateCode: Number(c.rateCode ?? c.adc_rate ?? c.rate ?? 0),
    muxCode: Number(c.muxCode ?? c.adc_mux ?? c.mux ?? 0),
    dacCode: Number(c.dacCode ?? c.dac_code ?? NaN),
    vdacReadback: Number(c.vdac ?? c.vdacReadback ?? NaN),
    iinReadback: Number(c.iin ?? c.iinValue ?? NaN),
  };
}

function AdcCard() {
  const status = deviceStatus.value;
  return (
    <GlassCard title="ADC Channels">
      <div class="analog-grid">
        {[0, 1, 2, 3].map((i) => {
          const c = readChannel(status, i);
          return (
            <div class="analog-item" key={i}>
              <div class="uppercase-tag">CH {CH_NAMES[i]}</div>
              <BigValue value={Number.isFinite(c.adcValue) ? c.adcValue! : NaN} unit="V" precision={3} />
              <div class="analog-row">
                <label class="uppercase-tag">Range</label>
                <select class="input" value={String(c.rangeCode ?? 0)}>
                  {ADC_RANGES.map((r) => (
                    <option key={r.code} value={String(r.code)}>{r.label}</option>
                  ))}
                </select>
              </div>
              <div class="analog-row">
                <label class="uppercase-tag">Rate</label>
                <select class="input" value={String(c.rateCode ?? 0)}>
                  {ADC_RATES.map((r) => (
                    <option key={r.code} value={String(r.code)}>{r.label}</option>
                  ))}
                </select>
              </div>
            </div>
          );
        })}
      </div>
    </GlassCard>
  );
}

function VdacCard() {
  const status = deviceStatus.value;
  const mac = deviceMac.value;
  const [pending, setPending] = useState<Record<number, number>>({});

  const setCode = (ch: number, code: number) => {
    setPending((p) => ({ ...p, [ch]: code }));
  };
  const commit = async (ch: number) => {
    if (!mac) return;
    const code = pending[ch];
    if (code == null) return;
    try {
      await api.channel.setDac(mac, ch, code);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("setDac failed", e);
    }
  };

  return (
    <GlassCard title="VDAC">
      <div class="analog-grid">
        {[0, 1, 2, 3].map((i) => {
          const c = readChannel(status, i);
          const current = pending[i] ?? (Number.isFinite(c.dacCode) ? c.dacCode! : 0);
          return (
            <div class="analog-item" key={i}>
              <div class="uppercase-tag">CH {CH_NAMES[i]}</div>
              <BigValue value={Number.isFinite(c.vdacReadback) ? c.vdacReadback! : NaN} unit="V" precision={3} />
              <div class="analog-row">
                <input
                  type="range"
                  min={0}
                  max={65535}
                  value={current}
                  onInput={(e) => setCode(i, parseInt((e.currentTarget as HTMLInputElement).value, 10))}
                  onChange={() => commit(i)}
                />
                <span class="mono">{current}</span>
              </div>
            </div>
          );
        })}
      </div>
    </GlassCard>
  );
}

interface IdacInfo {
  rails?: {
    name: string;
    voltage: number;
    slot: number;
  }[];
}

function IdacCard() {
  const [data, setData] = useState<IdacInfo | null>(null);
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const r = await api.idac();
        if (alive) setData(r);
      } catch {
        /* ignore */
      }
      if (alive) setTimeout(tick, 2000);
    };
    tick();
    return () => { alive = false; };
  }, []);

  const rails = Array.isArray(data?.rails) ? data!.rails! : [];
  const display = rails.length > 0
    ? rails
    : [
        { name: "LevelShift", voltage: NaN, slot: 0 },
        { name: "VADJ1", voltage: NaN, slot: 1 },
        { name: "VADJ2", voltage: NaN, slot: 2 },
      ];

  return (
    <GlassCard title="IDAC Rails">
      <div class="analog-grid">
        {display.map((rail, idx) => (
          <div class="analog-item" key={idx}>
            <div class="uppercase-tag">{rail.name}</div>
            <BigValue value={Number.isFinite(rail.voltage) ? rail.voltage : NaN} unit="V" precision={3} />
            <div class="analog-row">
              <span class="uppercase-tag">Slot</span>
              <span class="mono">{rail.slot}</span>
            </div>
          </div>
        ))}
      </div>
    </GlassCard>
  );
}

function IinCard() {
  const status = deviceStatus.value;
  return (
    <GlassCard title="IIN Channels">
      <div class="analog-grid">
        {[0, 1, 2, 3].map((i) => {
          const c = readChannel(status, i);
          const isIin = c.functionCode === 4 || c.functionCode === 7;
          return (
            <div class="analog-item" key={i}>
              <div class="uppercase-tag">CH {CH_NAMES[i]}</div>
              <BigValue
                value={isIin && Number.isFinite(c.iinReadback) ? c.iinReadback! : NaN}
                unit="mA"
                precision={2}
              />
              <div class="analog-row">
                <span class="uppercase-tag">Mode</span>
                <span class="mono">{isIin ? "IIN" : "—"}</span>
              </div>
            </div>
          );
        })}
      </div>
    </GlassCard>
  );
}

function DiagnosticsCard() {
  const [diag, setDiag] = useState<any>(null);
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const r = await api.diagnostics();
        if (alive) setDiag(r);
      } catch {
        /* ignore */
      }
      if (alive) setTimeout(tick, 1500);
    };
    tick();
    return () => { alive = false; };
  }, []);

  const slots = Array.isArray(diag?.slots)
    ? diag.slots
    : diag && typeof diag === "object"
      ? Object.keys(diag).map((k) => ({
          name: k,
          raw: typeof diag[k] === "object" ? diag[k]?.raw : null,
          value: typeof diag[k] === "object" ? diag[k]?.value : diag[k],
          unit: typeof diag[k] === "object" ? diag[k]?.unit : null,
        }))
      : [];

  return (
    <GlassCard title="Diagnostic Slots">
      <table class="kv-table">
        <thead>
          <tr>
            <th>Name</th>
            <th>Value</th>
            <th>Raw</th>
            <th>Unit</th>
          </tr>
        </thead>
        <tbody>
          {slots.map((s: any, idx: number) => (
            <tr key={idx}>
              <td>{s.name ?? "—"}</td>
              <td class="mono">{typeof s.value === "number" ? s.value.toFixed(3) : String(s.value ?? "—")}</td>
              <td class="mono">{s.raw ?? "—"}</td>
              <td>{s.unit ?? ""}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </GlassCard>
  );
}

export function Analog() {
  return (
    <div class="tab-stack">
      <AdcCard />
      <VdacCard />
      <IdacCard />
      <IinCard />
      <DiagnosticsCard />
    </div>
  );
}
