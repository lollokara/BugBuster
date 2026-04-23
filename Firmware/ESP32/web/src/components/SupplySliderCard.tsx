import { useEffect, useMemo, useState } from "preact/hooks";
import { api, PairingRequiredError } from "../api/client";
import { BarGauge } from "./BarGauge";
import { BigValue } from "./BigValue";
import { GlassCard } from "./GlassCard";

export interface SupplySliderCardProps {
  title: string;
  idacChannel: number;
  controlKey: string;
  enabled: boolean;
  measuredVoltage: number;
  idacChannelStatus: any;
  min: number;
  max: number;
  color: string;
  mac: string | null;
  invertSlider?: boolean;
}

function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, value));
}

function estimateCode(ch: any, voltage: number): number {
  const midpoint = Number(ch?.midpointV ?? (Number(ch?.vMin) + Number(ch?.vMax)) / 2);
  const step = Number(ch?.stepMv ?? 0) / 1000;
  if (!Number.isFinite(midpoint) || !Number.isFinite(step) || step <= 0) {
    return Number(ch?.code ?? 0);
  }
  // DS4424 mapping on BugBuster rails is inverted:
  // negative DAC codes increase output voltage, positive codes decrease it.
  return clamp(Math.round((midpoint - voltage) / step), -127, 127);
}

function evalPoly(poly: unknown, code: number): number {
  const arr = Array.isArray(poly) ? poly.map((v) => Number(v)) : [];
  if (arr.length < 4 || arr.some((v) => !Number.isFinite(v))) return NaN;
  // Firmware reports polynomial over normalized code cn = code / 127.
  const cn = code / 127.0;
  return arr[0]! + arr[1]! * cn + arr[2]! * cn * cn + arr[3]! * cn * cn * cn;
}

export function SupplySliderCard({
  title,
  idacChannel,
  controlKey,
  enabled,
  measuredVoltage,
  idacChannelStatus,
  min,
  max,
  color,
  mac,
  invertSlider = false,
}: SupplySliderCardProps) {
  const currentTarget = Number(idacChannelStatus?.targetV ?? idacChannelStatus?.target ?? min);
  const [value, setValue] = useState(Number.isFinite(currentTarget) ? currentTarget : min);
  const [busy, setBusy] = useState<"apply" | "enable" | null>(null);
  const [status, setStatus] = useState<string | null>(null);

  useEffect(() => {
    if (Number.isFinite(currentTarget)) setValue(clamp(currentTarget, min, max));
  }, [currentTarget, min, max]);

  const preview = useMemo(() => {
    if (!idacChannelStatus?.calibrated || !idacChannelStatus?.polyValid) return NaN;
    const code = estimateCode(idacChannelStatus, value);
    return evalPoly(idacChannelStatus?.calPoly, code);
  }, [idacChannelStatus, value]);

  const apply = async () => {
    if (!mac) return;
    setBusy("apply");
    setStatus(null);
    try {
      await api.idacSetVoltage(mac, idacChannel, value);
      setStatus("Applied");
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  const toggleEnable = async () => {
    if (!mac) return;
    setBusy("enable");
    setStatus(null);
    try {
      await api.ioexp.setControl(mac, controlKey, !enabled);
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setStatus(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  const sliderValue = invertSlider ? min + max - value : value;

  const onSliderInput = (e: Event) => {
    const raw = parseFloat((e.currentTarget as HTMLInputElement).value);
    if (!Number.isFinite(raw)) return;
    const mapped = invertSlider ? min + max - raw : raw;
    setValue(clamp(mapped, min, max));
  };

  return (
    <GlassCard
      title={title}
      actions={
        <button
          class={"pill" + (enabled ? " active" : "")}
          disabled={!mac || busy !== null}
          onClick={toggleEnable}
        >
          {busy === "enable" ? "..." : enabled ? "ON" : "OFF"}
        </button>
      }
    >
      <BigValue value={measuredVoltage} unit="V" precision={3} />
      <BarGauge
        value={Number.isFinite(measuredVoltage) ? measuredVoltage : value}
        min={min}
        max={max}
        color={color}
      />
      <div class="analog-row">
        <input
          type="range"
          min={min}
          max={max}
          step="0.001"
          value={String(sliderValue)}
          onInput={onSliderInput}
        />
        <input
          class="input"
          type="number"
          min={min}
          max={max}
          step="0.001"
          value={value.toFixed(3)}
          style={{ maxWidth: "96px" }}
          onInput={(e) => setValue(clamp(parseFloat((e.currentTarget as HTMLInputElement).value || "0"), min, max))}
        />
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Preview</span>
        <span class={Number.isFinite(preview) ? "mono" : "text-warn"}>
          {Number.isFinite(preview) ? `${preview.toFixed(3)} V` : "Not calibrated"}
        </span>
      </div>
      <button class="btn primary" disabled={!mac || busy !== null} onClick={apply}>
        {busy === "apply" ? "Applying..." : "Apply"}
      </button>
      {status && <div class="text-dim" style={{ marginTop: "8px", fontSize: "0.78rem" }}>{status}</div>}
    </GlassCard>
  );
}
