// =============================================================================
// API client — typed fetch wrapper around the ESP32 /api surface.
//
// Auth: admin token is cached in localStorage under bb:admin-token:<MAC>
// and sent as X-BugBuster-Admin-Token on mutating requests. On a 401 we
// clear the cache and raise an event so the app can re-open the pairing
// modal.
// =============================================================================

/** Matches the #define ADMIN_TOKEN_HEADER in Firmware/ESP32/src/config.h */
export const ADMIN_TOKEN_HEADER = "X-BugBuster-Admin-Token";

const TOKEN_KEY_PREFIX = "bb:admin-token:";

export class PairingRequiredError extends Error {
  constructor() {
    super("Admin token missing or rejected — pairing required");
    this.name = "PairingRequiredError";
  }
}

export interface DeviceInfo {
  siliconRev: number;
  siliconId0: string;
  siliconId1: string;
  macAddress: string;
  spiOk: boolean;
}

export interface PairingInfo {
  macAddress: string;
  tokenFingerprint: string | null;
  transport: "http" | "usb";
}

export interface BoardRail {
  value: number;
  locked: boolean;
}

export interface BoardProfile {
  id: string;
  name: string;
  description: string;
  rails: { vlogic: BoardRail; vadj1: BoardRail; vadj2: BoardRail };
  pinCount: number;
}

export interface BoardState {
  active: string | null;
  available: BoardProfile[];
}

function tokenKey(mac: string): string {
  return TOKEN_KEY_PREFIX + mac.toLowerCase();
}

export function getCachedToken(mac: string): string | null {
  try {
    return localStorage.getItem(tokenKey(mac));
  } catch {
    return null;
  }
}

export function setCachedToken(mac: string, token: string): void {
  try {
    localStorage.setItem(tokenKey(mac), token);
  } catch {
    /* ignore quota errors — next request will just prompt again */
  }
}

export function clearCachedToken(mac: string): void {
  try {
    localStorage.removeItem(tokenKey(mac));
  } catch {
    /* ignore */
  }
}

type Method = "GET" | "POST" | "PUT" | "DELETE";

interface RequestOptions {
  method?: Method;
  body?: unknown;
  mac?: string;
  admin?: boolean;
  signal?: AbortSignal;
}

async function request<T>(path: string, opts: RequestOptions = {}): Promise<T> {
  const { method = "GET", body, mac, admin = false, signal } = opts;
  const headers: Record<string, string> = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (admin && mac) {
    const token = getCachedToken(mac);
    if (!token) throw new PairingRequiredError();
    headers[ADMIN_TOKEN_HEADER] = token;
  }

  const res = await fetch(path, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
    signal,
  });

  if (res.status === 401) {
    if (mac) clearCachedToken(mac);
    window.dispatchEvent(new CustomEvent("bb:pairing-required"));
    throw new PairingRequiredError();
  }

  if (!res.ok) {
    let msg = `${res.status} ${res.statusText}`;
    try {
      const j = await res.json();
      if (j && typeof j.error === "string") msg = j.error;
    } catch {
      /* ignore parse error */
    }
    throw new Error(msg);
  }

  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

/* ---- Typed endpoints ---- */

export const api = {
  deviceInfo: () => request<DeviceInfo>("/api/device/info"),
  pairingInfo: () => request<PairingInfo>("/api/pairing/info"),
  pairingVerify: (mac: string, token: string) =>
    fetch("/api/pairing/verify", {
      method: "POST",
      headers: { [ADMIN_TOKEN_HEADER]: token },
    }).then((r) => {
      if (r.status === 200) {
        setCachedToken(mac, token);
        return true;
      }
      return false;
    }),

  status: () => request<any>("/api/status"),
  scope: (since: number) => request<{ seq: number; samples: number[][] }>(
    `/api/scope?since=${since}`,
  ),
  faults: () => request<any>("/api/faults"),
  debug: () => request<any>("/api/debug"),

  board: () => request<BoardState>("/api/board"),
  boardSelect: (mac: string, boardId: string) =>
    request<{ ok: boolean; active: string }>("/api/board/select", {
      method: "POST",
      body: { boardId },
      mac,
      admin: true,
    }),

  channel: {
    get: (ch: number) => request<any>(`/api/channel/${ch}`),
    setDac: (mac: string, ch: number, code: number) =>
      request<void>(`/api/channel/${ch}/dac`, {
        method: "POST",
        body: { code },
        mac,
        admin: true,
      }),
    setFunction: (mac: string, ch: number, func: number) =>
      request<void>(`/api/channel/${ch}/function`, {
        method: "POST",
        body: { function: func },
        mac,
        admin: true,
      }),
  },

  gpio: () => request<any>("/api/gpio"),
  dio: () => request<any>("/api/dio"),
  idac: () => request<any>("/api/idac"),
  usbpd: () => request<any>("/api/usbpd"),
  hat: () => request<any>("/api/hat"),
  hatLaStatus: () => request<any>("/api/hat/la/status"),
  diagnostics: () => request<any>("/api/diagnostics"),
  selftest: () => request<any>("/api/selftest"),
  uartConfig: () => request<any>("/api/uart/config"),
  wifi: () => request<any>("/api/wifi"),

  mux: Object.assign(() => request<any>("/api/mux"), {
    setSwitch: (mac: string, device: number, switchNum: number, closed: boolean) =>
      request<void>("/api/mux/switch", {
        method: "POST",
        body: { device, switch: switchNum, closed },
        mac,
        admin: true,
      }),
    setAll: (mac: string, states: number[]) =>
      request<void>("/api/mux/all", {
        method: "POST",
        body: { states },
        mac,
        admin: true,
      }),
  }),

  ioexp: Object.assign(() => request<any>("/api/ioexp"), {
    /** control: "vadj1" | "vadj2" | "efuse1" | "efuse2" | "efuse3" | "efuse4" | "15v_a" | "mux" | "usb_hub" */
    setControl: (mac: string, control: string, on: boolean) =>
      request<void>("/api/ioexp/control", {
        method: "POST",
        body: { control, on },
        mac,
        admin: true,
      }),
  }),

  lshift: {
    setOe: (mac: string, enabled: boolean) =>
      request<void>("/api/lshift/oe", {
        method: "POST",
        body: { enabled },
        mac,
        admin: true,
      }),
  },

  wavegen: {
    start: (mac: string, args: { type: string; freq?: number; amplitude?: number; offset?: number }) =>
      request<void>("/api/wavegen/start", {
        method: "POST",
        body: { freq: 100, amplitude: 1, offset: 0, ...args },
        mac,
        admin: true,
      }),
    stop: (mac: string) =>
      request<void>("/api/wavegen/stop", {
        method: "POST",
        mac,
        admin: true,
      }),
  },
};
