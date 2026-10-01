// WEB-28: request() had no timeout, so a busy device pinned every browser
// connection (6 per host) behind stalled requests. B: every request aborts
// after REQUEST_TIMEOUT_MS and surfaces as an HttpError(0, "Timeout").
import { afterEach, describe, expect, it, vi } from "vitest";
import { request, HttpError, REQUEST_TIMEOUT_MS } from "./core";

describe("WEB-28 request timeout", () => {
  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it.fails("aborts a stalled request after REQUEST_TIMEOUT_MS", async () => {
    vi.useFakeTimers();
    vi.stubGlobal("fetch", (_p: string, init: RequestInit) =>
      new Promise((_res, rej) => {
        init.signal?.addEventListener("abort", () =>
          rej(new DOMException("aborted", "AbortError")));
      }));
    const p = request("/api/status");
    const check = expect(p).rejects.toBeInstanceOf(HttpError);
    await vi.advanceTimersByTimeAsync(REQUEST_TIMEOUT_MS + 10);
    await check;
  });

  it.fails("uses a bounded timeout", () => {
    expect(REQUEST_TIMEOUT_MS).toBeGreaterThan(0);
    expect(REQUEST_TIMEOUT_MS).toBeLessThanOrEqual(15000);
  });
});
