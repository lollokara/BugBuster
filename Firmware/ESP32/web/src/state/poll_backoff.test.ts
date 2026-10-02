// WEB-28: only SignalPath stretched its poll interval when the device was
// unreachable; every other poller kept firing at its fixed rate. Every
// self-rescheduling poll loop must route its delay through pollIntervalFor().
import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const src = join(dirname(fileURLToPath(import.meta.url)), "..");

function walk(dir: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) walk(p, out);
    else if (/\.tsx?$/.test(name) && !name.endsWith(".test.ts")) out.push(p);
  }
  return out;
}

describe("WEB-28 shared poll backoff", () => {
  it("no poll loop reschedules itself with a bare numeric delay", () => {
    const offenders: string[] = [];
    for (const f of walk(src)) {
      const text = readFileSync(f, "utf8");
      // ScopeCanvas fallback polling is a data stream, not a status poller.
      if (f.endsWith("ScopeCanvas.tsx")) continue;
      const re = /setTimeout\(tick,\s*(\d+|ms|intervalMs)\)/g;
      for (const m of text.matchAll(re)) offenders.push(`${relative(src, f)}: ${m[0]}`);
    }
    expect(offenders).toEqual([]);
  });
});
