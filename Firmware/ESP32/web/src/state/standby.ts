// =============================================================================
// state/standby.ts - logical client presence for system standby.
//
// A browser tab is a control client only while the app is actually connected to
// the device (status polling succeeding). It registers a random non-zero client
// id per connection epoch, refreshes it every 10 s (the device expires it after
// 45 s and keeps at most 8) and releases it when the app disconnects or the page
// goes away. A refresh is not activity. Cable/Wi-Fi association is not presence.
// =============================================================================

import { computed, effect, signal } from "@preact/signals";
import { api, HttpError } from "../api/client";
import type { StandbyStatus } from "../api/client";
import { deviceReachable, deviceStatus } from "./signals";

export const PRESENCE_REFRESH_MS = 10_000;

export type PresenceCapability = "unknown" | "supported" | "unsupported";

export function newClientId(
  fill: (a: Uint32Array) => void = (a) => { crypto.getRandomValues(a); },
): number {
  const a = new Uint32Array(1);
  do { fill(a); } while (a[0] === 0);
  return a[0]!;
}

/** Firmware without standby answers the routes 404/405. */
export function isUnsupported(e: unknown): boolean {
  return e instanceof HttpError && (e.status === 404 || e.status === 405);
}

export interface PresenceIo {
  send: (clientId: number, present: boolean, signal?: AbortSignal) => Promise<StandbyStatus | void>;
  /** Best-effort release that may outlive the page (fetch keepalive). */
  release?: (clientId: number) => void;
}

export interface PresenceOptions {
  refreshMs?: number;
  newId?: () => number;
  onStatus?: (s: StandbyStatus) => void;
  onCapability?: (c: PresenceCapability) => void;
}

export class PresenceController {
  clientId = 0;
  capability: PresenceCapability = "unknown";
  detached = false;
  private ctl: AbortController | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private failures = 0;

  constructor(private readonly io: PresenceIo, private readonly opts: PresenceOptions = {}) {}

  /** Start a fresh epoch with a new random id. No-op while detached or unsupported. */
  open(): void {
    if (this.detached || this.capability === "unsupported") return;
    this.halt();
    this.clientId = (this.opts.newId ?? newClientId)();
    this.ctl = new AbortController();
    void this.beat(this.clientId, this.ctl.signal);
  }

  /** Stop and release the current epoch (best effort). */
  close(): void {
    const id = this.clientId;
    this.halt();
    if (id !== 0 && this.capability !== "unsupported") this.io.release?.(id);
  }

  /** Stop without a release: the device is unreachable, the TTL will expire the id. */
  suspend(): void {
    this.halt();
  }

  /** Release and hold closed (no heartbeat) until attach(). */
  detach(): void {
    this.close();
    this.detached = true;
  }

  attach(): void {
    this.detached = false;
    this.open();
  }

  /** Refresh immediately (tab became visible after throttled timers). */
  refreshNow(): void {
    if (this.clientId === 0 || this.ctl === null) return;
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = null;
    void this.beat(this.clientId, this.ctl.signal);
  }

  private halt(): void {
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = null;
    this.ctl?.abort();
    this.ctl = null;
    this.clientId = 0;
  }

  private setCapability(c: PresenceCapability): void {
    if (this.capability === c) return;
    this.capability = c;
    this.opts.onCapability?.(c);
  }

  private async beat(id: number, signal: AbortSignal): Promise<void> {
    try {
      const st = await this.io.send(id, true, signal);
      if (signal.aborted) return;
      this.setCapability("supported");
      this.failures = 0;
      if (st) this.opts.onStatus?.(st);
    } catch (e) {
      if (signal.aborted) return;
      if (isUnsupported(e)) {
        this.setCapability("unsupported");
        this.halt();
        return;
      }
      this.failures += 1;
      if (this.failures === 1) console.debug("standby presence refresh failed", e);
    }
    if (!signal.aborted) {
      this.timer = setTimeout(() => { void this.beat(id, signal); }, this.opts.refreshMs ?? PRESENCE_REFRESH_MS);
    }
  }
}

/* ---- App singleton ---- */

export const standbyStatus = signal<StandbyStatus | null>(null);
export const standbyCapability = signal<PresenceCapability>("unknown");

export const standbyPresence = new PresenceController(
  {
    send: (id, present, signal) => api.standby.presence(id, present, signal),
    release: (id) => {
      void fetch("/api/standby/presence", {
        method: "POST",
        keepalive: true,
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ clientId: id, present: false }),
      }).catch(() => { /* the device expires the id on its own */ });
    },
  },
  {
    onStatus: (s) => { standbyStatus.value = s; },
    onCapability: (c) => { standbyCapability.value = c; },
  },
);

/** True once /api/status has answered and has not failed twice in a row. */
const appConnected = computed(() => deviceStatus.value !== null && deviceReachable.value);

/**
 * Bind the presence epoch to the app connection. Returns the cleanup (call from
 * an effect): it removes every listener, disposes the effect and releases the id.
 */
export function startStandbyPresence(ctl: PresenceController = standbyPresence): () => void {
  const disposeEffect = effect(() => {
    if (appConnected.value) ctl.open();
    else ctl.suspend();
  });
  const onPageHide = () => ctl.close();
  const onPageShow = () => { if (appConnected.peek()) ctl.open(); };
  const onVisible = () => { if (document.visibilityState === "visible") ctl.refreshNow(); };
  window.addEventListener("pagehide", onPageHide);
  window.addEventListener("pageshow", onPageShow);
  document.addEventListener("visibilitychange", onVisible);
  return () => {
    window.removeEventListener("pagehide", onPageHide);
    window.removeEventListener("pageshow", onPageShow);
    document.removeEventListener("visibilitychange", onVisible);
    disposeEffect();
    ctl.close();
  };
}
