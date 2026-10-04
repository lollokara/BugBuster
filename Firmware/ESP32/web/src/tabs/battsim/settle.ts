// =============================================================================
// battsim/settle.ts - long run actions (load / new / unload / delete / reopen).
//
// The P4 finishes these before it replies, which for a big run takes longer than
// the browser's request timeout, and the reply can be lost. The P4 keeps going,
// so on a transport failure poll until the device shows the result instead of
// reporting an error. A refusal from the P4 itself is final.
// =============================================================================

import type { BsStatus } from "./history";

export const ACT = { defaults: 14, newRun: 7, start: 8, pause: 9, stop: 10, unload: 11, load: 12, del: 13, reopen: 15 };

const LONG: number[] = [ACT.newRun, ACT.unload, ACT.load, ACT.del, ACT.reopen];
export const isLongAction = (id: number) => LONG.includes(id);

export const actionLabel = (id: number, run?: number): string => {
  const what = id === ACT.del ? "Deleting run" : id === ACT.load ? "Loading run" : id === ACT.unload ? "Unloading run"
    : id === ACT.newRun ? "Creating run" : id === ACT.reopen ? "Reopening run" : "Working";
  return run !== undefined ? `${what} #${run}...` : `${what}...`;
};

/** Has the device finished `id`? `ids` is only needed for delete. */
export function actionSettled(id: number, run: number | undefined, st: BsStatus | null, ids: number[] | null,
                              prevRun: number | undefined): boolean {
  switch (id) {
    case ACT.del: return run !== undefined && ids !== null && !ids.includes(run);
    case ACT.load: return st !== null && run !== undefined && st.state !== 0 && st.runId === run;
    case ACT.unload: return st !== null && st.state === 0;
    case ACT.newRun: return st !== null && st.state !== 0 && st.runId !== (prevRun ?? 0);
    case ACT.reopen: return st !== null && st.state === 1;
    default: return true;
  }
}

/** The P4 answered and refused (as opposed to no answer / S3 side failure). */
export const isRefusal = (e: unknown): boolean => e instanceof Error && e.message.startsWith("rejected by the DAQ HAT");

/** Why the P4 refused, in terms of what the user can do about it. */
export function refusalText(id: number, lastError: number, errorText: string[]): string {
  if (lastError === 3) {
    if (id === ACT.del) return "This run is loaded on the DAQ HAT. Unload it first, then delete it.";
    if (id === ACT.newRun || id === ACT.load) return "A run is active. Pause or stop it first.";
  }
  return errorText[lastError] || `error ${lastError}`;
}

export interface SettleDeps {
  status(): Promise<BsStatus>;
  runIds(): Promise<number[]>;
  sleep(ms: number): Promise<void>;
  now(): number;
}

export async function runLongAction(deps: SettleDeps, send: () => Promise<void>, id: number, run?: number,
                                    opts: { timeoutMs?: number; pollMs?: number } = {}): Promise<void> {
  const { timeoutMs = 60000, pollMs = 1000 } = opts;
  let prevRun: number | undefined;
  try { prevRun = (await deps.status()).runId; } catch { /* baseline only */ }
  try {
    await send();
    return;
  } catch (e) {
    if (isRefusal(e)) throw e;
  }
  const deadline = deps.now() + timeoutMs;
  do {
    await deps.sleep(pollMs);
    let st: BsStatus | null = null;
    let ids: number[] | null = null;
    try { st = await deps.status(); } catch { /* P4 busy: keep polling */ }
    if (id === ACT.del) { try { ids = await deps.runIds(); } catch { /* keep polling */ } }
    if (actionSettled(id, run, st, ids, prevRun)) return;
  } while (deps.now() < deadline);
  throw new Error(`${actionLabel(id).replace("...", "")} did not finish in ${Math.round(timeoutMs / 1000)} s`);
}

/** The identity keys and chemistry defaults are locked while a run is loaded, so the DAQ HAT
 *  refuses a new run's parameters. Unloading keeps the loaded run on flash; an active run is never touched. */
export async function prepareForNewRun(deps: SettleDeps, unload: () => Promise<void>): Promise<void> {
  let st: BsStatus;
  try { st = await deps.status(); } catch { return; }
  if (st.state === 2) throw new Error("A run is active. Pause or stop it first.");
  if (st.state !== 0) await runLongAction(deps, unload, ACT.unload);
}
