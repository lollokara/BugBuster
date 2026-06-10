import { useCallback, useState } from "preact/hooks";
export function errorMessage(error) {
    return error instanceof Error ? error.message : String(error);
}
/** Wrap a mutating UI action with consistent busy/error/result state. */
export function useAsyncAction() {
    const [state, setState] = useState({
        busy: false,
        error: null,
        result: null,
    });
    const run = useCallback(async (action) => {
        setState((prev) => ({ ...prev, busy: true, error: null }));
        try {
            const result = await action();
            setState({ busy: false, error: null, result });
            return result;
        }
        catch (error) {
            setState({ busy: false, error: errorMessage(error), result: null });
            return null;
        }
    }, []);
    const reset = useCallback(() => {
        setState({ busy: false, error: null, result: null });
    }, []);
    return { ...state, run, reset };
}
