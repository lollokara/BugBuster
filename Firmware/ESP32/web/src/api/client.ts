// =============================================================================
// api/client.ts — compatibility facade: re-exports core + types, defines api{}.
//
// All consumers import from "../../api/client" as before. Internals have been
// split into:
//   api/core.ts   — auth cache, error classes, request(), adminRawFetch()
//   api/types.ts  — shared domain interfaces and type aliases
// =============================================================================

export {
  ADMIN_TOKEN_HEADER,
  PairingRequiredError,
  HttpError,
  IoOwnerRejectError,
  isPersistentlyRemembered,
  getCachedToken,
  setCachedToken,
  clearCachedToken,
  request,
  adminRawFetch,
} from "./core";

export type {
  RequestOptions,
} from "./core";

export type {
  DeviceInfo,
  PairingInfo,
  SelftestSuppliesCached,
  ScriptStatus,
  ScriptStorageStatus,
  AutorunStatus,
  SelftestStatus,
  QuickSetupSummary,
  QuickSetupList,
  QuickSetupPayload,
  QuickSetupApplyResult,
  BoardRail,
  BoardProfile,
  BoardState,
  WavegenType,
  OtaPartition,
  OtaInfo,
  OtaUploadResult,
  UpdateCheckResult,
  UpdateStatus,
} from "./types";

import {
  ADMIN_TOKEN_HEADER,
  PairingRequiredError,
  HttpError,
  getCachedToken,
  setCachedToken,
  clearCachedToken,
  request,
  adminRawFetch,
} from "./core";

import type {
  DeviceInfo,
  PairingInfo,
  BoardState,
  SelftestStatus,
  SelftestSuppliesCached,
  ScriptStatus,
  ScriptStorageStatus,
  AutorunStatus,
  QuickSetupList,
  QuickSetupPayload,
  QuickSetupApplyResult,
  OtaInfo,
  OtaUploadResult,
  UpdateCheckResult,
  UpdateStatus,
} from "./types";

/* ---- Typed endpoints ---- */

export const api = {
  deviceInfo: () => request<DeviceInfo>("/api/device/info"),
  pairingInfo: () => request<PairingInfo>("/api/pairing/info"),
  pairingVerify: (
    mac: string,
    token: string,
    options: { remember?: boolean } = {},
  ) =>
    fetch("/api/pairing/verify", {
      method: "POST",
      headers: { [ADMIN_TOKEN_HEADER]: token },
    }).then((r) => {
      if (r.status === 200) {
        setCachedToken(mac, token, options);
        return true;
      }
      return false;
    }),

  pairingRotate: (mac: string) =>
    request<{ ok: boolean; token: string }>("/api/pairing/rotate", {
      method: "POST",
      mac,
      admin: true,
    }),

  status: () => request<any>("/api/status"),
  overview: () => request<{
    idac: any;
    ioexp: any;
    rails: Array<{ rail: number; name: string; voltage: number; ok: boolean }>;
  }>("/api/overview"),
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
    request<any>("/api/hat/reset", { method: "POST", mac, admin: true }),
  hatDetect: (mac: string) =>
    request<any>("/api/hat/detect", { method: "POST", mac, admin: true }),
  hatGetPower: () => request<any>("/api/hat/power"),
  hatSetPower: (mac: string, connector: 0 | 1, enable: boolean) =>
    request<any>("/api/hat/power", {
      method: "POST",
      body: { connector, enable },
      mac,
      admin: true,
    }),
  hatV2Caps: () => request<any>("/api/hat/v2/caps"),
  hatV2Rails: () => request<any>("/api/hat/v2/rails"),
  hatV2SetRailEnable: (mac: string, railId: number, enable: boolean) =>
    request<any>("/api/hat/v2/rail/enable", {
      method: "POST",
      body: { railId, enable },
      mac,
      admin: true,
    }),
  hatV2SetRailVoltage: (mac: string, railId: number, voltageMv: number) =>
    request<any>("/api/hat/v2/rail/voltage", {
      method: "POST",
      body: { railId, voltageMv },
      mac,
      admin: true,
    }),
  hatV2SetLed: (mac: string, ledId: number, colorCode: number) =>
    request<any>("/api/hat/v2/led", {
      method: "POST",
      body: { ledId, colorCode },
      mac,
      admin: true,
    }),
  hatV2SetLaRoute: (mac: string, route: number) =>
    request<any>("/api/hat/v2/la/route", {
      method: "POST",
      body: { route },
      mac,
      admin: true,
    }),
  hatV2CalibrateStart: (mac: string, railId: number) =>
    request<any>("/api/hat/v2/calibrate/start", {
      method: "POST",
      body: { railId },
      mac,
      admin: true,
    }),
  hatV2CalibrateStatus: () => request<any>("/api/hat/v2/calibrate/status"),
  hatV2CalibrateImport: (mac: string, railId: number, points: Array<{ dacCode: number; measuredV: number }>) =>
    request<any>("/api/hat/v2/calibrate/import", {
      method: "POST",
      body: { railId, points },
      mac,
      admin: true,
    }),
  hatV2SetIoBank: (mac: string, dirs: number, ups: number, dns: number, vals: number) =>
    request<any>("/api/hat/v2/io_bank", {
      method: "POST",
      body: { dirs, ups, dns, vals },
      mac,
      admin: true,
    }),
  hatV2SetLevelShift: (mac: string, oe: boolean, dir: boolean) =>
    request<any>("/api/hat/v2/level_shift", {
      method: "POST",
      body: { oe, dir },
      mac,
      admin: true,
    }),
  hatV2SetIoVoltage: (mac: string, voltageMv: number) =>
    request<any>("/api/hat/v2/io_voltage", {
      method: "POST",
      body: { voltageMv },
      mac,
      admin: true,
    }),
  hatV2SetupSwd: (mac: string, targetVoltageMv: number, connector: number) =>
    request<any>("/api/hat/v2/swd/setup", {
      method: "POST",
      body: { targetVoltageMv, connector },
      mac,
      admin: true,
    }),
  hatV2LaLogEnable: (mac: string, enable: boolean) =>
    request<any>("/api/hat/v2/la/log/enable", {
      method: "POST",
      body: { enable },
      mac,
      admin: true,
    }),
  hatV2LaLogPoll: () => request<any>("/api/hat/v2/la/log"),
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

  scripts: {
    files: (mac: string) =>
      request<{ files: string[] }>("/api/scripts/files", { mac, admin: true }),
    storage: (mac: string) =>
      request<ScriptStorageStatus>("/api/scripts/storage", { mac, admin: true }),
    download: async (mac: string, name: string) => {
      const res = await adminRawFetch(mac, `/api/scripts/files/get?name=${encodeURIComponent(name)}`);
      return res.text();
    },
    upload: async (mac: string, name: string, source: string) => {
      const res = await adminRawFetch(mac, `/api/scripts/files?name=${encodeURIComponent(name)}`, {
        method: "POST",
        headers: { "Content-Type": "text/plain; charset=utf-8" },
        body: source,
      });
      return (await res.json()) as { ok?: boolean; err?: string };
    },
    delete: (mac: string, name: string) =>
      request<{ ok?: boolean; err?: string }>(`/api/scripts/files?name=${encodeURIComponent(name)}`, {
        method: "DELETE",
        mac,
        admin: true,
      }),
    runFile: (mac: string, name: string) =>
      request<{ ok?: boolean; id?: number; err?: string }>(`/api/scripts/run-file?name=${encodeURIComponent(name)}`, {
        method: "POST",
        mac,
        admin: true,
      }),
    eval: async (mac: string, source: string, persist: boolean) => {
      const res = await adminRawFetch(mac, `/api/scripts/eval?persist=${persist ? "true" : "false"}`, {
        method: "POST",
        headers: { "Content-Type": "text/plain; charset=utf-8" },
        body: source,
      });
      return (await res.json()) as { ok?: boolean; id?: number; err?: string };
    },
    stop: (mac: string) =>
      request<{ ok?: boolean }>("/api/scripts/stop", { method: "POST", mac, admin: true }),
    reset: (mac: string) =>
      request<{ ok?: boolean }>("/api/scripts/reset", { method: "POST", mac, admin: true }),
    status: (mac: string) =>
      request<ScriptStatus>("/api/scripts/status", { mac, admin: true }),
    logs: async (mac: string, since?: number) => {
      const path = since === undefined ? "/api/scripts/logs" : `/api/scripts/logs?since=${since}`;
      const res = await adminRawFetch(mac, path);
      const text = await res.text();
      const next = Number(res.headers.get("X-BugBuster-Log-Next") ?? "0");
      return { text, next: Number.isFinite(next) ? next : since ?? 0 };
    },
    autorun: (mac: string) =>
      request<AutorunStatus>("/api/scripts/autorun/status", { mac, admin: true }),
    enableAutorun: (mac: string, name: string) =>
      request<{ ok?: boolean; err?: string }>(`/api/scripts/autorun/enable?name=${encodeURIComponent(name)}`, {
        method: "POST",
        mac,
        admin: true,
      }),
    disableAutorun: (mac: string) =>
      request<{ ok?: boolean; err?: string }>("/api/scripts/autorun/disable", {
        method: "POST",
        mac,
        admin: true,
      }),
  },
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
      request<void>("/api/wavegen/stop", { method: "POST", mac, admin: true }),
  },

  faultsClearAll: (mac: string) =>
    request<void>("/api/faults/clear", { method: "POST", mac, admin: true }),
  faultsClearChannel: (mac: string, channel: number) =>
    request<void>(`/api/faults/clear/${channel}`, { method: "POST", mac, admin: true }),
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
    request<void>("/api/device/reset", { method: "POST", mac, admin: true }),

  /* ---- GitHub firmware autoupdate ---- */
  update: {
    check: () => request<UpdateCheckResult>("/api/update/check", { admin: true }),
    status: () => request<UpdateStatus>("/api/update/status", { admin: true }),
    apply: (rp2040: boolean, esp32: boolean) =>
      request<{ success: boolean }>("/api/update/apply", {
        method: "POST",
        admin: true,
        body: JSON.stringify({ rp2040, esp32 }),
      }),
  },

  /* ---- OTA ---- */
  ota: {
    info: () => request<OtaInfo>("/api/ota/info"),
    rollback: (mac: string) =>
      request<{ success: boolean; message?: string }>("/api/ota/rollback", {
        method: "POST",
        mac,
        admin: true,
      }),
    uploadFirmware: (
      mac: string,
      file: Blob,
      onProgress?: (sent: number, total: number) => void,
    ) => uploadOtaImage(mac, "/api/ota/upload", file, true, onProgress),
    uploadSpiffs: (
      mac: string,
      file: Blob,
      onProgress?: (sent: number, total: number) => void,
    ) => uploadOtaImage(mac, "/api/ota/uploadfs", file, false, onProgress),
  },
};

async function sha256Hex(file: Blob): Promise<string> {
  const buf = await file.arrayBuffer();
  const digest = await crypto.subtle.digest("SHA-256", buf);
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

async function uploadOtaImage(
  mac: string,
  path: string,
  file: Blob,
  withSha: boolean,
  onProgress?: (sent: number, total: number) => void,
): Promise<OtaUploadResult> {
  const token = getCachedToken(mac);
  if (!token) throw new PairingRequiredError();

  const url = withSha
    ? `${path}?sha256=${await sha256Hex(file)}`
    : path;

  return await new Promise<OtaUploadResult>((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", url, true);
    xhr.setRequestHeader(ADMIN_TOKEN_HEADER, token);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.responseType = "text";
    if (xhr.upload && onProgress) {
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) onProgress(e.loaded, e.total);
      };
    }
    xhr.onload = () => {
      if (xhr.status === 401) {
        clearCachedToken(mac);
        window.dispatchEvent(new CustomEvent("bb:pairing-required"));
        reject(new PairingRequiredError());
        return;
      }
      if (xhr.status < 200 || xhr.status >= 300) {
        let msg = `${xhr.status} ${xhr.statusText}`;
        try {
          const j = JSON.parse(xhr.responseText);
          if (j && typeof j.error === "string") msg = j.error;
        } catch {
          if (xhr.responseText) msg = xhr.responseText;
        }
        reject(new HttpError(xhr.status, xhr.statusText, msg));
        return;
      }
      try {
        resolve(JSON.parse(xhr.responseText) as OtaUploadResult);
      } catch {
        resolve({ success: true });
      }
    };
    xhr.onerror = () => reject(new Error("Upload network error"));
    xhr.onabort = () => reject(new Error("Upload aborted"));
    xhr.send(file);
  });
}
