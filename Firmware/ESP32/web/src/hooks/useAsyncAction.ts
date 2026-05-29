import { useCallback, useState } from "preact/hooks";

export interface AsyncActionState<T> {
  busy: boolean;
  error: string | null;
  result: T | null;
}

export function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/** Wrap a mutating UI action with consistent busy/error/result state. */
export function useAsyncAction<T = void>() {
  const [state, setState] = useState<AsyncActionState<T>>({
    busy: false,
    error: null,
    result: null,
  });

  const run = useCallback(async (action: () => Promise<T>): Promise<T | null> => {
    setState((prev) => ({ ...prev, busy: true, error: null }));
    try {
      const result = await action();
      setState({ busy: false, error: null, result });
      return result;
    } catch (error) {
      setState({ busy: false, error: errorMessage(error), result: null });
      return null;
    }
  }, []);

  const reset = useCallback(() => {
    setState({ busy: false, error: null, result: null });
  }, []);

  return { ...state, run, reset };
}
