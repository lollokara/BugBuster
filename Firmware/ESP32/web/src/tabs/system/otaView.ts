// Pure helpers for the GitHub update card (tested against firmware keys).
import type { UpdateCheckResult, UpdateComponent } from "../../api/types";

export function componentHasUpdate(c: UpdateComponent | undefined): boolean {
  return Boolean(c?.updateAvailable);
}

export function updateSelection(r: UpdateCheckResult) {
  const rp2040 = componentHasUpdate(r.rp2040);
  const esp32 = componentHasUpdate(r.esp32);
  return { rp2040, esp32, upToDate: !rp2040 && !esp32 };
}
