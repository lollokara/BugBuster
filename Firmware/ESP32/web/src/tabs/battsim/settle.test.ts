// Long run actions: poll until the device shows the result instead of surfacing a timeout.
import { describe, expect, it } from "vitest";
import { HttpError } from "../../api/core";
import type { BsStatus } from "./history";
import { ACT, actionSettled, isLongAction, isRefusal, prepareForNewRun, refusalText, runLongAction, type SettleDeps } from "./settle";

const st = (state: number, runId: number) => ({ state, runId } as BsStatus);

function deps(script: { status: () => BsStatus | Error; ids: () => number[] | Error }) {
  let t = 0;
  const calls = { status: 0, ids: 0 };
  const d: SettleDeps = {
    status: async () => { calls.status++; const r = script.status(); if (r instanceof Error) throw r; return r; },
    runIds: async () => { calls.ids++; const r = script.ids(); if (r instanceof Error) throw r; return r; },
    sleep: async (ms) => { t += ms; },
    now: () => t,
  };
  return { d, calls };
}

describe("battsim long actions", () => {
  it("settled predicates", () => {
    expect(actionSettled(ACT.del, 5, null, null, undefined)).toBe(false);
    expect(actionSettled(ACT.del, 5, null, [1, 5], undefined)).toBe(false);
    expect(actionSettled(ACT.del, 5, null, [1], undefined)).toBe(true);
    expect(actionSettled(ACT.load, 2, st(0, 0), null, undefined)).toBe(false);
    expect(actionSettled(ACT.load, 2, st(4, 2), null, undefined)).toBe(true);
    expect(actionSettled(ACT.unload, undefined, st(0, 0), null, 2)).toBe(true);
    expect(actionSettled(ACT.newRun, undefined, st(1, 2), null, 2)).toBe(false);
    expect(actionSettled(ACT.newRun, undefined, st(1, 3), null, 2)).toBe(true);
    expect(actionSettled(ACT.reopen, undefined, st(1, 2), null, 2)).toBe(true);
    expect(isLongAction(ACT.load) && !isLongAction(ACT.start)).toBe(true);
  });

  it("a lost reply polls until the run is gone and sends the action once", async () => {
    let polls = 0;
    const { d } = deps({
      status: () => (polls++ < 2 ? new HttpError(500, "x", "HAT busy: a run action is in progress") : st(4, 2)),
      ids: () => (polls < 4 ? [1, 2, 5] : [1, 2]),
    });
    let sent = 0;
    await runLongAction(d, async () => { sent++; throw new HttpError(0, "Timeout", "no response within 10000 ms"); }, ACT.del, 5, { pollMs: 10 });
    expect(sent).toBe(1);
  });

  it("an action that never settles reports a timeout, not success", async () => {
    const { d } = deps({ status: () => st(4, 2), ids: () => [1, 2, 5] });
    await expect(runLongAction(d, async () => { throw new Error("HAT timeout"); }, ACT.del, 5, { timeoutMs: 3000, pollMs: 1000 }))
      .rejects.toThrow(/did not finish/);
  });

  it("a P4 refusal is final and not polled", async () => {
    const { d, calls } = deps({ status: () => st(4, 2), ids: () => [1, 2] });
    const refusal = new HttpError(200, "Request failed", "rejected by the DAQ HAT");
    await expect(runLongAction(d, async () => { throw refusal; }, ACT.del, 2)).rejects.toBe(refusal);
    expect(isRefusal(refusal)).toBe(true);
    expect(calls.ids).toBe(0);
    expect(calls.status).toBe(1); // baseline only
  });

  it("explains BUSY in terms of the loaded run", () => {
    expect(refusalText(ACT.del, 3, [])).toContain("Unload it first");
    expect(refusalText(ACT.load, 3, [])).toContain("Pause or stop");
    expect(refusalText(ACT.del, 9, ["", "", "", "", "", "", "", "", "", "run not found"])).toBe("run not found");
  });

  it("a new run unloads a loaded run first and never touches an active one", async () => {
    let state = 4;
    const { d } = deps({ status: () => st(state, state ? 2 : 0), ids: () => [1, 2] });
    let unloads = 0;
    await prepareForNewRun(d, async () => { unloads++; state = 0; });
    expect(unloads).toBe(1);

    const idle = deps({ status: () => st(0, 0), ids: () => [1, 2] });
    await prepareForNewRun(idle.d, async () => { throw new Error("must not unload"); });

    const active = deps({ status: () => st(2, 2), ids: () => [1, 2] });
    await expect(prepareForNewRun(active.d, async () => { throw new Error("must not unload"); })).rejects.toThrow(/active/);
  });
});
