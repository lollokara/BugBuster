import { describe, expect, it } from "vitest";
import type { StandbyStatus } from "../../api/client";
import { TIMEOUT_OPTIONS, formatCountdown, idleLine, isHardwareReady, stateLabel, timeoutLabel } from "./standbyView";

const base: StandbyStatus = {
  schema: 1, state: "active", ready: true, stage: 0, generation: 1, timeoutSeconds: 300, clients: 0,
  failedStage: 0, inhibitors: 0, completed: 0, failed: 0, skipped: 0, idleRemainingMs: 272_000,
};

describe("standbyView", () => {
  it("offers exactly the persisted choices", () => {
    expect(TIMEOUT_OPTIONS.map((o) => o.seconds).sort((a, b) => a - b)).toEqual([0, 60, 300, 900]);
    expect(timeoutLabel(0)).toBe("Off");
    expect(timeoutLabel(900)).toBe("15 min");
  });

  it("formats a countdown", () => {
    expect(formatCountdown(272_000)).toBe("4:32");
    expect(formatCountdown(61_001)).toBe("1:02");
    expect(formatCountdown(-5)).toBe("0:00");
  });

  it("explains what holds the device awake", () => {
    expect(idleLine(base)).toBe("Standby in 4:32");
    expect(idleLine({ ...base, clients: 2 })).toBe("Held awake by 2 connected clients");
    expect(idleLine({ ...base, clients: 1, inhibitors: 4 })).toBe("Held awake by 1 connected client and running work");
    expect(idleLine({ ...base, timeoutSeconds: 0 })).toBe("Automatic standby is off");
    expect(idleLine({ ...base, state: "asleep", ready: false })).toBe("Standby");
    expect(idleLine(null)).toBe("--");
  });

  it("labels every state and keeps unknown ones visible", () => {
    expect(stateLabel({ ...base, state: "fault_safe" })).toBe("Fault (outputs safe)");
    expect(stateLabel({ ...base, state: "mystery" })).toBe("mystery");
  });

  it("hardware is ready only when active and settled", () => {
    expect(isHardwareReady(base)).toBe(true);
    expect(isHardwareReady({ ...base, ready: false })).toBe(false);
    expect(isHardwareReady({ ...base, state: "waking", ready: true })).toBe(false);
    expect(isHardwareReady(null)).toBe(false);
  });
});
