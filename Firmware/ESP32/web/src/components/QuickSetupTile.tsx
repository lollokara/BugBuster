import { useState } from "preact/hooks";
import {
  api,
  PairingRequiredError,
  type QuickSetupPayload,
  type QuickSetupSummary,
} from "../api/client";
import { GlassCard } from "./GlassCard";
import { Led } from "./Led";

export interface QuickSetupTileProps {
  slotIndex: number;
  slot?: QuickSetupSummary;
  mac: string | null;
  onRefresh: () => void | Promise<void>;
}

type BusyAction = "save" | "load" | "apply" | "clear";

function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

function firstText(...values: unknown[]): string | null {
  for (const value of values) {
    if (typeof value === "string" && value.trim()) return value.trim();
  }
  return null;
}

function firstNumber(...values: unknown[]): number | null {
  for (const value of values) {
    if (typeof value === "number" && Number.isFinite(value)) return value;
  }
  return null;
}

function formatTime(raw: unknown): string {
  if (typeof raw === "string" && raw.trim()) return raw.trim();
  const seconds = typeof raw === "number" && Number.isFinite(raw) ? raw : NaN;
  if (!Number.isFinite(seconds) || seconds <= 0) return "-";
  return new Date(seconds * 1000).toLocaleString();
}

function summarizePayload(payload: QuickSetupPayload): string {
  const analog = asRecord(payload.analog);
  const channels = Array.isArray(analog.channels) ? analog.channels.length : 0;
  const idac = asRecord(payload.idac);
  const codes = Array.isArray(idac.codes) ? idac.codes.join(",") : "-";
  const gpio = Array.isArray(payload.gpio) ? payload.gpio.length : 0;
  return `CH ${channels || "-"} / IDAC ${codes} / IO ${gpio || "-"}`;
}

export function QuickSetupTile({ slotIndex, slot, mac, onRefresh }: QuickSetupTileProps) {
  const summary = asRecord(slot?.summary);
  const occupied = Boolean(slot?.occupied);
  const name = firstText(slot?.name, summary.name) ?? `Slot ${slotIndex + 1}`;
  const updated = formatTime(firstNumber(slot?.ts, slot?.timestamp, summary.ts, summary.timestamp) ?? slot?.updatedAt);
  const summaryText = firstText(slot?.summary, summary.summary, summary.description) ?? (occupied ? "Saved" : "Empty");
  const [busy, setBusy] = useState<BusyAction | null>(null);
  const [detail, setDetail] = useState<string | null>(null);

  const run = async (action: BusyAction, fn: () => Promise<string | null>) => {
    setBusy(action);
    setDetail(null);
    try {
      const next = await fn();
      if (next) setDetail(next);
      await onRefresh();
    } catch (e) {
      if (!(e instanceof PairingRequiredError)) {
        setDetail(e instanceof Error ? e.message : String(e));
      }
    } finally {
      setBusy(null);
    }
  };

  const save = () =>
    run("save", async () => {
      if (!mac) return "Pairing required";
      const payload = await api.quicksetupSave(mac, slotIndex);
      return summarizePayload(payload);
    });

  const load = () =>
    run("load", async () => {
      const payload = await api.quicksetupGet(slotIndex);
      return summarizePayload(payload);
    });

  const apply = () =>
    run("apply", async () => {
      if (!mac) return "Pairing required";
      const result = await api.quicksetupApply(mac, slotIndex);
      if (Array.isArray(result.failed) && result.failed.length > 0) {
        return `Failed ${result.failed.join(", ")}`;
      }
      return result.ok === false ? "Apply failed" : "Applied";
    });

  const clear = () =>
    run("clear", async () => {
      if (!mac) return "Pairing required";
      await api.quicksetupDelete(mac, slotIndex);
      return "Cleared";
    });

  return (
    <GlassCard title={`Setup ${slotIndex + 1}`}>
      <div class="kv-row">
        <span class="uppercase-tag">State</span>
        <span class="led-wrap">
          <Led state={occupied ? "on" : "off"} />
          <span class={occupied ? "text-ok" : "text-muted"}>{occupied ? "Saved" : "Empty"}</span>
        </span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Name</span>
        <span class="mono">{name}</span>
      </div>
      <div class="kv-row">
        <span class="uppercase-tag">Updated</span>
        <span class="mono">{updated}</span>
      </div>
      <div class="text-dim" style={{ minHeight: "32px", marginTop: "8px", fontSize: "0.8rem" }}>
        {detail ?? summaryText}
      </div>
      <div class="dio-row" style={{ marginTop: "12px" }}>
        <button class="btn primary" disabled={!mac || busy !== null} onClick={save}>
          {busy === "save" ? "..." : "Save"}
        </button>
        <button class="btn" disabled={!occupied || busy !== null} onClick={load}>
          {busy === "load" ? "..." : "Load"}
        </button>
        <button class="btn" disabled={!mac || !occupied || busy !== null} onClick={apply}>
          {busy === "apply" ? "..." : "Apply"}
        </button>
        <button class="btn" disabled={!mac || !occupied || busy !== null} onClick={clear}>
          {busy === "clear" ? "..." : "Clear"}
        </button>
      </div>
    </GlassCard>
  );
}
