// =============================================================================
// Global app signals — reactive state shared across tabs.
// =============================================================================

import { signal, computed } from "@preact/signals";
import type { BoardState, DeviceInfo, PairingInfo } from "../api/client";

/* ---- Pairing ---- */

export const deviceInfo = signal<DeviceInfo | null>(null);
export const pairingInfo = signal<PairingInfo | null>(null);
export const pairingRequired = signal<boolean>(false);

export const deviceMac = computed(() =>
  pairingInfo.value?.macAddress ?? deviceInfo.value?.macAddress ?? null,
);

/* ---- Core status ---- */

/** Last /api/status snapshot. Typed as `any` for now — refine incrementally. */
export const deviceStatus = signal<any>(null);

/* ---- Board profile ---- */

export const boardState = signal<BoardState | null>(null);

/* ---- Scope ---- */

export type PlotMode = "overlay" | "stacked";

export interface ScopeSample {
  t: number;              // sample time (seconds since capture start)
  v: [number, number, number, number];
}

export const scopeBuffer = signal<ScopeSample[]>([]);
export const scopeSeq = signal<number>(0);
export const scopeRunning = signal<boolean>(true);
export const scopePlotMode = signal<PlotMode>("overlay");
export const scopeChannelEnabled = signal<[boolean, boolean, boolean, boolean]>(
  [true, true, true, true],
);
export const scopeChannelOffset = signal<[number, number, number, number]>([
  0, 0, 0, 0,
]);
export const scopeChannelInvert = signal<[boolean, boolean, boolean, boolean]>([
  false, false, false, false,
]);
export const scopeTriggerLevel = signal<number>(0);
export const scopeTimeBase = signal<number>(1); /* seconds of window */

/* Ring-buffer size; drop oldest samples beyond this. */
export const SCOPE_RING_CAPACITY = 8192;

export function pushScopeSamples(seq: number, samples: number[][]): void {
  if (samples.length === 0) return;
  const buf = scopeBuffer.value.slice();
  // Firmware /api/scope bucket layout (see webserver.cpp handle_get_scope):
  //   [t_ms, ch0_avg, ch1_avg, ch2_avg, ch3_avg,
  //    ch0_min, ch0_max, ch1_min, ch1_max, ch2_min, ch2_max, ch3_min, ch3_max]
  // t_ms is millis_now() on the device; convert to seconds for plot math.
  for (const s of samples) {
    buf.push({
      t: (s[0] ?? 0) / 1000,
      v: [s[1] ?? 0, s[2] ?? 0, s[3] ?? 0, s[4] ?? 0],
    });
  }
  if (buf.length > SCOPE_RING_CAPACITY) {
    buf.splice(0, buf.length - SCOPE_RING_CAPACITY);
  }
  scopeBuffer.value = buf;
  scopeSeq.value = seq;
}

/* ---- Sparklines (Overview tab) ---- */

export const SPARK_RING_CAPACITY = 200;

export const channelSparks = signal<number[][]>([[], [], [], []]);

export function pushChannelSamples(values: [number, number, number, number]): void {
  const next = channelSparks.value.map((arr, i) => {
    const copy = arr.slice();
    copy.push(values[i]!);
    if (copy.length > SPARK_RING_CAPACITY) copy.shift();
    return copy;
  });
  channelSparks.value = next;
}
