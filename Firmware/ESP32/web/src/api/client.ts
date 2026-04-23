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

export interface SelftestSuppliesCached {
  available: boolean;
  timestampMs: number;
  rails: Array<{ rail: number; name: string; voltageV: number }>;
}

export interface SelftestStatus {
  boot?: {
    ran?: boolean;
    passed?: boolean;
    vadj1V?: number;
    vadj2V?: number;
    vlogicV?: number;
  };
  calibration?: {
    status?: number;
    channel?: number;
    points?: number;
    lastVoltageV?: number;
    errorMv?: number;
  };
  workerEnabled?: boolean;
  supplyMonitorActive?: boolean;
}

export interface QuickSetupSummary {
  index?: number;
  occupied?: boolean;
  summary?: unknown;
  name?: string;
  ts?: number;
  timestamp?: number;
  updatedAt?: string;
}

export interface QuickSetupList {
  slots: QuickSetupSummary[];
}

export type QuickSetupPayload = Record<string, unknown>;

export interface QuickSetupApplyResult {
  ok?: boolean;
  applied?: unknown;
  failed?: string[];
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

export type WavegenType = "sine" | "square" | "triangle" | "sawtooth";

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
    throw new HttpError(res.status, res.statusText, msg);
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
    setDacVoltage: (mac: string, ch: number, voltage: number, bipolar = false) =>
      request<void>(`/api/channel/${ch}/dac`, {
        method: "POST",
        body: { voltage, bipolar },
        mac,
        admin: true,
      }),
    setDacCurrent: (mac: string, ch: number, current_mA: number) =>
      request<void>(`/api/channel/${ch}/dac`, {
        method: "POST",
        body: { current_mA },
        mac,
        admin: true,
      }),
    setAdcConfig: (mac: string, ch: number, mux: number, range: number, rate: number) =>
      request<void>(`/api/channel/${ch}/adc/config`, {
        method: "POST",
        body: { mux, range, rate },
        mac,
        admin: true,
      }),
    setDinConfig: (
      mac: string,
      ch: number,
      cfg: {
        thresh: number;
        threshMode?: boolean;
        debounce: number;
        sink?: number;
        sinkRange?: boolean;
        ocDet?: boolean;
        scDet?: boolean;
      },
    ) =>
      request<void>(`/api/channel/${ch}/din/config`, {
        method: "POST",
        body: cfg,
        mac,
        admin: true,
      }),
    setDoConfig: (
      mac: string,
      ch: number,
      cfg: { mode: number; srcSelGpio?: boolean; t1?: number; t2?: number },
    ) =>
      request<void>(`/api/channel/${ch}/do/config`, {
        method: "POST",
        body: cfg,
        mac,
        admin: true,
      }),
    setDoState: (mac: string, ch: number, on: boolean) =>
      request<void>(`/api/channel/${ch}/do/set`, {
        method: "POST",
        body: { on },
        mac,
        admin: true,
      }),
    setVoutRange: (mac: string, ch: number, bipolar: boolean) =>
      request<void>(`/api/channel/${ch}/vout/range`, {
        method: "POST",
        body: { bipolar },
        mac,
        admin: true,
      }),
    setCurrentLimit: (mac: string, ch: number, limit8mA: boolean) =>
      request<void>(`/api/channel/${ch}/ilimit`, {
        method: "POST",
        body: { limit8mA },
        mac,
        admin: true,
      }),
    setAvdd: (mac: string, ch: number, select: number) =>
      request<void>(`/api/channel/${ch}/avdd`, {
        method: "POST",
        body: { select },
        mac,
        admin: true,
      }),
    setRtdConfig: (mac: string, ch: number, current_uA: number) =>
      request<void>(`/api/channel/${ch}/rtd/config`, {
        method: "POST",
        body: { current_uA },
        mac,
        admin: true,
      }),
  },

  gpio: () => request<any>("/api/gpio"),
  gpioSetConfig: (mac: string, gpio: number, mode: number, pulldown: boolean) =>
    request<void>(`/api/gpio/${gpio}/config`, {
      method: "POST",
      body: { mode, pulldown },
      mac,
      admin: true,
    }),
  gpioSetValue: (mac: string, gpio: number, value: boolean) =>
    request<void>(`/api/gpio/${gpio}/set`, {
      method: "POST",
      body: { value },
      mac,
      admin: true,
    }),
  dio: () => request<any>("/api/dio"),
  dioGet: (io: number) => request<any>(`/api/dio/${io}`),
  dioSetConfig: (mac: string, io: number, mode: number) =>
    request<void>(`/api/dio/${io}/config`, {
      method: "POST",
      body: { mode },
      mac,
      admin: true,
    }),
  dioSetValue: (mac: string, io: number, value: boolean) =>
    request<void>(`/api/dio/${io}/set`, {
      method: "POST",
      body: { value },
      mac,
      admin: true,
    }),
  idac: () => request<any>("/api/idac"),
  idacSetCode: (mac: string, ch: number, code: number) =>
    request<any>("/api/idac/code", {
      method: "POST",
      body: { ch, code },
      mac,
      admin: true,
    }),
  idacSetVoltage: (mac: string, ch: number, voltage: number) =>
    request<any>("/api/idac/voltage", {
      method: "POST",
      body: { ch, voltage },
      mac,
      admin: true,
    }),
  idacCalPoint: (mac: string, ch: number, code: number, measuredV: number) =>
    request<any>("/api/idac/cal/point", {
      method: "POST",
      body: { ch, code, measuredV },
      mac,
      admin: true,
    }),
  idacCalClear: (mac: string, ch: number) =>
    request<any>("/api/idac/cal/clear", {
      method: "POST",
      body: { ch },
      mac,
      admin: true,
    }),
  idacCalSave: (mac: string) =>
    request<any>("/api/idac/cal/save", {
      method: "POST",
      mac,
      admin: true,
    }),
  usbpd: () => request<any>("/api/usbpd"),
  usbpdSelect: (mac: string, voltage: 5 | 9 | 12 | 15 | 18 | 20) =>
    request<any>("/api/usbpd/select", {
      method: "POST",
      body: { voltage },
      mac,
      admin: true,
    }),
  usbpdRequestCaps: (mac: string) =>
    request<any>("/api/usbpd/caps", {
      method: "POST",
      mac,
      admin: true,
    }),
  hat: () => request<any>("/api/hat"),
  hatLaStatus: () => request<any>("/api/hat/la/status"),
  hatSetPin: (mac: string, pin: number, fn: number) =>
    request<any>("/api/hat/config", {
      method: "POST",
      body: { pin, function: fn },
      mac,
      admin: true,
    }),
  hatSetPins: (mac: string, pins: number[]) =>
    request<any>("/api/hat/config", {
      method: "POST",
      body: { pins },
      mac,
      admin: true,
    }),
  hatReset: (mac: string) =>
    request<any>("/api/hat/reset", {
      method: "POST",
      mac,
      admin: true,
    }),
  hatDetect: (mac: string) =>
    request<any>("/api/hat/detect", {
      method: "POST",
      mac,
      admin: true,
    }),
  diagnostics: () => request<any>("/api/diagnostics"),
  diagnosticsSetConfig: (mac: string, slot: number, source: number) =>
    request<any>("/api/diagnostics/config", {
      method: "POST",
      body: { slot, source },
      mac,
      admin: true,
    }),
  selftestStatus: () => request<SelftestStatus>("/api/selftest"),
  selftest: () => request<SelftestStatus>("/api/selftest"),
  selftestWorker: (mac: string, enabled: boolean) =>
    request<SelftestStatus>("/api/selftest/worker", {
      method: "POST",
      body: { enabled },
      mac,
      admin: true,
    }),
  selftestSupplies: () => request<any>("/api/selftest/supplies"),
  selftestSuppliesCached: () => request<SelftestSuppliesCached>("/api/selftest/supplies/cached"),
  selftestSupply: (rail: number) => request<any>(`/api/selftest/supply/${rail}`),
  selftestCalibrate: (mac: string, channel: number) =>
    request<any>("/api/selftest/calibrate", {
      method: "POST",
      body: { channel },
      mac,
      admin: true,
    }),
  quicksetupList: () => request<QuickSetupList>("/api/quicksetup"),
  quicksetupGet: (slot: number) => request<QuickSetupPayload>(`/api/quicksetup/${slot}`),
  quicksetupSave: (mac: string, slot: number) =>
    request<QuickSetupPayload>(`/api/quicksetup/${slot}`, {
      method: "POST",
      mac,
      admin: true,
    }),
  quicksetupApply: (mac: string, slot: number) =>
    request<QuickSetupApplyResult>(`/api/quicksetup/${slot}/apply`, {
      method: "POST",
      mac,
      admin: true,
    }),
  quicksetupDelete: (mac: string, slot: number) =>
    request<void>(`/api/quicksetup/${slot}/delete`, {
      method: "POST",
      mac,
      admin: true,
    }),
  uartConfig: () => request<any>("/api/uart/config"),
  uartPins: () => request<any>("/api/uart/pins"),
  uartSetConfig: (
    mac: string,
    id: number,
    cfg: {
      uartNum?: number;
      txPin?: number;
      rxPin?: number;
      baudrate?: number;
      dataBits?: number;
      parity?: number;
      stopBits?: number;
      enabled?: boolean;
    },
  ) =>
    request<any>(`/api/uart/${id}/config`, {
      method: "POST",
      body: cfg,
      mac,
      admin: true,
    }),
  wifi: () => request<any>("/api/wifi"),
  wifiScan: () => request<any>("/api/wifi/scan"),
  wifiConnect: (mac: string, ssid: string, password: string) =>
    request<{ success: boolean; ip: string }>("/api/wifi/connect", {
      method: "POST",
      body: { ssid, password },
      mac,
      admin: true,
    }),

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
    /** control: "vadj1" | "vadj2" | "efuse1" | "efuse2" | "efuse3" | "efuse4" | "15v" | "mux" | "usb" */
    setControl: (mac: string, control: string, on: boolean) =>
      request<void>("/api/ioexp/control", {
        method: "POST",
        body: { control, on },
        mac,
        admin: true,
      }),
    faults: () => request<any>("/api/ioexp/faults"),
    setFaultConfig: (mac: string, auto_disable: boolean, log_events: boolean) =>
      request<any>("/api/ioexp/fault_config", {
        method: "POST",
        body: { auto_disable, log_events },
        mac,
        admin: true,
      }),
  }),

  lshift: {
    setOe: (mac: string, enabled: boolean) =>
      request<void>("/api/lshift/oe", {
        method: "POST",
        body: { on: enabled },
        mac,
        admin: true,
      }),
  },

  wavegen: {
    start: (
      mac: string,
      args: {
        channel: number;
        waveform: 0 | 1 | 2 | 3;
        mode: 0 | 1;
        freq_hz: number;
        amplitude: number;
        offset: number;
      },
    ) =>
      request<void>("/api/wavegen/start", {
        method: "POST",
        body: args,
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

  faultsClearAll: (mac: string) =>
    request<void>("/api/faults/clear", {
      method: "POST",
      mac,
      admin: true,
    }),
  faultsClearChannel: (mac: string, channel: number) =>
    request<void>(`/api/faults/clear/${channel}`, {
      method: "POST",
      mac,
      admin: true,
    }),
  faultsSetMasks: (mac: string, alertMask: number, supplyMask: number) =>
    request<void>("/api/faults/mask", {
      method: "POST",
      body: { alertMask, supplyMask },
      mac,
      admin: true,
    }),
  faultsSetChannelMask: (mac: string, channel: number, mask: number) =>
    request<void>(`/api/faults/mask/${channel}`, {
      method: "POST",
      body: { mask },
      mac,
      admin: true,
    }),
  deviceReset: (mac: string) =>
    request<void>("/api/device/reset", {
      method: "POST",
      mac,
      admin: true,
    }),
};
