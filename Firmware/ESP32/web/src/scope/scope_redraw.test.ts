// WEB-28: the scope canvas redrew at 60 fps even with no new data, and the
// SSE error counter never reset, so three errors spread over a long session
// latched the page into 4 req/s polling.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const here = dirname(fileURLToPath(import.meta.url));

describe("WEB-28 scope canvas", () => {
  it.fails("redraw gate skips frames whose inputs did not change", async () => {
    const mod = await import(/* @vite-ignore */ "./redrawGate" + "");
    const gate = mod.makeRedrawGate();
    const buf: unknown[] = [];
    expect(gate([buf, 1, "overlay"])).toBe(true);
    expect(gate([buf, 1, "overlay"])).toBe(false);
    expect(gate([buf, 2, "overlay"])).toBe(true);
    expect(gate([[], 2, "overlay"])).toBe(true);
  });

  it.fails("SSE error counter resets on a successful open or message", () => {
    const src = readFileSync(join(here, "ScopeCanvas.tsx"), "utf8");
    expect(src).toMatch(/onopen\s*=\s*\(\)\s*=>\s*\{[^}]*consecutiveErrors\s*=\s*0/);
  });
});
