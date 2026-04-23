// =============================================================================
// Analog tab — desktop-parity control surface (ADC / VDAC / IDAC / IIN / DIAG)
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { BigValue } from "../../components/BigValue";
import { ChDOverlay } from "../../components/ChDOverlay";
import { api, PairingRequiredError } from "../../api/client";
import {
  deviceStatus,
  deviceMac,
  supplyMonitorActive,
  startSelftestStatusPolling,
} from "../../state/signals";
import {
  ADC_MUX_OPTIONS,
  ADC_RANGE_OPTIONS,
  ADC_RATE_OPTIONS,
  DIAG_SOURCE_OPTIONS,
} from "../../config/options";

const CH_NAMES = ["A", "B", "C", "D"] as const;

function readChannel(status: any, i: number) {
  const arr = status?.channels;
  if (!Array.isArray(arr)) return {};
  const c = arr[i] ?? {};
  return {
    adcValue: Number(c.adcValue ?? c.adc_value ?? NaN),
    functionCode: Number(c.functionCode ?? c.function_code ?? -1),
    rangeCode: Number(c.adcRange ?? c.adc_range ?? 0),
    rateCode: Number(c.adcRate ?? c.adc_rate ?? 0),
    muxCode: Number(c.adcMux ?? c.adc_mux ?? 0),
    dacCode: Number(c.dacCode ?? c.dac_code ?? NaN),
    dacValue: Number(c.dacValue ?? c.dac_value ?? NaN),
    iinValue: Number(c.iinValue ?? c.iin ?? NaN),
  };
}

function AdcCard() {
  const status = deviceStatus.value;
  const mac = deviceMac.value;
  const [pending, setPending] = useState<Record<number, { mux: number; range: number; rate: number }>>({});

  const apply = async (ch: number) => {
    if (!mac) return;
    const cfg = pending[ch];
    if (!cfg) return;
    try {
      await api.channel.setAdcConfig(mac, ch, cfg.mux, cfg.range, cfg.rate);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("setAdcConfig failed", e);
    }
  };

  return (
    <GlassCard title="ADC Channels">
      <div class="analog-grid">
        {[0, 1, 2, 3].map((i) => {
          const c = readChannel(status, i);
          const cfg = pending[i] ?? {
            mux: Number.isFinite(c.muxCode) ? c.muxCode : 0,
            range: Number.isFinite(c.rangeCode) ? c.rangeCode : 0,
            rate: Number.isFinite(c.rateCode) ? c.rateCode : 0,
          };
          return (
            <ChDOverlay key={i} active={i === 3 && supplyMonitorActive.value}>
              <div class="analog-item">
                <div class="uppercase-tag">CH {CH_NAMES[i]}</div>
                <BigValue value={Number(c.adcValue ?? NaN)} unit="V" precision={3} />
                <div class="analog-row">
                  <label>Range</label>
                  <select
                    class="input"
                    value={String(cfg.range)}
                    onChange={(e) =>
                      setPending((p) => ({
                        ...p,
                        [i]: { ...cfg, range: parseInt((e.currentTarget as HTMLSelectElement).value, 10) },
                      }))
                    }
                  >
                    {ADC_RANGE_OPTIONS.map((r) => (
                      <option key={r.code} value={String(r.code)}>
                        {r.label}
                      </option>
                    ))}
                  </select>
                </div>
                <div class="analog-row">
                  <label>Rate</label>
                  <select
                    class="input"
                    value={String(cfg.rate)}
                    onChange={(e) =>
                      setPending((p) => ({
                        ...p,
                        [i]: { ...cfg, rate: parseInt((e.currentTarget as HTMLSelectElement).value, 10) },
                      }))
                    }
                  >
                    {ADC_RATE_OPTIONS.map((r) => (
                      <option key={r.code} value={String(r.code)}>
                        {r.label}
                      </option>
                    ))}
                  </select>
                </div>
                <div class="analog-row">
                  <label>Mux</label>
                  <select
                    class="input"
                    value={String(cfg.mux)}
                    onChange={(e) =>
                      setPending((p) => ({
                        ...p,
                        [i]: { ...cfg, mux: parseInt((e.currentTarget as HTMLSelectElement).value, 10) },
                      }))
                    }
                  >
                    {ADC_MUX_OPTIONS.map((r) => (
                      <option key={r.code} value={String(r.code)}>
                        {r.label}
                      </option>
                    ))}
                  </select>
                </div>
                <button class="btn" disabled={!mac} onClick={() => apply(i)}>
                  Apply ADC
                </button>
              </div>
            </ChDOverlay>
          );
        })}
      </div>
    </GlassCard>
  );
}

function VdacCard() {
  const status = deviceStatus.value;
  const mac = deviceMac.value;
  const [pendingCode, setPendingCode] = useState<Record<number, number>>({});
  const [pendingVoltage, setPendingVoltage] = useState<Record<number, number>>({});
  const [bipolar, setBipolar] = useState<Record<number, boolean>>({});

  const setCode = async (ch: number) => {
    if (!mac) return;
    const code = pendingCode[ch];
    if (code == null) return;
    try {
      await api.channel.setDac(mac, ch, code);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("setDac failed", e);
    }
  };

  const setVoltage = async (ch: number) => {
    if (!mac) return;
    const voltage = pendingVoltage[ch];
    if (voltage == null) return;
    try {
      await api.channel.setDacVoltage(mac, ch, voltage, !!bipolar[ch]);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("setDacVoltage failed", e);
    }
  };

  const setRange = async (ch: number, value: boolean) => {
    setBipolar((p) => ({ ...p, [ch]: value }));
    if (!mac) return;
    try {
      await api.channel.setVoutRange(mac, ch, value);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("setVoutRange failed", e);
    }
  };

  return (
    <GlassCard title="VDAC (code/voltage/range)">
      <div class="analog-grid">
        {[0, 1, 2, 3].map((i) => {
          const c = readChannel(status, i);
          const code = pendingCode[i] ?? (Number.isFinite(c.dacCode) ? c.dacCode : 0);
          const voltage = pendingVoltage[i] ?? (Number.isFinite(c.dacValue) ? c.dacValue : 0);
          const isBipolar = !!bipolar[i];
          return (
            <ChDOverlay key={i} active={i === 3 && supplyMonitorActive.value}>
            <div class="analog-item">
              <div class="uppercase-tag">CH {CH_NAMES[i]}</div>
              <BigValue value={Number(c.dacValue ?? NaN)} unit="V" precision={3} />
              <div class="analog-row">
                <label>Bipolar</label>
                <input type="checkbox" checked={isBipolar} onChange={(e) => setRange(i, (e.currentTarget as HTMLInputElement).checked)} />
              </div>
              <div class="analog-row">
                <label>DAC code</label>
                <input
                  class="input"
                  type="number"
                  min={0}
                  max={65535}
                  value={String(code)}
                  onInput={(e) => setPendingCode((p) => ({ ...p, [i]: parseInt((e.currentTarget as HTMLInputElement).value || "0", 10) }))}
                />
              </div>
              <button class="btn" disabled={!mac} onClick={() => setCode(i)}>
                Apply code
              </button>
              <div class="analog-row">
                <label>Voltage</label>
                <input
                  class="input"
                  type="number"
                  step="0.001"
                  value={String(voltage)}
                  onInput={(e) => setPendingVoltage((p) => ({ ...p, [i]: parseFloat((e.currentTarget as HTMLInputElement).value || "0") }))}
                />
              </div>
              <button class="btn" disabled={!mac} onClick={() => setVoltage(i)}>
                Apply voltage
              </button>
            </div>
            </ChDOverlay>
          );
        })}
      </div>
    </GlassCard>
  );
}

function IdacCard() {
  const mac = deviceMac.value;
  const [data, setData] = useState<any>(null);
  const [pendingCode, setPendingCode] = useState<Record<number, number>>({});
  const [pendingVoltage, setPendingVoltage] = useState<Record<number, number>>({});
  const [calCode, setCalCode] = useState<Record<number, number>>({});
  const [calV, setCalV] = useState<Record<number, number>>({});

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
    return () => {
      alive = false;
    };
  }, []);

  const channels = Array.isArray(data?.channels) ? data.channels : [];
  const setCode = async (ch: number) => {
    if (!mac) return;
    try {
      await api.idacSetCode(mac, ch, pendingCode[ch] ?? 0);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("idacSetCode failed", e);
    }
  };
  const setVoltage = async (ch: number) => {
    if (!mac) return;
    try {
      await api.idacSetVoltage(mac, ch, pendingVoltage[ch] ?? 0);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("idacSetVoltage failed", e);
    }
  };
  const addCal = async (ch: number) => {
    if (!mac) return;
    try {
      await api.idacCalPoint(mac, ch, calCode[ch] ?? 0, calV[ch] ?? 0);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("idacCalPoint failed", e);
    }
  };
  const clearCal = async (ch: number) => {
    if (!mac) return;
    try {
      await api.idacCalClear(mac, ch);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("idacCalClear failed", e);
    }
  };
  const saveCal = async () => {
    if (!mac) return;
    try {
      await api.idacCalSave(mac);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("idacCalSave failed", e);
    }
  };

  return (
    <GlassCard title="IDAC Rails / Calibration">
      <div class="analog-grid">
        {channels.map((ch: any, idx: number) => {
          const code = pendingCode[idx] ?? Number(ch.code ?? 0);
          const voltage = pendingVoltage[idx] ?? Number(ch.targetV ?? 0);
          return (
            <div class="analog-item" key={idx}>
              <div class="uppercase-tag">{ch.name ?? `CH ${idx}`}</div>
              <BigValue value={Number(ch.targetV ?? NaN)} unit="V" precision={3} />
              <div class="analog-row">
                <label>Code</label>
                <input class="input" type="number" min={-127} max={127} value={String(code)} onInput={(e) => setPendingCode((p) => ({ ...p, [idx]: parseInt((e.currentTarget as HTMLInputElement).value || "0", 10) }))} />
              </div>
              <button class="btn" disabled={!mac} onClick={() => setCode(idx)}>
                Apply code
              </button>
              <div class="analog-row">
                <label>Voltage</label>
                <input class="input" type="number" step="0.001" value={String(voltage)} onInput={(e) => setPendingVoltage((p) => ({ ...p, [idx]: parseFloat((e.currentTarget as HTMLInputElement).value || "0") }))} />
              </div>
              <button class="btn" disabled={!mac} onClick={() => setVoltage(idx)}>
                Apply V
              </button>
              <details>
                <summary class="uppercase-tag">Calibration</summary>
                <div class="analog-row">
                  <label>Cal code</label>
                  <input class="input" type="number" min={-127} max={127} value={String(calCode[idx] ?? 0)} onInput={(e) => setCalCode((p) => ({ ...p, [idx]: parseInt((e.currentTarget as HTMLInputElement).value || "0", 10) }))} />
                </div>
                <div class="analog-row">
                  <label>Measured V</label>
                  <input class="input" type="number" step="0.001" value={String(calV[idx] ?? 0)} onInput={(e) => setCalV((p) => ({ ...p, [idx]: parseFloat((e.currentTarget as HTMLInputElement).value || "0") }))} />
                </div>
                <button class="btn" disabled={!mac} onClick={() => addCal(idx)}>
                  Add point
                </button>
                <button class="btn" disabled={!mac} onClick={() => clearCal(idx)}>
                  Clear ch
                </button>
              </details>
            </div>
          );
        })}
      </div>
      <button class="btn primary" disabled={!mac} onClick={saveCal}>
        Save calibration
      </button>
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
          return (
            <ChDOverlay key={i} active={i === 3 && supplyMonitorActive.value}>
            <div class="analog-item">
              <div class="uppercase-tag">CH {CH_NAMES[i]}</div>
              <BigValue value={Number(c.iinValue ?? NaN)} unit="mA" precision={2} />
              <div class="mono text-dim">
                ADC {Number.isFinite(Number(c.adcValue ?? NaN)) ? Number(c.adcValue ?? 0).toFixed(3) : "—"} V
              </div>
            </div>
            </ChDOverlay>
          );
        })}
      </div>
    </GlassCard>
  );
}

function DiagnosticsCard() {
  const mac = deviceMac.value;
  const [diag, setDiag] = useState<any>(null);
  const [slot, setSlot] = useState(0);
  const [source, setSource] = useState(0);

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
    return () => {
      alive = false;
    };
  }, []);

  const apply = async () => {
    if (!mac) return;
    try {
      await api.diagnosticsSetConfig(mac, slot, source);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("diagnosticsSetConfig failed", e);
    }
  };

  return (
    <GlassCard title="Diagnostic Slots">
      <div class="analog-row">
        <label>Slot</label>
        <input class="input" type="number" min={0} max={3} value={String(slot)} onInput={(e) => setSlot(parseInt((e.currentTarget as HTMLInputElement).value || "0", 10))} />
      </div>
      <div class="analog-row">
        <label>Source</label>
        <select
          class="input"
          value={String(source)}
          onChange={(e) =>
            setSource(
              parseInt((e.currentTarget as HTMLSelectElement).value || "0", 10),
            )
          }
        >
          {DIAG_SOURCE_OPTIONS.map((opt) => (
            <option key={opt.code} value={String(opt.code)}>
              {opt.label}
            </option>
          ))}
        </select>
      </div>
      <button class="btn" disabled={!mac} onClick={apply}>
        Apply diagnostic source
      </button>
      <pre class="debug-dump mono">{JSON.stringify(diag, null, 2)}</pre>
    </GlassCard>
  );
}

export function Analog() {
  useEffect(() => startSelftestStatusPolling(), []);

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
