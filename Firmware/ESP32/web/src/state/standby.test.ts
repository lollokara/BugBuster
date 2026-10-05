import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { HttpError } from "../api/core";
import { PRESENCE_REFRESH_MS, PresenceController, newClientId } from "./standby";

function make(send: (id: number, present: boolean, signal?: AbortSignal) => Promise<unknown>) {
  const release = vi.fn();
  const ids = [11, 22, 33];
  const ctl = new PresenceController({ send: send as never, release }, { newId: () => ids.shift()! });
  return { ctl, release };
}

describe("PresenceController", () => {
  beforeEach(() => { vi.useFakeTimers(); });
  afterEach(() => { vi.useRealTimers(); });

  it("registers immediately and refreshes every 10 s", async () => {
    const send = vi.fn(async () => undefined);
    const { ctl } = make(send);
    ctl.open();
    await vi.advanceTimersByTimeAsync(0);
    expect(send).toHaveBeenCalledTimes(1);
    expect(send.mock.calls[0]!.slice(0, 2)).toEqual([11, true]);
    await vi.advanceTimersByTimeAsync(PRESENCE_REFRESH_MS - 1);
    expect(send).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(2);
    expect(send).toHaveBeenCalledTimes(2);
    expect(PRESENCE_REFRESH_MS).toBe(10_000);
    ctl.close();
  });

  it("each epoch gets a new id and close releases the old one", async () => {
    const send = vi.fn(async () => undefined);
    const { ctl, release } = make(send);
    ctl.open();
    await vi.advanceTimersByTimeAsync(0);
    ctl.close();
    expect(release).toHaveBeenCalledWith(11);
    expect(ctl.clientId).toBe(0);
    ctl.open();
    expect(ctl.clientId).toBe(22);
    ctl.close();
  });

  it("suspend stops the heartbeat without a release (device unreachable)", async () => {
    const send = vi.fn(async () => undefined);
    const { ctl, release } = make(send);
    ctl.open();
    await vi.advanceTimersByTimeAsync(0);
    ctl.suspend();
    await vi.advanceTimersByTimeAsync(60_000);
    expect(send).toHaveBeenCalledTimes(1);
    expect(release).not.toHaveBeenCalled();
  });

  it("aborts an in-flight request on close", async () => {
    let seen: AbortSignal | undefined;
    const send = vi.fn((_id: number, _p: boolean, signal?: AbortSignal) => {
      seen = signal;
      return new Promise<never>(() => { /* never settles */ });
    });
    const { ctl } = make(send as never);
    ctl.open();
    await vi.advanceTimersByTimeAsync(0);
    ctl.close();
    expect(seen?.aborted).toBe(true);
  });

  it("goes quiet once on a firmware without standby", async () => {
    const send = vi.fn(async () => { throw new HttpError(404, "Not Found", "not found"); });
    const caps: string[] = [];
    const ctl = new PresenceController({ send: send as never }, { newId: () => 5, onCapability: (c) => caps.push(c) });
    ctl.open();
    await vi.advanceTimersByTimeAsync(0);
    await vi.advanceTimersByTimeAsync(120_000);
    expect(send).toHaveBeenCalledTimes(1);
    expect(ctl.capability).toBe("unsupported");
    expect(caps).toEqual(["unsupported"]);
    ctl.open();
    expect(send).toHaveBeenCalledTimes(1);
  });

  it("keeps retrying on transient errors", async () => {
    const send = vi.fn()
      .mockRejectedValueOnce(new HttpError(0, "Timeout", "no response"))
      .mockResolvedValue(undefined);
    const { ctl } = make(send as never);
    ctl.open();
    await vi.advanceTimersByTimeAsync(0);
    expect(ctl.capability).toBe("unknown");
    await vi.advanceTimersByTimeAsync(PRESENCE_REFRESH_MS + 1);
    expect(send).toHaveBeenCalledTimes(2);
    expect(ctl.capability).toBe("supported");
    ctl.close();
  });

  it("detach releases and stays closed until attach", async () => {
    const send = vi.fn(async () => undefined);
    const { ctl, release } = make(send);
    ctl.open();
    await vi.advanceTimersByTimeAsync(0);
    ctl.detach();
    expect(release).toHaveBeenCalledWith(11);
    ctl.open();
    await vi.advanceTimersByTimeAsync(PRESENCE_REFRESH_MS * 3);
    expect(send).toHaveBeenCalledTimes(1);
    ctl.attach();
    await vi.advanceTimersByTimeAsync(0);
    expect(send).toHaveBeenCalledTimes(2);
    expect(send.mock.calls[1]!.slice(0, 2)).toEqual([22, true]);
    ctl.close();
  });

  it("refreshNow beats immediately and resets the timer", async () => {
    const send = vi.fn(async () => undefined);
    const { ctl } = make(send);
    ctl.open();
    await vi.advanceTimersByTimeAsync(0);
    await vi.advanceTimersByTimeAsync(4_000);
    ctl.refreshNow();
    await vi.advanceTimersByTimeAsync(0);
    expect(send).toHaveBeenCalledTimes(2);
    await vi.advanceTimersByTimeAsync(PRESENCE_REFRESH_MS - 1);
    expect(send).toHaveBeenCalledTimes(2);
    ctl.close();
  });
});

describe("newClientId", () => {
  it("never returns zero", () => {
    const seq = [0, 0, 7];
    const id = newClientId((a) => { a[0] = seq.shift()!; });
    expect(id).toBe(7);
  });
});
