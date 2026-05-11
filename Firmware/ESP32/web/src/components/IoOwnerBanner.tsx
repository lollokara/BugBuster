// =============================================================================
// IoOwnerBanner — shown above a tab when another session holds its IO slots.
//
// Visible when EITHER:
//   • lease.blocked is true  (set by the periodic useIoLease poll), OR
//   • ioRejectEvent is non-null for a slot covered by this lease
//     (set immediately when a write returns HTTP 409).
// =============================================================================

import type { IoLeaseHandle } from "../api/io_lease";
import { ownerKindName, ioRejectEvent } from "../api/io_lease";
import { getCachedToken } from "../api/client";
import { deviceMac } from "../state/signals";

interface Props {
  lease: IoLeaseHandle;
  /** The slot numbers this tab owns — used to match reject events. */
  slots: number[];
}

export function IoOwnerBanner({ lease, slots }: Props) {
  // Check for an immediate reject event covering one of our slots.
  const rejectEvt = ioRejectEvent.value;
  const rejectForUs =
    rejectEvt !== null && slots.includes(rejectEvt.slot);

  const showBanner = lease.blocked.value || rejectForUs;
  if (!showBanner) return null;

  // Prefer the reject-event kind (most recent write attempt), else the lease kind.
  const kind = rejectForUs ? rejectEvt!.currentOwnerKind : lease.ownerKind.value;

  const mac = deviceMac.value;
  const isAdmin = !!(mac && getCachedToken(mac));

  const handleForceTake = async () => {
    // Clear reject event optimistically so the banner hides on success.
    ioRejectEvent.value = null;
    try {
      await lease.forceTake();
    } catch (e) {
      console.warn("Force-take failed:", e);
    }
  };

  const slotLabel = rejectForUs
    ? `slot ${rejectEvt!.slot < 12 ? `IO${rejectEvt!.slot + 1}` : `CH${rejectEvt!.slot - 12}`}`
    : "IO slots";

  return (
    <div class="io-owner-banner">
      <div class="io-owner-banner__text">
        <span>IO held by {ownerKindName(kind)}</span>
        {rejectForUs && (
          <span class="io-owner-banner__sub">
            Write to {slotLabel} rejected — blocked by {ownerKindName(kind)}
          </span>
        )}
      </div>
      {isAdmin && (
        <button class="btn btn-sm" onClick={handleForceTake}>
          Force take (admin)
        </button>
      )}
    </div>
  );
}
