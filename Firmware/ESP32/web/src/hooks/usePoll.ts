import { useEffect, useRef, useState } from "preact/hooks";

export interface UsePollOptions<T> {
  immediate?: boolean;
  enabled?: boolean;
  onError?: (error: unknown) => void;
  initialValue?: T | null;
}

/**
 * Poll an async producer with the latest callback body while guarding unmounts.
 * Deliberately small: ESP32 web UI uses this for lightweight status cards and
 * keeps backoff/domain-specific behavior at the call site when needed.
 */
export function usePoll<T>(
  fn: () => Promise<T>,
  intervalMs: number,
  options: UsePollOptions<T> = {},
): T | null {
  const { immediate = true, enabled = true, onError, initialValue = null } = options;
  const [value, setValue] = useState<T | null>(initialValue);
  const fnRef = useRef(fn);
  const onErrorRef = useRef(onError);

  useEffect(() => {
    fnRef.current = fn;
  }, [fn]);

  useEffect(() => {
    onErrorRef.current = onError;
  }, [onError]);

  useEffect(() => {
    if (!enabled) return;
    let alive = true;
    let timeout: ReturnType<typeof setTimeout> | null = null;

    const tick = async () => {
      if (!alive) return;
      try {
        const result = await fnRef.current();
        if (alive) setValue(result);
      } catch (error) {
        if (alive) onErrorRef.current?.(error);
      }
      if (alive) timeout = setTimeout(tick, intervalMs);
    };

    if (immediate) {
      tick();
    } else {
      timeout = setTimeout(tick, intervalMs);
    }

    return () => {
      alive = false;
      if (timeout !== null) clearTimeout(timeout);
    };
  }, [enabled, immediate, intervalMs]);

  return value;
}
