import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { generatedCompletions } from "./completions.generated";
import { bugbusterCompletionOptions } from "./completions";

// The firmware API catalogue the generated table is built from.
const catalogue = JSON.parse(
  readFileSync(resolve(__dirname, "../../../../../../python/firmware_modules/stubs/firmware_api.json"), "utf8"),
) as {
  modules: Array<{ name: string; functions: Array<{ name: string; params: Array<{ name: string; default: string | null; kind: string }> }>;
    classes: Array<{ name: string; kind: string; methods: Array<{ name: string }> }>; constants: Array<{ name: string }> }>;
};

const byLabel = new Map(generatedCompletions.map((c) => [c.label, c]));

describe("generated completions", () => {
  it("cover every module, function, class and constant of the catalogue", () => {
    for (const mod of catalogue.modules) {
      expect(byLabel.has(mod.name), mod.name).toBe(true);
      for (const fn of mod.functions) expect(byLabel.has(`${mod.name}.${fn.name}`), fn.name).toBe(true);
      for (const cls of mod.classes) expect(byLabel.has(`${mod.name}.${cls.name}`), cls.name).toBe(true);
      for (const c of mod.constants) expect(byLabel.has(`${mod.name}.${c.name}`), c.name).toBe(true);
    }
    // namespace methods keep their full path
    expect(byLabel.has("daq.run.status")).toBe(true);
    expect(byLabel.has("daq.run.new")).toBe(true);
  });

  it("insert the required arguments as placeholders and leave optional ones out", () => {
    expect(byLabel.get("bugbuster.sleep")?.snippet).toBe("bugbuster.sleep(${ms})");
    expect(byLabel.get("daq.vdut")?.snippet).toBe("daq.vdut()");
    expect(byLabel.get("daq.run.new")?.snippet).toBe("daq.run.new(${name}, ${chem}, ${cells}, ${capacity_mah})");
    expect(byLabel.get("bugbuster.I2C")?.snippet).toBe("bugbuster.I2C(${sda_io}, ${scl_io})");
  });

  it("carry documentation and no stale names", () => {
    expect(byLabel.get("bugbuster.rail_power_up")?.info).toContain("efuse_mask");
    expect(byLabel.has("get_pin")).toBe(false);
    expect(byLabel.has("set_freq")).toBe(false);
    expect(byLabel.get("bugbuster.FUNC_VOUT")?.detail.length).toBeGreaterThan(0);
  });

  it("are exposed to CodeMirror as completions", () => {
    expect(bugbusterCompletionOptions).toHaveLength(generatedCompletions.length);
    const sleep = bugbusterCompletionOptions.find((o) => o.label === "bugbuster.sleep");
    expect(sleep?.apply).toBeDefined();
  });
});
