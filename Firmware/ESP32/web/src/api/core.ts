// =============================================================================
// api/core.ts — auth cache, error classes, typed fetch wrapper
// =============================================================================

export const ADMIN_TOKEN_HEADER = "X-BugBuster-Admin-Token";

const TOKEN_KEY_PREFIX = "bb:admin-token:";
const REMEMBER_KEY_PREFIX = "bb:remember-token:";

export class PairingRequiredError extends Error {
  constructor() {
    super("Admin token missing or rejected — pairing required");
    this.name = "PairingRequiredError";
  }
}

export class HttpError extends Error {
  constructor(
    public readonly status: number,
    public readonly statusText: string,
    message: string,
  ) {
    super(message);
    this.name = "HttpError";
  }
}

export class IoOwnerRejectError extends Error {
  constructor(
    public readonly slot: number,
    public readonly currentOwnerKind: number,
  ) {
    super(`IO ownership conflict: slot ${slot} held by kind ${currentOwnerKind}`);
    this.name = "IoOwnerRejectError";
  }
}

function tokenKey(mac: string): string {
  return TOKEN_KEY_PREFIX + mac.toLowerCase();
}

function rememberKey(mac: string): string {
  return REMEMBER_KEY_PREFIX + mac.toLowerCase();
}

export function isPersistentlyRemembered(mac: string): boolean {
  try {
    return localStorage.getItem(rememberKey(mac)) === "1";
  } catch {
    return false;
  }
}

export function getCachedToken(mac: string): string | null {
  const key = tokenKey(mac);
  try {
    const session = sessionStorage.getItem(key);
    if (session) return session;
  } catch { /* ignore */ }
  try {
    const persistent = localStorage.getItem(key);
    if (persistent) {
      if (!isPersistentlyRemembered(mac)) {
        try {
          sessionStorage.setItem(key, persistent);
          localStorage.removeItem(key);
        } catch { /* ignore */ }
      }
      return persistent;
    }
  } catch { /* ignore */ }
  return null;
}

export function setCachedToken(
  mac: string,
  token: string,
  options: { remember?: boolean } = {},
): void {
  const key = tokenKey(mac);
  const persist = options.remember ?? isPersistentlyRemembered(mac);
  try {
    if (persist) {
      localStorage.setItem(key, token);
      localStorage.setItem(rememberKey(mac), "1");
      try { sessionStorage.removeItem(key); } catch { /* ignore */ }
    } else {
      sessionStorage.setItem(key, token);
      try {
        localStorage.removeItem(key);
        localStorage.removeItem(rememberKey(mac));
      } catch { /* ignore */ }
    }
  } catch { /* ignore quota errors */ }
}

export function clearCachedToken(mac: string): void {
  const key = tokenKey(mac);
  try { sessionStorage.removeItem(key); } catch { /* ignore */ }
  try {
    localStorage.removeItem(key);
    localStorage.removeItem(rememberKey(mac));
  } catch { /* ignore */ }
}

type Method = "GET" | "POST" | "PUT" | "DELETE";

export interface RequestOptions {
  method?: Method;
  body?: unknown;
  mac?: string;
  admin?: boolean;
  signal?: AbortSignal;
}

export async function request<T>(path: string, opts: RequestOptions = {}): Promise<T> {
  const { method = "GET", body, mac, admin = false, signal } = opts;
  const headers: Record<string, string> = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (admin && mac) {
    const token = getCachedToken(mac);
    if (!token) throw new PairingRequiredError();
    headers[ADMIN_TOKEN_HEADER] = token;
  }

  let res: Response;
  try {
    res = await fetch(path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      signal,
    });
  } catch (err) {
    throw new HttpError(0, "Network Error", err instanceof Error ? err.message : "fetch failed");
  }

  if (res.status === 401) {
    if (mac) clearCachedToken(mac);
    window.dispatchEvent(new CustomEvent("bb:pairing-required"));
    throw new PairingRequiredError();
  }

  if (res.status === 409) {
    let slot = 0xFF;
    let currentOwnerKind = 0;
    try {
      const j = await res.json();
      if (j && typeof j.slot === "number") slot = j.slot;
      if (j && typeof j.current_owner_kind === "number") currentOwnerKind = j.current_owner_kind;
    } catch { /* ignore */ }
    throw new IoOwnerRejectError(slot, currentOwnerKind);
  }

  if (!res.ok) {
    let msg = `${res.status} ${res.statusText}`;
    try {
      const j = await res.json();
      if (j && typeof j.error === "string") msg = j.error;
    } catch { /* ignore */ }
    throw new HttpError(res.status, res.statusText, msg);
  }

  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

export async function adminRawFetch(mac: string, path: string, init: RequestInit = {}): Promise<Response> {
  const token = getCachedToken(mac);
  if (!token) throw new PairingRequiredError();

  const res = await fetch(path, {
    ...init,
    headers: {
      ...(init.headers as Record<string, string> | undefined),
      [ADMIN_TOKEN_HEADER]: token,
    },
  });

  if (res.status === 401) {
    clearCachedToken(mac);
    window.dispatchEvent(new CustomEvent("bb:pairing-required"));
    throw new PairingRequiredError();
  }

  if (!res.ok) {
    let msg = `${res.status} ${res.statusText}`;
    try {
      const j = await res.json();
      if (j && typeof j.error === "string") msg = j.error;
    } catch {
      try {
        const text = await res.text();
        if (text) msg = text;
      } catch { /* ignore */ }
    }
    throw new HttpError(res.status, res.statusText, msg);
  }

  return res;
}
