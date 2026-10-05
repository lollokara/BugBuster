// Pure view helpers for the standby card (kept out of the component for tests).

import type { StandbyStatus } from "../../api/client";

export const TIMEOUT_OPTIONS: ReadonlyArray<{ seconds: number; label: string }> = [
  { seconds: 60, label: "1 min" },
  { seconds: 300, label: "5 min" },
  { seconds: 900, label: "15 min" },
  { seconds: 0, label: "Off" },
];

export function timeoutLabel(seconds: number): string {
  return TIMEOUT_OPTIONS.find((o) => o.seconds === seconds)?.label ?? `${seconds} s`;
}

const STATE_LABELS: Record<string, string> = {
  active: "Active",
  preparing: "Entering standby",
  asleep: "Standby",
  waking: "Waking",
  fault_safe: "Fault (outputs safe)",
};

export function stateLabel(s: StandbyStatus | null | undefined): string {
  if (!s) return "--";
  return STATE_LABELS[s.state] ?? s.state;
}

export function formatCountdown(ms: number): string {
  const total = Math.max(0, Math.ceil(ms / 1000));
  const m = Math.floor(total / 60);
  const sec = total % 60;
  return `${m}:${String(sec).padStart(2, "0")}`;
}

/** One line on what the inactivity timer is doing right now. */
export function idleLine(s: StandbyStatus | null | undefined): string {
  if (!s) return "--";
  if (s.state !== "active") return stateLabel(s);
  if (s.timeoutSeconds === 0) return "Automatic standby is off";
  if (s.clients > 0 || s.inhibitors !== 0) {
    const parts: string[] = [];
    if (s.clients > 0) parts.push(`${s.clients} connected client${s.clients === 1 ? "" : "s"}`);
    if (s.inhibitors !== 0) parts.push("running work");
    return `Held awake by ${parts.join(" and ")}`;
  }
  return `Standby in ${formatCountdown(s.idleRemainingMs)}`;
}

/** BUSY/waking replies mean hardware work must wait; never replay automatically. */
export function isHardwareReady(s: StandbyStatus | null | undefined): boolean {
  return !!s && s.state === "active" && s.ready;
}
