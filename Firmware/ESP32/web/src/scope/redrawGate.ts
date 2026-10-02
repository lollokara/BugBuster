// WEB-28: skip canvas frames whose inputs are identical (by reference) to the
// previous frame. Signal values change identity on every update, so a
// reference compare is enough.
export function makeRedrawGate(): (keys: readonly unknown[]) => boolean {
  let prev: readonly unknown[] | null = null;
  return (keys) => {
    const changed =
      prev === null ||
      prev.length !== keys.length ||
      keys.some((k, i) => !Object.is(k, prev![i]));
    prev = keys;
    return changed;
  };
}
