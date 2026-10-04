// The admin token must survive for the page lifetime even when browser storage is unusable
// (private window, blocked site data, sandboxed frame): a silently dropped token looped pairing.
import { afterEach, describe, expect, it, vi } from "vitest";
import { clearCachedToken, getCachedToken, setCachedToken } from "./core";

const broken = () => ({
  getItem: () => { throw new Error("blocked"); },
  setItem: () => { throw new Error("blocked"); },
  removeItem: () => { throw new Error("blocked"); },
});

describe("token cache without storage", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("keeps the token in memory and clears it", () => {
    vi.stubGlobal("localStorage", broken());
    vi.stubGlobal("sessionStorage", broken());
    setCachedToken("aa:bb:cc", "tok");
    expect(getCachedToken("aa:bb:cc")).toBe("tok");
    clearCachedToken("aa:bb:cc");
    expect(getCachedToken("aa:bb:cc")).toBeNull();
  });
});
