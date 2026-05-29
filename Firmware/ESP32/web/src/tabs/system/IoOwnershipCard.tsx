import { useEffect, useState } from "preact/hooks";
import { GlassCard } from "../../components/GlassCard";
import { getCachedToken } from "../../api/client";
import {
  ioOwnerStatus,
  ioForceRelease,
  ownerKindName,
  type OwnerSlot,
} from "../../api/io_lease";
import { deviceMac } from "../../state/signals";

export function IoOwnershipCard() {
  const mac = deviceMac.value;
  const isAdmin = !!(mac && getCachedToken(mac));
  const [slots, setSlots] = useState<OwnerSlot[]>([]);
  const [forceError, setForceError] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      if (!alive) return;
      try {
        const data = await ioOwnerStatus();
        if (alive) setSlots(data);
      } catch {
        /* ignore — device may not have this endpoint yet */
      }
      if (alive) setTimeout(tick, 2000);
    };
    tick();
    return () => { alive = false; };
  }, []);

  const handleForce = async (slot: number) => {
    setForceError(null);
    try {
      await ioForceRelease(slot);
      // Refresh immediately
      const data = await ioOwnerStatus();
      setSlots(data);
    } catch (e) {
      setForceError(e instanceof Error ? e.message : "Force release failed");
    }
  };

  const activeSlots = slots.filter((s) => s.kind !== 0);

  return (
    <GlassCard title="IO Ownership">
      {forceError && <div class="text-dim" style="color:var(--color-err)">{forceError}</div>}
      {activeSlots.length === 0 ? (
        <div class="text-dim">All slots free</div>
      ) : (
        <table class="ownership-table">
          <thead>
            <tr>
              <th>Slot</th>
              <th>Owner</th>
              <th>Lease expires</th>
              {isAdmin && <th></th>}
            </tr>
          </thead>
          <tbody>
            {activeSlots.map((s) => {
              const expiry = s.lease_until_ms === 0
                ? "∞"
                : new Date(s.lease_until_ms).toLocaleTimeString();
              return (
                <tr key={s.slot}>
                  <td class="mono">{s.slot < 12 ? `IO${s.slot + 1}` : `CH${s.slot - 12}`}</td>
                  <td>{ownerKindName(s.kind)}</td>
                  <td class="mono">{expiry}</td>
                  {isAdmin && (
                    <td>
                      <button class="btn btn-sm" onClick={() => handleForce(s.slot)}>
                        Force release
                      </button>
                    </td>
                  )}
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </GlassCard>
  );
}
