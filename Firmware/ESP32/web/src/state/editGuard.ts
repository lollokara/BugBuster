// =============================================================================
// editGuard — guard a polled status field against being overwritten by an
// in-flight poll response that races a fresh user-initiated write.
//
// Pattern (already used for selftest in signals.ts):
//   1. Capture `startedMs = Date.now()` BEFORE awaiting the poll fetch.
//   2. When the user mutates locally, call `bumpLocalWrite()` which records
//      "now" + a small grace window.
//   3. After the poll resolves, only apply the response if
//      `shouldApplyPoll(startedMs)` returns true. Otherwise the local write
//      is more recent and the poll response is stale.
//
// `graceMs` is added on top of the bare timestamp comparison so a poll that
// started just after the write but before the firmware processed it still
// gets discarded — without it, a poll inflight at write_time + 1 ms would
// race-win even though its response reflects pre-write state.
// =============================================================================

export interface WriteGuard {
  /** Mark a fresh local write at the current wallclock time. Call BEFORE the
   *  HTTP request that performs the write — that way an immediately-following
   *  poll sees a newer guard timestamp. */
  bumpLocalWrite: () => void;

  /** Returns true when a poll that started at `pollStartedMs` is still
   *  considered authoritative (no newer local write intervened). */
  shouldApplyPoll: (pollStartedMs: number) => boolean;

  /** Reset the guard (e.g. on tab unmount or device reconnect). */
  reset: () => void;
}

export function createWriteGuard(graceMs: number = 750): WriteGuard {
  let lastWriteMs = 0;
  return {
    bumpLocalWrite() {
      lastWriteMs = Date.now();
    },
    shouldApplyPoll(pollStartedMs: number) {
      // The poll is authoritative iff it began at or after the last local
      // write, plus the grace window to absorb firmware-side propagation
      // latency.
      return pollStartedMs >= lastWriteMs + graceMs;
    },
    reset() {
      lastWriteMs = 0;
    },
  };
}
