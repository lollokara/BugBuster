// =============================================================================
// IO lease helpers — single-owner IO reservation over HTTP.
//
// Every IO-mutating tab calls useIoLease() at its root to claim the slots it
// needs. The hook holds a 5 s lease renewed every 2 s while the tab is
// mounted, and releases on unmount. When the server returns IO_HELD_BY_OTHER
// the returned `blocked` signal goes true so the tab can render a banner.
//
// Transport: uses the existing request() helper from api/client.ts.
// Endpoints: /api/io/owner (GET), /api/io/owner/claim (POST),
//            /api/io/owner/release (DELETE), /api/io/owner/force (POST).
// These are implemented on the firmware side (webserver.cpp).
// =============================================================================

import { signal, type Signal } from "@preact/signals";
import { useEffect } from "preact/hooks";
import { getCachedToken, ADMIN_TOKEN_HEADER, PairingRequiredError, type IoOwnerRejectError } from "./client";
import { deviceMac } from "../state/signals";

// Re-export so callers only need to import from one place.
export { IoOwnerRejectError } from "./client";

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export type OwnerSlot = {
  slot: number;
  kind: number;
  session: number;       // firmware key is "session" (not session_id)
  lease_until_ms: number;
  free?: boolean;
};

/** Numeric owner-kind codes (mirror io_owner_kind_t in io_owner.h). */
export const IO_OWNER_KIND: Record<number, string> = {
  0: "None",
  1: "USB",
  2: "HTTP",
  3: "Script",
  4: "CLI",
  5: "Internal",
};

export function ownerKindName(kind: number): string {
  return IO_OWNER_KIND[kind] ?? `Kind ${kind}`;
}

// ---------------------------------------------------------------------------
// Global IO-reject signal — lightest pattern: a module-level Preact signal.
// Any mutation that catches an IoOwnerRejectError calls notifyIoReject() to
// push the conflict details here. IoOwnerBanner subscribes to this signal so
// it surfaces the banner immediately without waiting for the next poll cycle.
// ---------------------------------------------------------------------------

export interface IoRejectEvent {
  slot: number;
  currentOwnerKind: number;
}

/** Reactive signal; latest reject event or null when no active conflict. */
export const ioRejectEvent = signal<IoRejectEvent | null>(null);

/**
 * Call this from any mutation catch-block when an `IoOwnerRejectError` is caught.
 * The `IoOwnerBanner` rendered by the owning tab will pick it up immediately.
 *
 * Example:
 *   try { await api.channel.setDac(...); }
 *   catch (e) { if (e instanceof IoOwnerRejectError) notifyIoReject(e); }
 */
export function notifyIoReject(err: IoOwnerRejectError): void {
  ioRejectEvent.value = { slot: err.slot, currentOwnerKind: err.currentOwnerKind };
}

// Status bytes the firmware can return in the response body
const IO_OK = 0x00;
const IO_HELD_BY_OTHER = 0x01;

// ---------------------------------------------------------------------------
// HTTP helpers — all go through the existing request() internals via fetch.
// We call fetch directly here because we need to inspect the raw response
// body (which may be a binary Uint8Array) and the existing request() helper
// is generic-JSON-only. We reproduce its auth / error normalisation.
// ---------------------------------------------------------------------------

async function authedFetch(
  path: string,
  init: RequestInit & { admin?: boolean } = {},
): Promise<Response> {
  const mac = deviceMac.value;
  const headers: Record<string, string> = {
    ...(init.headers as Record<string, string> | undefined),
  };
  if (mac) {
    const token = getCachedToken(mac);
    if (token) headers[ADMIN_TOKEN_HEADER] = token;
  }

  let res: Response;
  try {
    res = await fetch(path, { ...init, headers });
  } catch (err) {
    throw new Error(err instanceof Error ? err.message : "fetch failed");
  }

  if (res.status === 401) {
    window.dispatchEvent(new CustomEvent("bb:pairing-required"));
    throw new PairingRequiredError();
  }
  return res;
}

// ---------------------------------------------------------------------------
// Public API functions
// ---------------------------------------------------------------------------

/**
 * Claim a set of IO slots.
 *
 * Returns the raw response body as a Uint8Array. The caller inspects byte[0]:
 *   0x00 = IO_OK
 *   0x01 = IO_HELD_BY_OTHER (byte[1] = current owner kind)
 */
export async function ioClaim(
  slots: number[],
  leaseMs: number,
  purpose: string,
): Promise<Uint8Array> {
  const res = await authedFetch("/api/io/owner/claim", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ slots, lease_ms: leaseMs, purpose }),
  });
  if (!res.ok && res.status !== 409) {
    // 409 = Conflict (IO_HELD_BY_OTHER) — we want the body for that case
    throw new Error(`ioClaim failed: ${res.status}`);
  }
  const buf = await res.arrayBuffer();
  return new Uint8Array(buf);
}

/**
 * Release a set of slots (or all owned slots if null).
 */
export async function ioRelease(slots: number[] | null): Promise<void> {
  const body = slots === null ? {} : { slots };
  const res = await authedFetch("/api/io/owner/release", {
    method: "DELETE",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!res.ok) {
    // Best-effort; don't throw — release failures should not block UI.
    console.warn("ioRelease failed:", res.status);
  }
}

/**
 * Query the full 16-slot ownership table.
 */
export async function ioOwnerStatus(): Promise<OwnerSlot[]> {
  const res = await authedFetch("/api/io/owner");
  if (!res.ok) throw new Error(`ioOwnerStatus failed: ${res.status}`);
  const body = await res.json();
  // Firmware returns { "slots": [...] } — normalise to bare array.
  const arr: OwnerSlot[] = Array.isArray(body) ? body : (Array.isArray(body?.slots) ? body.slots : []);
  return arr;
}

/**
 * Force-release a single slot. Requires an admin token (enforced by firmware).
 */
export async function ioForceRelease(slot: number): Promise<void> {
  const res = await authedFetch("/api/io/owner/force", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ slot }),
  });
  if (!res.ok) throw new Error(`ioForceRelease failed: ${res.status}`);
}

// ---------------------------------------------------------------------------
// useIoLease hook (Preact + Signals)
// ---------------------------------------------------------------------------

export interface IoLeaseHandle {
  /** True when the server says a different session holds our slots. */
  blocked: Signal<boolean>;
  /** The kind code of the current blocking owner (0 when not blocked). */
  ownerKind: Signal<number>;
  /**
   * Force-take the slots using the admin token. Clears `blocked` on success.
   * Throws if the firmware rejects (e.g. missing admin token).
   */
  forceTake: () => Promise<void>;
}

const LEASE_MS = 5_000;
const RENEW_INTERVAL_MS = 2_000;

export function useIoLease(slots: number[], purpose: string): IoLeaseHandle {
  // Stable signal instances per hook call — created once, never replaced.
  const blocked = signal(false);
  const ownerKind = signal(0);

  useEffect(() => {
    let alive = true;
    let renewTimer: number | null = null;

    const tryAcquire = async () => {
      if (!alive) return;
      try {
        const resp = await ioClaim(slots, LEASE_MS, purpose);
        if (!alive) return;
        if (resp[0] === IO_OK) {
          blocked.value = false;
          ownerKind.value = 0;
        } else if (resp[0] === IO_HELD_BY_OTHER) {
          blocked.value = true;
          ownerKind.value = resp[1] ?? 0;
        }
      } catch {
        // Network error — don't flip blocked, just retry on next interval.
      }
      if (alive) {
        renewTimer = window.setTimeout(tryAcquire, RENEW_INTERVAL_MS);
      }
    };

    void tryAcquire();

    return () => {
      alive = false;
      if (renewTimer !== null) window.clearTimeout(renewTimer);
      // Best-effort release — don't await, we're in cleanup.
      void ioRelease(slots);
    };
    // slots and purpose are stable per mount; no need in dep array.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const forceTake = async () => {
    for (const slot of slots) {
      await ioForceRelease(slot);
    }
    // Re-attempt claim immediately after force-releasing.
    try {
      const resp = await ioClaim(slots, LEASE_MS, purpose);
      if (resp[0] === IO_OK) {
        blocked.value = false;
        ownerKind.value = 0;
      }
    } catch {
      // Caller will see blocked still true.
    }
  };

  return { blocked, ownerKind, forceTake };
}
