// =============================================================================
// Overview tab — pairing strip, analog status, digital IO, and supply outputs.
// =============================================================================

import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { BigValue } from "../../components/BigValue";
import { Sparkline } from "../../components/Sparkline";
import { Led } from "../../components/Led";
import { ChDOverlay } from "../../components/ChDOverlay";
import { DigitalIOGrid } from "../../components/DigitalIOGrid";
import { SupplySliderCard } from "../../components/SupplySliderCard";
import { QuickSetupTile } from "../../components/QuickSetupTile";
import { CH_COLORS } from "../../scope/ScopeCanvas";
import { api, HttpError, PairingRequiredError, type QuickSetupSummary } from "../../api/client";
import {
  CHANNEL_FUNCTION_LABELS,
  CHANNEL_FUNCTION_OPTIONS,
} from "../../config/options";
import {
  deviceStatus,
  deviceInfo,
  pairingInfo,
  deviceMac,
  channelSparks,
  pushChannelSamples,
  selftestWorkerEnabled,
  supplyMonitorActive,
  setSelftestStatus,
  startSelftestStatusPolling,
} from "../../state/signals";

const CH_NAMES = ["A", "B", "C", "D"] as const;
const OUTPUT_FUNCTION_CODES = new Set([1, 2, 10]);

function functionCodeFromChannel(ch: any): number {
  const raw = ch?.functionCode ?? ch?.function_code;
  if (typeof raw === "number" && Number.isFinite(raw)) return raw;
  const fn = String(ch?.function ?? "").toUpperCase();
  if (fn === "VOUT") return 1;
  if (fn === "IOUT") return 2;
  if (fn === "IOUT_HART") return 10;
  return -1;
}

function displayValueFromChannel(ch: any, funcCode: number): number {
  const adc = Number(ch?.adcValue ?? ch?.adc_value ?? NaN);
  const dac = Number(ch?.dacValue ?? ch?.dac_value ?? NaN);
  if (OUTPUT_FUNCTION_CODES.has(funcCode)) {
    return Number.isFinite(dac) ? dac : adc;
  }
  return adc;
}

function funcLabel(code: number | undefined, fallback?: string): string {
  if (typeof code === "number" && CHANNEL_FUNCTION_LABELS[code]) {
    return CHANNEL_FUNCTION_LABELS[code]!;
  }
  if (fallback) return fallback;
  if (typeof code === "number") return `CH_FUNC_${code}`;
  return "-";
}

function shortMac(value: string | undefined): string {
  const compact = String(value ?? "").replace(/[^0-9a-f]/gi, "");
  return compact ? compact.slice(-4).toUpperCase() : "--";
}

export function Overview() {
  const status = deviceStatus.value;
  const info = deviceInfo.value;
  const pairing = pairingInfo.value;
  const mac = deviceMac.value;
  const [busyChannel, setBusyChannel] = useState<number | null>(null);
  const [busyWorker, setBusyWorker] = useState(false);
  const [rails, setRails] = useState<Record<number, number>>({});
  const [idac, setIdac] = useState<any>(null);
  const [ioexp, setIoexp] = useState<any>(null);
  const [quicksetupSupported, setQuicksetupSupported] = useState<boolean | null>(null);
  const [quicksetupSlots, setQuicksetupSlots] = useState<QuickSetupSummary[]>([]);
  const [quicksetupError, setQuicksetupError] = useState<string | null>(null);

  useEffect(() => startSelftestStatusPolling(), []);

  useEffect(() => {
    if (!status || !Array.isArray(status.channels)) return;
    const arr = status.channels;
    const vals: [number, number, number, number] = [
      displayValueFromChannel(arr[0], functionCodeFromChannel(arr[0])),
      displayValueFromChannel(arr[1], functionCodeFromChannel(arr[1])),
      displayValueFromChannel(arr[2], functionCodeFromChannel(arr[2])),
      displayValueFromChannel(arr[3], functionCodeFromChannel(arr[3])),
    ];
    pushChannelSamples(vals);
  }, [status]);

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      const [idacResult, ioexpResult, r0, r1, r2] = await Promise.allSettled([
        api.idac(),
        api.ioexp(),
        api.selftestSupply(0),
        api.selftestSupply(1),
        api.selftestSupply(2),
      ]);
      if (!alive) return;
      if (idacResult.status === "fulfilled") setIdac(idacResult.value);
      if (ioexpResult.status === "fulfilled") setIoexp(ioexpResult.value);
      const next: Record<number, number> = {};
      [r0, r1, r2].forEach((result, rail) => {
        if (result.status === "fulfilled") {
          const voltage = Number((result.value as any)?.voltage ?? (result.value as any)?.voltageV ?? NaN);
          if (Number.isFinite(voltage)) next[rail] = voltage;
        }
      });
      setRails((prev) => ({ ...prev, ...next }));
      if (alive) window.setTimeout(tick, 2500);
    };
    void tick();
    return () => {
      alive = false;
    };
  }, []);

  const refreshQuickSetups = async () => {
    try {
      const result = await api.quicksetupList();
      setQuicksetupSupported(true);
      setQuicksetupError(null);
      setQuicksetupSlots(Array.isArray(result.slots) ? result.slots : []);
    } catch (e) {
      if (e instanceof HttpError && e.status === 404) {
        setQuicksetupSupported(false);
        setQuicksetupError(null);
        setQuicksetupSlots([]);
        return;
      }
      setQuicksetupSupported(true);
      setQuicksetupError(e instanceof Error ? e.message : String(e));
    }
  };

  useEffect(() => {
    void refreshQuickSetups();
  }, []);

  const channels = Array.isArray(status?.channels) ? status.channels : [];
  const diagnostics = status?.diagnostics ?? {};
  const dieTemp = Number(diagnostics.dieTemp ?? diagnostics.die_temp ?? status?.dieTemp ?? NaN);
  const spiOk = !!(info?.spiOk ?? status?.spiOk ?? status?.spi_ok);
  const fwStr = info
    ? (info as any).fwMajor !== undefined
      ? `${(info as any).fwMajor}.${(info as any).fwMinor}.${(info as any).fwPatch}`
      : `silicon ${info.siliconRev}`
    : "-";
  const fullMac = pairing?.macAddress ?? info?.macAddress;
  const idacChannels = Array.isArray(idac?.channels) ? idac.channels : [];
  const enables = ioexp?.enables ?? {};
  const sparks = channelSparks.value;
  const quicksetupDisplaySlots = Array.from({ length: 4 }, (_, i) => {
    return quicksetupSlots.find((slot, pos) => Number(slot?.index ?? pos) === i) ?? {
      index: i,
      occupied: false,
    };
  });

  const setFunction = async (ch: number, func: number) => {
    if (!mac) return;
    setBusyChannel(ch);
    try {
      await api.channel.setFunction(mac, ch, func);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        console.warn("setFunction failed", e);
      }
    } finally {
      setBusyChannel(null);
    }
  };

  const toggleWorker = async () => {
    if (!mac) return;
    setBusyWorker(true);
    try {
      setSelftestStatus(await api.selftestWorker(mac, !selftestWorkerEnabled.value));
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) console.warn("selftestWorker failed", e);
    } finally {
      setBusyWorker(false);
    }
  };

  return (
    <div class="tab-stack">
      <GlassCard class="overview-pairing">
        <div style={{ display: "flex", flexWrap: "wrap", alignItems: "center", gap: "14px" }}>
          <span class="uppercase-tag">Pairing</span>
          <span class="mono">MAC ...{shortMac(fullMac)}</span>
          <span style={{ display: "inline-flex", alignItems: "center", gap: "6px" }}>
            <Led state={spiOk ? "on" : "err"} />
            <span class={spiOk ? "text-ok" : "text-err"}>{spiOk ? "SPI OK" : "SPI FAIL"}</span>
          </span>
          <span class="mono">FW {fwStr}</span>
          <span class="mono">
            Die {Number.isFinite(dieTemp) ? `${dieTemp.toFixed(1)}C` : "-"}
          </span>
        </div>
      </GlassCard>

      <section>
        <h3 class="uppercase-tag" style={{ marginBottom: "10px" }}>Analog Channels</h3>
        <div class="overview-channels">
          {[0, 1, 2, 3].map((i) => {
            const ch = channels[i] ?? {};
            const funcCode = functionCodeFromChannel(ch);
            const funcName = funcLabel(funcCode, ch.function);
            const displayValue = displayValueFromChannel(ch, funcCode);
            const card = (
              <GlassCard title={`Channel ${CH_NAMES[i]}`}>
                <div class="ch-tile-head">
                  <span class="ch-swatch" style={{ color: CH_COLORS[i], background: CH_COLORS[i] }} />
                  <span class="uppercase-tag">{funcName}</span>
                </div>
                <div class="analog-row" style={{ marginTop: "6px" }}>
                  <label class="uppercase-tag">Function</label>
                  <select
                    class="input"
                    value={String(Number.isFinite(funcCode) ? funcCode : 0)}
                    disabled={!mac || busyChannel === i || (i === 3 && supplyMonitorActive.value)}
                    onChange={(e) =>
                      setFunction(i, parseInt((e.currentTarget as HTMLSelectElement).value, 10))
                    }
                  >
                    {CHANNEL_FUNCTION_OPTIONS.map((opt) => (
                      <option key={opt.code} value={String(opt.code)}>
                        {opt.label}
                      </option>
                    ))}
                  </select>
                </div>
                <BigValue value={displayValue} unit="V" precision={3} />
                <div style={{ marginTop: "10px" }}>
                  <Sparkline values={sparks[i] ?? []} color={CH_COLORS[i]} height={56} />
                </div>
              </GlassCard>
            );
            return (
              <ChDOverlay key={i} active={i === 3 && supplyMonitorActive.value}>
                {card}
              </ChDOverlay>
            );
          })}
        </div>
      </section>

      <section>
        <h3 class="uppercase-tag" style={{ marginBottom: "10px" }}>Digital IO</h3>
        <GlassCard>
          <DigitalIOGrid />
        </GlassCard>
      </section>

      <section>
        <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: "12px", marginBottom: "10px" }}>
          <h3 class="uppercase-tag">Output Configuration</h3>
          <button
            class={"pill" + (selftestWorkerEnabled.value ? " active" : "")}
            disabled={!mac || busyWorker}
            onClick={toggleWorker}
          >
            {busyWorker ? "..." : `Supply monitor ${selftestWorkerEnabled.value ? "ON" : "OFF"}`}
          </button>
        </div>
        <div class="overview-rails">
          <SupplySliderCard
            title="Level-Shifter"
            idacChannel={0}
            controlKey="mux"
            enabled={!!enables.mux}
            measuredVoltage={rails[2] ?? NaN}
            idacChannelStatus={idacChannels[0]}
            min={1.8}
            max={5}
            color="var(--blue)"
            mac={mac}
            invertSlider={true}
          />
          <SupplySliderCard
            title="VADJ1"
            idacChannel={1}
            controlKey="vadj1"
            enabled={!!enables.vadj1}
            measuredVoltage={rails[0] ?? NaN}
            idacChannelStatus={idacChannels[1]}
            min={3}
            max={15}
            color="var(--green)"
            mac={mac}
            invertSlider={true}
          />
          <SupplySliderCard
            title="VADJ2"
            idacChannel={2}
            controlKey="vadj2"
            enabled={!!enables.vadj2}
            measuredVoltage={rails[1] ?? NaN}
            idacChannelStatus={idacChannels[2]}
            min={3}
            max={15}
            color="var(--amber)"
            mac={mac}
            invertSlider={true}
          />
        </div>
      </section>

      {quicksetupSupported && (
        <section>
          <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: "12px", marginBottom: "10px" }}>
            <h3 class="uppercase-tag">Quick Setups</h3>
            {quicksetupError && <span class="text-warn mono">{quicksetupError}</span>}
          </div>
          <div class="overview-channels">
            {quicksetupDisplaySlots.map((slot, i) => (
              <QuickSetupTile
                key={i}
                slotIndex={i}
                slot={slot}
                mac={mac}
                onRefresh={refreshQuickSetups}
              />
            ))}
          </div>
        </section>
      )}
    </div>
  );
}
