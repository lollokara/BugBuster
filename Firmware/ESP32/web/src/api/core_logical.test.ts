// TR-11b: a failed action must reject whether the firmware answers 4xx (new)
// or 200 {"ok": false} (old firmware). A: 200 + ok:false resolved as success.
import { afterEach, describe, expect, it, vi } from "vitest";
import { request, HttpError } from "./core";

function stubFetch(status: number, body: unknown) {
  vi.stubGlobal("fetch", async () => new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  }));
}

describe("TR-11b logical failures", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("rejects a POST answered 200 {ok:false}", async () => {
    stubFetch(200, { ok: false, error: "channel must be 0-3" });
    await expect(request("/api/channel/9/dac", { method: "POST", body: {} }))
      .rejects.toBeInstanceOf(HttpError);
  });

  it("rejects a POST answered 400 with the device message", async () => {
    stubFetch(400, { ok: false, error: "channel must be 0-3" });
    await expect(request("/api/channel/9/dac", { method: "POST", body: {} }))
      .rejects.toThrow("channel must be 0-3");
  });

  it("keeps GET status replies with ok:false as data", async () => {
    stubFetch(200, { ok: false, hatPresent: false });
    await expect(request("/api/hat/calibration")).resolves.toEqual({ ok: false, hatPresent: false });
  });
});
