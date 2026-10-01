// @vitest-environment happy-dom
// WEB-KEYS (DAQ-11, WEB-21, WEB-22): the web UI read and sent JSON keys the
// firmware never produces or reads. Each mapper is fed the real captured
// fixture (tests/fixtures/http) or keys parsed from the firmware source.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { afterEach, describe, expect, it, vi } from "vitest";
import { api } from "../api/client";
import { setCachedToken } from "../api/core";
import type { UpdateCheckResult } from "../api/types";
import { daqView } from "./daq/daqView";
import { updateSelection } from "./system/otaView";
import { idacRows } from "./voltages/voltagesView";

const REPO = resolve(__dirname, "../../../../..");
const fixture = (name: string) =>
  JSON.parse(readFileSync(resolve(REPO, "tests/fixtures/http", name), "utf-8"));
const fwSource = (rel: string) =>
  readFileSync(resolve(REPO, "Firmware/ESP32/src", rel), "utf-8");

describe("DAQ tab (WEB-21 / DAQ-11)", () => {
  const daq = fixture("daq.json");
  const vdut = fixture("daq__vdut__status.json");

  it.fails("shows the measured VDUT voltage and current", () => {
    const v = daqView(daq, vdut);
    expect(v.vdut.measuredV).toBeCloseTo(vdut.measuredVoltageV, 6);
    expect(v.vdut.measuredA).toBeCloseTo(vdut.measuredCurrentMa / 1000, 9);
  });

  it.fails("labels the HAT from typeName and fwMajor/fwMinor", () => {
    const v = daqView(daq, vdut);
    expect(v.typeLabel).toBe(daq.typeName);
    expect(v.version).toBe(`${daq.fwMajor}.${daq.fwMinor}`);
  });

  it.fails("reports calibration as unknown (firmware exposes no per-range flags)", () => {
    expect(daqView(daq, vdut).calibration).toBe("unknown");
  });
});

describe("GitHub update card (WEB-22)", () => {
  const component = (updateAvailable: boolean) => ({
    available: true, availableBuildId: "b2", version: "9.9.9",
    updateAvailable, size: 1, sha256: "",
  });

  it("firmware emits updateAvailable, not newer", () => {
    const src = fwSource("update/update_manager.cpp");
    expect(src).toContain('"updateAvailable"');
    expect(src).not.toContain('"newer"');
  });

  it.fails("offers the update the firmware reports", () => {
    const r = {
      channel: "nightly", manifestBuildId: "b2", commit: "c",
      rp2040: component(false), esp32: component(true),
    } as unknown as UpdateCheckResult;
    expect(updateSelection(r)).toEqual({ rp2040: false, esp32: true, upToDate: false });
  });
});

describe("Voltages tab IDAC card", () => {
  it.fails("shows each channel's targetV", () => {
    const idac = fixture("idac.json");
    const rows = idacRows(idac);
    expect(rows.map((r) => r.voltage)).toEqual(idac.channels.map((c: any) => c.targetV));
  });
});

describe("RTD config", () => {
  afterEach(() => vi.unstubAllGlobals());

  it.fails("sends a key the firmware reads", async () => {
    const src = fwSource("net/api_core.cpp");
    const handler = src.slice(src.indexOf("api_channel_rtd_config(int ch"));
    const read = new Set(
      [...handler.slice(0, handler.indexOf("\n}\n")).matchAll(/body_get\(body, "(\w+)"\)/g)].map((m) => m[1]),
    );
    expect(read.size).toBeGreaterThan(0);

    let sent: Record<string, unknown> = {};
    vi.stubGlobal("fetch", vi.fn(async (_url: string, init?: RequestInit) => {
      sent = JSON.parse(String(init?.body ?? "{}"));
      return new Response("{}", { status: 200 });
    }));
    setCachedToken("aa:bb", "tok", { remember: true });
    await api.channel.setRtdConfig("aa:bb", 0, 500);
    expect(Object.keys(sent).filter((k) => read.has(k))).not.toEqual([]);
  });
});
