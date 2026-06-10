import { useEffect, useRef, useState } from "preact/hooks";
/**
 * Poll an async producer with the latest callback body while guarding unmounts.
 * Deliberately small: ESP32 web UI uses this for lightweight status cards and
 * keeps backoff/domain-specific behavior at the call site when needed.
 */
export function usePoll(fn, intervalMs, options = {}) {
    const { immediate = true, enabled = true, onError, initialValue = null } = options;
    const [value, setValue] = useState(initialValue);
    const fnRef = useRef(fn);
    const onErrorRef = useRef(onError);
    useEffect(() => {
        fnRef.current = fn;
    }, [fn]);
    useEffect(() => {
        onErrorRef.current = onError;
    }, [onError]);
    useEffect(() => {
        if (!enabled)
            return;
        let alive = true;
        let timeout = null;
        const tick = async () => {
            if (!alive)
                return;
            try {
                const result = await fnRef.current();
                if (alive)
                    setValue(result);
            }
            catch (error) {
                if (alive)
                    onErrorRef.current?.(error);
            }
            if (alive)
                timeout = setTimeout(tick, intervalMs);
        };
        if (immediate) {
            tick();
        }
        else {
            timeout = setTimeout(tick, intervalMs);
        }
        return () => {
            alive = false;
            if (timeout !== null)
                clearTimeout(timeout);
        };
    }, [enabled, immediate, intervalMs]);
    return value;
}
