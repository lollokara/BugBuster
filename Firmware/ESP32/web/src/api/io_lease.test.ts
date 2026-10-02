// @vitest-environment happy-dom
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { afterEach, describe, expect, it, vi } from "vitest";
import { ioClaim, ioForceRelease, ioOwnerStatus, ioRelease } from "./io_lease";

// Registered (method, uri) pairs, read from the firmware so the test can never
// drift from what the device actually serves.
function registeredRoutes(): Set<string> {
  const src = readFileSync(
    resolve(__dirname, "../../../src/web/webserver.cpp"),
    "utf-8",
  );
  const out = new Set<string>();
  for (const m of src.matchAll(/\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*HTTP_([A-Z]+)/g)) {
    out.add(`${m[2]} ${m[1]}`);
  }
  return out;
}

describe("io_lease routes (IO-11)", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("parses the firmware route table", () => {
    expect(registeredRoutes().has("GET /api/io/owner")).toBe(true);
  });

  // IO-11: remove `.fails` when io_lease.ts calls POST/DELETE /api/io/owner.
  it.fails("calls only routes the firmware registers", async () => {
    const calls: string[] = [];
    vi.stubGlobal("fetch", vi.fn(async (url: string, init?: RequestInit) => {
      calls.push(`${init?.method ?? "GET"} ${url}`);
      return new Response(JSON.stringify({ slots: [] }), { status: 200 });
    }));

    await ioClaim([0], 5000, "test");
    await ioRelease([0]);
    await ioOwnerStatus();
    await ioForceRelease(0);

    const routes = registeredRoutes();
    expect(calls.filter((c) => !routes.has(c))).toEqual([]);
  });
});
